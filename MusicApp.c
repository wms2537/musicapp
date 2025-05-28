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
#include <math.h>  // For sqrt, pow, cosf, fabsf
#include <time.h>  // For logging timestamps
#include <signal.h> // For signal handling
#include "const.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

// 全局状态变量初始化
playback_state_t current_state = STOPPED;
playback_speed_t current_speed = SPEED_1_0X;
equalizer_mode_t current_eq_mode = EQ_NORMAL;
long current_position = 0;
long total_frames = 0;
char playlist[10][256];
int playlist_count = 0;
int current_track = 0;
long data_chunk_offset = 0; // Store the actual position of the data chunk
bool track_change_requested = false; // Flag for manual track changes
bool auto_next_requested = false; // Flag for automatic track changes

// FIR滤波器系数 - 正确归一化的滤波器 (sum ≈ 1.0)
static double bass_boost_coeffs[FIR_TAP_NUM] = {
    -0.005, -0.005, -0.005, 0.0, 0.01, 0.02, 0.03, 0.04,
    0.05, 0.06, 0.07, 0.08, 0.09, 0.09, 0.09, 0.08,
    0.08, 0.09, 0.09, 0.09, 0.08, 0.07, 0.06, 0.05,
    0.04, 0.03, 0.02, 0.01, 0.0, -0.005, -0.005, -0.005
};

static double treble_boost_coeffs[FIR_TAP_NUM] = {
    0.01, 0.005, 0.0, -0.01, -0.025, -0.04, -0.05, -0.04,
    -0.02, 0.01, 0.05, 0.10, 0.15, 0.19, 0.21, 0.22,
    0.22, 0.21, 0.19, 0.15, 0.10, 0.05, 0.01, -0.02,
    -0.04, -0.05, -0.04, -0.025, -0.01, 0.0, 0.005, 0.01
};

static double vocal_enhance_coeffs[FIR_TAP_NUM] = {
    0.005, 0.005, 0.01, 0.01, 0.015, 0.02, 0.025, 0.03,
    0.04, 0.05, 0.06, 0.075, 0.09, 0.10, 0.11, 0.115,
    0.115, 0.11, 0.10, 0.09, 0.075, 0.06, 0.05, 0.04,
    0.03, 0.025, 0.02, 0.015, 0.01, 0.01, 0.005, 0.005
};

static short delay_buffer_left[FIR_TAP_NUM] = {0};
static short delay_buffer_right[FIR_TAP_NUM] = {0};
static int delay_index_left = 0;
static int delay_index_right = 0;

// 时间拉伸插值的前一个样本存储
static short interpolation_prev_samples[2] = {0, 0};

// 时间拉伸缓冲区
static short stretch_buffer[STRETCH_FRAME_SIZE * 2] = {0};
static short overlap_buffer[STRETCH_OVERLAP_SIZE] = {0};
static int stretch_buffer_pos = 0;

// 日志文件指针
static FILE* log_file = NULL;


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

// 日志功能实现
void write_log(const char* type, const char* message) {
    if (log_file == NULL) {
        log_file = fopen("music_app.log", "a");
        if (log_file == NULL) {
            return;
        }
    }
    
    time_t now;
    struct tm *tm_info;
    char timestamp[20];
    
    time(&now);
    tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    fprintf(log_file, "[%s] %s: %s\n", timestamp, type, message);
    fflush(log_file);
}

void log_user_operation(const char* operation, const char* result) {
    char log_msg[LOG_BUFFER_SIZE];
    snprintf(log_msg, sizeof(log_msg), "User operation '%s' - %s", operation, result);
    write_log("USER", log_msg);
}

void log_program_info(const char* info_type, const char* message) {
    char log_msg[LOG_BUFFER_SIZE];
    snprintf(log_msg, sizeof(log_msg), "%s: %s", info_type, message);
    write_log("SYSTEM", log_msg);
}

