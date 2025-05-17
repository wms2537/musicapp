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
static int debug_enabled = 1; // Runtime flag to enable/disable DEBUG logs. Set to 1 for testing.

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

// --- Music File Playlist Globals ---
char **music_files = NULL;
int num_music_files = 0;
int current_track_idx = 0;

// --- Playback Speed Globals ---
const double PLAYBACK_SPEED_FACTORS[] = {0.5, 1.0, 1.5, 2.0};
const int NUM_SPEED_LEVELS = sizeof(PLAYBACK_SPEED_FACTORS) / sizeof(PLAYBACK_SPEED_FACTORS[0]);
int current_speed_idx = 1; // Index for 1.0x speed

// --- Equalizer Globals ---
const FIRFilter *EQ_PRESETS[] = {&FIR_NORMAL, &FIR_BASS_BOOST, &FIR_TREBLE_BOOST};
const int NUM_EQ_PRESETS = sizeof(EQ_PRESETS) / sizeof(EQ_PRESETS[0]);
int current_eq_idx = 0; // Default to Normal/Flat
static short fir_history[MAX_FIR_TAPS -1]; // Buffer to store previous samples for FIR calculation
static int fir_filter_history_idx = 0;    // Index for circular FIR history buffer

// --- WSOLA State Global ---
WSOLA_State *wsola_state = NULL;

