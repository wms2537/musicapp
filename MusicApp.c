#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h> // Added for LLONG_MAX
#include <alsa/asoundlib.h>
#include <fcntl.h> // For non-blocking stdin
#include <errno.h> // For snd_strerror(errno)
#include <math.h>  // For sqrt, pow if we use more advanced curves later
#include <time.h>    // For timestamps in logging
#include <stdarg.h>  // For va_list in logging
#include "const.h"

#ifndef SND_PCM_FORMAT_UNKNOWN // Guard against redefinition if const.h or other includes might have it
#define SND_PCM_FORMAT_UNKNOWN (-1) // Example, actual value might differ or be in alsa/asoundlib.h
#endif

// --- Logging Globals and Function ---
FILE *log_fp = NULL;
const char *LOG_FILE_NAME = "music_app.log";
static int debug_enabled = 0; // Runtime flag to enable/disable DEBUG logs. Default to 0 (off).

// Forward declaration for app_log if DBG is defined before app_log full definition
void app_log(const char *type, const char *format, ...);

// Macro for conditional debug logging
#define DBG(...) do { if (debug_enabled) app_log("DEBUG", __VA_ARGS__); } while(0)

void app_log(const char *type, const char *format, ...) {
    if (log_fp == NULL) { // Fallback if log file couldn't be opened
        // This is a last resort, should not happen if main's check is correct.
        // If it does, print to stderr to indicate critical log failure.
        fprintf(stderr, "CRITICAL_LOG_FAILURE: Attempted to log when log_fp is NULL. Type: %s\n", type);
        va_list args_err;
        va_start(args_err, format);
        vfprintf(stderr, format, args_err);
        va_end(args_err);
        fprintf(stderr, "\n");
        return;
    }

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[20]; // "YYYY-MM-DD HH:MM:SS"
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

    fprintf(log_fp, "[%s] [%s] ", timestamp, type);

    va_list args;
    va_start(args, format);
    vfprintf(log_fp, format, args);
    va_end(args);

    fprintf(log_fp, "\n");
    fflush(log_fp); // Ensure logs are written immediately
}

// --- ALSA Mixer Globals for Volume Control ---
snd_mixer_t *mixer_handle = NULL;
snd_mixer_elem_t *mixer_elem = NULL;
long vol_min, vol_max; // Raw volume range

const int NUM_VOLUME_LEVELS = 4;
int current_volume_level_idx = 2;
const char *sound_card_name = "default";
const char *mixer_control_name = "PCM";

// max output for the speaker on board is not accurate
bool USR_BOARD_SPEAKER_MAX = true;
bool playback_paused = false; // Added for Pause/Resume functionality
static float hpf_prev_input = 0.0f;
static float hpf_prev_output = 0.0f;
static const float HPF_ALPHA = 0.995f; // High-pass filter coefficient (~70Hz cutoff at 44.1kHz)

// --- Music File Playlist Globals ---
char **music_files = NULL;
int num_music_files = 0;
int current_track_idx = 0;

// --- Playback Speed Globals ---
const double PLAYBACK_SPEED_FACTORS[] = {0.5, 1.0, 1.5, 2.0};
const int NUM_SPEED_LEVELS = sizeof(PLAYBACK_SPEED_FACTORS) / sizeof(PLAYBACK_SPEED_FACTORS[0]);
int current_speed_idx = 1; // Index for 1.0x speed

// --- Speed Control Method Flag ---
// Flag to determine which speed control method to use
// true: Use WSOLA (pitch-preserving)
// false: Use simple seek-based speed control (changes pitch)
bool use_wsola_for_speed_control = true;
// For simple speed control (when use_wsola_for_speed_control is false)
const int SIMPLE_SPEED_SEEK_AMOUNTS[] = {SPEED_FACTOR_05X, SPEED_FACTOR_10X, SPEED_FACTOR_15X, SPEED_FACTOR_20X}; // Corresponding to 0.5x, 1.0x, 1.5x, 2.0x

// --- Equalizer Globals ---
const FIRFilter *EQ_PRESETS[] = {&FIR_NORMAL, &FIR_BASS_BOOST, &FIR_TREBLE_BOOST};
const int NUM_EQ_PRESETS = sizeof(EQ_PRESETS) / sizeof(EQ_PRESETS[0]);
int current_eq_idx = 0; // Default to Normal/Flat
static short fir_history[MAX_FIR_TAPS -1]; // Buffer to store previous samples for FIR calculation
static int fir_filter_history_idx = 0;    // Index for circular FIR history buffer

// --- WSOLA State Global ---
WSOLA_State *wsola_state = NULL;

// --- WSOLA Function Prototypes ---
static void generate_hann_window(float *float_window, int length);
static void convert_float_window_to_q15(const float* float_window, short* q15_window, int length);
static float calculate_normalized_cross_correlation(const short *segment1, const short *segment2, int length);
static bool get_segment_from_ring_buffer(const WSOLA_State *state, int start_index_in_ring, int length, short *output_segment);
static long long find_best_match_segment(
    const WSOLA_State *state, 
    const short *target_segment_for_comparison, 
    int ideal_search_center_ring_idx, 
    int *best_segment_start_offset_from_ideal_center_ptr
);

bool wsola_init(WSOLA_State **state_ptr, int sample_rate_arg, int num_channels_arg, double initial_speed_factor_arg, 
                int analysis_frame_ms_arg, float overlap_percentage_arg, int search_window_ms_arg);
int wsola_process(WSOLA_State *state, const short *input_samples, int num_input_samples, 
                  short *output_buffer, int max_output_samples);
void wsola_destroy(WSOLA_State **state_ptr);

void initialize_fir_history() {
    for (int i = 0; i < MAX_FIR_TAPS - 1; ++i) {
        fir_history[i] = 0;
    }
    fir_filter_history_idx = 0; // Reset circular buffer index
}

// Applies an FIR filter to a block of 16-bit PCM audio samples.
// Input and output buffers can be the same for in-place processing if carefully managed,
// but separate buffers are safer and often clearer.
// This function assumes single-channel (mono) data for simplicity or that stereo data is interleaved
// and will be processed sample by sample (left, right, left, right...)
// If stereo, the filter is applied independently to each channel if called per channel,
// or if called on the whole buffer, it will mix channels based on tap count if not careful.
// For true stereo FIR, you'd typically process each channel with its own history.
// This implementation processes samples sequentially, assuming they are mono or caller handles stereo separation.
void apply_fir_filter(short *input_buffer, short *output_buffer, int num_samples, const FIRFilter *filter) {
    // --- TEMPORARY BYPASS FOR DEBUGGING --- (REMOVING THIS)
    // if (input_buffer != output_buffer) {
    //     memcpy(output_buffer, input_buffer, num_samples * sizeof(short));
    // }
    // return; 
    // --- END TEMPORARY BYPASS ---

    if (filter == NULL || filter->num_taps == 0 || filter->num_taps > MAX_FIR_TAPS) {
        // Invalid filter, pass through or handle error
        if (input_buffer != output_buffer) {
            memcpy(output_buffer, input_buffer, num_samples * sizeof(short));
        }
        return;
    }

    // For each output sample
    for (int i = 0; i < num_samples; ++i) {
        double filtered_sample = 0.0;

        // Current input sample
        short current_sample = input_buffer[i];

        // Convolve with filter coefficients
        // Start with the current sample and the first coefficient
        filtered_sample += current_sample * filter->coeffs[0];

        // Then convolve with historical samples using circular buffer
        int history_len = filter->num_taps - 1;
        if (history_len > 0) {
            for (int j = 1; j < filter->num_taps; ++j) {
                // Access history: (current_write_idx - j + history_len) % history_len
                // fir_filter_history_idx is the current position to write current_sample into.
                // So, the j-th previous sample (coeffs[j] is for x[n-j]) is at this offset from fir_filter_history_idx.
                int access_idx = (fir_filter_history_idx - j + history_len) % history_len;
                filtered_sample += fir_history[access_idx] * filter->coeffs[j];
            }
        }

        // Update history: Store current_sample in circular buffer
        if (history_len > 0) {
            fir_history[fir_filter_history_idx] = current_sample;
            fir_filter_history_idx = (fir_filter_history_idx + 1) % history_len;
        }

        // Clamp and store the result
        // Output is 16-bit, so clamp to short range
        if (filtered_sample > 32767.0) filtered_sample = 32767.0;
        else if (filtered_sample < -32768.0) filtered_sample = -32768.0;
        output_buffer[i] = (short)round(filtered_sample);
    }
}

void set_volume_by_level_idx(int level_idx) {
    if (mixer_elem == NULL) {
        return;
    }

    if (level_idx < 0) level_idx = 0;
    if (level_idx >= NUM_VOLUME_LEVELS) level_idx = NUM_VOLUME_LEVELS - 1;

    long target_raw_volume;
    double proportion = 0.0;

    if (NUM_VOLUME_LEVELS > 1) {
        proportion = (double)level_idx / (NUM_VOLUME_LEVELS - 1);
    } else if (NUM_VOLUME_LEVELS == 1 && level_idx == 0) {
        proportion = 1.0; // For a single level, set to max proportion
    } else {
        proportion = 0.0; // Default to min proportion for invalid NUM_VOLUME_LEVELS
        if (NUM_VOLUME_LEVELS <= 0) {
            fprintf(stderr, "Warning: NUM_VOLUME_LEVELS is invalid (%d), using min volume.\n", NUM_VOLUME_LEVELS);
        }
    }

    if (NUM_VOLUME_LEVELS > 0) { // Ensure this branch is taken if NUM_VOLUME_LEVELS is valid
      target_raw_volume = vol_min + (long)(proportion * (vol_max - vol_min) + 0.5);
    } else { // Fallback if NUM_VOLUME_LEVELS is not sensible (e.g. 0 or negative)
      target_raw_volume = (vol_min + vol_max) / 2; // Should not happen with const NUM_VOLUME_LEVELS = 4
    }


    if (target_raw_volume < vol_min) target_raw_volume = vol_min;
    if (target_raw_volume > vol_max) target_raw_volume = vol_max;

    int ret_set_vol;
    // We assume snd_mixer_selem_has_playback_volume(mixer_elem) was true from init_mixer
    ret_set_vol = snd_mixer_selem_set_playback_volume_all(mixer_elem, target_raw_volume);

    if (ret_set_vol < 0) {
        fprintf(stderr, "Error setting raw playback volume (target val: %ld): %s\n", target_raw_volume, snd_strerror(ret_set_vol));
    } else {
        current_volume_level_idx = level_idx;
        double percentage_of_raw_range = 0.0;
        if (vol_max > vol_min) {
             percentage_of_raw_range = ((double)(target_raw_volume - vol_min) / (vol_max - vol_min)) * 100.0;
        } else if (target_raw_volume == vol_min && vol_max == vol_min) {
             percentage_of_raw_range = 100.0;
        }

        printf("Volume set to level %d/%d (Target Raw ALSA Val: %ld, Raw Range: %ld-%ld, ~%.0f%% of raw)\n",
               current_volume_level_idx + 1, NUM_VOLUME_LEVELS, target_raw_volume, vol_min, vol_max, percentage_of_raw_range);

        // VERIFY by getting the volume back
        long actual_raw_val_L = -1;
        int get_raw_err_L = snd_mixer_selem_get_playback_volume(mixer_elem, SND_MIXER_SCHN_FRONT_LEFT, &actual_raw_val_L);

        if (get_raw_err_L < 0) {
            fprintf(stderr, "VERIFY Warning: Could not get raw playback volume for Front Left (err: %s).\n", snd_strerror(get_raw_err_L));
        } else {
            printf("VERIFY: Raw ALSA Val after set for Front Left: %ld\n", actual_raw_val_L);
            if (actual_raw_val_L != target_raw_volume) {
                 printf("VERIFY Discrepancy: Target Raw was %ld, ALSA reports %ld for Front Left.\n", target_raw_volume, actual_raw_val_L);
            }
        }
    }
}