bool debug_msg(int result, const char *str) {
    if (result < 0) {
        char error_msg[LOG_BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "%s failed! result = %d, err_info = %s", str, result, snd_strerror(result));
        log_program_info("ERROR", error_msg);
        exit(1);
    }
    return true;
}

// 重置音频处理状态
void reset_audio_processing_state() {
    // 清空FIR滤波器延迟缓冲区
    memset(delay_buffer_left, 0, sizeof(delay_buffer_left));
    memset(delay_buffer_right, 0, sizeof(delay_buffer_right));
    delay_index_left = 0;
    delay_index_right = 0;
    
    // 清空插值历史
    interpolation_prev_samples[0] = 0;
    interpolation_prev_samples[1] = 0;
    
    // 清空时间拉伸缓冲区
    memset(stretch_buffer, 0, sizeof(stretch_buffer));
    memset(overlap_buffer, 0, sizeof(overlap_buffer));
    
    // 重置时间拉伸算法中的静态变量
    reset_time_stretch_static_vars();
}

bool open_music_file(const char *path_name) {
    if (fp != NULL) {
        fclose(fp);
    }
    
    fp = fopen(path_name, "rb");
    if (fp == NULL) {
        char error_msg[LOG_BUFFER_SIZE];
        snprintf(error_msg, sizeof(error_msg), "Error opening WAV file: %s", path_name);
        log_program_info("ERROR", error_msg);
        return false;
    }
    
    char info_msg[LOG_BUFFER_SIZE];
    snprintf(info_msg, sizeof(info_msg), "Successfully opened file: %s", path_name);
    log_program_info("INFO", info_msg);
    // Read the basic WAV header first (up to format chunk)
    fread(&wav_header.chunk_id, 1, 4, fp);           // "RIFF"
    fread(&wav_header.chunk_size, 1, 4, fp);         // File size - 8
    fread(&wav_header.format, 1, 4, fp);             // "WAVE"
    fread(&wav_header.sub_chunk1_id, 1, 4, fp);      // "fmt "
    fread(&wav_header.sub_chunk1_size, 1, 4, fp);    // Format chunk size
    
    if (strncmp(wav_header.chunk_id, "RIFF", 4) != 0 ||
        strncmp(wav_header.format, "WAVE", 4) != 0 ||
        strncmp(wav_header.sub_chunk1_id, "fmt ", 4) != 0) {
        log_program_info("ERROR", "File does not appear to be a valid WAV file (missing RIFF/WAVE/fmt markers)");
        fclose(fp);
        fp = NULL;
        return false;
    }
    
    // Read the format chunk data
    fread(&wav_header.audio_format, 1, 2, fp);
    fread(&wav_header.num_channels, 1, 2, fp);
    fread(&wav_header.sample_rate, 1, 4, fp);
    fread(&wav_header.byte_rate, 1, 4, fp);
    fread(&wav_header.block_align, 1, 2, fp);
    fread(&wav_header.bits_per_sample, 1, 2, fp);
    
    // Skip any extra bytes in the format chunk
    long fmt_extra_bytes = wav_header.sub_chunk1_size - 16;
    if (fmt_extra_bytes > 0) {
        fseek(fp, fmt_extra_bytes, SEEK_CUR);
    }
    
    // Now search for the data chunk
    char chunk_id[4];
    uint32_t chunk_size;
    data_chunk_offset = 0;
    
    while (fread(chunk_id, 1, 4, fp) == 4) {
        fread(&chunk_size, 1, 4, fp);
        
        if (strncmp(chunk_id, "data", 4) == 0) {
            // Found the data chunk
            wav_header.sub_chunk2_size = chunk_size;
            memcpy(wav_header.sub_chunk2_id, chunk_id, 4);
            data_chunk_offset = ftell(fp);
            break;
        } else {
            // Skip this chunk
            fseek(fp, chunk_size, SEEK_CUR);
        }
    }
    
    if (data_chunk_offset == 0) {
        log_program_info("ERROR", "Could not find data chunk in WAV file");
        fclose(fp);
        fp = NULL;
        return false;
    }
    
    // 计算总帧数
    total_frames = wav_header.sub_chunk2_size / wav_header.block_align;
    current_position = 0;
    printf("------------- WAV Header Info -------------\n");
    printf("RIFF ID: %.4s, Chunk Size: %u, Format: %.4s\n", wav_header.chunk_id, wav_header.chunk_size, wav_header.format);
    printf("Subchunk1 ID: %.4s, Subchunk1 Size: %u\n", wav_header.sub_chunk1_id, wav_header.sub_chunk1_size);
    printf("Audio Format: %u (1=PCM), Num Channels: %u\n", wav_header.audio_format, wav_header.num_channels);
    printf("Sample Rate: %u, Byte Rate: %u\n", wav_header.sample_rate, wav_header.byte_rate);
    printf("Block Align: %u, Bits Per Sample: %u\n", wav_header.block_align, wav_header.bits_per_sample);
    printf("Data ID: %.4s, Data Size: %u\n", wav_header.sub_chunk2_id, wav_header.sub_chunk2_size);
    printf("Data chunk starts at offset: %ld\n", data_chunk_offset);
    printf("-----------------------------------------\n");
    
    // 重置音频处理状态以避免静态变量污染
    reset_audio_processing_state();
    return true;
}

