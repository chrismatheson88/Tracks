# Tracks App Agent Guide

## Project Overview

Tracks is a high-performance, real-time desktop music library, set list manager, multi-channel backing track player, and metronome/guide audio application with multi-output routing, live key/tempo shifting, tempo automation, section cues, and seamless song-to-song transitions.

It is implemented as a native C++17 desktop application (`Tracks.app`) built with CMake, JUCE 8, SQLite3, and Signalsmith Stretch.

Primary entry points:

- `Source/Main.cpp` — JUCE application, macOS dock icon setup, and `DocumentWindow` entry point.
- `Source/MainComponent.cpp` — Attached color-tinted tab navigation (`Library`, `Live`, `Settings`), view switcher, and transport container (docked bar in Library/Settings, expanded full-height mixer panel in Live).
- `Source/Audio/AudioEngine.cpp` — Real-time JUCE audio engine with multi-channel stereo/mono routing, Signalsmith Stretch bus-grouped pitch/tempo shifting, `TempoMap` click automation, section skip/repeat, click-only loop mode, song transition crossfade tail, and post-mix peak metering.
- `Source/Database/DatabaseService.cpp` — SQLite3 persistence layer with atomic transactions, schema migrations, arrangement/section/tempo-point persistence, and cascading foreign key deletes.

## Project Structure

```
Tracks/
├── CMakeLists.txt                     # Root CMake project configuration (Release default, -O3 DSP)
├── .gitignore                         # Build, database, backup, and local cache ignore rules
├── AGENTS.md                          # Native C++ developer & agent guide
├── README.md                          # User documentation and build instructions
│
├── assets/                            # Audio packs & branding (core defaults embedded via BinaryData)
│   ├── clicks/
│   │   ├── tone/                      # Embedded default click pack (quarter, accent, eighth, sixteenth)
│   │   ├── Classic/                   # Additional click pack (M4A)
│   │   └── Blip/                      # Additional click pack (M4A)
│   ├── guide/
│   │   ├── female/                    # Embedded default count-in voice (count-1..8.wav)
│   │   └── playback_female/           # Section & count guide cues (M4A)
│   └── icon/
│       ├── TracksIcon.png             # App bundle & dock icon
│       └── TracksLogo.png             # In-app header logo (blue/purple/pink palette)
│
├── Source/                            # Application source code
│   ├── Main.cpp                       # App entry point & MainWindow
│   ├── MainComponent.h / .cpp         # Header tabs (Library/Live/Settings), routing, Relaunch button
│   ├── Common/
│   │   ├── AppTheme.h / .cpp          # LookAndFeel dark theme & blue/purple/pink tab/page tints
│   │   └── MacDockIcon.h / .mm        # macOS dock icon helper
│   ├── Models/
│   │   ├── AvailableSections.h        # Canonical section cue vocabulary
│   │   ├── Song.h, SongTrack.h        # Song, SongArrangement, SongSection, TempoPoint, SongTrack
│   │   └── Setlist.h, SetlistEntry.h  # Setlist, SetlistEntry, SongTransitionMode
│   ├── Database/
│   │   └── DatabaseService.h / .cpp   # SQLite3 engine, migrations, transactions, cascade deletes
│   ├── Audio/
│   │   ├── AudioEngine.h / .cpp       # Multi-channel player, StretchBus DSP, transitions, metering
│   │   ├── Metronome.h / .cpp         # Click subdivisions, compound meters, TempoMap, voice cues
│   │   ├── PitchShiftUtils.h          # Semitone math, +/-10 BPM limits, drum/FX pitch exclusions
│   │   ├── SectionJumpUtils.h         # Section spans, pre-play & live jumps, transition planning
│   │   ├── TempoMap.h                 # Closed-form piecewise constant & linear-ramp beat<->frame
│   │   └── MidiTempoImporter.h        # Standard MIDI File (.mid) tempo map importer
│   ├── Services/
│   │   ├── AudioAttachmentService.h / .cpp # WAV/MP3/M4A inspection, duration checks, storage copy
│   │   └── LibraryBackupService.h / .cpp   # Compressed .zip library backup & restore
│   ├── UI/
│   │   ├── Widgets/
│   │   │   ├── BeatPlayheadComponent.h / .cpp  # Waveform/section timeline, count-in, non-uniform bars
│   │   │   ├── CalendarPopupComponent.h / .cpp # Setlist date picker popup
│   │   │   ├── TimePopupComponent.h / .cpp     # Setlist time picker popup
│   │   │   ├── OutputMeterComponent.h / .cpp   # Multi-channel peak meters (30 FPS)
│   │   │   └── TransportComponent.h / .cpp     # Docked transport & Live expanded mixer/faders
│   │   └── Views/
│   │       ├── LibraryView.h / .cpp   # Auto-saving song library, search bar, arrangement/section/tempo editor
│   │       ├── OpenSetView.h / .cpp   # "Live" tab: compact set controls, setlist songs, searchable song adder
│   │       └── SettingsView.h / .cpp  # Audio device, click/voice packs, default volumes, .zip backup/restore
│   └── Tests/                         # Automated C++ test targets
│       ├── DatabaseTests.cpp
│       └── AudioEngineTests.cpp
│
├── ThirdParty/                        # External dependencies
│   ├── JUCE/                          # JUCE 8 framework (ignored by git; auto-fetched if absent)
│   └── signalsmith-stretch/           # Header-only polyphonic pitch/time-stretch library
│
├── build/                             # CMake build directory (ignored by git)
├── uploaded_tracks/                   # Attached user stems (ignored by git)
├── Backup/                            # Local backup archives (ignored by git)
└── tracks_library.sqlite3             # Local SQLite database (ignored by git)
```