// --- WSOLA Function Prototypes ---
static void generate_hanning_window(float *float_window, int length);
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
    // --- TEMPORARY BYPASS FOR DEBUGGING --- 
    if (input_buffer != output_buffer) {
        memcpy(output_buffer, input_buffer, num_samples * sizeof(short));
    }
    return; 
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

    while ((opt_char = getopt(argc, argv, "m:f:r:d:")) != -1) {
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
                    } else {
                        snd_pcm_pause(pcm_handle, 0);
                        app_log("INFO", "Playback RESUMED.");
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
                    app_log("INFO", "Playback speed: %.1fx", PLAYBACK_SPEED_FACTORS[current_speed_idx]);
                    if (wsola_state) {
                        wsola_state->current_speed_factor = PLAYBACK_SPEED_FACTORS[current_speed_idx];
                    }
                }
                else if (c_in == ']') { // Increase speed
                    if (current_speed_idx < NUM_SPEED_LEVELS - 1) {
                        current_speed_idx++;
                    }
                    app_log("INFO", "Playback speed: %.1fx", PLAYBACK_SPEED_FACTORS[current_speed_idx]);
                    if (wsola_state) {
                        wsola_state->current_speed_factor = PLAYBACK_SPEED_FACTORS[current_speed_idx];
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

        read_ret = fread(buff, 1, buffer_size, fp);
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

        unsigned char *buffer_for_alsa = NULL;
        snd_pcm_uframes_t frames_for_alsa = 0;
        bool processed_buffer_allocated = false;
        short *resampled_audio_buffer_s16 = NULL; // For resampled audio before ALSA
        short *eq_processed_audio_buffer_s16 = NULL; // For EQ processed audio
        int num_samples_in_buff = read_ret / (wav_header.bits_per_sample / 8);

        // Determine current samples format (e.g. S16_LE)
        // For FIR filtering, we need signed 16-bit samples.
        // The `buff` contains raw bytes. We need to interpret these as samples.
        // Assuming S16_LE for now as it's common and handled by ALSA setup.
        // If format is different, conversion will be needed here.

        if (pcm_format != SND_PCM_FORMAT_S16_LE) {
            app_log("WARNING", "FIR EQ currently assumes S16_LE input. Format is %s. EQ will be bypassed for this chunk.", snd_pcm_format_name(pcm_format));
            // If not S16_LE, bypass EQ or implement conversion
            // For now, bypass by setting buffer_for_alsa to buff directly later if no speed change
        } else {
            // EQ Processing (S16_LE)
            eq_processed_audio_buffer_s16 = (short*)malloc(num_samples_in_buff * sizeof(short));
            if (eq_processed_audio_buffer_s16 == NULL) {
                app_log("ERROR", "Failed to allocate memory for EQ output buffer. EQ will be bypassed for this chunk.");
                // Fallback: use original buff samples if EQ alloc fails
                // This means if speed adjustment also happens, it will use original data
            } else {
                // Apply FIR filter
                // Note: buff contains unsigned char. Need to cast/treat as short* for S16_LE
                apply_fir_filter((short*)buff, eq_processed_audio_buffer_s16, num_samples_in_buff, EQ_PRESETS[current_eq_idx]);
                // After this, eq_processed_audio_buffer_s16 contains the filtered audio data
            }
        }

        double current_speed = PLAYBACK_SPEED_FACTORS[current_speed_idx];
        short* source_buffer_for_speed_change_s16 = (eq_processed_audio_buffer_s16 != NULL) ? eq_processed_audio_buffer_s16 : (short*)buff;
        int num_source_samples_for_speed_change = num_samples_in_buff;

        // --- WSOLA / Speed Change Logic --- 
        bool use_wsola = (wsola_state != NULL && pcm_format == SND_PCM_FORMAT_S16_LE && wav_header.num_channels == 1);

        if (use_wsola) {
            // WSOLA Path (Pitch Preserving)
            // Estimate max possible output samples from WSOLA for this input chunk
            // Max output = input_samples / min_speed_factor (e.g., 0.5x)
            // A safe buffer: num_source_samples_for_speed_change / 0.5 (i.e., * 2) + some margin for frame alignment.
            int max_wsola_output_samples = (int)((double)num_source_samples_for_speed_change / PLAYBACK_SPEED_FACTORS[0]) + wsola_state->analysis_frame_samples;
            resampled_audio_buffer_s16 = (short *)malloc(max_wsola_output_samples * sizeof(short));

            if (resampled_audio_buffer_s16 == NULL) {
                app_log("ERROR", "Failed to allocate memory for WSOLA output buffer. Using simple speed change for this chunk.");
                use_wsola = false; // Fallback
                processed_buffer_allocated = false;
            } else {
                int wsola_output_count = wsola_process(wsola_state, 
                                                       source_buffer_for_speed_change_s16, 
                                                       num_source_samples_for_speed_change, 
                                                       resampled_audio_buffer_s16, 
                                                       max_wsola_output_samples);
                if (wsola_output_count > 0) {
                    buffer_for_alsa = (unsigned char*)resampled_audio_buffer_s16;
                    frames_for_alsa = wsola_output_count / wav_header.num_channels; // wsola_output_count is total samples
                    processed_buffer_allocated = true;
                } else {
                    // No output from WSOLA, or error
                    app_log("WARNING", "WSOLA processing returned 0 samples.");
                    buffer_for_alsa = NULL; // No data to play
                    frames_for_alsa = 0;
                    free(resampled_audio_buffer_s16); // Free the buffer we allocated
                    resampled_audio_buffer_s16 = NULL;
                    processed_buffer_allocated = false;
                }
            }
        }
        
        // Fallback to simple speed change if WSOLA not used or failed for this chunk
        if (!use_wsola) {
            double current_speed = PLAYBACK_SPEED_FACTORS[current_speed_idx];
            if (fabs(current_speed - 1.0) < 1e-6) { // No speed change or WSOLA path not taken
                buffer_for_alsa = (unsigned char*)source_buffer_for_speed_change_s16;
                frames_for_alsa = frames_read_this_iteration;
                if (eq_processed_audio_buffer_s16 != NULL) {
                    buffer_for_alsa = (unsigned char*)eq_processed_audio_buffer_s16;
        } else {
                    buffer_for_alsa = buff; 
                }
                processed_buffer_allocated = false; // No new buffer was allocated for speed change here
            } else { // Simple pitch-changing speed adjustment
            int target_num_output_samples = (int)round((double)num_source_samples_for_speed_change / current_speed);
                frames_for_alsa = target_num_output_samples / wav_header.num_channels;
            if (target_num_output_samples > 0) {
                    size_t resampled_buffer_size_bytes = target_num_output_samples * sizeof(short);
                resampled_audio_buffer_s16 = (short *)malloc(resampled_buffer_size_bytes);
                if (resampled_audio_buffer_s16 == NULL) {
                        app_log("ERROR", "Failed to allocate memory for simple speed-adjusted buffer. Playing normal for this chunk.");
                        buffer_for_alsa = (unsigned char*)source_buffer_for_speed_change_s16;
                    frames_for_alsa = frames_read_this_iteration;
                        processed_buffer_allocated = false;
                } else {
                        processed_buffer_allocated = true;
                    double input_sample_cursor = 0.0;
                    int actual_output_samples_generated = 0;
                    for (int out_sample_num = 0; out_sample_num < target_num_output_samples; ++out_sample_num) {
                        int input_sample_to_sample_idx = (int)floor(input_sample_cursor);
                        if (input_sample_to_sample_idx >= num_source_samples_for_speed_change) {
                            target_num_output_samples = actual_output_samples_generated;
                            frames_for_alsa = actual_output_samples_generated / wav_header.num_channels;
                            break;
                        }
                        resampled_audio_buffer_s16[actual_output_samples_generated] = 
                            source_buffer_for_speed_change_s16[input_sample_to_sample_idx];
                        actual_output_samples_generated++;
                        input_sample_cursor += current_speed;
                    }
                        frames_for_alsa = actual_output_samples_generated / wav_header.num_channels;
                    if (frames_for_alsa == 0 && processed_buffer_allocated) { 
                        free(resampled_audio_buffer_s16);
                        resampled_audio_buffer_s16 = NULL; 
                        processed_buffer_allocated = false;
                            buffer_for_alsa = NULL;
                    } else if (processed_buffer_allocated) {
                        buffer_for_alsa = (unsigned char*)resampled_audio_buffer_s16;
                    }
                }
            } else {
                frames_for_alsa = 0;
                    buffer_for_alsa = NULL;
                }
            }
        }

        // ALSA Write Loop
        if (frames_for_alsa > 0 && buffer_for_alsa != NULL) {
            unsigned char *ptr_to_write = buffer_for_alsa;
            snd_pcm_uframes_t remaining_frames_to_write = frames_for_alsa;

            while (remaining_frames_to_write > 0) {
                snd_pcm_sframes_t frames_written_alsa = snd_pcm_writei(pcm_handle, ptr_to_write, remaining_frames_to_write);
                if (frames_written_alsa < 0) {
                    if (frames_written_alsa == -EPIPE) {
                        app_log("WARNING", "ALSA underrun occurred (EPIPE), preparing interface.");
                        snd_pcm_prepare(pcm_handle);
                    } else {
                        app_log("ERROR", "ALSA snd_pcm_writei error: %s", snd_strerror(frames_written_alsa));
                        if (processed_buffer_allocated) {
                            free(resampled_audio_buffer_s16);
                        }
                        goto playback_end; 
                    }
                } else {
                    ptr_to_write += frames_written_alsa * wav_header.block_align;
                    remaining_frames_to_write -= frames_written_alsa;
                }
            }
        }

        if (processed_buffer_allocated && resampled_audio_buffer_s16 != NULL) {
            free(resampled_audio_buffer_s16);
        }
        if (eq_processed_audio_buffer_s16 != NULL) {
            free(eq_processed_audio_buffer_s16);
        }
        
        // This original block for breaking on partial read needs context:
        // Is it after every write, or related to the initial fread?
        // The `fread` is at the top of the loop. `read_ret` is its result.
        // If `read_ret < buffer_size` means less than a full buffer was read from the file.
        // This usually signifies end-of-file or very near it.
        // The original code broke here, indicating "stop after playing this potentially partial buffer".
        // This logic should be preserved.
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

// Generates a Hanning window of a given length.
// Output `window` values are typically floats [0,1], here we might store Q15 shorts if direct scaling desired
// For now, this function will be a placeholder as we need to decide on float vs fixed-point for window.
// Let's assume it populates a float array first, then it's converted to short if needed.
// For simplicity here, we'll compute float values and assume they'll be scaled appropriately when used.
static void generate_hanning_window(float *float_window, int length) {
    if (length <= 0) return;
    for (int i = 0; i < length; i++) {
        float_window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (float)(length - 1)));
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

    long long sum_s1s2 = 0;
    long long sum_s1_sq = 0;
    long long sum_s2_sq = 0;

    for (int i = 0; i < length; ++i) {
        short s1 = segment1[i];
        short s2 = segment2[i];
        sum_s1s2 += (long long)s1 * s2;
        sum_s1_sq += (long long)s1 * s1;
        sum_s2_sq += (long long)s2 * s2;
    }

    if (sum_s1_sq == 0 || sum_s2_sq == 0) {
        // If either segment has zero energy, correlation is typically 0, unless both are zero energy then it can be 1.
        // For practical purposes in audio, if one is silence, consider correlation 0.
        // app_log("DEBUG", "NCC: Zero energy in one or both segments. s1_sq=%lld, s2_sq=%lld", sum_s1_sq, sum_s2_sq);
        return 0.0f;
    }

    double denominator = sqrt((double)sum_s1_sq) * sqrt((double)sum_s2_sq);
    if (denominator == 0) { // Should be caught by sum_s1_sq/sum_s2_sq check, but for safety.
        return 0.0f;
    }
    
    return (float)((double)sum_s1s2 / denominator);
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
    // If S_w is 0 or negative, this means no search beyond the ideal center.
    // This can be a valid configuration for minimal processing or testing.

    // Allocate a temporary buffer for candidate segments from the ring buffer
    short *candidate_segment = (short *)malloc(N_o * sizeof(short));
    if (!candidate_segment) {
        app_log("ERROR", "find_best_match: Failed to allocate memory for candidate_segment.");
        *best_segment_start_offset_from_ideal_center_ptr = 0;
        return -1e18; // Error
    }

    // Initialize with a very low correlation value (since we want to maximize)
    // For float, -FLT_MAX is suitable. For long long, a very negative number.
    long long max_correlation = -LLONG_MAX; // Use LLONG_MAX from <limits.h> (implicitly included by others)
    *best_segment_start_offset_from_ideal_center_ptr = 0; // Default to no offset (ideal center)
    bool match_found = false;

    if (S_w <= 0) { // No search window, or invalid search window
        // No search window, just check the ideal_search_center_ring_idx
        if (get_segment_from_ring_buffer(state, ideal_search_center_ring_idx, N_o, candidate_segment)) {
            float ncc_float = calculate_normalized_cross_correlation(target_segment_for_comparison, candidate_segment, N_o);
            max_correlation = (long long)(ncc_float * 1000000.0f); // Scale float NCC to long long
            *best_segment_start_offset_from_ideal_center_ptr = 0; // Offset is 0 by definition
            match_found = true;
        } else {
            // This case should ideally be prevented by checks in wsola_process before calling find_best_match
            app_log("WARNING", "find_best_match: (No search window) Could not get segment at ideal_search_center_ring_idx %d", ideal_search_center_ring_idx);
            // max_correlation remains -LLONG_MAX, *best_segment_start_offset_from_ideal_center_ptr remains 0
        }
    } else {
        // Search from -S_w to +S_w around the ideal_search_center_ring_idx
        for (int offset = -S_w; offset <= S_w; ++offset) {
            int current_candidate_start_ring_idx = (ideal_search_center_ring_idx + offset + state->input_buffer_capacity) % state->input_buffer_capacity;
            
            if (!get_segment_from_ring_buffer(state, current_candidate_start_ring_idx, N_o, candidate_segment)) {
                // This might happen if the search window goes into an area of the ring buffer that doesn't have valid data yet
                // Or if an earlier part of the ring buffer has already been overwritten.
                // app_log("DEBUG", "find_best_match: Could not get candidate segment at offset %d (ring_idx %d). Skipping.", offset, current_candidate_start_ring_idx);
                continue; // Skip this candidate
            }

            float current_ncc_float = calculate_normalized_cross_correlation(target_segment_for_comparison, candidate_segment, N_o);
            long long current_correlation = (long long)(current_ncc_float * 1000000.0f); // Scale float NCC to long long

            if (current_correlation > max_correlation) {
                max_correlation = current_correlation;
                *best_segment_start_offset_from_ideal_center_ptr = offset;
                match_found = true;
            }
        }
    }

    free(candidate_segment);

    if (!match_found) {
        // This could happen if S_w > 0 but get_segment_from_ring_buffer failed for all offsets,
        // or if S_w <=0 and the initial get_segment also failed.
        // This indicates a potential issue with data availability or search window logic.
        app_log("WARNING", "find_best_match: No valid segment found in search window. Ideal center idx: %d, S_w: %d", ideal_search_center_ring_idx, S_w);
        // Return the very small number, best_offset already 0
    }
    
    // app_log("DEBUG", "find_best_match: Best offset = %d, Max Corr = %.4f", *best_segment_start_offset_from_ideal_center_ptr, max_correlation);
    // Corrected log format for long long max_correlation, and display the original float NCC value for clarity
    DBG("find_best_match: Best offset = %d, Max Corr (scaled) = %lld, NCC_float = %.4f", 
            *best_segment_start_offset_from_ideal_center_ptr, max_correlation, (float)max_correlation / 1000000.0f);
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
    s->overlap_samples = (int)(s->analysis_frame_samples * overlap_percentage_arg);
    s->analysis_hop_samples = s->analysis_frame_samples - s->overlap_samples; // Nominal input hop at 1x speed
    // Synthesis hop depends on speed factor, calculated during processing, but nominal is same as analysis_hop
    s->synthesis_hop_samples = s->analysis_hop_samples; // This is the target output advancement for each frame at 1x speed
                                                       // Effective output hop = analysis_hop_samples / speed_factor

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
    // Generate Hanning window (float first, then convert to Q15 short)
    float *temp_float_window = (float *)malloc(s->analysis_frame_samples * sizeof(float));
    if (!temp_float_window) {
        app_log("ERROR", "wsola_init: Failed to allocate memory for temp_float_window.");
        wsola_destroy(&s); return false;
    }
    generate_hanning_window(temp_float_window, s->analysis_frame_samples);
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

    // --- TEMPORARY BYPASS FOR 1.0x SPEED --- 
    if (fabs(state->current_speed_factor - 1.0) < 1e-6) {
        DBG("WSOLA_PROCESS: Speed is 1.0x, performing direct copy bypass.");
        int samples_to_copy = (num_input_samples < max_output_samples) ? num_input_samples : max_output_samples;
        if (input_samples && samples_to_copy > 0) {
            memcpy(output_buffer, input_samples, samples_to_copy * sizeof(short));
            // Nullify WSOLA internal buffers and reset counters as if it processed, to avoid issues if speed changes later
            // This is a bit crude; a proper flush/reset might be needed if switching from this bypass to active WSOLA.
            // For now, just ensure input buffer doesn't grow indefinitely if 1.0x is used for a while.
            wsola_add_input_to_ring_buffer(state, input_samples, num_input_samples); // Still add to ring buffer
            
            long long min_retainable_abs_offset = state->next_ideal_input_frame_start_sample_offset 
                                            - state->search_window_samples 
                                            - state->overlap_samples; 
            if (min_retainable_abs_offset < 0) min_retainable_abs_offset = 0;
            int samples_to_discard = (int)(min_retainable_abs_offset - state->input_ring_buffer_stream_start_offset);
            if (samples_to_discard > 0) {
                if (samples_to_discard > state->input_buffer_content) {
                    samples_to_discard = state->input_buffer_content;
                }
                state->input_buffer_read_pos = (state->input_buffer_read_pos + samples_to_discard) % state->input_buffer_capacity;
                state->input_buffer_content -= samples_to_discard;
                state->input_ring_buffer_stream_start_offset += samples_to_discard;
            }
            state->next_ideal_input_frame_start_sample_offset += num_input_samples; // Advance ideal input marker
            state->total_output_samples_generated += samples_to_copy;
            return samples_to_copy;
        }
        return 0;
    }
    // --- END TEMPORARY BYPASS ---

    if (input_samples && num_input_samples > 0) {
        wsola_add_input_to_ring_buffer(state, input_samples, num_input_samples);
    }

    int output_samples_written = 0;
    int N = state->analysis_frame_samples;
    int N_o = state->overlap_samples;
    int H_a = state->analysis_hop_samples; // Nominal analysis hop = N - N_o

    // Effective synthesis hop size (how many output samples we generate per frame)
    int H_s_eff = (int)round((double)H_a / state->current_speed_factor);
    if (H_s_eff <= 0) H_s_eff = 1; // Ensure progress even at very high speed factors

    DBG("WSOLA_PROCESS_ENTRY: num_input=%d, max_output=%d, current_speed=%.2f, H_s_eff=%d, input_content_start=%d", 
           num_input_samples, max_output_samples, state->current_speed_factor, H_s_eff, state->input_buffer_content);

    int loop_iterations = 0;
    while (output_samples_written + H_s_eff <= max_output_samples) {
        loop_iterations++;
        app_log("DEBUG", "WSOLA_PROCESS_LOOP_ITER: iter=%d, out_written=%d, H_s_eff=%d", loop_iterations, output_samples_written, H_s_eff); // ADD THIS LOG

        long long latest_required_stream_offset = state->next_ideal_input_frame_start_sample_offset + N + state->search_window_samples;
        long long latest_available_stream_offset = state->input_ring_buffer_stream_start_offset + state->input_buffer_content;

        if (latest_available_stream_offset < latest_required_stream_offset) {
            app_log("DEBUG", "WSOLA_LOOP_BREAK_NO_DATA: iter=%d, avail=%lld, req=%lld", loop_iterations, latest_available_stream_offset, latest_required_stream_offset); // SIMPLIFIED LOG
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
            // This is a critical issue, implies logic error or insufficient data despite checks.
            // Attempt to advance input markers to prevent infinite loop if possible, or break.
            // For now, just break to avoid bad state propagation.
            break; 
        }

        // Apply Hanning window (Q15)
        for (int i = 0; i < N; ++i) {
            long long val_ll = (long long)state->current_synthesis_segment[i] * state->analysis_window_function[i];
            state->current_synthesis_segment[i] = (short)(val_ll >> 15); // Q15 scaling with bit-shift
            // double scaled_val_double = (double)val_ll / 32767.0; // analysis_window_function is scaled by 32767
            // if (scaled_val_double > 32767.0) scaled_val_double = 32767.0;
            // if (scaled_val_double < -32768.0) scaled_val_double = -32768.0;
            // state->current_synthesis_segment[i] = (short)round(scaled_val_double);
        }

        int samples_to_write_this_frame = H_s_eff;
        int samples_written_this_frame_ola = 0;
        int samples_written_this_frame_new = 0;

        // 1. Overlap-Add N_o samples and write to output_buffer
        for (int i = 0; i < N_o; ++i) {
            if (output_samples_written < max_output_samples && samples_written_this_frame_ola < samples_to_write_this_frame) {
                long long sum = (long long)state->output_overlap_add_buffer[i] + state->current_synthesis_segment[i];
                sum >>= 1; // Divide by 2 to maintain approximate unity gain after summing two windowed signals
                if (sum > 32767) sum = 32767;
                if (sum < -32768) sum = -32768;
                output_buffer[output_samples_written++] = (short)sum;
                samples_written_this_frame_ola++;
            } else {
                break; // Output buffer full or frame's sample quota met
            }
        }

        // 2. Write the new part from current_synthesis_segment, potentially stretching or compressing
        int needed_new_output_samples = samples_to_write_this_frame - samples_written_this_frame_ola;
        if (needed_new_output_samples > 0) {
            int available_new_samples_in_segment = N - N_o; // This is H_a

            if (available_new_samples_in_segment <= 0 && needed_new_output_samples > 0) {
                 app_log("WARNING", "WSOLA: No new samples available in synthesis segment (H_a=0), but needed %d output. Silence padding.", needed_new_output_samples);
                 for (int i = 0; i < needed_new_output_samples; ++i) {
                     if (output_samples_written < max_output_samples) {
                         output_buffer[output_samples_written++] = 0; // Pad with silence
                         samples_written_this_frame_new++;
                     } else break;
                 }
            } else if (available_new_samples_in_segment > 0) {
                double new_sample_read_cursor = 0.0;
                // The source for these new samples is current_synthesis_segment[N_o] onwards
                short *new_part_source = state->current_synthesis_segment + N_o;

                for (int i = 0; i < needed_new_output_samples; ++i) {
                    if (output_samples_written < max_output_samples) {
                        int source_idx_floor = (int)floor(new_sample_read_cursor);
                        double fraction = new_sample_read_cursor - source_idx_floor;

                        short sample1, sample2;

                        if (source_idx_floor >= available_new_samples_in_segment - 1) {
                            // At or beyond the second to last sample, or if only one sample available
                            sample1 = new_part_source[available_new_samples_in_segment - 1];
                            sample2 = sample1; // No next sample to interpolate with, use last sample
                        } else {
                            sample1 = new_part_source[source_idx_floor];
                            sample2 = new_part_source[source_idx_floor + 1];
                        }
                        
                        // Linear interpolation
                        short interpolated_sample = (short)round((1.0 - fraction) * sample1 + fraction * sample2);
                        output_buffer[output_samples_written++] = interpolated_sample;
                        samples_written_this_frame_new++;
                        
                        // Advance read cursor: we want to map `needed_new_output_samples` to `available_new_samples_in_segment`
                        // This is equivalent to resampling `available_new_samples_in_segment` to `needed_new_output_samples`
                        if (needed_new_output_samples > 0) { // Avoid division by zero if somehow needed_new_output_samples became 0
                           new_sample_read_cursor += (double)available_new_samples_in_segment / (double)needed_new_output_samples;
                        } else {
                            // Should not happen if needed_new_output_samples > 0 is checked before the loop
                            // but as a safeguard:
                            new_sample_read_cursor += 1.0; 
                        }
                    } else {
                        break; // Output buffer full
                    }
                }
            }
        }
        
        // If fewer than H_s_eff samples were written (e.g. output buffer became full mid-frame)
        // then the loop condition `output_samples_written + H_s_eff <= max_output_samples` will handle breaking.

        // Update the output_overlap_add_buffer with the tail of the current *windowed* synthesis segment
        // This is N_o samples from current_synthesis_segment[N-N_o] to current_synthesis_segment[N-1]
        // which is current_synthesis_segment[H_a] to current_synthesis_segment[N-1]
        for (int i = 0; i < N_o; ++i) {
            state->output_overlap_add_buffer[i] = state->current_synthesis_segment[H_a + i];
        }

        // Update input stream pointers
        state->next_ideal_input_frame_start_sample_offset += H_a; // Advance ideal input marker by one analysis hop

        // Update total samples processed and generated - this needs to be more precise
        // state->total_output_samples_generated += (output count in this iteration)
        // state->total_input_samples_processed = state->next_ideal_input_frame_start_sample_offset; (roughly)

        // Discard old data from ring buffer that's no longer needed for search or processing
        // Oldest sample needed for next search: state->next_ideal_input_frame_start_sample_offset - S_w
        // (actually, it's for an N_o segment starting there)
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

    // --- Unconditional Discard Logic --- 
    // Always try to discard old data after a processing pass, regardless of loop iterations.
    // This helps `input_ring_buffer_stream_start_offset` catch up with `next_ideal_input_frame_start_sample_offset`.
    if (state->input_buffer_content > 0) { // Only act if there's content
        long long min_retainable_abs_offset = state->next_ideal_input_frame_start_sample_offset 
                                            - state->search_window_samples 
                                            - state->overlap_samples; // Earliest point an N_o segment for correlation could start
        if (min_retainable_abs_offset < 0) min_retainable_abs_offset = 0;

        int samples_to_discard = (int)(min_retainable_abs_offset - state->input_ring_buffer_stream_start_offset);
        
        app_log("DEBUG", "WSOLA_POST_DISCARD_CHECK: min_retain_abs=%lld, ring_start_abs=%lld, content_before=%d, samples_to_discard_calc=%d", 
           min_retainable_abs_offset, state->input_ring_buffer_stream_start_offset, state->input_buffer_content, samples_to_discard);

        if (samples_to_discard > 0) {
            if (samples_to_discard > state->input_buffer_content) {
                app_log("WARNING", "WSOLA_POST_DISCARD: Attempting to discard %d, but only %d content. Clamping.", samples_to_discard, state->input_buffer_content);
                samples_to_discard = state->input_buffer_content;
            }
            state->input_buffer_read_pos = (state->input_buffer_read_pos + samples_to_discard) % state->input_buffer_capacity;
            state->input_buffer_content -= samples_to_discard;
            state->input_ring_buffer_stream_start_offset += samples_to_discard;
            app_log("DEBUG", "WSOLA_POST_DISCARD_DONE: discarded=%d, new_ring_start_abs=%lld, new_read_pos=%d, new_content=%d", 
               samples_to_discard, state->input_ring_buffer_stream_start_offset, state->input_buffer_read_pos, state->input_buffer_content);
        }
    }
    // --- End Unconditional Discard Logic ---

    // state->total_output_samples_generated += output_samples_written; // Moved this update to after the loop
    state->total_output_samples_generated += output_samples_written; // Ensure this is updated correctly
    // state->total_input_samples_processed = state->next_ideal_input_frame_start_sample_offset; // This is an approximation

    return output_samples_written;
}