// FIR滤波器实现
// 添加重置标志
static bool need_reset_static_vars = false;

void reset_time_stretch_static_vars() {
    need_reset_static_vars = true;
}

// 保持音调的时间拉伸算法 - 恢复到工作状态
void apply_time_stretch(short* input, short* output, int input_length, int* output_length, float speed_factor, int max_output_length) {
    *output_length = 0;
    
    if (speed_factor <= 0.0f || speed_factor == 1.0f) {
        // 无变化，直接复制
        int copy_length = input_length < max_output_length ? input_length : max_output_length;
        for (int i = 0; i < copy_length; i++) {
            output[i] = input[i];
        }
        *output_length = copy_length;
        return;
    }
    
    int num_channels = wav_header.num_channels;
    int frame_size = STRETCH_FRAME_SIZE; // 使用原来的512
    int hop_synthesis = frame_size / 4; // 恢复原来的设置128
    int hop_analysis;
    
    // 特殊处理0.5x速度以减少机器人音效
    if (speed_factor == 0.5f) {
        hop_analysis = hop_synthesis / 2; // 64样本，但使用特殊处理
    } else {
        hop_analysis = (int)(hop_synthesis * speed_factor);
    }
    
    // 汉宁窗函数
    static float hanning_window[STRETCH_FRAME_SIZE];
    static bool window_initialized = false;
    
    // 重置静态变量（文件切换时）
    if (need_reset_static_vars) {
        window_initialized = false;
        need_reset_static_vars = false;
    }
    
    if (!window_initialized) {
        for (int i = 0; i < frame_size; i++) {
            hanning_window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (frame_size - 1)));
        }
        window_initialized = true;
    }
    
    int input_pos = 0;
    int output_pos = 0;
    
    // 清零输出缓冲区
    for (int i = 0; i < max_output_length; i++) {
        output[i] = 0;
    }
    
    if (speed_factor == 0.5f) {
        // 0.5x保证完全连续的overlap-add
        int grain_size = 1024;      // grain大小
        int analysis_hop = 256;     // 输入步长（小）
        int synthesis_hop = 512;    // 输出步长（大，2x实现0.5x）
        
        int input_pos = 0;
        int output_pos = 0;
        
        while (input_pos + grain_size < input_length && output_pos + grain_size < max_output_length) {
            // 每个grain都完整复制，确保连续性
            for (int i = 0; i < grain_size; i++) {
                for (int ch = 0; ch < num_channels; ch++) {
                    int in_idx = input_pos + i * num_channels + ch;
                    int out_idx = output_pos + i * num_channels + ch;
                    
                    if (in_idx < input_length && out_idx < max_output_length) {
                        // 累加方式确保平滑过渡
                        int existing = output[out_idx];
                        int new_sample = input[in_idx];
                        
                        if (existing == 0) {
                            output[out_idx] = new_sample;
                        } else {
                            // 使用加权平均，确保平滑
                            output[out_idx] = (short)((existing + new_sample) / 2);
                        }
                    }
                }
            }
            
            // 关键比例：输入跳跃小，输出跳跃大
            input_pos += analysis_hop;   // 前进256
            output_pos += synthesis_hop; // 前进512，实现时间拉伸
        }
        
        *output_length = output_pos;
        return;
    } else {
        // 1.5x和2.0x的正常处理（保持不变）
        while (input_pos + frame_size < input_length && output_pos + frame_size < max_output_length) {
            for (int ch = 0; ch < num_channels; ch++) {
                for (int i = 0; i < frame_size; i++) {
                    int sample_idx = input_pos + i * num_channels + ch;
                    int out_idx = output_pos + i * num_channels + ch;
                    
                    if (sample_idx < input_length && out_idx < max_output_length) {
                        float windowed_sample = input[sample_idx] * hanning_window[i];
                        output[out_idx] += (short)(windowed_sample * 0.5f);
                    }
                }
            }
            
            input_pos += hop_analysis;
            output_pos += hop_synthesis;
        }
    }
    
    *output_length = output_pos;
}