## Building and Testing

### Prerequisites
- macOS with Xcode Command Line Tools (`clang++`, `make`)
- CMake 3.22+ (`.venv/bin/cmake` or system `cmake`)

### Build Commands

```bash
# Configure with CMake (defaults to Release; AudioEngine/Metronome compile with -O3)
.venv/bin/cmake -B build -S .

# Build Tracks application and tests
make -C build Tracks DatabaseTests AudioEngineTests -j 8

# Run C++ unit/integration tests
./build/DatabaseTests
./build/AudioEngineTests

# Sync Release bundle to the canonical launch/relaunch path
rm -rf build/Tracks_artefacts/Tracks.app && cp -R build/Tracks_artefacts/Release/Tracks.app build/Tracks_artefacts/Tracks.app && touch build/Tracks_artefacts/Tracks.app

# Launch the compiled macOS app
open build/Tracks_artefacts/Tracks.app
```

> **Note:** Because CMake defaults to `CMAKE_BUILD_TYPE=Release`, `Tracks.app` is built at `build/Tracks_artefacts/Release/Tracks.app`. Always copy it to `build/Tracks_artefacts/Tracks.app` after rebuilding so both `open build/Tracks_artefacts/Tracks.app` and the in-app **Relaunch** button pick up the new binary.

## Audio Assets

Bundled application assets live in `assets/`:

- **Click packs** (`assets/clicks/<pack>/`): `tone` (embedded default WAVs: `quarter`, `accent`, `eighth`, `sixteenth`), plus selectable packs such as `Classic` and `Blip` (M4A).
- **Guide voice packs** (`assets/guide/<pack>/`): `female` (embedded default `count-1.wav`–`count-8.wav`), plus `playback_female` (M4A count and spoken section cues like Intro, Verse, Chorus, Bridge, Ending).
- **Icons** (`assets/icon/`): `TracksIcon.png` and `TracksLogo.png` (embedded into `AudioBinaryData`).

Core defaults are embedded at compile time via `juce_add_binary_data` in `CMakeLists.txt`; additional packs under `assets/clicks/` and `assets/guide/` are discovered dynamically at runtime.