bool init_mixer() {
    snd_mixer_selem_id_t *sid;
    int err;
    const char* final_mixer_name_used = mixer_control_name;

    if ((err = snd_mixer_open(&mixer_handle, 0)) < 0) {
        fprintf(stderr, "Mixer open error: %s\n", snd_strerror(err));
        mixer_handle = NULL; return false;
    }
    if ((err = snd_mixer_attach(mixer_handle, sound_card_name)) < 0) {
        fprintf(stderr, "Mixer attach error for %s: %s\n", sound_card_name, snd_strerror(err));
        snd_mixer_close(mixer_handle); mixer_handle = NULL; return false;
    }
    if ((err = snd_mixer_selem_register(mixer_handle, NULL, NULL)) < 0) {
        fprintf(stderr, "Mixer register error: %s\n", snd_strerror(err));
        snd_mixer_close(mixer_handle); mixer_handle = NULL; return false;
    }
    if ((err = snd_mixer_load(mixer_handle)) < 0) {
        fprintf(stderr, "Mixer load error: %s\n", snd_strerror(err));
        snd_mixer_close(mixer_handle); mixer_handle = NULL; return false;
    }

    snd_mixer_selem_id_alloca(&sid);
    if (sid == NULL) {
        fprintf(stderr, "Failed to allocate mixer selem id on stack.\n");
        snd_mixer_close(mixer_handle); mixer_handle = NULL; return false;
    }
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, final_mixer_name_used);

    mixer_elem = snd_mixer_find_selem(mixer_handle, sid);
    if (!mixer_elem) {
        fprintf(stderr, "Unable to find simple mixer control '%s'; trying 'Master'.\n", final_mixer_name_used);
        final_mixer_name_used = "Master";
        snd_mixer_selem_id_set_name(sid, final_mixer_name_used);
        mixer_elem = snd_mixer_find_selem(mixer_handle, sid);
        if (!mixer_elem) {
            fprintf(stderr, "Unable to find '%s' control either. Volume control will be disabled.\n", final_mixer_name_used);
            snd_mixer_close(mixer_handle); mixer_handle = NULL; return false;
        }
    }
    printf("Using '%s' volume control.\n", final_mixer_name_used);

    if (!snd_mixer_selem_has_playback_volume(mixer_elem)) {
        fprintf(stderr, "Mixer element '%s' does not support generic playback volume. Volume control will be disabled.\n", final_mixer_name_used);
        snd_mixer_close(mixer_handle); mixer_handle = NULL; return false;
    }

    // Get raw volume range
    if (snd_mixer_selem_get_playback_volume_range(mixer_elem, &vol_min, &vol_max) < 0) {
        fprintf(stderr, "Error getting raw volume range for '%s': %s. Volume control will be disabled.\n", final_mixer_name_used, snd_strerror(errno));
        snd_mixer_close(mixer_handle); mixer_handle = NULL; return false;
    }

	if (USR_BOARD_SPEAKER_MAX){
		// vol_max for the board is about 500
		vol_max = 512;
	}

    printf("Raw volume range for '%s': Min=%ld, Max=%ld\n", final_mixer_name_used, vol_min, vol_max);

    if (vol_min == vol_max) {
        printf("Warning: Mixer control '%s' (raw) has a fixed volume (Min==Max). Volume adjustment may not have an effect.\n", final_mixer_name_used);
    }

    set_volume_by_level_idx(current_volume_level_idx); // Set initial volume
    return true;
}

void increase_volume() {
    if (!mixer_handle || !mixer_elem) return;
    if (current_volume_level_idx < NUM_VOLUME_LEVELS - 1) {
        set_volume_by_level_idx(current_volume_level_idx + 1);
    } else {
        printf("Volume at maximum level.\n");
    }
}

void decrease_volume() {
    if (!mixer_handle || !mixer_elem) return;
    if (current_volume_level_idx > 0) {
        set_volume_by_level_idx(current_volume_level_idx - 1);
    } else {
        printf("Volume at minimum level.\n");
    }
}

bool debug_msg(int result, const char *str) {
    if (result < 0) {
        fprintf(stderr, "err: %s 失败!, result = %d, err_info = %s \n", str, result, snd_strerror(result));
        exit(1);
    }
    return true;
}

