# MusicApp

A simple command-line music player application written in C that uses ALSA for audio playback.

## Features

*   Playback of WAV files.
*   Volume control.
*   Pause/Resume functionality.
*   Track navigation (next/previous).
*   Playback speed control (simple pitch-changing or WSOLA pitch-preserving).
*   Basic equalizer presets.

## Dependencies

*   ALSA library (`libasound2`)
*   C compiler (e.g., `gcc`)
*   Math library (`libm`)

## Compilation (Manual)

To compile the application manually (without Docker):

```bash
gcc -o MusicApp MusicApp.c const.h -lasound -lm -D_GNU_SOURCE
```

## Usage (Manual)

```bash
./MusicApp [options] <music_file1.wav> [music_file2.wav ...]
```

### Options:

*   `-f <format_code>`: Specify PCM format code.
    *   Supported codes: 1 (S16_LE), 2 (S16_BE), 3 (S24_LE), 4 (S24_BE), 5 (S24_3LE), 6 (S24_3BE), 7 (S32_LE), 8 (S32_BE)
*   `-r <rate_code>`: Specify sample rate code.
    *   Supported codes: 8 (8000Hz), 44 (44100Hz), 48 (48000Hz), 88 (88200Hz)
*   `-d <device_code>`: Specify output device (0 for default/board speaker, 1 for external).
*   `-s <speed_method>`: Specify speed control method (0 for simple seek-based, 1 for WSOLA).

### In-app Controls (while playing):

*   `+`: Increase volume
*   `-`: Decrease volume
*   `p`: Pause/Resume playback
*   `f`: Seek forward 10 seconds
*   `b`: Seek backward 10 seconds
*   `.`: Next track
*   `,`: Previous track
*   `[`: Decrease playback speed
*   `]`: Increase playback speed
*   `1`, `2`, `3`: Cycle through EQ presets (Normal, Bass Boost, Treble Boost)

## Running with Docker

This application can be run inside a Docker container. This is useful for ensuring a consistent runtime environment, especially for ALSA dependencies.

### Prerequisites

*   Docker installed and running.

### Building the Docker Image

1.  Ensure you have the `Dockerfile` (provided in the project) in the same directory as `MusicApp.c` and `const.h`.
2.  Open your terminal in the project directory and run:

    ```bash
    docker build -t musicapp-image .
    ```

### Running the Docker Container

To run the application using the Docker image:

```bash
docker run --rm -it \
    --device /dev/snd \
    -v "$(pwd)/your_music_directory:/app/music" \
    musicapp-image music/your_music_file.wav [music/another_file.wav ...] [options]
```

**Explanation of the `docker run` command:**

*   `--rm`: Automatically removes the container when it exits.
*   `-it`: Runs the container in interactive mode with a TTY, so you can use keyboard controls.
*   `--device /dev/snd`: **Crucial step.** This shares your host's sound device with the container, allowing ALSA to work.
*   `-v "$(pwd)/your_music_directory:/app/music"`: This is an example of mounting a local directory containing your music files into the container at `/app/music`.
    *   Replace `$(pwd)/your_music_directory` with the actual path to your music files on your host machine.
    *   Inside the container, you'll then refer to your music files as `music/your_music_file.wav`.
    *   If your music files are in the same directory as the `Dockerfile` and `MusicApp.c`, and you copied them into the image using `COPY music_files/ ./music_files/` in the Dockerfile, you might not need this volume mount, or you'd adjust the path accordingly.
*   `musicapp-image`: The name of the Docker image you built.
*   `music/your_music_file.wav ...`: Arguments passed to the `MusicApp` executable inside the container (paths to your music files, relative to the container's `/app` directory or the mounted volume).
*   `[options]`: Any of the command-line options supported by `MusicApp` (e.g., `-r 44`, `-s 1`).

**Important Considerations for Docker and ALSA:**

*   **Permissions**: The user inside the Docker container might need to be part of the `audio` group to access `/dev/snd`. The `ubuntu` base image's default user (`root`) should typically have access, but if you change the user in the Dockerfile, you might need to add it to the `audio` group (the GID of the `audio` group can vary, so this sometimes requires inspecting the host's GID for `audio` and matching it).
*   **PulseAudio**: If your host system uses PulseAudio, you might need additional configuration to route ALSA sound from the container through PulseAudio. This can involve mounting the PulseAudio socket (`/run/user/$(id -u)/pulse/native` or `/var/run/pulse/native`) and setting the `PULSE_SERVER` environment variable. The `--device /dev/snd` method is often simpler and works for many direct ALSA applications.
*   **Music Files**: Ensure your music files are accessible to the Docker container, either by `COPY`ing them into the image during the build or by mounting a volume at runtime using the `-v` flag as shown.