## Arrangements & Attached User Audio

Each song can have zero, one, or multiple **Arrangements** (`SongArrangement`), each with its own name, tempo, key, time signature, optional audio stems, **Track Sections**, **Tempo Map**, and remembered 8th/16th click subdivision settings.

- **Trackless arrangements** are first-class: an arrangement may have no audio folder (click/guide-only) while still carrying its own tempo/key/time signature, sections, spoken section cues, tempo automation, and song-end transitions.
- **Supported stem formats:** WAV, MP3, and M4A.
- **Storage layout:** Attached audio files are copied into:
  - `uploaded_tracks/<song-id>_<sanitized-title>/<arrangement-id>_<sanitized-name>/tracks/`
- **Important rules:**
  - Copy files only when an arrangement is confirmed/saved; browsing a folder must not copy or modify files.
  - Clearing audio tracks from an arrangement or removing an arrangement deletes its `uploaded_tracks/` subfolder via `AudioAttachmentService::removeArrangementStorage()`.
  - Deleting a song removes its entire `uploaded_tracks/<song-id>_*` directory via `AudioAttachmentService::removeSongStorage()`.
  - Unsupported or unreadable audio files must be reported and not attached.
  - Stem durations are checked across all files in an arrangement; mismatches display in full inside a multi-line scrollable warning box (one file per line) while remaining attachable.
  - Destructive actions (**Delete Song**, **Delete Set**, **Remove Arrangement**) require confirmation dialogs.

## Persistence & Database Conventions

SQLite database:

- Default library database: `tracks_library.sqlite3` in the project root (local state, untracked).
- SQLite connections enable `PRAGMA foreign_keys = ON;` so cascading deletes (`ON DELETE CASCADE`) are enforced.
- Database access is centralized in `Source/Database/DatabaseService.h/.cpp`.
- Core tables: `songs`, `arrangements`, `tracks`, `arrangement_sections`, `arrangement_tempo_points`, `setlists`, `setlist_items`, `app_settings`.
- Save methods (`saveSong`, `saveSetlist`) execute inside atomic SQLite transactions (`BEGIN TRANSACTION; ... COMMIT;`).
- `saveSong` upserts arrangements in place (preserving existing `arrangements.id` so `setlist_items.arrangement_id` stays intact) and recreates child `tracks`, `arrangement_sections`, and `arrangement_tempo_points`.
- `LibraryBackupService` exports/imports full `.zip` snapshots containing `tracks_library.sqlite3` and `uploaded_tracks/` (clearing cached audio before import and reloading UI state afterward).

## Audio Engine & Routing

- **Multi-channel routing:**
  - Backing tracks route to their configured `outputChannel` (0-indexed) and `stereoOutput` flag.
  - Stereo tracks route across `[outputChannel, outputChannel + 1]` when available; at channel boundaries or in mono mode, stereo audio folds down cleanly so right-channel content is never lost.
  - Mono tracks route directly to `outputChannel`.
  - Click, Guide, and per-track channel dropdowns live below each vertical fader on the **Live** screen (Click & Guide channel routing is persisted in `app_settings` and restored on startup/device change).
- **Live Pitch & Tempo Shifting:**
  - Powered by `Signalsmith Stretch`, grouped into `StretchBus` instances by `(outputChannel, routeAsStereo, pitchShiftExcluded)` with `splitComputation = true` for low CPU usage.
  - Key changes transpose melodic stems by the shortest interval (`[-6, +6]` semitones) while excluding drum, percussion, FX, click, and guide stems (`shouldExcludeFromPitchShift()` in `PitchShiftUtils.h`).
  - Live tempo changes on track-bearing arrangements are clamped to `±10 BPM` around the arrangement's recorded tempo (unclamped `20–300 BPM` in click-only or trackless arrangements).