void open_music_file(const char *path_name) {
    fp = fopen(path_name, "rb");
    if (fp == NULL) {
        perror("Error opening WAV file");
        exit(EXIT_FAILURE);
    }
    wav_header_size = fread(&wav_header, 1, sizeof(struct WAV_HEADER), fp);
    if (wav_header_size < 44) {
        fprintf(stderr, "Error: Incomplete WAV header read. Read %d bytes.\n", wav_header_size);
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    if (strncmp(wav_header.chunk_id, "RIFF", 4) != 0 ||
        strncmp(wav_header.format, "WAVE", 4) != 0 ||
        strncmp(wav_header.sub_chunk1_id, "fmt ", 4) != 0) {
        fprintf(stderr, "Error: File does not appear to be a valid WAV file (missing RIFF/WAVE/fmt markers).\n");
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    printf("------------- WAV Header Info -------------\n");
    printf("RIFF ID: %.4s, Chunk Size: %u, Format: %.4s\n", wav_header.chunk_id, wav_header.chunk_size, wav_header.format);
    printf("Subchunk1 ID: %.4s, Subchunk1 Size: %u\n", wav_header.sub_chunk1_id, wav_header.sub_chunk1_size);
    printf("Audio Format: %u (1=PCM), Num Channels: %u\n", wav_header.audio_format, wav_header.num_channels);
    printf("Sample Rate: %u, Byte Rate: %u\n", wav_header.sample_rate, wav_header.byte_rate);
    printf("Block Align: %u, Bits Per Sample: %u\n", wav_header.block_align, wav_header.bits_per_sample);
    if (strncmp(wav_header.sub_chunk2_id, "data", 4) == 0) {
        printf("Data ID: %.4s, Data Size: %u\n", wav_header.sub_chunk2_id, wav_header.sub_chunk2_size);
    } else {
        printf("Warning: 'data' chunk ID not found immediately. sub_chunk2_id: %.4s\n", wav_header.sub_chunk2_id);
    }
    printf("-----------------------------------------\n");
}

// Function to load the current track
bool load_track(int track_idx) {
    if (track_idx < 0 || track_idx >= num_music_files) {
        app_log("ERROR", "Track index %d out of bounds (0-%d).", track_idx, num_music_files - 1);
        return false;
    }
    if (fp != NULL) {
        fclose(fp);
        fp = NULL;
    }
    app_log("INFO", "Loading track %d/%d: %s", track_idx + 1, num_music_files, music_files[track_idx]);
    open_music_file(music_files[track_idx]); // open_music_file handles its own errors/exit

    // Reset playback state for the new track
    playback_paused = false; 
    initialize_fir_history(); // Reset FIR history for new track
    hpf_prev_input = 0.0f;    // Reset HPF state for new track
    hpf_prev_output = 0.0f;   // Reset HPF state for new track

    // --- Re-initialize WSOLA for the new track if necessary ---
    if (wsola_state != NULL) {
        wsola_destroy(&wsola_state); // Destroy existing state
    }
    // Attempt to init WSOLA if new track is compatible (S16_LE Mono)
    // Note: `rate` and `pcm_format` should be updated by open_music_file and subsequent logic in main for the *first* track.
    // For subsequent tracks, `wav_header` is updated by open_music_file. We need to use these fresh values.
    if (wav_header.audio_format == 1 && wav_header.bits_per_sample == 16 && wav_header.num_channels == 1) { // PCM S16_LE MONO
        // Use current_speed_idx which holds the user's desired speed setting.
        // `rate` for ALSA might differ slightly from `wav_header.sample_rate` due to `set_rate_near`.
        // It's crucial WSOLA uses the actual sample rate of the data it processes.
        // Assuming `wav_header.sample_rate` is the true rate of the file data before any ALSA adjustments.
        if (!wsola_init(&wsola_state, wav_header.sample_rate, wav_header.num_channels, 
                        PLAYBACK_SPEED_FACTORS[current_speed_idx],
                        DEFAULT_ANALYSIS_FRAME_MS, DEFAULT_OVERLAP_PERCENTAGE, DEFAULT_SEARCH_WINDOW_MS)) {
            app_log("ERROR", "Failed to re-initialize WSOLA for new track. Pitch-preserving speed control disabled for this track.");
        }
    } else {
        app_log("INFO", "New track is not S16_LE Mono. WSOLA disabled for this track. Format: %d, Channels: %d, Bits: %d", 
                wav_header.audio_format, wav_header.num_channels, wav_header.bits_per_sample);
        wsola_state = NULL; // Ensure it remains NULL
    }

    // Critical: Re-check and apply ALSA parameters if they can change per track
    // For now, we assume parameters like rate/format are compatible or re-derived
    // in open_music_file and subsequent ALSA setup in main.
    // If not, a more robust re-initialization of ALSA hw_params might be needed here.
    // snd_pcm_prepare(pcm_handle); // Prepare ALSA for new track data

    return true;
}

int main(int argc, char *argv[]) {
    int opt_char;
    // Initialize rate and pcm_format to indicate they haven't been set by user or defaults yet
    rate = 0; 
    pcm_format = SND_PCM_FORMAT_UNKNOWN; // Or another sentinel

    bool file_opened = false;
    bool user_specified_rate = false;
    bool user_specified_format = false;
    int original_stdin_flags = -1;

    log_fp = fopen(LOG_FILE_NAME, "a");
    if (log_fp == NULL) {
        // This is a critical failure: cannot open log file.
        // Print to stderr as per fallback logic, then exit.
        perror("CRITICAL_ERROR: Failed to open log file music_app.log");
        exit(EXIT_FAILURE);
    }

    initialize_fir_history(); // Initialize FIR history at the start
    app_log("INFO", "MusicApp starting...");

    // --- Argument Parsing for multiple music files ---
    // Store music file paths provided as non-option arguments
    // We will look for files after all options are processed by getopt

    while ((opt_char = getopt(argc, argv, "m:f:r:d:s:")) != -1) {
        switch (opt_char) {
            case 'm':
                // The -m option for a single file is now less relevant if we take multiple files.
                // We can choose to make it an error, ignore it, or treat it as one of the files.
                // For now, let's print a warning and tell user to just list files.
                fprintf(stderr, "Warning: -m option is deprecated for single file. List files directly after options.\n");
                // If you still want to support -m as one of the files:
                // if (num_music_files == 0) { // crude way to handle if no files listed yet
                //     music_files = malloc(sizeof(char*));
                //     music_files[0] = strdup(optarg);
                //     num_music_files = 1;
                // }
                // file_opened = true; // This logic will change
                break;
            case 'f': {
                int format_code = atoi(optarg);
                user_specified_format = true;
                switch (format_code) {
                    // Using ALSA defined enums directly is preferred if command line maps to them.
                    // If format_code is a custom code, it needs to map to these enums.
                    // For simplicity, assuming format_code might directly correspond to common patterns or a simplified set.
                    // This section needs to map user's codes to actual snd_pcm_format_t enums.
                    // Example: if user types '161' for S16_LE, map it to SND_PCM_FORMAT_S16_LE.
                    // Let's assume for now the codes are just placeholders for direct enum values if they were passed differently
                    // OR we create a mapping. The original code had magic numbers like 161.
                    // We will now map a new set of simpler codes to the ALSA enums.
                    // 1 -> S16_LE, 2 -> S16_BE, 3 -> S24_LE, 4 -> S24_BE, etc.
                    case 1: pcm_format = SND_PCM_FORMAT_S16_LE; break;
                    case 2: pcm_format = SND_PCM_FORMAT_S16_BE; break;
                    case 3: pcm_format = SND_PCM_FORMAT_S24_LE; break;
                    case 4: pcm_format = SND_PCM_FORMAT_S24_BE; break;
                    case 5: pcm_format = SND_PCM_FORMAT_S24_3LE; break;
                    case 6: pcm_format = SND_PCM_FORMAT_S24_3BE; break;
                    case 7: pcm_format = SND_PCM_FORMAT_S32_LE; break;
                    case 8: pcm_format = SND_PCM_FORMAT_S32_BE; break;
                    default:
                        fprintf(stderr, "Unsupported format code: %d. Format will be inferred from WAV header if possible.\n", format_code);
                        fprintf(stderr, "Supported codes: 1 (S16_LE), 2 (S16_BE), 3 (S24_LE), 4 (S24_BE), 5 (S24_3LE), 6 (S24_3BE), 7 (S32_LE), 8 (S32_BE)\n");
                        user_specified_format = false; // Mark as not successfully set by user
                        pcm_format = SND_PCM_FORMAT_UNKNOWN; // Reset to unknown
                        break;
                }
                if (pcm_format != SND_PCM_FORMAT_UNKNOWN) {
                     printf("User selected format code %d: %s\n", format_code, snd_pcm_format_name(pcm_format));
                }
                break;
            }
            case 'r': {
                int rate_code = atoi(optarg);
                user_specified_rate = true;
                if (rate_code == 44) { rate = 44100; }
                else if (rate_code == 88) { rate = 88200; }
                else if (rate_code == 8) { rate = 8000; }
                else if (rate_code == 48) { rate = 48000; }
                else {
                    fprintf(stderr, "Unsupported rate code: %d. Try to use WAV header rate.\n", rate_code);
                    user_specified_rate = false; // Mark as not successfully set by user
                    rate = 0; // Reset rate
                }
                if (rate != 0) {
                    printf("User selected rate: %u Hz\n", rate);
                }
                break;
            }
			case 'd':{
				int device = atoi(optarg);
				if (device){
					USR_BOARD_SPEAKER_MAX = false;
					printf("Using external sound output device");
				} else{
					printf("Using default sound output device");
				}
				break;
			}
            case 's':{
                int speed_method = atoi(optarg);
                if (speed_method == 0){
                    use_wsola_for_speed_control = false;
                    printf("Using simple seek-based speed control (changes pitch)\n");
                } else {
                    use_wsola_for_speed_control = true;
                    printf("Using WSOLA pitch-preserving speed control\n");
                }
                break;
            }
            default:
                exit(EXIT_FAILURE);
        }
    }

    // After getopt, optind is the index of the first non-option argument
    if (optind < argc) {
        num_music_files = argc - optind;
        music_files = malloc(num_music_files * sizeof(char*));
        if (music_files == NULL) {
            app_log("ERROR", "Failed to allocate memory for music file list.");
            exit(EXIT_FAILURE);
        }
        for (int i = 0; i < num_music_files; i++) {
            music_files[i] = argv[optind + i];
        }
        current_track_idx = 0; // Start with the first file in the list
        if (!load_track(current_track_idx)) {
             // load_track calls open_music_file which exits on failure, 
             // but if load_track itself fails (e.g. bad index, though not here), handle it.
             exit(EXIT_FAILURE); 
        }
        file_opened = true; // Mark that we have at least one file to play
    } else {
        // No music files provided as non-option arguments
        // Check if -m was used (legacy) - this part needs to be cleaner if -m is kept
        // For now, if no files after options, it's an error.
        app_log("ERROR", "No music files provided.");
        fprintf(stderr, "Usage: %s [options] <music_file1.wav> [music_file2.wav ...]\n", argv[0]);
        fprintf(stderr, "Options: [-f <format_code>] [-r <rate_code>] [-d <device_code>]\n");
        exit(EXIT_FAILURE);
    }

    // Determine rate if not set by user
    if (!user_specified_rate || rate == 0) {
        if (wav_header.sample_rate > 0) {
            rate = wav_header.sample_rate;
            printf("Using sample rate from WAV header: %u Hz\n", rate);
        } else {
            fprintf(stderr, "Error: Could not determine sample rate from WAV header and not specified by user. Defaulting to 44100 Hz.\n");
            rate = 44100; // A common default
        }
    }

    // Determine format if not set by user
    if (!user_specified_format || pcm_format == SND_PCM_FORMAT_UNKNOWN) {
        printf("Attempting to infer PCM format from WAV header (BitsPerSample: %d)...\n", wav_header.bits_per_sample);
        switch (wav_header.bits_per_sample) {
            case 8: pcm_format = SND_PCM_FORMAT_U8; break; // Common for 8-bit
            case 16: pcm_format = SND_PCM_FORMAT_S16_LE; break; // Common for 16-bit
            case 24: pcm_format = SND_PCM_FORMAT_S24_LE; break; // Common for 24-bit
            case 32: pcm_format = SND_PCM_FORMAT_S32_LE; break; // Common for 32-bit
            default:
                fprintf(stderr, "Error: Unsupported bits_per_sample in WAV header (%d) and format not specified by user.\n", wav_header.bits_per_sample);
                fprintf(stderr, "Please specify a format using -f option.\n");
                exit(EXIT_FAILURE);
        }
        printf("Inferred PCM format: %s\n", snd_pcm_format_name(pcm_format));
    }

    // Final check if critical parameters are set
    if (rate == 0 || pcm_format == SND_PCM_FORMAT_UNKNOWN) {
        app_log("ERROR", "Critical parameters (rate or format) could not be determined.");
        exit(EXIT_FAILURE);
    }

    // --- Initialize WSOLA --- 
    // WSOLA currently supports S16_LE Mono only.
    if (pcm_format == SND_PCM_FORMAT_S16_LE && wav_header.num_channels == 1) {
        if (!wsola_init(&wsola_state, rate, wav_header.num_channels, 
                        PLAYBACK_SPEED_FACTORS[current_speed_idx],
                        DEFAULT_ANALYSIS_FRAME_MS, DEFAULT_OVERLAP_PERCENTAGE, DEFAULT_SEARCH_WINDOW_MS)) {
            app_log("ERROR", "Failed to initialize WSOLA. Pitch-preserving speed control will be disabled.");
            // wsola_state will be NULL, subsequent checks for wsola_state will bypass it.
        }
    } else {
        app_log("INFO", "WSOLA pitch-preserving speed control currently requires S16_LE Mono. Format: %s, Channels: %d. Using simple speed change.", snd_pcm_format_name(pcm_format), wav_header.num_channels);
        wsola_state = NULL; // Ensure it's NULL if not compatible
    }

    debug_msg(snd_pcm_hw_params_malloc(&hw_params), "分配snd_pcm_hw_params_t结构体");
    pcm_name = strdup(sound_card_name);
    debug_msg(snd_pcm_open(&pcm_handle, pcm_name, stream, 0), "打开PCM设备");
    debug_msg(snd_pcm_hw_params_any(pcm_handle, hw_params), "配置空间初始化");
    debug_msg(snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED), "设置交错模式");
    debug_msg(snd_pcm_hw_params_set_format(pcm_handle, hw_params, pcm_format), "设置样本格式");
    
    unsigned int actual_rate_from_alsa = rate;
    debug_msg(snd_pcm_hw_params_set_rate_near(pcm_handle, hw_params, &actual_rate_from_alsa, 0), "设置采样率");
    if (rate != actual_rate_from_alsa) {
        printf("Notice: Requested sample rate %u Hz, ALSA set to %u Hz.\n", rate, actual_rate_from_alsa);
        rate = actual_rate_from_alsa;
    }

    debug_msg(snd_pcm_hw_params_set_channels(pcm_handle, hw_params, wav_header.num_channels), "设置通道数");
    buffer_size = period_size * periods;
    buff = (unsigned char *)malloc(buffer_size);
    if (buff == NULL) {
        app_log("ERROR", "Failed to allocate memory for playback buffer.");
        exit(EXIT_FAILURE);
    }
    if (wav_header.block_align == 0) {
        app_log("ERROR", "WAV header block_align is zero.");
        exit(EXIT_FAILURE);
    }
    snd_pcm_uframes_t local_period_size_frames = period_size / wav_header.block_align;
    frames = buffer_size / wav_header.block_align;
    debug_msg(snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hw_params, &frames), "设置缓冲区大小");
    debug_msg(snd_pcm_hw_params_set_period_size_near(pcm_handle, hw_params, &local_period_size_frames, 0), "设置周期大小");
    
    snd_pcm_hw_params_get_period_size(hw_params, &local_period_size_frames, 0);
    snd_pcm_hw_params_get_buffer_size(hw_params, &frames);
    // printf("ALSA Actual period size: %lu frames, ALSA Actual buffer size: %lu frames\n", local_period_size_frames, frames);

    debug_msg(snd_pcm_hw_params(pcm_handle, hw_params), "加载硬件配置参数到驱动");

    // --- Query actual ALSA parameters after setting ---
    snd_pcm_format_t actual_format;
    unsigned int actual_rate_check;
    unsigned int actual_channels_check;
    snd_pcm_hw_params_get_format(hw_params, &actual_format);
    snd_pcm_hw_params_get_rate(hw_params, &actual_rate_check, 0);
    snd_pcm_hw_params_get_channels(hw_params, &actual_channels_check);
    printf("ALSA Configured - Format: %s (%d), Rate: %u Hz, Channels: %u\n", 
           snd_pcm_format_name(actual_format), actual_format, actual_rate_check, actual_channels_check);
    if (actual_format == SND_PCM_FORMAT_UNKNOWN) {
        app_log("CRITICAL_ERROR", "ALSA configured to UNKNOWN format despite attempts to set it. pcm_format was: %s (%d)", snd_pcm_format_name(pcm_format), pcm_format);
        // Potentially exit or handle error more gracefully
    }
    // --- End Query actual ALSA parameters ---

    snd_pcm_hw_params_free(hw_params);

    if (!init_mixer()) {
        app_log("WARNING", "Failed to initialize mixer. Volume control will not be available.");
    } else {
        app_log("INFO", "Volume control initialized. Use '+' to increase, '-' to decrease volume. 'p' to pause/resume. ',' for prev, '.' for next track. '['/']' for speed. '1'/'2'/'3' for EQ.");
        original_stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (original_stdin_flags != -1) {
            if (fcntl(STDIN_FILENO, F_SETFL, original_stdin_flags | O_NONBLOCK) == -1) {
                perror("fcntl F_SETFL O_NONBLOCK"); original_stdin_flags = -1;
            }
        } else { perror("fcntl F_GETFL"); }
    }

    printf("Starting playback...\n");
    int read_ret;
    while (1) {
        // Initialize per-iteration dynamic buffers to NULL
        short *eq_processed_audio_buffer_s16_this_iter = NULL;
        short *resampled_audio_buffer_s16_this_iter = NULL;
        // This flag will track if buffer_for_alsa points to resampled_audio_buffer_s16_this_iter
        bool buffer_for_alsa_is_resampled_output = false;

        // Handle Pause/Resume input first
        if (mixer_handle && original_stdin_flags != -1) {
            char c_in = -1;
            if (read(STDIN_FILENO, &c_in, 1) == 1) {
                if (c_in == '+') { increase_volume(); }
                else if (c_in == '-') { decrease_volume(); }
                else if (c_in == 'p') { // Toggle Pause/Resume
                    playback_paused = !playback_paused;
                    if (playback_paused) {
                        snd_pcm_pause(pcm_handle, 1);
                        app_log("INFO", "Playback PAUSED. Press 'p' to resume.");
                        // Reset HPF state on pause
                        hpf_prev_input = 0.0f;
                        hpf_prev_output = 0.0f;
                        initialize_fir_history(); // Reset FIR as well
                    } else {
                        snd_pcm_pause(pcm_handle, 0);
                        app_log("INFO", "Playback RESUMED.");
                        // Reset HPF state on resume
                        hpf_prev_input = 0.0f;
                        hpf_prev_output = 0.0f;
                        initialize_fir_history(); // Reset FIR as well
                    }
                }
                else if (c_in == 'f') { // Seek Forward
                    long seek_offset = 10 * wav_header.byte_rate; // 10 seconds
                    if (fseek(fp, seek_offset, SEEK_CUR) == 0) {
                        app_log("INFO", "Seek FORWARD 10 seconds.");
                        // Optional: Consider clearing/resetting ALSA buffer if issues occur
                        // snd_pcm_drop(pcm_handle);
                    } else {
                        perror("Seek forward failed");
                        app_log("WARNING", "Seek forward failed for %ld bytes.", seek_offset);
                    }
                }
                else if (c_in == 'b') { // Seek Backward
                    long seek_offset = -10 * wav_header.byte_rate; // 10 seconds backward
                    if (fseek(fp, seek_offset, SEEK_CUR) == 0) {
                        app_log("INFO", "Seek BACKWARD 10 seconds.");
                        // Optional: Consider clearing/resetting ALSA buffer
                        // snd_pcm_drop(pcm_handle);
                        // snd_pcm_prepare(pcm_handle);
                    } else {
                        //perror("Seek backward failed"); // fseek might fail if seeking before start
                        // Try seeking to the beginning of the data if seek_offset is too large
                        fseek(fp, sizeof(struct WAV_HEADER), SEEK_SET); // Go to start of data
                        app_log("INFO", "Seek BACKWARD 10 seconds (or to start of data).");
                    }
                }
                else if (c_in == '.') { // Next Track
                    if (num_music_files > 1) {
                        current_track_idx = (current_track_idx + 1) % num_music_files;
                        if (load_track(current_track_idx)) {
                            app_log("INFO", "Playing NEXT track: %s", music_files[current_track_idx]);
                            // Ensure ALSA is ready for new data. If sample rates/formats can vary wildly,
                            // a more robust ALSA re-init (like in main after first load) might be needed.
                            // For now, assume open_music_file and subsequent ALSA setup handles it or files are compatible.
                            // snd_pcm_drain(pcm_handle); // Drain old data
                            // snd_pcm_prepare(pcm_handle); // Prepare for new data
                        } else {
                            app_log("ERROR", "Failed to load next track.");
                            // Decide how to handle this - stop playback, try next, etc.
                        }
                    } else {
                        app_log("INFO", "No next track available.");
                    }
                }
                else if (c_in == ',') { // Previous Track
                    if (num_music_files > 1) {
                        current_track_idx = (current_track_idx - 1 + num_music_files) % num_music_files;
                        if (load_track(current_track_idx)) {
                            app_log("INFO", "Playing PREVIOUS track: %s", music_files[current_track_idx]);
                            // Similar ALSA considerations as for next track
                            // snd_pcm_drain(pcm_handle);
                            // snd_pcm_prepare(pcm_handle);
                        } else {
                            app_log("ERROR", "Failed to load previous track.");
                        }
                    } else {
                        app_log("INFO", "No previous track available.");
                    }
                }
                else if (c_in == '[') { // Decrease speed
                    if (current_speed_idx > 0) {
                        current_speed_idx--;
                    }
                    double new_speed = PLAYBACK_SPEED_FACTORS[current_speed_idx];
                    app_log("INFO", "Playback speed: %.1fx", new_speed);
                    if (use_wsola_for_speed_control) {
                        if (wsola_state) {
                            wsola_state->current_speed_factor = new_speed;
                            // Update the synthesis hop samples immediately when speed changes
                            // For slower speeds (< 1.0), synthesis_hop should be LARGER than analysis_hop
                            // For faster speeds (> 1.0), synthesis_hop should be SMALLER than analysis_hop
                            int new_hop = (int)round(wsola_state->analysis_hop_samples / new_speed);
                            if (new_hop <= 0) new_hop = 1; // Safety check for very high speeds
                            wsola_state->synthesis_hop_samples = new_hop;
                        }
                    } else {
                        // Simple seek-based speed control (from Music_App.c)
                        int seek_amount = SIMPLE_SPEED_SEEK_AMOUNTS[current_speed_idx];
                        if (seek_amount != 0) {
                            app_log("INFO", "Simple speed control: applying seek offset %d", seek_amount);
                        }
                    }
                }
                else if (c_in == ']') { // Increase speed
                    if (current_speed_idx < NUM_SPEED_LEVELS - 1) {
                        current_speed_idx++;
                    }
                    double new_speed = PLAYBACK_SPEED_FACTORS[current_speed_idx];
                    app_log("INFO", "Playback speed: %.1fx", new_speed);
                    if (use_wsola_for_speed_control) {
                        if (wsola_state) {
                            wsola_state->current_speed_factor = new_speed;
                            // Update the synthesis hop samples immediately when speed changes
                            // For slower speeds (< 1.0), synthesis_hop should be LARGER than analysis_hop
                            // For faster speeds (> 1.0), synthesis_hop should be SMALLER than analysis_hop
                            int new_hop = (int)round(wsola_state->analysis_hop_samples / new_speed);
                            if (new_hop <= 0) new_hop = 1; // Safety check for very high speeds
                            wsola_state->synthesis_hop_samples = new_hop;
                        }
                    } else {
                        // Simple seek-based speed control (from Music_App.c)
                        int seek_amount = SIMPLE_SPEED_SEEK_AMOUNTS[current_speed_idx];
                        if (seek_amount != 0) {
                            app_log("INFO", "Simple speed control: applying seek offset %d", seek_amount);
                        }
                    }
                }
                else if (c_in >= '1' && c_in <= '0' + NUM_EQ_PRESETS) {
                    int new_eq_idx = c_in - '1';
                    if (new_eq_idx >= 0 && new_eq_idx < NUM_EQ_PRESETS && new_eq_idx != current_eq_idx) {
                        current_eq_idx = new_eq_idx;
                        initialize_fir_history(); // Reset history when changing EQ
                        app_log("INFO", "Equalizer changed to: %s", EQ_PRESETS[current_eq_idx]->name);
                    } else if (new_eq_idx == current_eq_idx) {
                        app_log("INFO", "Equalizer already set to: %s", EQ_PRESETS[current_eq_idx]->name);
                    } else {
                        app_log("WARNING", "Invalid EQ preset number: %c", c_in);
                    }
                }
                while(read(STDIN_FILENO, &c_in, 1) == 1 && c_in != '\n'); // Consume rest of line
            }
        }

        if (playback_paused) {
            usleep(100000); // Sleep for 100ms to reduce CPU usage while paused
            continue;       // Skip reading and writing audio data
        }

        // Simple speed control implementation
        if (!use_wsola_for_speed_control) {
            // Apply seek-based speed control
            int seek_amount = SIMPLE_SPEED_SEEK_AMOUNTS[current_speed_idx];
            if (seek_amount != 0) {
                if (fseek(fp, seek_amount, SEEK_CUR) != 0) {
                    if (seek_amount < 0) {
                        // If seeking backward fails (likely at the start of file)
                        long current_position = ftell(fp);
                        if (current_position + seek_amount < sizeof(struct WAV_HEADER)) {
                            // Prevent seeking before header
                            fseek(fp, sizeof(struct WAV_HEADER), SEEK_SET);
                            app_log("DEBUG", "Simple speed: Prevented seeking before WAV header.");
                        }
                    } else {
                        app_log("WARNING", "Simple speed: Seek failed for amount %d.", seek_amount);
                    }
                }
            }
        }

        read_ret = fread(buff, 1, buffer_size, fp);

        // Calculate num_samples_in_buff AFTER fread and BEFORE its first use
        int num_samples_in_buff = 0;
        if (wav_header.bits_per_sample > 0) { // Prevent division by zero
            num_samples_in_buff = read_ret / (wav_header.bits_per_sample / 8);
        } else {
            app_log("ERROR", "wav_header.bits_per_sample is 0. Cannot calculate num_samples_in_buff.");
            // Handle error appropriately - perhaps break or continue, depending on desired behavior
            // For now, let it proceed, but subsequent logic might fail or produce 0 samples.
        }

        if (read_ret == 0) {
            app_log("INFO", "End of music file input! (fread returned 0)");
            // --- Autoplay Next Track --- 
            if (num_music_files > 1 && current_track_idx < num_music_files -1 ){
                current_track_idx++;
                app_log("INFO", "Auto-playing next track...");
                if(load_track(current_track_idx)){
                    // snd_pcm_prepare(pcm_handle); // Prepare ALSA for new track
                    continue; // Continue to next iteration of while(1) to play new track
                } else {
                    app_log("ERROR", "Failed to auto-play next track. Stopping.");
                    break; // Or handle error differently
                }
            } else if (num_music_files > 1 && current_track_idx == num_music_files -1) {
                 app_log("INFO", "End of playlist.");
                 break;
            } else {
                 break; // Single file or last file in playlist ended
            }
        }
        if (read_ret < 0) {
            perror("Error reading PCM data from file");
            app_log("ERROR", "Error reading PCM data from file: %s", strerror(errno));
            break;
        }

        snd_pcm_uframes_t frames_read_this_iteration = read_ret / wav_header.block_align;
        
        if (frames_read_this_iteration == 0) {
            if (read_ret > 0) { // Partial frame data
                app_log("INFO", "Partial frame data at end of file, %d bytes ignored.", read_ret);
            }
            // If read_ret was 0, the earlier check (fread returned 0) handles it.
            // If partial frame, existing logic was to break. Let's maintain that.
            break;
        }

        unsigned char *buffer_for_alsa = NULL; // This will point to the data source for ALSA processing
        snd_pcm_uframes_t frames_for_alsa = 0; // Number of frames in buffer_for_alsa

        // Original logic for EQ processing:
        if (pcm_format != SND_PCM_FORMAT_S16_LE) {
            app_log("WARNING", "FIR EQ currently assumes S16_LE input. Format is %s. EQ will be bypassed for this chunk.", snd_pcm_format_name(pcm_format));
            // If EQ is bypassed, subsequent operations might use 'buff' directly if no speed change.
            // For simplicity, let's assume buffer_for_alsa will be 'buff' if no EQ/speed change.
        } else {
            eq_processed_audio_buffer_s16_this_iter = (short*)malloc(num_samples_in_buff * sizeof(short));
            if (eq_processed_audio_buffer_s16_this_iter == NULL) {
                app_log("ERROR", "Failed to allocate memory for EQ output buffer. EQ will be bypassed for this chunk.");
            } else {
                apply_fir_filter((short*)buff, eq_processed_audio_buffer_s16_this_iter, num_samples_in_buff, EQ_PRESETS[current_eq_idx]);
            }
        }

        double current_speed = PLAYBACK_SPEED_FACTORS[current_speed_idx];
        // Determine source for speed change: EQ'd buffer if available, else raw 'buff'
        short* source_buffer_for_speed_change_s16 = (eq_processed_audio_buffer_s16_this_iter != NULL) ? eq_processed_audio_buffer_s16_this_iter : (short*)buff;
        int num_source_samples_for_speed_change = num_samples_in_buff;

        // --- WSOLA / Speed Change Logic --- 
        // WSOLA is used when:
        // 1. use_wsola_for_speed_control is true
        // 2. The WSOLA state exists (initialized successfully)
        // 3. Format is S16_LE mono (supported format)
        // 4. Speed is not 1.0x (no need to process for normal speed)
        bool use_wsola = (use_wsola_for_speed_control && 
                         wsola_state != NULL && 
                         pcm_format == SND_PCM_FORMAT_S16_LE && 
                         wav_header.num_channels == 1 &&
                         fabs(current_speed - 1.0) >= 1e-6);

        if (use_wsola) {
            DBG("MainLoop: Using WSOLA for speed %.2fx", current_speed);
            // WSOLA Path (Pitch Preserving)
            // Estimate max possible output samples from WSOLA for this input chunk
            // Max output = input_samples / min_speed_factor (e.g., 0.5x)
            // A safe buffer: num_source_samples_for_speed_change / 0.5 (i.e., * 2) + some margin for frame alignment.
            int max_wsola_output_samples = (int)((double)num_source_samples_for_speed_change / PLAYBACK_SPEED_FACTORS[0]) + wsola_state->analysis_frame_samples;
            resampled_audio_buffer_s16_this_iter = (short *)malloc(max_wsola_output_samples * sizeof(short));

            if (resampled_audio_buffer_s16_this_iter == NULL) {
                app_log("ERROR", "Failed to allocate memory for WSOLA output buffer. Using simple speed change for this chunk.");
                use_wsola = false; // Fallback
            } else {
                int wsola_output_count_samples = wsola_process(wsola_state, 
                                                       source_buffer_for_speed_change_s16, 
                                                       num_source_samples_for_speed_change, 
                                                       resampled_audio_buffer_s16_this_iter, 
                                                       max_wsola_output_samples);
                if (wsola_output_count_samples > 0) {
                    buffer_for_alsa = (unsigned char*)resampled_audio_buffer_s16_this_iter;
                    frames_for_alsa = wsola_output_count_samples / wav_header.num_channels;
                    buffer_for_alsa_is_resampled_output = true;
                } else {
                    // No output from WSOLA, or error
                    app_log("WARNING", "WSOLA processing returned 0 samples.");
                    buffer_for_alsa = NULL; // No data to play
                    frames_for_alsa = 0;
                    free(resampled_audio_buffer_s16_this_iter); // Free the buffer we allocated
                    resampled_audio_buffer_s16_this_iter = NULL;
                    buffer_for_alsa_is_resampled_output = false;
                }
            }
        }
        
        // Fallback to simple speed change if WSOLA not used or failed for this chunk
        if (!buffer_for_alsa_is_resampled_output) { // if WSOLA didn't set the buffer
            if (fabs(current_speed - 1.0) < 1e-6) { // No speed change
                buffer_for_alsa = (unsigned char*)source_buffer_for_speed_change_s16; // Points to eq_output or buff
                frames_for_alsa = frames_read_this_iteration;
                buffer_for_alsa_is_resampled_output = false; // Not from dedicated speed resampling malloc
            } else { // Simple pitch-changing speed adjustment
                int target_num_output_samples = (int)round((double)num_source_samples_for_speed_change / current_speed);
                if (target_num_output_samples > 0) {
                    size_t resampled_buffer_size_bytes = target_num_output_samples * sizeof(short);
                    // Ensure resampled_audio_buffer_s16_this_iter is used here if not by WSOLA
                    if (resampled_audio_buffer_s16_this_iter != NULL && !use_wsola) { 
                        // This case should ideally not happen if WSOLA failed and freed, then use_wsola becomes false.
                        // But as a safeguard, if it's already alloc'd and use_wsola is false, free it before re-alloc for simple speed.
                        // However, current logic: if WSOLA fails, use_wsola becomes false, then this simple speed can run.
                        // If WSOLA alloc'd then failed to produce output, it should free its buffer.
                        // For simplicity, let simple_speed always try to alloc its own if resampled_audio_buffer_s16_this_iter is NULL.
                        // If resampled_audio_buffer_s16_this_iter is NOT NULL here, it means WSOLA alloc'd but produced 0 output, and it should have been freed.
                        // This logic needs to be clean: simple speed should use its own buffer if WSOLA isn't providing one.
                    }
                    // Let simple speed attempt its own allocation if WSOLA didn't provide a buffer
                    // or if WSOLA was skipped.
                    if (resampled_audio_buffer_s16_this_iter == NULL) { // Only malloc if not already available from a failed WSOLA attempt that should have freed
                         resampled_audio_buffer_s16_this_iter = (short *)malloc(resampled_buffer_size_bytes);
                    } else if (use_wsola) {
                        // This means WSOLA ran, alloc'd, but produced 0 output. That buffer should be freed by WSOLA logic,
                        // or handled carefully here. For now, assume if use_wsola was true and we are here, buffer_for_alsa_is_resampled_output is false.
                        // This implies WSOLA output 0. The resampled_audio_buffer_s16_this_iter might still be its failed buffer.
                        // To be safe, if simple speed runs after a failed WSOLA that alloc'd, it should ensure buffer is suitable or realloc.
                        // This part of logic is tricky. Let's assume if WSOLA produced 0, resampled_audio_buffer_s16_this_iter is available for simple speed,
                        // but its size might be wrong. It's safer for simple speed to manage its own buffer if it runs.
                        // For now, let's stick to the original structure where resampled_audio_buffer_s16 (now _this_iter) is potentially reused.
                    }


                    if (resampled_audio_buffer_s16_this_iter == NULL) {
                        app_log("ERROR", "Failed to allocate memory for simple speed-adjusted buffer. Playing normal for this chunk.");
                        buffer_for_alsa = (unsigned char*)source_buffer_for_speed_change_s16;
                        frames_for_alsa = frames_read_this_iteration;
                        buffer_for_alsa_is_resampled_output = false;
                    } else {
                        // ... (Simple speed resampling logic fills resampled_audio_buffer_s16_this_iter) ...
                        // This is copied from existing code, ensure it correctly populates resampled_audio_buffer_s16_this_iter
                        double input_sample_cursor = 0.0;
                        int actual_output_samples_generated = 0;
                        for (int out_sample_num = 0; out_sample_num < target_num_output_samples; ++out_sample_num) {
                            int input_sample_to_sample_idx = (int)floor(input_sample_cursor);
                            if (input_sample_to_sample_idx >= num_source_samples_for_speed_change) {
                                target_num_output_samples = actual_output_samples_generated; // Adjust target
                                break;
                            }
                            resampled_audio_buffer_s16_this_iter[actual_output_samples_generated] = 
                                source_buffer_for_speed_change_s16[input_sample_to_sample_idx];
                            actual_output_samples_generated++;
                            input_sample_cursor += current_speed;
                        }
                        frames_for_alsa = actual_output_samples_generated / wav_header.num_channels;

                        if (frames_for_alsa > 0) {
                            buffer_for_alsa = (unsigned char*)resampled_audio_buffer_s16_this_iter;
                            buffer_for_alsa_is_resampled_output = true;
                        } else {
                            // No output from simple speed, buffer_for_alsa remains NULL or unset.
                            // resampled_audio_buffer_s16_this_iter will be freed if alloc'd.
                             buffer_for_alsa_is_resampled_output = false; // Ensure this is false
                        }
                    }
                } else { // target_num_output_samples is 0
                    frames_for_alsa = 0;
                    buffer_for_alsa_is_resampled_output = false;
                }
            }
        }
        
        // If after all processing, buffer_for_alsa is still NULL (e.g. WSOLA and simple speed produced 0 output)
        // and source_buffer_for_speed_change_s16 was the intended fallback (e.g. for 1x speed or error)
        if (buffer_for_alsa == NULL && frames_for_alsa == 0) {
            // This might happen if speed changes resulted in 0 output frames.
            // Fallback to original source if appropriate (e.g. if speed was 1.0x initially and somehow skipped)
            // Or, it simply means no audio for this chunk.
            // For now, if frames_for_alsa is 0, skip ALSA write.
        }

        // --- Step 2 Part 2: Always copy to a private buffer unless freshly malloc'd by speed processing ---
        unsigned char* alsa_payload_final = NULL;
        snd_pcm_uframes_t frames_for_alsa_final = frames_for_alsa;
        bool alsa_payload_final_is_dynamic_and_needs_free = false;

        if (buffer_for_alsa_is_resampled_output && buffer_for_alsa != NULL) {
            // 'buffer_for_alsa' points to 'resampled_audio_buffer_s16_this_iter', which was freshly malloc'd for speed.
            alsa_payload_final = buffer_for_alsa;
            alsa_payload_final_is_dynamic_and_needs_free = true;
        } else if (buffer_for_alsa != NULL && frames_for_alsa > 0) {
            // 'buffer_for_alsa' points to 'buff' or 'eq_processed_audio_buffer_s16_this_iter'. Make a private copy.
            app_log("INFO", "Making private copy of buffer for ALSA write (source was not fresh speed-change malloc).");
            size_t bytes_to_copy = frames_for_alsa * wav_header.block_align;
            alsa_payload_final = (unsigned char *)malloc(bytes_to_copy);
            if (alsa_payload_final) {
                memcpy(alsa_payload_final, buffer_for_alsa, bytes_to_copy);
                alsa_payload_final_is_dynamic_and_needs_free = true;
            } else {
                app_log("ERROR", "Failed to allocate private copy for ALSA. Skipping write for this chunk.");
                frames_for_alsa_final = 0; // Prevent writing with NULL buffer
            }
        } else {
            // No buffer determined (e.g. read_ret was 0 and led to frames_for_alsa=0 earlier, or processing yielded 0 frames)
            frames_for_alsa_final = 0;
        }

        // --- Step 2 Part 1: DMA Check ---
        if (frames_for_alsa_final > 0) {
            snd_pcm_sframes_t pcm_delay_val;
            int pcm_delay_ret = snd_pcm_delay(pcm_handle, &pcm_delay_val);
            if (pcm_delay_ret == 0) {
                if (snd_pcm_state(pcm_handle) == SND_PCM_STATE_RUNNING && pcm_delay_val > (snd_pcm_sframes_t)frames_for_alsa_final) {
                    app_log("WARNING", "ALSA CHECK: PCM delay (%ld frames) > frames_for_alsa (%lu frames).", pcm_delay_val, frames_for_alsa_final);
                }
            } else {
                app_log("WARNING", "ALSA CHECK: snd_pcm_delay failed: %s", snd_strerror(pcm_delay_ret));
            }
        }
        
        // --- Step 1: CRC Check ---
        static unsigned long long main_loop_alsa_chunk_counter = 0;
        if (frames_for_alsa_final > 0 && alsa_payload_final != NULL) {
            unsigned long long crc_sum = 0;
            short* samples_for_crc = (short*)alsa_payload_final;
            size_t total_samples_for_crc = frames_for_alsa_final * wav_header.num_channels;
            for (size_t crc_i = 0; crc_i < total_samples_for_crc; ++crc_i) {
                crc_sum += samples_for_crc[crc_i];
            }
            app_log("TRACE", "ALSA_CHUNK %llu frames=%lu crc=%llx",
                    main_loop_alsa_chunk_counter++, frames_for_alsa_final, crc_sum);
        }

        // ALSA Write Loop (using alsa_payload_final and frames_for_alsa_final)
        if (frames_for_alsa_final > 0 && alsa_payload_final != NULL) {
            // High-pass filter to remove low-frequency hum
            if (pcm_format == SND_PCM_FORMAT_S16_LE && wav_header.num_channels == 1) {
                int total_samples_hpf = frames_for_alsa_final * wav_header.num_channels;
                short *hpf_samples = (short *)alsa_payload_final; // Operate on the final buffer for ALSA
                for (int hpf_i = 0; hpf_i < total_samples_hpf; ++hpf_i) {
                    float in_hpf = (float)hpf_samples[hpf_i];
                    float out_hpf = HPF_ALPHA * (hpf_prev_output + in_hpf - hpf_prev_input);
                    hpf_prev_input = in_hpf;
                    hpf_prev_output = out_hpf;
                    int samp_hpf = (int)roundf(out_hpf);
                    if (samp_hpf > 32767) samp_hpf = 32767;
                    if (samp_hpf < -32768) samp_hpf = -32768;
                    hpf_samples[hpf_i] = (short)samp_hpf;
                }
            }

            unsigned char *ptr_to_write = alsa_payload_final;
            snd_pcm_uframes_t remaining_frames_to_write = frames_for_alsa_final;

            while (remaining_frames_to_write > 0) {
                snd_pcm_sframes_t frames_written_alsa = snd_pcm_writei(pcm_handle, ptr_to_write, remaining_frames_to_write);
                if (frames_written_alsa < 0) {
                    if (frames_written_alsa == -EPIPE) {
                        app_log("WARNING", "ALSA underrun occurred (EPIPE), preparing interface.");
                        snd_pcm_prepare(pcm_handle);
                    } else {
                        app_log("ERROR", "ALSA snd_pcm_writei error: %s", snd_strerror(frames_written_alsa));
                        if (buffer_for_alsa_is_resampled_output && resampled_audio_buffer_s16_this_iter != NULL) {
                            free(resampled_audio_buffer_s16_this_iter);
                            resampled_audio_buffer_s16_this_iter = NULL;
                        }
                        goto playback_end; 
                    }
                } else {
                    ptr_to_write += frames_written_alsa * wav_header.block_align;
                    remaining_frames_to_write -= frames_written_alsa;
                }
            }
        }

        // Free dynamically allocated buffers for this iteration
        if (eq_processed_audio_buffer_s16_this_iter != NULL) {
            // If alsa_payload_final was a copy OF eq_processed_audio_buffer_s16_this_iter,
            // eq_processed_audio_buffer_s16_this_iter still needs to be freed.
            // If alsa_payload_final directly IS eq_processed_audio_buffer_s16_this_iter (this shouldn't happen with current copy logic),
            // then it would be freed by alsa_payload_final_is_dynamic_and_needs_free.
            // The "always copy" logic means alsa_payload_final is either resampled_output or a new copy.
            // So eq_processed_audio_buffer_s16_this_iter is safe to free here if it was allocated.
            free(eq_processed_audio_buffer_s16_this_iter);
            eq_processed_audio_buffer_s16_this_iter = NULL;
        }

        if (resampled_audio_buffer_s16_this_iter != NULL) {
            // If alsa_payload_final is resampled_audio_buffer_s16_this_iter, it will be freed below.
            // If alsa_payload_final is a copy of something else, resampled_audio_buffer_s16_this_iter (if alloc'd e.g. by failed WSOLA) needs freeing.
            if (!alsa_payload_final_is_dynamic_and_needs_free || (alsa_payload_final_is_dynamic_and_needs_free && alsa_payload_final != (unsigned char*)resampled_audio_buffer_s16_this_iter)) {
                 free(resampled_audio_buffer_s16_this_iter);
            }
             resampled_audio_buffer_s16_this_iter = NULL;
        }
        
        if (alsa_payload_final_is_dynamic_and_needs_free && alsa_payload_final != NULL) {
            free(alsa_payload_final);
            alsa_payload_final = NULL;
        }
        
        if (read_ret < buffer_size) {
            app_log("INFO", "End of music file (partial buffer read). This was the last chunk.");
            break;
        }
    }

playback_end:
    app_log("INFO", "Playback finished or stopped.");
    snd_pcm_drain(pcm_handle);
    if (mixer_handle && original_stdin_flags != -1) {
        if (fcntl(STDIN_FILENO, F_SETFL, original_stdin_flags) == -1) {
            perror("fcntl F_SETFL (restore)");
        }
    }
    fclose(fp);
    snd_pcm_close(pcm_handle);
    if (mixer_handle) { snd_mixer_close(mixer_handle); }
    free(buff);
    free(pcm_name);
    if (music_files != NULL) { // Free the list of music files
        // Note: if strdup was used for music_files[i], those would need freeing too.
        // Here, they point to argv, so no individual free needed.
        free(music_files);
    }
    app_log("INFO", "MusicApp exiting normally.");
    if (log_fp != NULL) {
        fclose(log_fp);
        log_fp = NULL; // Good practice to NULL out closed file pointers
    }
    if (wsola_state != NULL) {
        wsola_destroy(&wsola_state);
    }
    return 0;
}

