# Tracks

Tracks is a high-performance, real-time desktop music library, set list manager, multi-channel backing track player, and metronome/guide application built with native C++17 and JUCE 8.

## Features

- **Auto-Saving Music Library**:
  - Manage songs with title, artist, tempo, key, time signature, and notes — edits auto-save automatically with no manual Save button needed.
  - Instant multi-word search bar above the song list, auto-selection of the top song on open, and confirmation prompts before deleting songs or removing arrangements.
- **Multiple Arrangements (With or Without Audio Stems)**:
  - Attach multiple WAV, MP3, or M4A stem folders per song as named arrangements (e.g., *Full Band*, *Acoustic*, *Short Ending*), or create **click/guide-only arrangements** with no audio tracks.
  - Unified **Add / Edit Arrangement** editor with **Track Sections** (spoken section cues and bar lengths) and **Tempo Map** automation (step & linear-ramp tempo changes plus Standard MIDI `.mid` tempo map import).
  - Full multi-line duration mismatch warnings when audio stem lengths differ.
- **Live Performance View (`Live`)**:
  - Compact left-hand setlist sidebar with set picker (showing set dates & times as muted subtitles in the dropdown), date/time pickers, searchable **Add song to set...** dropdown, and song reordering.
  - **Real-Time Pitch & Tempo Shifting**: Transpose keys live (`±6` semitones via Signalsmith Stretch, automatically skipping drums, percussion, FX, click, and guide stems) and adjust tempo live (`±10 BPM` on tracks, `20–300 BPM` click-only).
  - **Section Navigation & Click Loop**: Select a starting section before pressing Play (with a 1-bar count-in), queue live section skips or repeats (`◀◀` / `↻` / `▶▶`), or toggle click-only **Loop** mode to end cleanly at the end of the current bar.
  - **Song-to-Song Transitions**: Per-song end transition dropdown (**Stay in current song**, **Cue next song**, **Autoplay next song**, or **Crossfade** with a 1–4 bar up/down stepper and smooth cosine fade-out tail).
  - **Full-Height Live Mixer & Metering**: Vertical Master, Click, optional 8th/16th subdivision clicks, Guide, and per-track stem faders (mouse scroll-wheel disabled to prevent accidental bumps), per-strip output channel selectors below each fader (stereo pairs with automatic mono boundary fallback), and 30 FPS multi-channel output peak meters.
- **Metronome, Click Packs & Guide Voices**:
  - Standard and compound meters (`4/4`, `3/4`, `6/8`, `12/8`, `2/4`, `5/4`, `7/8`) with 2-bar spoken count-ins and section guide cues.
  - Selectable click sound packs (`tone`, `Classic`, `Blip`) and guide voice packs (`female`, `playback_female`).
- **Library Backup & Restore (`.zip`)**:
  - Export your entire library (SQLite database, songs, arrangements, stems, and setlists) to a single compressed `.zip` backup and restore from **Settings**.
- **Self-Contained & Offline**:
  - Default click/voice audio and icons are embedded into `Tracks.app`, backed by a local SQLite3 database.

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
  - `Audio/` — Real-time multi-channel engine (`AudioEngine`), `Metronome`, `TempoMap`, `MidiTempoImporter`, `PitchShiftUtils`, and `SectionJumpUtils`.
  - `Database/` — SQLite3 persistence layer (`DatabaseService`) with migrations and atomic transactions.
  - `Models/` — Domain structs (`Song`, `SongArrangement`, `SongSection`, `TempoPoint`, `SongTrack`, `Setlist`, `SetlistEntry`).
  - `Services/` — Audio file inspection/storage (`AudioAttachmentService`) and `.zip` backup/restore (`LibraryBackupService`).
  - `UI/` — Views (`LibraryView`, `OpenSetView`, `SettingsView`) and widgets (`TransportComponent`, `BeatPlayheadComponent`, `OutputMeterComponent`, `CalendarPopupComponent`, `TimePopupComponent`).
  - `Common/` — Dark theme & attached tab styling (`AppTheme`) and macOS dock icon helper.
  - `Tests/` — Automated C++ test suites (`DatabaseTests`, `AudioEngineTests`).
- `assets/` — Click packs, guide/count-in voice packs, and app icons/logo.
- `ThirdParty/` — JUCE 8 (auto-fetched if not present) and Signalsmith Stretch.
- `CMakeLists.txt` — CMake build configuration linking JUCE 8, BinaryData, Signalsmith Stretch, and SQLite3.
