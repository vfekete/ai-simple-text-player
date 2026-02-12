# ANSI UTF-8 Audio Player (C++)

Terminal audio player for Linux with:
- UTF-8 button UI (bpytop-like compact panel)
- 20x10 spectrum bars refreshed at 100 ms
- Keyboard and mouse controls
- GStreamer playback
- FFmpeg probing (`libavformat`) for track metadata

## Requirements (Ubuntu 24.04 LTS)

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
  libncursesw5-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
  libavformat-dev libavcodec-dev libavutil-dev
```

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Run

```bash
./build/ansi_audio_player song1.mp3 song2.flac
```

## Controls

- `Space`: Play/Pause toggle
- `<`: Previous song
- `>`: Next song
- `s` or `S`: Stop
- Mouse left click on UTF-8 buttons: previous / play-pause / stop / next
- `q`: Quit

## Notes

- The spectrum uses GStreamer `spectrum` element FFT data.
- Track info/duration is probed with FFmpeg `libavformat`.
- Best in a UTF-8 true-color terminal (e.g. kitty, wezterm, alacritty, recent xterm).