void apply_fir_filter(short* input, short* output, int length, equalizer_mode_t mode) {
    double* coeffs;
    
    switch(mode) {
        case EQ_BASS_BOOST:
            coeffs = bass_boost_coeffs;
            break;
        case EQ_TREBLE_BOOST:
            coeffs = treble_boost_coeffs;
            break;
        case EQ_VOCAL_ENHANCE:
            coeffs = vocal_enhance_coeffs;
            break;
        default:
            // 无滤波，直接复制
            for (int i = 0; i < length; i++) {
                output[i] = input[i];
            }
            return;
    }
    
    // 假设stereo (2 channels), 处理交错样本
    int num_channels = wav_header.num_channels;
    
    for (int i = 0; i < length; i++) {
        if (num_channels == 1) {
            // 单声道处理
            delay_buffer_left[delay_index_left] = input[i];
            
            double filtered_sample = 0.0;
            for (int j = 0; j < FIR_TAP_NUM; j++) {
                int index = (delay_index_left - j + FIR_TAP_NUM) % FIR_TAP_NUM;
                filtered_sample += coeffs[j] * delay_buffer_left[index];
            }
            
            delay_index_left = (delay_index_left + 1) % FIR_TAP_NUM;
            
            // 限制输出范围防止溢出
            if (filtered_sample > 32767) filtered_sample = 32767;
            if (filtered_sample < -32768) filtered_sample = -32768;
            
            output[i] = (short)filtered_sample;
        } else {
            // 立体声处理 - 左右声道分别处理
            if (i % 2 == 0) {
                // 左声道
                delay_buffer_left[delay_index_left] = input[i];
                
                double filtered_sample = 0.0;
                for (int j = 0; j < FIR_TAP_NUM; j++) {
                    int index = (delay_index_left - j + FIR_TAP_NUM) % FIR_TAP_NUM;
                    filtered_sample += coeffs[j] * delay_buffer_left[index];
                }
                
                delay_index_left = (delay_index_left + 1) % FIR_TAP_NUM;
                
                // 限制输出范围防止溢出
                if (filtered_sample > 32767) filtered_sample = 32767;
                if (filtered_sample < -32768) filtered_sample = -32768;
                
                output[i] = (short)filtered_sample;
            } else {
                // 右声道
                delay_buffer_right[delay_index_right] = input[i];
                
                double filtered_sample = 0.0;
                for (int j = 0; j < FIR_TAP_NUM; j++) {
                    int index = (delay_index_right - j + FIR_TAP_NUM) % FIR_TAP_NUM;
                    filtered_sample += coeffs[j] * delay_buffer_right[index];
                }
                
                delay_index_right = (delay_index_right + 1) % FIR_TAP_NUM;
                
                // 限制输出范围防止溢出
                if (filtered_sample > 32767) filtered_sample = 32767;
                if (filtered_sample < -32768) filtered_sample = -32768;
                
                output[i] = (short)filtered_sample;
            }
        }
    }
}