// --- WSOLA Implementation ---

// Helper to add input samples to the WSOLA ring buffer.
static void wsola_add_input_to_ring_buffer(WSOLA_State *state, const short *input_samples, int num_input_samples) {
    if (!state || !input_samples || num_input_samples <= 0) return;

    for (int i = 0; i < num_input_samples; ++i) {
        if (state->input_buffer_content >= state->input_buffer_capacity) {
            // Buffer is full, this shouldn't happen if wsola_process manages consumption correctly.
            // Overwrite oldest data by advancing read_pos and stream_start_offset.
            app_log("WARNING", "WSOLA input ring buffer full. Overwriting oldest sample.");
            state->input_buffer_read_pos = (state->input_buffer_read_pos + 1) % state->input_buffer_capacity;
            state->input_ring_buffer_stream_start_offset++;
            state->input_buffer_content--; 
        }
        state->input_buffer_ring[state->input_buffer_write_pos] = input_samples[i];
        state->input_buffer_write_pos = (state->input_buffer_write_pos + 1) % state->input_buffer_capacity;
        state->input_buffer_content++;
    }
}

// Generates a Hann window: 0.5 * (1 - cos(2*pi*i / (L-1)))
static void generate_hann_window(float *float_window, int length) {
    if (length <= 0) return;
    if (length == 1) { // Avoid division by zero for N-1 if N=1
        float_window[0] = 1.0f; // Or 0.0f, depending on desired behavior for single point window
        return;
    }
    for (int i = 0; i < length; i++) {
        float_window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * (float)i / (float)(length - 1)));
    }
}

