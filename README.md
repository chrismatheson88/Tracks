# Tracks

Tracks is a high-performance, real-time desktop music library, set list manager, multi-channel backing track player, and metronome/count-in application built with native C++ and JUCE 8.

## Features

- **Music Library**: Store songs with title, artist, tempo, key, time signature, and notes.
- **Multi-Track Backing Player**: Attach multiple WAV or MP3 stem files to each song with per-track gain, channel routing, and mute controls.
- **Smart Stereo Routing**: Stereo tracks route across `[channel, channel + 1]` if available, with automatic mono downmix fallback at the hardware channel boundary.
- **Set List Management**: Create and organize set lists, reorder songs on stage, and apply per-set overrides (tempo, key, time signature) with optional "Save Edits to Library" synchronization.
- **Real-Time Metronome & Count-In**:
  - 4/4 standard time with a 2-bar spoken voice count-in (`"one"`, `[rest]`, `"two"`, `[rest]`, `"one"`, `"two"`, `"three"`, `"four"`).
  - 6/8 and 12/8 compound meters with dotted-quarter tempo spacing and accent pulses.
  - Dedicated hardware routing channels and volume controls for Click and Guide.
- **Live Output Peak Metering**: Real-time multi-channel LED level meters with smooth exponential decay.
- **Self-Contained & Offline**: Embedded audio assets (clicks and female count-in voice) and local SQLite3 database.

## Quick Start (macOS)

### Prerequisites
- macOS with Xcode Command Line Tools installed (`clang++`, `make`)
- CMake 3.22+

### Build and Launch

```bash
# Configure with CMake
.venv/bin/cmake -B build -S .

# Build the Tracks application
.venv/bin/cmake --build build --target Tracks -j 8

# Run automated tests
.venv/bin/cmake --build build --target DatabaseTests AudioEngineTests -j 8
./build/DatabaseTests
./build/AudioEngineTests

# Launch Tracks
open build/Tracks_artefacts/Tracks.app
```

## Project Structure

- `Source/` — C++ application code (Models, Database, Audio Engine, Metronome, UI Views, Widgets).
- `assets/` — Embedded click WAVs and voice count-in WAVs.
- `CMakeLists.txt` — CMake build configuration linking JUCE 8 and native SQLite3.
