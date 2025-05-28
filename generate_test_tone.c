#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>

// Generate a 1kHz test tone WAV file
int main() {
    FILE *fp = fopen("1khz_tone.wav", "wb");
    if (!fp) {
        printf("Failed to create file\n");
        return 1;
    }
    
    // WAV header
    const char *riff = "RIFF";
    const char *wave = "WAVE";
    const char *fmt = "fmt ";
    const char *data = "data";
    
    uint32_t sample_rate = 44100;
    uint16_t num_channels = 2;  // Stereo
    uint16_t bits_per_sample = 16;
    uint32_t duration_seconds = 5;
    uint32_t num_samples = sample_rate * duration_seconds;
    uint32_t data_size = num_samples * num_channels * (bits_per_sample / 8);
    uint32_t file_size = 36 + data_size;
    
    // Write RIFF header
    fwrite(riff, 1, 4, fp);
    fwrite(&file_size, 4, 1, fp);
    fwrite(wave, 1, 4, fp);
    
    // Write fmt chunk
    fwrite(fmt, 1, 4, fp);
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1;  // PCM
    uint32_t byte_rate = sample_rate * num_channels * bits_per_sample / 8;
    uint16_t block_align = num_channels * bits_per_sample / 8;
    
    fwrite(&fmt_size, 4, 1, fp);
    fwrite(&audio_format, 2, 1, fp);
    fwrite(&num_channels, 2, 1, fp);
    fwrite(&sample_rate, 4, 1, fp);
    fwrite(&byte_rate, 4, 1, fp);
    fwrite(&block_align, 2, 1, fp);
    fwrite(&bits_per_sample, 2, 1, fp);
    
    // Write data chunk
    fwrite(data, 1, 4, fp);
    fwrite(&data_size, 4, 1, fp);
    
    // Generate 1kHz sine wave
    double frequency = 1000.0;
    double amplitude = 0.5;
    
    for (uint32_t i = 0; i < num_samples; i++) {
        double t = (double)i / sample_rate;
        double value = amplitude * sin(2.0 * M_PI * frequency * t);
        int16_t sample = (int16_t)(value * 32767);
        
        // Write stereo sample (same for both channels)
        fwrite(&sample, 2, 1, fp);
        fwrite(&sample, 2, 1, fp);
    }
    
    fclose(fp);
    printf("Generated 1khz_tone.wav - 5 seconds, 44.1kHz, stereo, 1kHz sine wave\n");
    return 0;
}