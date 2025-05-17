#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
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
short fir_history[MAX_FIR_TAPS -1]; // Buffer to store previous samples for FIR calculation

// --- WSOLA State Global ---
WSOLA_State *wsola_state = NULL;

// --- WSOLA Function Prototypes ---
static void generate_hanning_window(float *float_window, int length);
static float calculate_normalized_cross_correlation(const short *segment1, const short *segment2, int length);
static float find_best_match_segment(
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

        // Then convolve with historical samples
        for (int j = 1; j < filter->num_taps; ++j) {
            filtered_sample += fir_history[filter->num_taps - 1 - j] * filter->coeffs[j];
        }

        // Update history: Shift history and add current_sample
        // fir_history stores [y(n-N+1) ... y(n-2) y(n-1)] where N is num_taps
        // Shift from oldest to newest position before inserting new sample
        for (int j = 0; j < filter->num_taps - 2; ++j) {
            fir_history[j] = fir_history[j+1];
        }
        if (filter->num_taps > 1) { // Only store history if filter has more than one tap
            fir_history[filter->num_taps - 2] = current_sample; // Newest historical sample is current_sample
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
                    case 161: pcm_format = SND_PCM_FORMAT_S16_LE; break;
                    case 162: pcm_format = SND_PCM_FORMAT_S16_BE; break;
                    case 241: pcm_format = SND_PCM_FORMAT_S24_LE; break;
                    case 242: pcm_format = SND_PCM_FORMAT_S24_BE; break;
                    case 2431: pcm_format = SND_PCM_FORMAT_S24_3LE; break;
                    case 2432: pcm_format = SND_PCM_FORMAT_S24_3BE; break;
                    case 321: pcm_format = SND_PCM_FORMAT_S32_LE; break;
                    case 322: pcm_format = SND_PCM_FORMAT_S32_BE; break;
                    default:
                        fprintf(stderr, "Unsupported format code: %d. Try to infer from WAV header.\n", format_code);
                        user_specified_format = false; // Mark as not successfully set by user
                        pcm_format = SND_PCM_FORMAT_UNKNOWN;
                        break;
                }
                if (pcm_format != SND_PCM_FORMAT_UNKNOWN) {
                     printf("User selected format: %s\n", snd_pcm_format_name(pcm_format));
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
    printf("ALSA Actual period size: %lu frames, ALSA Actual buffer size: %lu frames\n", local_period_size_frames, frames);

    debug_msg(snd_pcm_hw_params(pcm_handle, hw_params), "加载硬件配置参数到驱动");
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
                    // No TODO needed here, logic is in the main playback section
                }
                else if (c_in == ']') { // Increase speed
                    if (current_speed_idx < NUM_SPEED_LEVELS - 1) {
                        current_speed_idx++;
                    }
                    app_log("INFO", "Playback speed: %.1fx", PLAYBACK_SPEED_FACTORS[current_speed_idx]);
                    // No TODO needed here, logic is in the main playback section
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

        if (fabs(current_speed - 1.0) < 1e-6) { // Compare double for equality with tolerance
            buffer_for_alsa = (unsigned char*)source_buffer_for_speed_change_s16;
            frames_for_alsa = num_source_samples_for_speed_change / wav_header.num_channels; // Assuming block_align is reliable for S16_LE stereo/mono
                                                                                         // More robust: num_source_samples (which is total samples, not per channel) / num_channels for frame count.
                                                                                         // block_align = (bits_per_sample/8) * num_channels. So frames_read_this_iteration = read_ret / block_align
            frames_for_alsa = frames_read_this_iteration; // This was already calculated and seems more direct

            // If EQ was applied, buffer_for_alsa points to eq_processed_audio_buffer_s16.
            // If EQ was bypassed or failed, it points to buff (cast to short* then back to unsigned char*).
            // No new allocation is needed here unless the source was `buff` and `eq_processed_audio_buffer_s16` was used.
            // This path is for NO speed change. If eq_processed_audio_buffer_s16 exists, it's the data to play.
            if (eq_processed_audio_buffer_s16 != NULL) {
                 buffer_for_alsa = (unsigned char*)eq_processed_audio_buffer_s16;
            } else {
                 buffer_for_alsa = buff; // Original data from file
            }

        } else {
            // Calculate target number of output frames/samples
            // Speed adjustment operates on samples. num_source_samples_for_speed_change is total samples (L+R for stereo)
            int target_num_output_samples = (int)round((double)num_source_samples_for_speed_change / current_speed);
            frames_for_alsa = target_num_output_samples / wav_header.num_channels; // Convert total samples to frames

            if (target_num_output_samples > 0) {
                size_t resampled_buffer_size_bytes = target_num_output_samples * sizeof(short); // Since we work with short samples
                resampled_audio_buffer_s16 = (short *)malloc(resampled_buffer_size_bytes);

                if (resampled_audio_buffer_s16 == NULL) {
                    app_log("ERROR", "Failed to allocate memory for speed-adjusted buffer (size %zu). Playing normal for this chunk.", resampled_buffer_size_bytes);
                    buffer_for_alsa = (unsigned char*)source_buffer_for_speed_change_s16; // Fallback to source (potentially EQd)
                    frames_for_alsa = frames_read_this_iteration;
                    processed_buffer_allocated = false; // Mark that the resampled_audio_buffer_s16 is not the one to free later
                } else {
                    processed_buffer_allocated = true; // resampled_audio_buffer_s16 was allocated
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
                    frames_for_alsa = actual_output_samples_generated / wav_header.num_channels; // Update based on actual generation

                    if (frames_for_alsa == 0 && processed_buffer_allocated) { 
                        free(resampled_audio_buffer_s16);
                        resampled_audio_buffer_s16 = NULL; 
                        processed_buffer_allocated = false;
                        buffer_for_alsa = NULL; // No data to play
                    } else if (processed_buffer_allocated) {
                        buffer_for_alsa = (unsigned char*)resampled_audio_buffer_s16;
                    }
                }
            } else {
                frames_for_alsa = 0;
                buffer_for_alsa = NULL; // No data to play
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
    return 0;
}

// --- WSOLA Implementation ---

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

// Calculates a simplified cross-correlation normalized by signal energies.
// Returns a value ideally between -1 and 1 (though precision with shorts might affect exact range).
// Higher absolute value means more similarity (or inverse similarity).
static float calculate_normalized_cross_correlation(const short *segment1, const short *segment2, int length) {
    if (length <= 0 || !segment1 || !segment2) {
        return 0.0f;
    }

    double sum_s1_s2 = 0.0;
    double sum_s1_sq = 0.0;
    double sum_s2_sq = 0.0;

    for (int i = 0; i < length; i++) {
        double s1_val = (double)segment1[i];
        double s2_val = (double)segment2[i];

        sum_s1_s2 += s1_val * s2_val;
        sum_s1_sq += s1_val * s1_val;
        sum_s2_sq += s2_val * s2_val;
    }

    if (sum_s1_sq == 0.0 || sum_s2_sq == 0.0) {
        // If either segment is all zeros, correlation is undefined or zero.
        // If they are both all zeros, they are perfectly correlated, but sqrt(0*0) is an issue.
        // Let's return 1.0 if both are zero energy (identical silence), 0 otherwise.
        return (sum_s1_sq == 0.0 && sum_s2_sq == 0.0) ? 1.0f : 0.0f;
    }

    double denominator = sqrt(sum_s1_sq * sum_s2_sq);
    if (denominator == 0.0) { // Should be caught by above, but as a safeguard
        return 0.0f;
    }

    return (float)(sum_s1_s2 / denominator);
}

// Helper function to extract a segment from the input ring buffer, handling wrap-around.
// Copies `length` samples starting from `start_index` in `state->input_buffer_ring` into `output_segment`.
// Assumes `output_segment` is pre-allocated with at least `length` capacity.
// Returns false if requested segment is not fully available in the ring buffer.
static bool get_segment_from_ring_buffer(const WSOLA_State *state, int start_index_in_ring, int length, short *output_segment) {
    if (!state || !state->input_buffer_ring || !output_segment || length <= 0) return false;
    if (length > state->input_buffer_content) { // Not enough data in the entire buffer
        // app_log("DEBUG", "get_segment: not enough content (%d) for length %d", state->input_buffer_content, length);
        return false;
    }

    for (int i = 0; i < length; i++) {
        int current_ring_idx = (start_index_in_ring + i) % state->input_buffer_capacity;
        output_segment[i] = state->input_buffer_ring[current_ring_idx];
    }
    return true;
}

// Finds the segment in the input ring buffer (within a search window) that best matches 
// the provided target_segment (typically the tail of the last synthesized frame).
// The search is centered around an ideal_start_offset_in_ring.
// state: WSOLA state containing ring buffer, search_window_samples, overlap_samples.
// target_segment_for_comparison: The segment to match against (e.g., state->output_overlap_add_buffer).
// ideal_search_center_ring_idx: The index in the ring buffer that is the center of our search range.
// best_segment_start_offset_from_ideal_center_ptr: Output, the offset from ideal_search_center_ring_idx of the best match found.
//                                                    A value of 0 means the ideal center itself was best or part of best segment.
// Returns: The highest correlation value found. If no valid segment found, returns a very low value (e.g. -2.0f).
static float find_best_match_segment(
    const WSOLA_State *state, 
    const short *target_segment_for_comparison, 
    int ideal_search_center_ring_idx, 
    int *best_segment_start_offset_from_ideal_center_ptr
) {
    if (!state || !target_segment_for_comparison || !best_segment_start_offset_from_ideal_center_ptr) {
        return -2.0f; // Indicate error
    }

    int N_o = state->overlap_samples;
    int S_w = state->search_window_samples;

    if (N_o <= 0) return -2.0f; // Nothing to compare

    *best_segment_start_offset_from_ideal_center_ptr = 0;
    float max_correlation = -2.0f; // Initialize with a value lower than any possible correlation

    short *candidate_segment = (short *)malloc(N_o * sizeof(short));
    if (!candidate_segment) {
        app_log("ERROR", "find_best_match: Failed to allocate temp candidate_segment buffer");
        return -2.0f; 
    }

    if (S_w == 0) {
        // No search window, just check the ideal_search_center_ring_idx
        if (get_segment_from_ring_buffer(state, ideal_search_center_ring_idx, N_o, candidate_segment)) {
            max_correlation = calculate_normalized_cross_correlation(target_segment_for_comparison, candidate_segment, N_o);
            *best_segment_start_offset_from_ideal_center_ptr = 0; // Offset is 0 by definition
        } else {
            max_correlation = -2.0f; // Segment not available
            *best_segment_start_offset_from_ideal_center_ptr = 0;
        }
    } else {
        // The search range is [ideal_search_center_ring_idx - S_w, ideal_search_center_ring_idx + S_w]
        for (int offset = -S_w; offset <= S_w; ++offset) {
            int candidate_start_ring_idx = (ideal_search_center_ring_idx + offset + state->input_buffer_capacity) % state->input_buffer_capacity;

            if (!get_segment_from_ring_buffer(state, candidate_start_ring_idx, N_o, candidate_segment)) {
                continue; 
            }

            float current_correlation = calculate_normalized_cross_correlation(target_segment_for_comparison, candidate_segment, N_o);

            if (current_correlation > max_correlation) {
                max_correlation = current_correlation;
                *best_segment_start_offset_from_ideal_center_ptr = offset;
            }
        }
        // If no correlation was found (e.g. all segments were unavailable in a search window)
        if (max_correlation == -2.0f) { 
            // app_log("DEBUG", "find_best_match: No valid correlation found in search window. Defaulting to offset 0.");
            *best_segment_start_offset_from_ideal_center_ptr = 0;
            // Optionally, could try to get the segment at offset 0 if all others failed, but current loop structure implies it was tried if S_w > 0.
            // If an error occurred or no segment was available, max_correlation remains -2.0f
        }
    }

    free(candidate_segment);
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


    // Input ring buffer: needs to hold enough for current frame, look-behind for OLA, and look-ahead for search
    // Minimum capacity: overlap_samples (for previous output tail) + analysis_frame_samples (current natural choice) + search_window_samples (for search).
    // A more generous size: 2*N + 2*S_w (N=analysis_frame_samples, S_w=search_window_samples) to simplify wrapping logic.
    // Let's use: N (current frame) + N_o (previous output overlap) + 2*S_w (search range) + some safety margin / input chunk size.
    // For simplicity, let's make it at least 2 * analysis_frame_samples + 2 * search_window_samples
    s->input_buffer_capacity = s->analysis_frame_samples + (2 * s->search_window_samples) + s->analysis_frame_samples; // A bit more than N + 2*S_w
    if (s->input_buffer_capacity < MAX_WSOLA_FRAME_SAMPLES * 2) { // Ensure some reasonable minimum
        s->input_buffer_capacity = MAX_WSOLA_FRAME_SAMPLES * 2;
    }


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