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
#include "const.h"

// --- Logging Helper ---
// Simple timestamp for logs (optional, can be expanded)
void print_log_prefix() {
    // For now, no timestamp to keep it simple like existing logs.
    // If timestamps are needed, time.h functions can be used here.
    // time_t now = time(NULL);
    // struct tm *t = localtime(&now);
    // printf("[%02d:%02d:%02d] ", t->tm_hour, t->tm_min, t->tm_sec);
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
        print_log_prefix(); fprintf(stderr, "LOG ERROR: Track index %d out of bounds (0-%d).\n", track_idx, num_music_files - 1);
        return false;
    }
    if (fp != NULL) {
        fclose(fp);
        fp = NULL;
    }
    print_log_prefix(); printf("LOG INFO: Loading track %d/%d: %s\n", track_idx + 1, num_music_files, music_files[track_idx]);
    open_music_file(music_files[track_idx]); // open_music_file handles its own errors/exit

    // Reset playback state for the new track
    playback_paused = false; 

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

    print_log_prefix(); printf("LOG INFO: MusicApp starting...\n");

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
            print_log_prefix(); fprintf(stderr, "LOG ERROR: Failed to allocate memory for music file list.\n");
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
        print_log_prefix(); fprintf(stderr, "LOG ERROR: No music files provided.\n");
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
        print_log_prefix(); fprintf(stderr, "LOG ERROR: Critical parameters (rate or format) could not be determined.\n");
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
        print_log_prefix(); fprintf(stderr, "LOG ERROR: Failed to allocate memory for playback buffer.\n");
        exit(EXIT_FAILURE);
    }
    if (wav_header.block_align == 0) {
        print_log_prefix(); fprintf(stderr, "LOG ERROR: WAV header block_align is zero.\n");
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
        print_log_prefix(); printf("LOG WARNING: Failed to initialize mixer. Volume control will not be available.\n");
    } else {
        print_log_prefix(); printf("LOG INFO: Volume control initialized. Use '+' to increase, '-' to decrease volume. 'p' to pause/resume. ',' for prev, '.' for next track. '['/']' for speed.\n");
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
                        print_log_prefix(); printf("LOG INFO: Playback PAUSED. Press 'p' to resume.\n");
                    } else {
                        snd_pcm_pause(pcm_handle, 0);
                        print_log_prefix(); printf("LOG INFO: Playback RESUMED.\n");
                    }
                }
                else if (c_in == 'f') { // Seek Forward
                    long seek_offset = 10 * wav_header.byte_rate; // 10 seconds
                    if (fseek(fp, seek_offset, SEEK_CUR) == 0) {
                        print_log_prefix(); printf("LOG INFO: Seek FORWARD 10 seconds.\n");
                        // Optional: Consider clearing/resetting ALSA buffer if issues occur
                        // snd_pcm_drop(pcm_handle);
                        // snd_pcm_prepare(pcm_handle);
                    } else {
                        perror("Seek forward failed");
                        print_log_prefix(); fprintf(stderr, "LOG WARNING: Seek forward failed for %ld bytes.\n", seek_offset);
                    }
                }
                else if (c_in == 'b') { // Seek Backward
                    long seek_offset = -10 * wav_header.byte_rate; // 10 seconds backward
                    if (fseek(fp, seek_offset, SEEK_CUR) == 0) {
                        print_log_prefix(); printf("LOG INFO: Seek BACKWARD 10 seconds.\n");
                        // Optional: Consider clearing/resetting ALSA buffer
                        // snd_pcm_drop(pcm_handle);
                        // snd_pcm_prepare(pcm_handle);
                    } else {
                        //perror("Seek backward failed"); // fseek might fail if seeking before start
                        // Try seeking to the beginning of the data if seek_offset is too large
                        fseek(fp, sizeof(struct WAV_HEADER), SEEK_SET); // Go to start of data
                         print_log_prefix(); printf("LOG INFO: Seek BACKWARD 10 seconds (or to start of data).\n");
                    }
                }
                else if (c_in == '.') { // Next Track
                    if (num_music_files > 1) {
                        current_track_idx = (current_track_idx + 1) % num_music_files;
                        if (load_track(current_track_idx)) {
                            // Potentially reset ALSA or prepare for new track. load_track should handle basics.
                            print_log_prefix(); printf("LOG INFO: Playing NEXT track: %s\n", music_files[current_track_idx]);
                            // Ensure ALSA is ready for new data. If sample rates/formats can vary wildly,
                            // a more robust ALSA re-init (like in main after first load) might be needed.
                            // For now, assume open_music_file and subsequent ALSA setup handles it or files are compatible.
                            // snd_pcm_drain(pcm_handle); // Drain old data
                            // snd_pcm_prepare(pcm_handle); // Prepare for new data
                        } else {
                            print_log_prefix(); fprintf(stderr, "LOG ERROR: Failed to load next track.\n");
                            // Decide how to handle this - stop playback, try next, etc.
                        }
                    } else {
                        print_log_prefix(); printf("LOG INFO: No next track available.\n");
                    }
                }
                else if (c_in == ',') { // Previous Track
                    if (num_music_files > 1) {
                        current_track_idx = (current_track_idx - 1 + num_music_files) % num_music_files;
                        if (load_track(current_track_idx)) {
                            print_log_prefix(); printf("LOG INFO: Playing PREVIOUS track: %s\n", music_files[current_track_idx]);
                            // Similar ALSA considerations as for next track
                            // snd_pcm_drain(pcm_handle);
                            // snd_pcm_prepare(pcm_handle);
                        } else {
                            print_log_prefix(); fprintf(stderr, "LOG ERROR: Failed to load previous track.\n");
                        }
                    } else {
                        print_log_prefix(); printf("LOG INFO: No previous track available.\n");
                    }
                }
                else if (c_in == '[') { // Decrease speed
                    if (current_speed_idx > 0) {
                        current_speed_idx--;
                    }
                    print_log_prefix(); printf("LOG INFO: Playback speed: %.1fx\n", PLAYBACK_SPEED_FACTORS[current_speed_idx]);
                    // TODO: Implement actual speed change logic here
                }
                else if (c_in == ']') { // Increase speed
                    if (current_speed_idx < NUM_SPEED_LEVELS - 1) {
                        current_speed_idx++;
                    }
                    print_log_prefix(); printf("LOG INFO: Playback speed: %.1fx\n", PLAYBACK_SPEED_FACTORS[current_speed_idx]);
                    // TODO: Implement actual speed change logic here
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
            print_log_prefix(); printf("LOG INFO: End of music file input! (fread returned 0)\n");
            // --- Autoplay Next Track --- 
            if (num_music_files > 1 && current_track_idx < num_music_files -1 ){
                current_track_idx++;
                print_log_prefix(); printf("LOG INFO: Auto-playing next track...\n");
                if(load_track(current_track_idx)){
                    // snd_pcm_prepare(pcm_handle); // Prepare ALSA for new track
                    continue; // Continue to next iteration of while(1) to play new track
                } else {
                    print_log_prefix(); fprintf(stderr, "LOG ERROR: Failed to auto-play next track. Stopping.\n");
                    break; // Or handle error differently
                }
            } else if (num_music_files > 1 && current_track_idx == num_music_files -1) {
                 print_log_prefix(); printf("LOG INFO: End of playlist.\n");
                 break;
            } else {
                 break; // Single file or last file in playlist ended
            }
        }
        if (read_ret < 0) {
            perror("Error reading PCM data from file");
            break;
        }
        snd_pcm_uframes_t frames_to_write = read_ret / wav_header.block_align;
        if (frames_to_write == 0 && read_ret > 0) { // Read some data but not enough for a full frame
             print_log_prefix(); printf("LOG INFO: Partial frame data at end of file, %d bytes ignored.\n", read_ret);
             break;
        }
         if (frames_to_write == 0) { // No complete frames to write
            break;
        }


        snd_pcm_sframes_t frames_written_alsa;
        unsigned char *ptr = buff;
        snd_pcm_uframes_t remaining_frames = frames_to_write;

        while(remaining_frames > 0) {
            frames_written_alsa = snd_pcm_writei(pcm_handle, ptr, remaining_frames);
            if (frames_written_alsa < 0) {
                if (frames_written_alsa == -EPIPE) {
                    fprintf(stderr, "underrun occurred -32, preparing interface...\n");
                    snd_pcm_prepare(pcm_handle);
                } else {
                    fprintf(stderr, "Error from snd_pcm_writei: %s\n", snd_strerror(frames_written_alsa));
                    goto playback_end; // Break outer loop
                }
            } else {
                ptr += frames_written_alsa * wav_header.block_align;
                remaining_frames -= frames_written_alsa;

                // If playback was paused and then resumed, ALSA might need prepare
                if (snd_pcm_state(pcm_handle) == SND_PCM_STATE_PAUSED && !playback_paused) {
                    // This case might not be strictly necessary with snd_pcm_pause(pcm_handle, 0)
                    // but good to be aware if issues arise after resume.
                    // snd_pcm_prepare(pcm_handle); // Potentially needed if resume is not smooth
                }
            }
        }

        if (read_ret < buffer_size) {
            print_log_prefix(); printf("LOG INFO: End of music file (partial buffer read).\n");
            break;
        }
    }

playback_end:
    print_log_prefix(); printf("LOG INFO: Playback finished or stopped.\n");
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
    print_log_prefix(); printf("LOG INFO: MusicApp exiting normally.\n");
    return 0;
}