- **Metronome, Tempo Map & Count-In:**
  - Clicks route to `clickChannel` with `clickGain`, plus optional independent 8th and 16th subdivision toggles/faders.
  - Spoken count-in and section guide cues route to `guideChannel` with `guideGain`.
  - Compound meters (`6/8`, `12/8`): tempo defines the dotted-quarter duration; beats subdivide into 3 eighth pulses with accents every 3 pulses.
  - 4/4 count-in pattern: `"one"`, `[rest]`, `"two"`, `[rest]`, `"one"`, `"two"`, `"three"`, `"four"`.
  - `TempoMap` supports step and linear-ramp tempo changes (manual or imported from `.mid` files via `MidiTempoImporter`) with proportional scaling when the live tempo is adjusted.
- **Section Navigation, Click Loop & Song Transitions:**
  - Structured timelines (track-bearing arrangements with `Use tracks` on, or trackless arrangements with sections) support pre-play start section selection (`◀◀`/`▶▶` with a 1-bar count-in), live queued section skip/repeat, and end-of-song transitions (`Stay in current song`, `Cue next song`, `Autoplay next song`, `Crossfade` 1–4 bars with cosine fade-out tail).
  - Pure click-only mode (no tracks and no sections, or `Use tracks` disabled) turns the Repeat button into a **Loop** toggle (active by default; deselecting stops cleanly at the end of the current bar).
- **Master output & metering:**
  - All output channels are scaled by `masterGain` and clamped to `[-0.95f, 0.95f]`.
  - Hardware output buffers are zero-filled before mixing to prevent stale-channel bleed.
  - Peak levels are tracked per channel with `0.82` decay for 30 FPS GUI metering.

## UI & LookAndFeel Conventions

- Uses JUCE 8 components and custom dark `AppTheme`.
- **Attached color-tinted tabs** derived from `TracksLogo.png`:
  - Recessed dark header (`#101014`) with attached top-rounded tabs for **Library** (blue `#3B82F6` / `#171B24`), **Live** (purple `#9D4EFF` / `#1D1826`), and **Settings** (pink `#EC2E8C` / `#231820`), seamlessly merging into subtly tinted page backgrounds.
- **Library view:**
  - Auto-selects the top song on open, includes a live multi-word search bar above the song list, and **auto-saves** all edits (immediate save for dropdowns, arrangements, sections, and tempo maps; 450ms debounced save + flush on blur/switch for text fields) with a `"Saving..."` / `"Saved"` status indicator.
  - **Add / Edit Arrangement** dialog combines audio folder selection (optional, with Clear), tempo/key/time sig, and tabbed **Track Sections** + **Tempo Map** (with MIDI import) editors.
- **Live view (`OpenSetView`):**
  - Left sidebar (25% width) stacks compact set controls (**Setlist** dropdown with small grey date/time subtitles in the popup list, **Date** & **Time** pickers, **New Set**, **Delete Set**) above the setlist song list, searchable **Add song to set...** dropdown, and reorder/remove buttons.
  - Right panel hosts the full-height expanded transport, transition dropdown + 1–4 bar crossfade stepper, waveform/section playhead, full-height vertical mixer faders (scroll-wheel disabled so scrolling doesn't nudge levels, with channel selectors below each fader), and output peak meters.
- **String encoding note:** Avoid raw UTF-8 punctuation (such as `—`) inside plain `const char*` literals passed to `juce::String`; use ASCII `-` or `juce::String::fromUTF8(...)` to prevent Latin-1 mojibake.

## Git Workflow

- Keep ignored: `build/`, `ThirdParty/JUCE/`, `*.sqlite3`, `uploaded_tracks/`, `Backup/`, `Tracks_Backup_*.zip`, `.DS_Store`.
- Before committing:
  ```bash
  git status
  git diff --stat
  ```
- Before pushing:
  ```bash
  make -C build DatabaseTests AudioEngineTests -j 8
  ./build/DatabaseTests
  ./build/AudioEngineTests
  ```