// Helper to convert float window to short Q15 window
static void convert_float_window_to_q15(const float* float_window, short* q15_window, int length) {
    for (int i = 0; i < length; i++) {
        // Scale float window [0, 1] to Q15 range [-32768, 32767], typically [0, 32767] for window
        q15_window[i] = (short)(float_window[i] * 32767.0f);
    }
}

// Helper function to calculate normalized cross-correlation between two segments.
// Segments are arrays of 16-bit signed PCM samples.
static float calculate_normalized_cross_correlation(const short *segment1, const short *segment2, int length) {
    if (segment1 == NULL || segment2 == NULL || length <= 0) {
        app_log("ERROR", "NCC: Invalid arguments (NULL segments or zero/negative length: %d)", length);
        return 0.0f; 
    }

    // Use double precision for better accuracy
    double sum_s1s2 = 0.0;
    double sum_s1_sq = 0.0;
    double sum_s2_sq = 0.0;

    // Zero-mean correlation is crucial for audio to remove DC offset influences
    double mean1 = 0.0, mean2 = 0.0;
    for (int i = 0; i < length; ++i) {
        mean1 += segment1[i];
        mean2 += segment2[i];
    }
    mean1 /= length;
    mean2 /= length;

    // Check for identical segments (would cause perfect 1.0 correlation)
    // This helps avoid the artificial exact 1.0 readings we're seeing
    bool segments_identical = true;
    for (int i = 0; i < length && segments_identical; ++i) {
        if (segment1[i] != segment2[i]) {
            segments_identical = false;
        }
    }
    
    // If segments are truly identical and not just silence, still allow a high but not perfect correlation
    if (segments_identical) {
        // Check if it's meaningful audio or just silence
        double energy = 0.0;
        for (int i = 0; i < length; ++i) {
            energy += (double)segment1[i] * segment1[i];
        }
        // If it's true silence, correlation is meaningless
        if (energy < 100.0 * length) {
            return 0.0f;
        }
        // Otherwise, return very high but not perfect correlation to avoid detection issues
        return 0.9999f;
    }

    // The internal triangular windowing has been removed.
    // Correlation is now performed on the (potentially pre-windowed by caller) mean-centered segments.
    for (int i = 0; i < length; ++i) {
        // Samples are mean-centered
        double s1 = (segment1[i] - mean1);
        double s2 = (segment2[i] - mean2);
        
        // Calculate correlation components
        sum_s1s2 += s1 * s2;
        sum_s1_sq += s1 * s1;
        sum_s2_sq += s2 * s2;
    }

    // Silence or very low energy detection
    // Using more aggressive threshold specifically for audio
    if (sum_s1_sq < 100.0 || sum_s2_sq < 100.0) {
        DBG("NCC: Very low energy detected (%f, %f), returning 0", sum_s1_sq, sum_s2_sq);
        return 0.0f;
    }

    double denominator = sqrt(sum_s1_sq) * sqrt(sum_s2_sq);
    if (denominator < 1.0e-9) {
        return 0.0f;
    }
    
    double correlation = sum_s1s2 / denominator;
    
    // Clamp to valid range
    if (correlation > 1.0) correlation = 1.0;
    if (correlation < -1.0) correlation = -1.0;
    
    // Energy balancing: Apply stronger penalty for energy imbalance
    // This is especially important for audio where one segment might be 
    // much louder than another but still have similar shape
    double energy_ratio = (sum_s1_sq < sum_s2_sq) ? 
                         sum_s1_sq / sum_s2_sq : 
                         sum_s2_sq / sum_s1_sq;
    
    // For severe energy imbalance (one segment much louder than other)
    if (energy_ratio < 0.3) {
        correlation *= sqrtf(energy_ratio); // Softened penalty
    }
    
    return (float)correlation;
}

