# Base image
FROM alpine:latest

# Install dependencies
# build-base includes gcc, make, and other common build tools.
# alsa-lib-dev provides the ALSA development files.
# pulseaudio-dev for PulseAudio client libraries.
# alsa-plugins for the ALSA to PulseAudio plugin.
# alsa-plugins-pulse for the PulseAudio plugin for ALSA.
# pulseaudio-utils for pactl and other tools
RUN apk update && apk add --no-cache \
    build-base \
    alsa-lib-dev \
    pulseaudio-dev \
    alsa-plugins \
    alsa-plugins-pulse \
    pulseaudio-utils

# Verify installation of alsa-plugins-pulse
RUN apk -L info alsa-plugins-pulse || echo "alsa-plugins-pulse info: not found or empty"

# ALSA config for PulseAudio - server will be specified by PULSE_SERVER env var
RUN <<EOF > /etc/asound.conf
pcm.!default {
    type pulse
}
ctl.!default {
    type pulse
}
EOF



# Copy application source code (const.h, MusicApp.c, and any .wav files if you want them in the image)
COPY const.h .
COPY MusicApp.c .
# If you have music files in a specific directory, e.g., 'music_files', copy them too:
# COPY music_files/ ./music_files/

# Compile the application
# The -D_GNU_SOURCE is added as it's present in your MusicApp.c includes
# The -lm is for the math library
# Alpine uses musl libc, which might define _GNU_SOURCE differently or have some features
# available by default. It's generally safe to keep it if your code relies on GNU extensions.
RUN gcc -o MusicApp MusicApp.c -lasound -lm -D_GNU_SOURCE

# Set working directory
WORKDIR /app

CMD ["/MusicApp", "./test.wav"] 