This setup should allow you to run your ALSA-based music application within a Docker container.

## Playback Speed Options

MusicApp provides two methods for controlling playback speed:

### 1. WSOLA (Waveform Similarity Overlap-Add) - Default

The WSOLA method changes playback speed without altering the pitch. This allows for:

* **Slow playback (0.5x)**: Useful for analyzing audio details or transcribing speech/music
* **Normal playback (1.0x)**: Standard speed
* **Fast playback (1.5x, 2.0x)**: Useful for quickly skimming through content

The WSOLA algorithm:
1. Analyzes the audio in short frames (30ms for optimal quality)
2. Finds the best matching segments using multi-resolution search and cross-correlation
3. Creates smooth, constant-power transitions between segments with 75% overlap
4. Preserves the original pitch and timbre, unlike simple sample rate changes
5. Maintains temporal continuity using intelligent segment selection and smoothing

Requirements for WSOLA:
* Currently supports 16-bit PCM mono files only (S16_LE format)
* Processing intensive - requires real-time buffer processing with floating-point accuracy

### 2. Simple Seek-Based Speed Control

The simple method (based on the Music_App.c implementation) changes playback speed by manipulating the file pointer position:

* **Slow playback (0.5x)**: Moves backward in the file, effectively repeating samples
* **Normal playback (1.0x)**: Standard playback without seeking
* **Fast playback (1.5x, 2.0x)**: Skips forward in the file, effectively dropping samples

This approach:
1. Is computationally simple and efficient (lower CPU usage)
2. Works with any audio format and number of channels
3. Changes both speed and pitch (similar to "chipmunk effect" at higher speeds)
4. May introduce some audio artifacts due to the simple implementation

To use the simple speed control method instead of WSOLA, use the `-s 0` command-line option.

## Known Issues / Future Work

*   **ALSA Re-initialization for Diverse Tracks**: When switching tracks in a playlist, if WAV files have different audio parameters (sample rate, channels, format), the ALSA re-initialization might not be robust enough. It currently relies on the initial setup logic. More dedicated ALSA reconfiguration might be needed in `load_track()`.
*   **Error Handling**: Further improvements to error handling and recovery.
*   **Advanced Logging**: While improved, log rotation or more configurable levels could be added.
*   **Code Structure**: For larger feature sets, refactoring into more functions/modules would be beneficial.
*   **FIR Filter Coefficients**: The current FIR filter coefficients for Bass Boost and Treble Boost in `const.h` are very basic examples and would need proper design using filter design tools for good audio quality. The current EQ implementation is primarily for S16_LE format audio.
*   **WSOLA Future Enhancements**: While the current implementation now produces high-quality audio at different speeds, it could be further extended with:
    * Stereo support (currently mono only)
    * Memory optimization for longer audio files
    * Support for additional audio formats beyond S16_LE
    * Variable frame length based on audio characteristics
    * GPU acceleration for correlation calculations on compatible systems
    * Adaptive search window sizing based on audio content

## Troubleshooting

*   **ALSA include errors (`alsa/asoundlib.h` not found):** Ensure `libasound2-dev` (or equivalent) is installed.
*   **Memory allocation failures:** If you see warnings about memory allocation for WSOLA buffers, try reducing the frame size in `const.h` or increase available memory.
*   **Poor audio quality with equalizer:** The current FIR filter coefficients are simple examples. For better audio quality, proper filter design would be needed.
*   **High CPU usage:** The WSOLA algorithm is computationally intensive, especially with larger frame sizes. Consider reducing the frame size or overlap percentage if CPU usage is too high.