// Helper function to safely get a segment of 'length' samples starting from
// 'start_index_in_ring' in the ring buffer.
// Returns true if successful, false if the segment cannot be fully extracted (e.g., not enough data).
static bool get_segment_from_ring_buffer(const WSOLA_State *state, int start_index_in_ring, int length, short *output_segment) {
    if (!state || !state->input_buffer_ring || !output_segment || length <= 0) {
        // app_log("ERROR", "get_segment: Invalid arguments (state=%p, ring=%p, out=%p, len=%d)", state, state ? state->input_buffer_ring : NULL, output_segment, length);
        return false;
    }
    if (length > state->input_buffer_content) {
        DBG("get_segment: not enough content (%d) in ring buffer for requested length %d. Start_idx_ring=%d", state->input_buffer_content, length, start_index_in_ring);
        return false;
    }

    for (int i = 0; i < length; i++) {
        int current_ring_idx = (start_index_in_ring + i) % state->input_buffer_capacity;
        // No additional check here for if current_ring_idx has been overwritten,
        // relying on input_buffer_content to be accurate.
        output_segment[i] = state->input_buffer_ring[current_ring_idx];
    }
    return true;
}

static long long find_best_match_segment(
    const WSOLA_State *state,
    const short *target_segment_for_comparison, // This is typically state->output_overlap_add_buffer
    int ideal_search_center_ring_idx,           // Ideal start of search in input_buffer_ring
    int *best_segment_start_offset_from_ideal_center_ptr)
{
    if (!state || !target_segment_for_comparison || !best_segment_start_offset_from_ideal_center_ptr) {
        app_log("ERROR", "find_best_match: NULL pointer argument.");
        if (best_segment_start_offset_from_ideal_center_ptr) *best_segment_start_offset_from_ideal_center_ptr = 0;
        return -LLONG_MAX; // Indicate error or no match found
    }

    int N_o = state->overlap_samples;
    int S_w = state->search_window_samples;
    
    // Static variables to maintain consistency between frames
    // Important for audio to avoid perceptual discontinuities
    static int previous_best_offset = 0;
    static float previous_correlation = 0.0f;
    static int consecutive_low_correlations = 0;
    
    // Allocate a temporary buffer for candidate segments from the ring buffer
    short *candidate_segment = (short *)malloc(N_o * sizeof(short));
    if (!candidate_segment) {
        app_log("ERROR", "find_best_match: Failed to allocate memory for candidate_segment.");
        *best_segment_start_offset_from_ideal_center_ptr = 0;
        return -LLONG_MAX; // Error
    }

    // Initialize correlation to very low value
    float max_correlation_float = -1.0f;
    long long max_correlation = -LLONG_MAX;
    *best_segment_start_offset_from_ideal_center_ptr = 0; // Default to no offset
    bool match_found = false;

    // For first frames (startup case), handle specially
    if (state->total_output_samples_generated == 0) {
        // First frame - check only 0 offset (traditional phase vocoder approach for start)
        if (get_segment_from_ring_buffer(state, ideal_search_center_ring_idx, N_o, candidate_segment)) {
            // First frame should always be position 0 for predictability
            max_correlation_float = 0.9999f; // High but not perfect correlation
            max_correlation = 999900; // 0.9999 scaled
            *best_segment_start_offset_from_ideal_center_ptr = 0;
            match_found = true;
            previous_best_offset = 0;
            previous_correlation = 0.9999f;
        }
    } 
    else if (S_w <= 0) {
        // No search window case - just analyze the ideal position
        if (get_segment_from_ring_buffer(state, ideal_search_center_ring_idx, N_o, candidate_segment)) {
            max_correlation_float = calculate_normalized_cross_correlation(
                target_segment_for_comparison, candidate_segment, N_o);
            max_correlation = (long long)(max_correlation_float * 1000000.0f);
            *best_segment_start_offset_from_ideal_center_ptr = 0;
            match_found = true;
        } else {
            app_log("WARNING", "find_best_match: Could not get segment at ideal_search_center_ring_idx %d", 
                   ideal_search_center_ring_idx);
        }
    } 
    else {
        // Adaptive multi-resolution search with improved oscillation prevention
        
        // Step 1: Coarse search with variable step size
        int step_size = MAX(1, S_w / 10); // More fine-grained initial search (was S_w/8)
        int coarse_best_offset = 0;
        float coarse_max_correlation = -1.0f;
        
        // Bias search range toward previous successful offsets for stability
        // but still explore the entire range to avoid getting stuck
        for (int offset = -S_w; offset <= S_w; offset += step_size) {
            int candidate_idx = (ideal_search_center_ring_idx + offset + state->input_buffer_capacity) 
                              % state->input_buffer_capacity;
            
            if (!get_segment_from_ring_buffer(state, candidate_idx, N_o, candidate_segment)) {
                continue;
            }
            
            // Symmetrize NCC inputs: Apply Hanning window to candidate_segment
            // to match the pre-windowed nature of target_segment_for_comparison.
            short windowed_candidate_for_ncc[N_o]; // N_o is overlap_samples
            for (int k = 0; k < N_o; ++k) {
                // state->analysis_window_function is Q15 short.
                long long val_ll = (long long)candidate_segment[k] * state->analysis_window_function[k];
                windowed_candidate_for_ncc[k] = (short)(val_ll >> 15);
            }

            float corr = calculate_normalized_cross_correlation(
                target_segment_for_comparison, windowed_candidate_for_ncc, N_o);
            
            // Progressive continuity bias - stronger for offsets closer to previous match
            // and when previous correlation was strong
            float continuity_factor = fabsf((float)(offset - previous_best_offset)) / (float)S_w;
            float continuity_bias = 0.0f;
            
            // Apply stronger continuity bias when last match was good 
            if (previous_correlation > 0.7f) {
                // Exponential bias that drops off quickly as we move away from previous offset
                continuity_bias = 0.08f * expf(-4.0f * continuity_factor);
            } else {
                // Weaker linear bias when previous match quality was poor
                continuity_bias = 0.04f * (1.0f - continuity_factor);
            }
            
            corr += continuity_bias;
            if (corr > 1.0f) corr = 1.0f;
            
            if (corr > coarse_max_correlation) {
                coarse_max_correlation = corr;
                coarse_best_offset = offset;
                match_found = true;
            }
        }
        
        // Step 2: Enhanced fine search with adaptive range
        if (match_found) {
            // Adapt fine search range based on coarse correlation quality
            int search_range = (coarse_max_correlation > 0.8f) ? 
                              (step_size + 2) : // Narrow range for good matches
                              (2 * step_size);  // Wider range for poorer matches
            
            int fine_search_min = MAX(-S_w, coarse_best_offset - search_range);
            int fine_search_max = MIN(S_w, coarse_best_offset + search_range);
            
            // Fine search step size - always 1 for best precision
            for (int offset = fine_search_min; offset <= fine_search_max; offset++) {
                // Skip offsets we already checked in the coarse step
                if ((offset % step_size) == 0 && offset >= -S_w && offset <= S_w) {
                    continue;
                }
                
                int candidate_idx = (ideal_search_center_ring_idx + offset + state->input_buffer_capacity) 
                                  % state->input_buffer_capacity;
                
                if (!get_segment_from_ring_buffer(state, candidate_idx, N_o, candidate_segment)) {
                    continue;
                }
                
                // Symmetrize NCC inputs: Apply Hanning window to candidate_segment
                short windowed_candidate_for_ncc_fine[N_o];
                for (int k = 0; k < N_o; ++k) {
                    long long val_ll = (long long)candidate_segment[k] * state->analysis_window_function[k];
                    windowed_candidate_for_ncc_fine[k] = (short)(val_ll >> 15);
                }

                float corr = calculate_normalized_cross_correlation(
                    target_segment_for_comparison, windowed_candidate_for_ncc_fine, N_o);
                
                // Fine search gets a smaller continuity bias
                float continuity_factor = fabsf((float)(offset - previous_best_offset)) / (float)S_w;
                float continuity_bias = 0.02f * (1.0f - continuity_factor);
                corr += continuity_bias;
                
                if (corr > 1.0f) corr = 1.0f;
                
                if (corr > max_correlation_float) {
                    max_correlation_float = corr;
                    *best_segment_start_offset_from_ideal_center_ptr = offset;
                    max_correlation = (long long)(corr * 1000000.0f);
                }
            }
        }
    }
    
    // Enhanced handling of low correlations
    if (!match_found || max_correlation_float < 0.1f) {
        consecutive_low_correlations++;
        
        // With multiple consecutive poor matches, implement a smooth fallback strategy
        if (consecutive_low_correlations > 2) {
            // After a few failures, use a weighted average of previous offset
            // and a small random component to maintain some variation while 
            // preventing large jumps
            int constrained_offset;
            
            if (consecutive_low_correlations < 5) {
                // Phase 1: Gradual transition toward previous offset
                constrained_offset = ((*best_segment_start_offset_from_ideal_center_ptr * 1) + 
                                     (previous_best_offset * 3)) / 4;
            } else {
                // Phase 2: Strong constraint to small variations around previous offset
                // Limit to +/- 10% of search window 
                int variation_limit = MAX(1, S_w / 10);
                int min_offset = MAX(-S_w, previous_best_offset - variation_limit);
                int max_offset = MIN(S_w, previous_best_offset + variation_limit);
                
                constrained_offset = previous_best_offset;
                
                // Try a few positions around the previous offset
                float best_emergency_corr = -1.0f;
                for (int test_offset = min_offset; test_offset <= max_offset; test_offset += MAX(1, variation_limit/2)) {
                    int candidate_idx = (ideal_search_center_ring_idx + test_offset + state->input_buffer_capacity) 
                                      % state->input_buffer_capacity;
                    
                    if (get_segment_from_ring_buffer(state, candidate_idx, N_o, candidate_segment)) {
                        float test_corr = calculate_normalized_cross_correlation(
                            target_segment_for_comparison, candidate_segment, N_o);
                        
                        if (test_corr > best_emergency_corr) {
                            best_emergency_corr = test_corr;
                            constrained_offset = test_offset;
                        }
                    }
                }
            }
            
            app_log("DEBUG", "find_best_match: Very low correlation (%.4f). Strong smoothing: %d -> %d", 
                   max_correlation_float, 
                   *best_segment_start_offset_from_ideal_center_ptr, 
                   constrained_offset);
                   
            *best_segment_start_offset_from_ideal_center_ptr = constrained_offset;
            
            // The following lines artificially inflated the correlation.
            // We are removing this to allow the actual low correlation to propagate
            // to previous_correlation for the next frame.
            // max_correlation_float = 0.3f; 
            // max_correlation = 300000; // 0.3 scaled
        }
    } else {
        consecutive_low_correlations = 0; // Reset counter with successful match
    }
    
    // Protect against extreme jumps that cause perceptual discontinuities
    if (previous_correlation > 0.7f && max_correlation_float > 0.1f) {
        int max_allowed_jump = S_w / 2; // Limit maximum frame-to-frame jump
        
        if (abs(*best_segment_start_offset_from_ideal_center_ptr - previous_best_offset) > max_allowed_jump) {
            // Smooth the transition by moving only partway toward the new offset
            int smoothed_offset = previous_best_offset + 
                                 ((*best_segment_start_offset_from_ideal_center_ptr - previous_best_offset) / 2);
            
            app_log("DEBUG", "find_best_match: Large offset jump smoothed: %d -> %d", 
                   *best_segment_start_offset_from_ideal_center_ptr, smoothed_offset);
                   
            *best_segment_start_offset_from_ideal_center_ptr = smoothed_offset;
        }
    }
    
    // Free the temporary buffer
    free(candidate_segment);
    
    app_log("DEBUG", "find_best_match: Final offset=%d, Correlation=%.4f, ConsecLowCorr=%d", 
           *best_segment_start_offset_from_ideal_center_ptr, max_correlation_float, consecutive_low_correlations);
    
    // Update for next call
    previous_best_offset = *best_segment_start_offset_from_ideal_center_ptr;
    previous_correlation = max_correlation_float;
    
    return max_correlation;
}

