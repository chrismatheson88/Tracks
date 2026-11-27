# Tracks

Tracks is a high-performance, real-time desktop music library, set list manager, multi-channel backing track player, and metronome/guide application built with native C++17 and JUCE 8.

## Features

- **Auto-Saving Music Library**:
  - Manage songs with title, artist, tempo, key, time signature, and notes — edits auto-save automatically with no manual Save button needed.
  - Instant multi-word search bar above the song list, auto-selection of the top song on open, and confirmation prompts before deleting songs or removing arrangements.
- **Multiple Arrangements (Tracks, Trackless, or "Just click")**:
  - Attach multiple WAV, MP3, or M4A stem folders per song as named arrangements (e.g., *Full Band*, *Acoustic*, *Short Ending*), create **click/guide-only arrangements** with sections and tempo maps, or switch any song to **"Just click"** mode directly on the Live screen.
  - Unified **Add / Edit Arrangement** editor with **Track Sections** (spoken section cues and bar lengths) and **Tempo Map** automation (step & linear-ramp tempo changes plus Standard MIDI `.mid` tempo map import).
  - Full multi-line duration mismatch warnings when audio stem lengths differ.
- **Low-Memory Disk Streaming & Automatic Sample-Rate Sync**:
  - Stems stream in chunks from disk with background prefetch and automatic cache eviction to keep RAM and CPU usage low.
  - Phase-locked real-time sample-rate resampling ensures 44.1 kHz stems stay 100% locked to the click on 48 kHz devices (such as AirPods, MacBook speakers, and 48 kHz audio interfaces), automatically reloading when switching output devices.
- **Live Performance View (`Live`)**:
  - Opens maximised with a compact left-hand setlist sidebar: set picker (with set dates & times as muted subtitles), date/time pickers, searchable **Add song to set...** dropdown (appends at the bottom without interrupting your current song), and song reordering.
  - **Setlist Overrides & Save to Library**: Live BPM, Key, and Time Signature changes highlight in amber with `*` in the setlist; click **Save changes to library** in the top-right to commit click/trackless edits back to your Library defaults.
  - **Real-Time Pitch & Tempo Shifting**: Transpose keys live (`±6` semitones via Signalsmith Stretch, automatically excluding drums, percussion, FX, click, and guide stems) and adjust tempo live (`±10 BPM` on tracks, `20–300 BPM` click-only).
  - **Direct Section Navigation, Fade-Out Stop & Click Restart**:
    - Click any section directly on the timeline (or use `◀◀` / `↻` / `▶▶`) to pre-select a starting section with a 1-bar count-in or queue a seamless live jump/repeat.
    - Dedicated **Fade-Out Stop** button performs a smooth 4-second raised-cosine fade-out (click again to stop immediately).
    - In **"Just click"** mode, toggle **Loop** to end cleanly at the end of the current bar, or press **Back to Start** (`↺`, left of Previous Song) to temporarily cancel the loop, restart the full count-in at the end of the bar, and reinstate looping afterward.
  - **Song-to-Song Transitions**: Per-song **At end:** transition dropdown (**Stay in current song**, **Cue next song**, **Autoplay next song**, or **Crossfade** with a 1–4 bar stepper and smooth cosine fade-out tail).
  - **Full-Height Live Mixer & Metering**: Vertical Master, Click, optional 8th/16th subdivision clicks, Guide, and per-track stem faders (scroll-wheel disabled to prevent accidental bumps), compact click-to-open channel routing buttons below each fader (stereo pairs with correlation-aware mono fallback), and 30 FPS multi-channel output peak meters.
- **Metronome, Click Packs & Guide Voices**:
  - Standard and compound meters (`4/4`, `3/4`, `6/8`, `12/8`, `2/4`, `5/4`, `7/8`) with spoken count-ins (`1,-,2,-,1,2,3,4` in 4/4; single beat-1 accent and `1,-,-,2,-,-,1,2,3,4,5,6` count-in in 6/8) and spoken section guide cues.
  - Selectable click sound packs (`tone`, `Classic`, `Blip`) and guide voice packs (`female`, `playback_female`).
  - **Offline Neural Song Announcements**: Optional Guide-channel text-to-speech announces the song name when you select or transition to a song, powered by local Kokoro-82M neural voices (loudness-matched to guide cues, with voice picker & Test button in Settings and automatic macOS system voice fallback).
- **MIDI Learn & Keyboard Shortcuts**:
  - Bind external MIDI controllers (Note, CC, Program Change) and customizable keyboard shortcuts in **Settings** for hands-free live transport and section control.
- **Library Backup & Restore (`.zip`)**:
  - Export your entire library (SQLite database, songs, arrangements, stems, and setlists) to a single portable `.zip` backup and restore from **Settings**.
- **Self-Contained & Offline**:
  - Default click/voice audio and icons are embedded into `Tracks.app`, backed by a local WAL-mode SQLite3 database.

## Quick Start (macOS)

### Prerequisites
- macOS with Xcode Command Line Tools installed (`clang++`, `make`)
- CMake 3.22+ (`.venv/bin/cmake` or system `cmake`)

### Build and Launch

```bash
# Configure with CMake (defaults to Release with -O3 DSP optimizations)
.venv/bin/cmake -B build -S .

# Build the Tracks application and automated tests
make -C build Tracks DatabaseTests AudioEngineTests -j 8

# Run automated tests
./build/DatabaseTests
./build/AudioEngineTests

# Sync the Release app bundle to the standard launch/Relaunch path
rm -rf build/Tracks_artefacts/Tracks.app && cp -R build/Tracks_artefacts/Release/Tracks.app build/Tracks_artefacts/Tracks.app && touch build/Tracks_artefacts/Tracks.app

# Launch Tracks
open build/Tracks_artefacts/Tracks.app
```

## Project Structure

- `Source/` — Native C++17 application code:
  - `Audio/` — Real-time multi-channel engine (`AudioEngine`), chunked disk streamer & resampler (`StreamingAudioTrack`), `Metronome`, `TempoMap`, `MidiTempoImporter`, `PitchShiftUtils`, and `SectionJumpUtils` (section jumps, transitions, and `ClickLoopController`).
  - `Database/` — SQLite3 WAL persistence layer (`DatabaseService`) with migrations, bulk queries, targeted updates, and atomic transactions.
  - `Models/` — Domain structs (`Song`, `SongArrangement`, `SongSection`, `TempoPoint`, `SongTrack`, `Setlist`, `SetlistEntry`, `MidiBinding`, `AvailableSections`).
  - `Services/` — Audio file inspection/storage (`AudioAttachmentService`), MIDI/keyboard control surface (`ControlSurfaceService`), and `.zip` backup/restore (`LibraryBackupService`).
  - `UI/` — Views (`LibraryView`, `OpenSetView`, `SettingsView`) and widgets (`TransportComponent`, `BeatPlayheadComponent`, `OutputMeterComponent`, `CalendarPopupComponent`, `TimePopupComponent`).
  - `Common/` — Dark theme & attached tab styling (`AppTheme`) and macOS dock icon helper.
  - `Tests/` — Automated C++ test suites (`DatabaseTests`, `AudioEngineTests`).
- `scripts/` — Local helper scripts (`kokoro_tts_worker.py` for offline neural TTS synthesis).
- `assets/` — Click packs, guide/count-in voice packs, and app icons/logo.
- `ThirdParty/` — JUCE 8 (auto-fetched if not present) and Signalsmith Stretch.
- `CMakeLists.txt` — CMake build configuration linking JUCE 8, BinaryData, Signalsmith Stretch, and SQLite3.
