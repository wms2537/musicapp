# MusicApp - A Simple WAV Player

This is a command-line WAV audio player for Linux using ALSA for playback and volume control.

## Features

*   Plays WAV audio files.
*   Volume control (+/- keys).
*   Pause/Resume playback ('p' key).
*   Seek forward 10 seconds ('f' key).
*   Seek backward 10 seconds ('b' key).
*   Playlist functionality: Plays multiple files specified on the command line.
*   Next track ('.' key).
*   Previous track (',' key).
*   Auto-plays next track in the playlist.
*   Playback speed selection and control ('[' to decrease, ']' to increase speed between 0.5x, 1.0x, 1.5x, 2.0x).
*   Selectable output device (on-board speaker vs external via -d option).
*   Enhanced logging to console (using `app_log` with timestamps and types) and to `music_app.log` file.
*   Audio Equalizer ( '1' for Normal, '2' for Bass Boost, '3' for Treble Boost).

## Compilation

To compile the application, you need the ALSA development libraries (e.g., `libasound2-dev` on Debian/Ubuntu).

```bash
gcc MusicApp.c -o MusicApp -lasound -lm
```

## Usage

```bash
./MusicApp [options] <music_file1.wav> [music_file2.wav ...]
```

**Options:**

*   `-f <format_code>`: Specify PCM format. (See source for codes, e.g., 161 for S16_LE).
    *   `161`: SND_PCM_FORMAT_S16_LE
    *   `162`: SND_PCM_FORMAT_S16_BE
    *   `241`: SND_PCM_FORMAT_S24_LE
    *   `242`: SND_PCM_FORMAT_S24_BE
    *   `2431`: SND_PCM_FORMAT_S24_3LE
    *   `2432`: SND_PCM_FORMAT_S24_3BE
    *   `321`: SND_PCM_FORMAT_S32_LE
    *   `322`: SND_PCM_FORMAT_S32_BE
*   `-r <rate_code>`: Specify sample rate. (e.g., 44 for 44100 Hz, 48 for 48000 Hz).
    *   `8`: 8000 Hz
    *   `44`: 44100 Hz
    *   `48`: 48000 Hz
    *   `88`: 88200 Hz
*   `-d <device_code>`: Specify output device.
    *   `0`: Default on-board speaker (may have limited max volume).
    *   `1`: External sound output device (assumes full volume range).

If format or rate are not specified, the program attempts to infer them from the WAV header.

**Interactive Controls (during playback):**

*   `+`: Increase volume.
*   `-`: Decrease volume.
*   `p`: Toggle pause/resume playback.
*   `f`: Seek forward 10 seconds.
*   `b`: Seek backward 10 seconds.
*   `.`: Play next track.
*   `,`: Play previous track.
*   `[`: Decrease playback speed (cycles through 0.5x, 1.0x, 1.5x, 2.0x).
*   `]`: Increase playback speed (cycles through 0.5x, 1.0x, 1.5x, 2.0x).
*   `1`: Set Equalizer to Normal (Flat)
*   `2`: Set Equalizer to Bass Boost (Note: Coefficients are illustrative)
*   `3`: Set Equalizer to Treble Boost (Note: Coefficients are illustrative)

## Dependencies

*   ALSA library (`libasound`)
*   Math library (`libm` for `sqrt`, `pow` - though not heavily used currently)

## Known Issues / Future Work

*   **ALSA Re-initialization for Diverse Tracks**: When switching tracks in a playlist, if WAV files have different audio parameters (sample rate, channels, format), the ALSA re-initialization might not be robust enough. It currently relies on the initial setup logic. More dedicated ALSA reconfiguration might be needed in `load_track()`.
*   **Error Handling**: Further improvements to error handling and recovery.
*   **Advanced Logging**: While improved, log rotation or more configurable levels could be added.
*   **Code Structure**: For larger feature sets, refactoring into more functions/modules would be beneficial.
*   **FIR Filter Coefficients**: The current FIR filter coefficients for Bass Boost and Treble Boost in `const.h` are very basic examples and would need proper design using filter design tools for good audio quality. The current EQ implementation is primarily for S16_LE format audio.

## Troubleshooting

*   **ALSA include errors (`alsa/asoundlib.h` not found):** Ensure `libasound2-dev` (or equivalent for your distribution) is installed and your compiler can find the ALSA headers. You might need to add `-I/usr/include/alsa` (or the correct path for your system) to your CFLAGS if they are in a non-standard location for your compiler.
*   **No sound / ALSA errors:** Check `music_app.log` for specific ALSA error messages. Ensure the correct sound card and mixer control are being used (`sound_card_name` and `mixer_control_name` in `MusicApp.c`). Try with a known good WAV file and simple parameters first.
*   **Volume control issues:** The application tries common mixer controls like "PCM" and "Master". If these don't work on your system, you may need to identify the correct mixer control name for your sound card (e.g., using `alsamixer`) and update `mixer_control_name` in `MusicApp.c`.