bool wsola_init(WSOLA_State **state_ptr, int sample_rate_arg, int num_channels_arg, double initial_speed_factor_arg,
                int analysis_frame_ms_arg, float overlap_percentage_arg, int search_window_ms_arg) {
    if (state_ptr == NULL) {
        app_log("ERROR", "wsola_init: NULL state_ptr provided.");
        return false;
    }
    if (*state_ptr != NULL) {
        app_log("WARNING", "wsola_init: *state_ptr is not NULL. Potential memory leak. Destroying existing state first.");
        wsola_destroy(state_ptr); // Attempt to clean up
    }

    WSOLA_State *s = (WSOLA_State *)malloc(sizeof(WSOLA_State));
    if (!s) {
        app_log("ERROR", "wsola_init: Failed to allocate memory for WSOLA_State.");
        return false;
    }
    memset(s, 0, sizeof(WSOLA_State)); // Initialize all to zero/NULL

    // Validate and store configuration
    if (sample_rate_arg <= 0 || num_channels_arg <= 0 || initial_speed_factor_arg <= 0.0 ||
        analysis_frame_ms_arg <= 0 || overlap_percentage_arg < 0.0f || overlap_percentage_arg >= 1.0f ||
        search_window_ms_arg < 0) { // search_window_ms can be 0 for no search
        app_log("ERROR", "wsola_init: Invalid parameters (sample_rate=%d, channels=%d, speed=%.2f, frame_ms=%d, overlap=%.2f, search_ms=%d).",
                sample_rate_arg, num_channels_arg, initial_speed_factor_arg, analysis_frame_ms_arg, overlap_percentage_arg, search_window_ms_arg);
        free(s);
        return false;
    }
    
    // For now, WSOLA implementation will be mono. Stereo needs interleaved processing or dual states.
    if (num_channels_arg != 1) {
        app_log("ERROR", "wsola_init: Currently only supports mono (1 channel), received %d channels.", num_channels_arg);
        free(s);
        return false;
    }

    s->sample_rate = sample_rate_arg;
    s->num_channels = num_channels_arg; // Will be 1 for now
    s->current_speed_factor = initial_speed_factor_arg;

    // Calculate derived parameters (integer samples)
    s->analysis_frame_samples = (s->sample_rate * analysis_frame_ms_arg) / 1000;
    // N_o (overlap_samples) is now N/2 for 50% OLA with sine window
    s->overlap_samples = s->analysis_frame_samples / 2;
    // Ensure overlap_samples is not zero if analysis_frame_samples is 1 (edge case)
    if (s->analysis_frame_samples == 1 && s->overlap_samples == 0) s->overlap_samples = 1; 

    s->analysis_hop_samples = s->analysis_frame_samples - s->overlap_samples; // Nominal input hop at 1x speed (will be N/2)
    
    // Synthesis hop depends on speed factor and should be adjusted based on playback speed
    // For slower speeds (< 1.0), synthesis_hop > analysis_hop
    // For faster speeds (> 1.0), synthesis_hop < analysis_hop
    int calculated_hop = (int)round(s->analysis_hop_samples / s->current_speed_factor);
    s->synthesis_hop_samples = (calculated_hop > 0) ? calculated_hop : 1; // Ensure positive hop

    s->search_window_samples = (s->sample_rate * search_window_ms_arg) / 1000;

    if (s->analysis_frame_samples <= 0 || s->overlap_samples < 0 || s->analysis_hop_samples <= 0) {
         app_log("ERROR", "wsola_init: Invalid derived frame/hop sizes (N=%d, No=%d, Ha=%d). Check frame_ms and overlap percentage.",
                s->analysis_frame_samples, s->overlap_samples, s->analysis_hop_samples);
        free(s);
        return false;
    }
    if (s->overlap_samples >= s->analysis_frame_samples) {
        app_log("ERROR", "wsola_init: Overlap samples (%d) must be less than analysis frame samples (%d).", s->overlap_samples, s->analysis_frame_samples);
        free(s);
        return false;
    }


    // Allocate memory for buffers
    s->analysis_window_function = (short *)malloc(s->analysis_frame_samples * sizeof(short));
    if (!s->analysis_window_function) {
        app_log("ERROR", "wsola_init: Failed to allocate memory for analysis_window_function.");
        wsola_destroy(&s); return false;
    }
    // Generate Hann window (float first, then convert to Q15 short)
    float *temp_float_window = (float *)malloc(s->analysis_frame_samples * sizeof(float));
    if (!temp_float_window) {
        app_log("ERROR", "wsola_init: Failed to allocate memory for temp_float_window.");
        wsola_destroy(&s); return false;
    }
    generate_hann_window(temp_float_window, s->analysis_frame_samples);
    convert_float_window_to_q15(temp_float_window, s->analysis_window_function, s->analysis_frame_samples);
    free(temp_float_window);


    // Input ring buffer capacity calculation:
    // It needs to hold at least one full input chunk from the main app, 
    // plus enough room for WSOLA to analyze (N samples) and search (2*S_w samples)
    // from data that might already be there or is part of the new chunk.
    // A common input chunk size (num_samples_in_buff) is around 12288 for 24KB S16LE mono.
    // Let's estimate a typical main_input_chunk_size_samples.
    // const int typical_main_input_chunk_size_samples = (24 * 1024) / 2; // Approx 12288 for S16LE
    // s->input_buffer_capacity = typical_main_input_chunk_size_samples + s->analysis_frame_samples + (2 * s->search_window_samples) + 2048; // Add some more buffer
    // Simpler: ensure it's larger than a large input chunk + analysis frame.
    // From logs, N=661, Sw=220. Typical input is ~12288 samples.
    // Required for one operation: N (current frame being built) + S_w (search margin).
    // Data in buffer: must hold enough for current + next + search window.
    // Let current_input_chunk_size be the samples read by fread = (BUF_LEN * periods) / bytes_per_sample (approx 12k for S16LE)
    // A robust size would be: typical_input_chunk_from_fread + N + 2*S_w.
    // For now, let's set a larger fixed minimum based on MAX_WSOLA_FRAME_SAMPLES, and ensure it's larger than typical input chunk.
    int estimated_max_input_chunk = (1024 * 2 * 20) / 2; // e.g. period_size (12k) * periods (2) / bytes_per_sample -> 12k * 2 / 2 = 12k. Let's assume up to 20k samples as a generous typical chunk.
                                                        // BUF_LEN is 1024. period_size is 12*1024. buffer_size (for fread) is period_size * periods = 24KB
    int samples_from_main_fread = (period_size * periods) / (s->sample_rate > 0 ? (wav_header.bits_per_sample / 8) : 2); // Calculate based on actual period_size
    if (samples_from_main_fread == 0 && s->sample_rate > 0) samples_from_main_fread = (12288 * 2) / (wav_header.bits_per_sample/8); // Fallback if period_size not set yet or similar, assume 12k*2 bytes
    if (samples_from_main_fread == 0) samples_from_main_fread = 12288; // Default if all else fails

    s->input_buffer_capacity = samples_from_main_fread + s->analysis_frame_samples + (2 * s->search_window_samples) + 1024; // Generous buffer
    
    // The old logic using MAX_WSOLA_FRAME_SAMPLES: 
    // s->input_buffer_capacity = s->analysis_frame_samples + (2 * s->search_window_samples) + s->analysis_frame_samples; 
    // if (s->input_buffer_capacity < MAX_WSOLA_FRAME_SAMPLES * 2) { 
    //     s->input_buffer_capacity = MAX_WSOLA_FRAME_SAMPLES * 2;
    // }

    s->input_buffer_ring = (short *)malloc(s->input_buffer_capacity * sizeof(short));
    if (!s->input_buffer_ring) {
        app_log("ERROR", "wsola_init: Failed to allocate memory for input_buffer_ring.");
        wsola_destroy(&s); return false;
    }
    s->input_buffer_write_pos = 0;
    s->input_buffer_read_pos = 0;
    s->input_buffer_content = 0;

    s->output_overlap_add_buffer = (short *)malloc(s->overlap_samples * sizeof(short));
    if (!s->output_overlap_add_buffer) {
        app_log("ERROR", "wsola_init: Failed to allocate memory for output_overlap_add_buffer.");
        wsola_destroy(&s); return false;
    }
    memset(s->output_overlap_add_buffer, 0, s->overlap_samples * sizeof(short)); // Initialize to silence

    s->current_synthesis_segment = (short *)malloc(s->analysis_frame_samples * sizeof(short));
    if (!s->current_synthesis_segment) {
        app_log("ERROR", "wsola_init: Failed to allocate memory for current_synthesis_segment.");
        wsola_destroy(&s); return false;
    }
    
    s->total_input_samples_processed = 0;
    s->total_output_samples_generated = 0;
    s->next_ideal_input_frame_start_sample_offset = 0; // Start at the beginning of the stream
    s->input_ring_buffer_stream_start_offset = 0;    // Initially, ring buffer index 0 is stream sample 0

    *state_ptr = s;
    app_log("INFO", "wsola_init: WSOLA state initialized successfully. N=%d, No=%d, Ha=%d, Sw=%d, InputRingCap=%d",
            s->analysis_frame_samples, s->overlap_samples, s->analysis_hop_samples, s->search_window_samples, s->input_buffer_capacity);
    return true;
}