// 播放控制功能
void toggle_pause() {
    if (current_state == PLAYING) {
        current_state = PAUSED;
        log_user_operation("PAUSE", "SUCCESS");
        printf("暂停播放\n");
    } else if (current_state == PAUSED) {
        current_state = PLAYING;
        log_user_operation("RESUME", "SUCCESS");
        printf("继续播放\n");
    }
}

void next_track() {
    if (playlist_count <= 1) {
        log_user_operation("NEXT_TRACK", "FAILED - No more tracks");
        printf("没有下一首歌曲\n");
        return;
    }
    
    current_track = (current_track + 1) % playlist_count;
    log_user_operation("NEXT_TRACK", "SUCCESS");
    printf("切换到下一首: %s\n", playlist[current_track]);
    
    // Set flag for main loop to handle the file change
    track_change_requested = true;
}

void previous_track() {
    if (playlist_count <= 1) {
        log_user_operation("PREVIOUS_TRACK", "FAILED - No more tracks");
        printf("没有上一首歌曲\n");
        return;
    }
    
    current_track = (current_track - 1 + playlist_count) % playlist_count;
    log_user_operation("PREVIOUS_TRACK", "SUCCESS");
    printf("切换到上一首: %s\n", playlist[current_track]);
    
    // Set flag for main loop to handle the file change
    track_change_requested = true;
}

void change_speed() {
    current_speed = (current_speed + 1) % 4;
    const char* speed_names[] = {"0.5x", "1.0x", "1.5x", "2.0x"};
    log_user_operation("CHANGE_SPEED", "SUCCESS");
    printf("播放速度切换为: %s\n", speed_names[current_speed]);
}

void seek_forward() {
    if (current_state == STOPPED) {
        log_user_operation("SEEK_FORWARD", "FAILED - Not playing");
        return;
    }
    
    long seek_frames = 10 * wav_header.sample_rate; // 10秒
    current_position += seek_frames;
    if (current_position >= total_frames) {
        current_position = total_frames - 1;
    }
    
    fseek(fp, data_chunk_offset + current_position * wav_header.block_align, SEEK_SET);
    log_user_operation("SEEK_FORWARD", "SUCCESS");
    printf("快进10秒\n");
}

void seek_backward() {
    if (current_state == STOPPED) {
        log_user_operation("SEEK_BACKWARD", "FAILED - Not playing");
        return;
    }
    
    long seek_frames = 10 * wav_header.sample_rate; // 10秒
    current_position -= seek_frames;
    if (current_position < 0) {
        current_position = 0;
    }
    
    fseek(fp, data_chunk_offset + current_position * wav_header.block_align, SEEK_SET);
    log_user_operation("SEEK_BACKWARD", "SUCCESS");
    printf("快退10秒\n");
}

void toggle_equalizer() {
    current_eq_mode = (current_eq_mode + 1) % 4;
    const char* eq_names[] = {"正常", "低音增强", "高音增强", "人声增强"};
    log_user_operation("TOGGLE_EQUALIZER", "SUCCESS");
    printf("均衡器模式: %s\n", eq_names[current_eq_mode]);
}

void print_status() {
    const char* state_names[] = {"播放中", "已暂停", "已停止"};
    const char* speed_names[] = {"0.5x", "1.0x", "1.5x", "2.0x"};
    const char* eq_names[] = {"正常", "低音增强", "高音增强", "人声增强"};
    
    printf("\n=== 播放状态 ===\n");
    if (playlist_count > 0) {
        printf("当前曲目: %d/%d - %s\n", current_track + 1, playlist_count, playlist[current_track]);
    }
    printf("播放状态: %s\n", state_names[current_state]);
    printf("播放速度: %s\n", speed_names[current_speed]);
    printf("均衡器: %s\n", eq_names[current_eq_mode]);
    if (total_frames > 0) {
        printf("进度: %ld/%ld (%.1f%%)\n", current_position, total_frames, 
               (double)current_position / total_frames * 100.0);
    }
    printf("==============\n\n");
}