void wsola_destroy(WSOLA_State **state_ptr) {
    if (state_ptr && *state_ptr) {
        WSOLA_State *s = *state_ptr;
        free(s->analysis_window_function);
        free(s->input_buffer_ring);
        free(s->output_overlap_add_buffer);
        free(s->current_synthesis_segment);
        // Add any other allocated resources here
        free(s);
        *state_ptr = NULL;
        app_log("INFO", "wsola_destroy: WSOLA state destroyed.");
    }
}

// Main WSOLA processing function
// Takes num_input_samples, processes them, and puts time-scaled samples into output_buffer.
// Returns the number of samples written to output_buffer.
int wsola_process(WSOLA_State *state, const short *input_samples, int num_input_samples, 
                  short *output_buffer, int max_output_samples) {
    if (!state || !output_buffer || max_output_samples <= 0) {
        return 0; // No state or no place to put output
    }

    // STATIC FADE WINDOWS REMOVED - NO LONGER USED
    // static float *synthesis_window = NULL; // This was for a different windowing approach, also removed earlier
    // static int last_synthesis_window_length = 0;

    // static float *fade_out = NULL; // REMOVE
    // static float *fade_in = NULL;  // REMOVE
    // static int last_fade_length = 0; // REMOVE

    if (input_samples && num_input_samples > 0) {
        wsola_add_input_to_ring_buffer(state, input_samples, num_input_samples);
    }

    int output_samples_written = 0;
    int N = state->analysis_frame_samples;
    int N_o = state->overlap_samples; // This is N/2
    int H_a = state->analysis_hop_samples; // This is N/2, the analysis hop for 50% OLA

    DBG("WSOLA_PROCESS_ENTRY: N=%d, N_o=%d(N/2), H_a=%d(N/2), speed=%.2f", 
           N, N_o, H_a, state->current_speed_factor);

    int loop_iterations = 0;
    long long latest_required_stream_offset_for_loop_check;
    long long latest_available_stream_offset_for_loop_check;

    // Loop as long as we can produce N_o samples and have space in the output buffer.
    while (output_samples_written + N_o <= max_output_samples) {
        loop_iterations++;

        latest_required_stream_offset_for_loop_check = state->next_ideal_input_frame_start_sample_offset + N; // Need a full N segment for analysis
        // For search, it was + N + search_window_samples. For 50% OLA, the search is still important.
        // The find_best_match_segment still needs a search window around the ideal point.
        // The requirement is for an N-length segment starting at ideal_search_center_ring_idx + best_offset.
        // Ideal search center is derived from next_ideal_input_frame_start_sample_offset.
        // So, we need data up to: next_ideal_input_frame_start_sample_offset + search_window_samples + N (worst case offset)
        latest_required_stream_offset_for_loop_check = state->next_ideal_input_frame_start_sample_offset + state->search_window_samples + N;

        latest_available_stream_offset_for_loop_check = state->input_ring_buffer_stream_start_offset + state->input_buffer_content;

        if (latest_available_stream_offset_for_loop_check < latest_required_stream_offset_for_loop_check) {
            DBG("WSOLA_LOOP_BREAK_NO_DATA: iter=%d, avail=%lld, req=%lld. Output written so far: %d", loop_iterations, latest_available_stream_offset_for_loop_check, latest_required_stream_offset_for_loop_check, output_samples_written);
            break; 
        }

        // Determine the ring buffer index for the center of our search.
        // This corresponds to where `state->next_ideal_input_frame_start_sample_offset` falls in the current ring.
        int ideal_search_center_ring_idx = 
            (int)((state->next_ideal_input_frame_start_sample_offset - state->input_ring_buffer_stream_start_offset + state->input_buffer_capacity)) 
            % state->input_buffer_capacity;

        int best_offset_from_ideal_center = 0;
        long long correlation = find_best_match_segment(state, 
                                                state->output_overlap_add_buffer, 
                                                ideal_search_center_ring_idx, 
                                                &best_offset_from_ideal_center);

        // Check if find_best_match_segment indicated an error or no valid segment found
        if (correlation == -LLONG_MAX) {
            app_log("WARNING", "WSOLA: find_best_match_segment returned error/no match. Using zero offset. Correlation: %lld", correlation);
            best_offset_from_ideal_center = 0; // Ensure fallback to zero offset
        } else if (correlation == 0 && loop_iterations <= 1) { // Check loop_iterations instead of total_output_samples_generated
            // If correlation is zero AND we are in the very first iteration of this wsola_process call
            // (output overlap buffer was definitely silent from init or previous call's end),
            // it's better to stick to the ideal segment (offset 0).
            app_log("DEBUG", "WSOLA: Correlation is 0 during initial loop iter (iter: %d). Forcing offset to 0 from %d.",
                    loop_iterations, best_offset_from_ideal_center);
            best_offset_from_ideal_center = 0;
        }

        // Actual start of the N-sample synthesis segment in the ring buffer
        int actual_synthesis_segment_start_ring_idx = 
            (ideal_search_center_ring_idx + best_offset_from_ideal_center + state->input_buffer_capacity) 
            % state->input_buffer_capacity;

        // Extract the N-sample segment into state->current_synthesis_segment
        if (!get_segment_from_ring_buffer(state, actual_synthesis_segment_start_ring_idx, N, state->current_synthesis_segment)) {
            app_log("ERROR", "WSOLA: Failed to get synthesis segment from ring buffer. Skipping frame.");
            break; 
        }

        // Apply the Q15 Sine analysis window to the entire current_synthesis_segment (N samples).
        for (int i = 0; i < N; ++i) { 
            long long val_ll = (long long)state->current_synthesis_segment[i] * state->analysis_window_function[i];
            state->current_synthesis_segment[i] = (short)(val_ll >> 15); 
        }

        // OLA produces N_o samples in this iteration.
        // int samples_to_write_this_frame = H_s_eff; // REMOVED H_s_eff
        int samples_written_this_frame_ola = 0;
        // int samples_written_this_frame_new = 0; // REMOVED, no separate "new" part write

        // --- Overlap-Add with 50% Sine Window --- 
        for (int i = 0; i < N_o; ++i) { // N_o is N/2. This loop now produces N_o samples.
            // Optional guard suggested by user: if (max_output_samples - output_samples_written < N_o) break;
            // This is covered by the main while loop condition: output_samples_written + N_o <= max_output_samples

            long long sum = (long long)state->output_overlap_add_buffer[i] + state->current_synthesis_segment[i];
            
            if (sum > 32767) sum = 32767;
            else if (sum < -32768) sum = -32768;
            
            output_buffer[output_samples_written++] = (short)sum;
            samples_written_this_frame_ola++; 
            // No break needed here for samples_written_this_frame_ola < N_o, loop runs N_o times if output has space.
        }

        // --- The "new" part (second half of current windowed segment) is NOT explicitly written here. ---
        // It will be handled by the OLA of the *next* iteration when the current second half
        // (stored in output_overlap_add_buffer) is summed with the next segment's first half.
        /* Entire block removed:
        int needed_new_output_samples = samples_to_write_this_frame - samples_written_this_frame_ola;
        if (needed_new_output_samples > 0) { ... }
        */
        
        // Update the output_overlap_add_buffer with the tail (second half) of the current *windowed* synthesis segment
        if (N_o > 0) { 
             memcpy(state->output_overlap_add_buffer, state->current_synthesis_segment + N_o, N_o * sizeof(short));
        }

        // Update input stream pointers for time scaling.
        // Advance by H_a (which is N_o) scaled by the speed factor.
        state->next_ideal_input_frame_start_sample_offset += (int)round((double)H_a / state->current_speed_factor);
        state->total_input_samples_processed = state->next_ideal_input_frame_start_sample_offset; // Approximation

        // Discard old data from ring buffer (logic for this remains the same)
        long long min_retainable_abs_offset = state->next_ideal_input_frame_start_sample_offset - state->search_window_samples - N_o; 
        if (min_retainable_abs_offset < 0) min_retainable_abs_offset = 0;

        long long current_ring_start_abs_offset = state->input_ring_buffer_stream_start_offset;
        int samples_to_discard = 0;
        if (state->input_buffer_content > 0) { 
            samples_to_discard = (int)(min_retainable_abs_offset - current_ring_start_abs_offset);
        }
        app_log("DEBUG", "WSOLA_DISCARD_CHECK: iter=%d, min_retain_abs=%lld, ring_start_abs=%lld, content_before=%d, samples_to_discard_calc=%d", 
           loop_iterations, min_retainable_abs_offset, current_ring_start_abs_offset, state->input_buffer_content, samples_to_discard);

        if (samples_to_discard > 0) {
            if (samples_to_discard > state->input_buffer_content) {
                app_log("WARNING", "WSOLA_DISCARD: Attempting to discard %d, but only %d content. Clamping.", samples_to_discard, state->input_buffer_content);
                samples_to_discard = state->input_buffer_content; 
            }
            state->input_buffer_read_pos = (state->input_buffer_read_pos + samples_to_discard) % state->input_buffer_capacity;
            state->input_buffer_content -= samples_to_discard;
            state->input_ring_buffer_stream_start_offset += samples_to_discard;
            app_log("DEBUG", "WSOLA_DISCARD_DONE: iter=%d, discarded=%d, new_ring_start_abs=%lld, new_read_pos=%d, new_content=%d", 
               loop_iterations, samples_to_discard, state->input_ring_buffer_stream_start_offset, state->input_buffer_read_pos, state->input_buffer_content);
        } else if (samples_to_discard < 0) {
             app_log("DEBUG", "WSOLA_DISCARD_SKIP: iter=%d, samples_to_discard_calc was %d (negative).", loop_iterations, samples_to_discard);
        }
    } // End of while loop

    app_log("DEBUG", "WSOLA_PROCESS_EXIT: loop_iters=%d, output_written=%d, input_content_end=%d, next_ideal_start=%lld", 
       loop_iterations, output_samples_written, state->input_buffer_content, state->next_ideal_input_frame_start_sample_offset);

    // --- Conservative Ring Buffer Management --- 
    // Only discard data we're absolutely sure we won't need anymore
    // Aggressive discarding can cause buffer underruns and audio artifacts
    if (state->input_buffer_content > 0) {
        // Keep a larger safety margin around the next search window
        // This prevents accidental discarding of data we might need
        // Especially important when changing speeds rapidly
        // const int SAFETY_MARGIN = MAX(state->overlap_samples, state->search_window_samples); // SAFETY_MARGIN not used in the revised logic below
        
        // Most conservative approach: 
        // 1. Keep data needed for next search window
        // The earliest sample needed is state->next_ideal_input_frame_start_sample_offset - state->search_window_samples.
        long long min_retainable_abs_offset = state->next_ideal_input_frame_start_sample_offset 
                                            - state->search_window_samples; 
                                            
        // Never discard data before stream position 0
        if (min_retainable_abs_offset < 0) min_retainable_abs_offset = 0;
        
        // Calculate how many samples can be safely discarded
        int samples_to_discard = (int)(min_retainable_abs_offset - state->input_ring_buffer_stream_start_offset);
        
        // Only log if we're actually discarding something
        if (samples_to_discard > 0) {
            DBG("WSOLA_RING_BUFFER: next_ideal=%lld, min_retain=%lld, content=%d, to_discard=%d", 
                state->next_ideal_input_frame_start_sample_offset,
                min_retainable_abs_offset, 
                state->input_buffer_content,
                samples_to_discard);
                
            // Prevent over-discarding (safety check)
            if (samples_to_discard > state->input_buffer_content / 2) {
                // Only discard half of the buffer at most to prevent rapid emptying
                samples_to_discard = state->input_buffer_content / 2;
                DBG("WSOLA_RING_BUFFER: Limiting discard to 50%% of buffer: %d samples", samples_to_discard);
            }
            
            // Only discard if we have enough samples to make it worthwhile
            // Prevents excessive small discard operations
            if (samples_to_discard >= state->analysis_hop_samples / 2) {
                // Update the buffer position tracking
                state->input_buffer_read_pos = (state->input_buffer_read_pos + samples_to_discard) % state->input_buffer_capacity;
                state->input_buffer_content -= samples_to_discard;
                state->input_ring_buffer_stream_start_offset += samples_to_discard;
                
                DBG("WSOLA_RING_BUFFER: Discarded %d samples, buffer now %d/%d full", 
                    samples_to_discard, state->input_buffer_content, state->input_buffer_capacity);
            }
        }
    }
    // --- End Conservative Ring Buffer Management ---

    // state->total_output_samples_generated += output_samples_written; // Moved this update to after the loop
    state->total_output_samples_generated += output_samples_written; // Ensure this is updated correctly
    // state->total_input_samples_processed = state->next_ideal_input_frame_start_sample_offset; // This is an approximation

    return output_samples_written;
}