void handle_user_input(char input) {
    switch(input) {
        case ' ': // 空格 - 暂停/继续
            toggle_pause();
            break;
        case 'n': // 下一首
            next_track();
            break;
        case 'p': // 上一首
            previous_track();
            break;
        case 's': // 切换速度
            if (current_state != STOPPED) {
                change_speed();
            } else {
                log_user_operation("CHANGE_SPEED", "FAILED - Not playing");
            }
            break;
        case 'f': // 快进
            if (current_state != STOPPED) {
                seek_forward();
            } else {
                log_user_operation("SEEK_FORWARD", "FAILED - Not playing");
            }
            break;
        case 'b': // 快退
            if (current_state != STOPPED) {
                seek_backward();
            } else {
                log_user_operation("SEEK_BACKWARD", "FAILED - Not playing");
            }
            break;
        case 'e': // 均衡器
            toggle_equalizer();
            break;
        case '+': // 增加音量
            increase_volume();
            break;
        case '-': // 减少音量
            decrease_volume();
            break;
        case 'i': // 信息
            print_status();
            break;
        case 'q': // 退出
            log_user_operation("QUIT", "SUCCESS");
            printf("退出程序\n");
            exit(0);
            break;
        case 'h': // 帮助
            printf("\n=== 操作帮助 ===\n");
            printf("空格: 暂停/继续\n");
            printf("n: 下一首\n");
            printf("p: 上一首\n");
            printf("s: 切换速度 (0.5x/1x/1.5x/2x)\n");
            printf("f: 快进10秒\n");
            printf("b: 快退10秒\n");
            printf("e: 切换均衡器模式\n");
            printf("+/-: 音量调节\n");
            printf("i: 显示状态信息\n");
            printf("h: 显示帮助\n");
            printf("q: 退出\n");
            printf("=================\n\n");
            break;
        default:
            break;
    }
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
    
    // 初始化日志
    log_program_info("STARTUP", "Music player starting up");
    
    // 初始化播放列表
    playlist_count = 0;
    current_track = 0;

    while ((opt_char = getopt(argc, argv, "m:f:r:d:")) != -1) {
        switch (opt_char) {
            case 'm':
                printf("打开文件: %s\n", optarg);
                // 添加到播放列表
                if (playlist_count < 10) {
                    strncpy(playlist[playlist_count], optarg, 255);
                    playlist[playlist_count][255] = '\0';
                    playlist_count++;
                }
                if (!file_opened) {
                    if (!open_music_file(optarg)) {
                        fprintf(stderr, "Failed to open music file: %s\n", optarg);
                        exit(EXIT_FAILURE);
                    }
                    file_opened = true;
                }
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

    if (!file_opened) {
        fprintf(stderr, "Error: Music file (-m option) is mandatory.\n");
        // Print usage here
        fprintf(stderr, "Usage: %s -m <music_filename.wav> [-f <format_code>] [-r <rate_code>]\n", argv[0]);
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
        fprintf(stderr, "Error: Critical parameters (rate or format) could not be determined.\n");
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
    
    // 确保不重复分配buff
    if (buff != NULL) {
        free(buff);
        buff = NULL;
    }
    
    buff = (unsigned char *)malloc(buffer_size);
    if (buff == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory for playback buffer.\n");
        exit(EXIT_FAILURE);
    }
    if (wav_header.block_align == 0) {
        fprintf(stderr, "Error: WAV header block_align is zero.\n");
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
        printf("Warning: Failed to initialize mixer. Volume control will not be available.\n");
    } else {
        printf("Volume control initialized. Use '+' to increase, '-' to decrease volume.\n");
        original_stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (original_stdin_flags != -1) {
            if (fcntl(STDIN_FILENO, F_SETFL, original_stdin_flags | O_NONBLOCK) == -1) {
                perror("fcntl F_SETFL O_NONBLOCK"); original_stdin_flags = -1;
            }
        } else { perror("fcntl F_GETFL"); }
    }

    printf("Starting playback...\n");
    printf("Press 'h' for help, 'q' to quit\n");
    
restart_playback:
    current_state = PLAYING;
    log_program_info("PLAYBACK", "Playback started");
    
    // 分配滤波后的缓冲区
    unsigned char *filtered_buff = (unsigned char *)malloc(buffer_size);
    if (filtered_buff == NULL) {
        log_program_info("ERROR", "Failed to allocate filtered buffer");
        if (buff) free(buff);
        exit(EXIT_FAILURE);
    }
    
    // 分配时间拉伸缓冲区 (allocate once, reuse for all iterations)
    short* temp_samples = (short*)malloc(buffer_size * 2); // 2x size for safety
    if (temp_samples == NULL) {
        log_program_info("ERROR", "Failed to allocate temp_samples buffer");
        if (filtered_buff) free(filtered_buff);
        if (buff) free(buff);
        exit(EXIT_FAILURE);
    }
    
    int read_ret;
    int speed_skip_counter = 0;
    
    while (1) {
        // 处理用户输入
        if (mixer_handle && original_stdin_flags != -1) {
            char c_in = -1;
            if (read(STDIN_FILENO, &c_in, 1) == 1) {
                handle_user_input(c_in);
                while(read(STDIN_FILENO, &c_in, 1) == 1 && c_in != '\n'); // Consume rest of line
            }
        }
        
        // 如果暂停，继续循环但不播放
        if (current_state == PAUSED) {
            usleep(50000); // 50ms
            continue;
        }
        
        // 如果停止，退出循环
        if (current_state == STOPPED) {
            break;
        }
        
        // 处理手动切换曲目请求
        if (track_change_requested) {
            track_change_requested = false;
            printf("DEBUG: Starting track change to: %s\n", playlist[current_track]);
            if (fp) {
                fclose(fp);
                fp = NULL;
            }
            printf("DEBUG: Closed previous file, opening new file\n");
            if (!open_music_file(playlist[current_track])) {
                printf("ERROR: Failed to open new track: %s\n", playlist[current_track]);
                current_state = STOPPED;
                break;
            }
            printf("DEBUG: Opened new file successfully\n");
            continue;
        }
        
        // 获取播放速度因子
        float speed_factor = 1.0f;
        switch(current_speed) {
            case SPEED_0_5X: speed_factor = 0.5f; break;
            case SPEED_1_0X: speed_factor = 1.0f; break;
            case SPEED_1_5X: speed_factor = 1.5f; break;
            case SPEED_2_0X: speed_factor = 2.0f; break;
        }
        
        read_ret = fread(buff, 1, buffer_size, fp);
        if (read_ret == 0) {
            log_program_info("PLAYBACK", "End of current track");
            // 设置自动切换标志
            if (playlist_count > 1) {
                auto_next_requested = true;
                break; // 退出循环，在安全位置处理切换
            } else {
                printf("End of music file input! (fread returned 0)\n");
                break;
            }
        }
        if (read_ret < 0) {
            log_program_info("ERROR", "Error reading PCM data from file");
            break;
        }
        
        current_position += read_ret / wav_header.block_align;
        
        snd_pcm_uframes_t frames_to_write = read_ret / wav_header.block_align;
        if (frames_to_write == 0 && read_ret > 0) {
             log_program_info("WARNING", "Partial frame data at end of file");
             break;
        }
        if (frames_to_write == 0) {
            break;
        }
        
        // 应用时间拉伸和均衡器滤波
        unsigned char *processing_buff = filtered_buff;
        int processing_length = read_ret;
        
        if (wav_header.bits_per_sample == 16) {
            short* input_samples = (short*)buff;
            int sample_count = read_ret / sizeof(short);
            short* output_samples = (short*)filtered_buff;
            
            // 首先应用时间拉伸（保持音调）
            int stretched_length = 0;
            if (speed_factor != 1.0f) {
                // Use the pre-allocated temp_samples buffer
                int max_output_samples = buffer_size / sizeof(short); // Use actual buffer size
                apply_time_stretch(input_samples, temp_samples, sample_count, &stretched_length, speed_factor, max_output_samples);
                processing_length = stretched_length * sizeof(short);
            } else {
                memcpy(temp_samples, input_samples, read_ret);
                stretched_length = sample_count;
            }
            
            // 然后应用均衡器滤波
            apply_fir_filter(temp_samples, output_samples, stretched_length, current_eq_mode);
            
            // 更新写入帧数为拉伸后的长度
            frames_to_write = stretched_length / wav_header.num_channels;
        } else {
            // 对于非16位样本，直接复制
            memcpy(filtered_buff, buff, read_ret);
        }
        
        snd_pcm_sframes_t frames_written_alsa;
        unsigned char *ptr = filtered_buff;
        snd_pcm_uframes_t remaining_frames = frames_to_write;

        while(remaining_frames > 0) {
            frames_written_alsa = snd_pcm_writei(pcm_handle, ptr, remaining_frames);
            if (frames_written_alsa < 0) {
                if (frames_written_alsa == -EPIPE) {
                    log_program_info("WARNING", "Audio underrun occurred, preparing interface");
                    snd_pcm_prepare(pcm_handle);
                } else {
                    char error_msg[LOG_BUFFER_SIZE];
                    snprintf(error_msg, sizeof(error_msg), "Error from snd_pcm_writei: %s", snd_strerror(frames_written_alsa));
                    log_program_info("ERROR", error_msg);
                    goto playback_end;
                }
            } else {
                ptr += frames_written_alsa * wav_header.block_align;
                remaining_frames -= frames_written_alsa;
            }
        }
        
        if (read_ret < buffer_size) {
            log_program_info("PLAYBACK", "End of music file (partial buffer read)");
            // 播放最后一块数据，下次循环read_ret==0时会触发切换
        }
    }
    
    // 处理自动切换到下一首
    if (auto_next_requested) {
        auto_next_requested = false;
        current_track = (current_track + 1) % playlist_count;
        log_user_operation("AUTO_NEXT_TRACK", "SUCCESS");
        printf("自动切换到下一首: %s\n", playlist[current_track]);
        
        fclose(fp);
        if (open_music_file(playlist[current_track])) {
            // 重新开始播放循环，buffers will be cleaned up automatically at the end
            goto restart_playback;
        } else {
            printf("ERROR: Failed to open next track: %s\n", playlist[current_track]);
        }
    }
    
    // 清理缓冲区（只在这里执行一次）
    printf("DEBUG: Starting buffer cleanup\n");
    if (filtered_buff) {
        printf("DEBUG: Freeing filtered_buff\n");
        free(filtered_buff);
        filtered_buff = NULL;
    }
    if (temp_samples) {
        printf("DEBUG: Freeing temp_samples\n");
        free(temp_samples);
        temp_samples = NULL;
    }
    printf("DEBUG: Buffer cleanup completed\n");

playback_end:
    current_state = STOPPED;
    log_program_info("PLAYBACK", "Playback finished or stopped");
    printf("Playback finished or stopped.\n");
    
    snd_pcm_drain(pcm_handle);
    if (mixer_handle && original_stdin_flags != -1) {
        if (fcntl(STDIN_FILENO, F_SETFL, original_stdin_flags) == -1) {
            log_program_info("WARNING", "Failed to restore stdin flags");
        }
    }
    
    if (fp) fclose(fp);
    snd_pcm_close(pcm_handle);
    if (mixer_handle) { snd_mixer_close(mixer_handle); }
    if (buff) {
        free(buff);
        buff = NULL;
    }
    if (pcm_name) {
        free(pcm_name);
        pcm_name = NULL;
    }
    
    if (log_file) {
        log_program_info("SHUTDOWN", "Music player shutting down");
        fclose(log_file);
    }
    
    return 0;
}