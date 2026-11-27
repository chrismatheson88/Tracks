# Tracks App Agent Guide

## Project Overview

Tracks is a high-performance, real-time desktop music library, set list manager, multi-channel backing track player, and metronome/guide application with chunked disk streaming, automatic sample-rate resampling, multi-output routing, live key/tempo shifting, tempo automation, section cues, MIDI/keyboard control surface support, offline neural TTS song announcements, and seamless song-to-song transitions.

It is implemented as a native C++17 desktop application (`Tracks.app`) built with CMake, JUCE 8, SQLite3 (WAL mode), and Signalsmith Stretch.

Primary entry points:

- `Source/Main.cpp` — JUCE application, macOS dock icon setup, and maximised `DocumentWindow` entry point (`display->userArea`).
- `Source/MainComponent.cpp` — Attached color-tinted tab navigation (`Library`, `Live`, `Settings`), view switcher, keyboard/MIDI action dispatch, song announcement coordination, and transport container (docked bar in Library/Settings, expanded full-height mixer panel in Live).
- `Source/Audio/AudioEngine.cpp` — Real-time JUCE audio engine with chunked 16-bit disk streaming (`StreamingAudioTrack`), phase-locked sample-rate resampling, multi-channel stereo/mono routing, Signalsmith Stretch bus-grouped pitch/tempo shifting, `TempoMap` click automation, section skip/repeat/direct-jump, 4-second raised-cosine fade-out stop, click-only loop & bar-end restart (`ClickLoopController`), song transition crossfade tail, offline Kokoro-82M / macOS TTS guide announcements, and post-mix peak metering.
- `Source/Database/DatabaseService.cpp` — SQLite3 persistence layer (WAL mode + FK indexes + bulk queries) with atomic transactions, targeted in-place updates, schema migrations, arrangement/section/tempo-point persistence, portable backup snapshots, and cascading foreign key deletes.

## Project Structure

```
Tracks/
├── CMakeLists.txt                     # Root CMake project configuration (Release default, -O3 DSP)
├── .gitignore                         # Build, database, backup, TTS model/cache, and local ignore rules
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
│   ├── icon/
│   │   ├── TracksIcon.png             # App bundle & dock icon
│   │   └── TracksLogo.png             # In-app header logo (blue/purple/pink palette)
│   └── tts/                           # Optional local Kokoro-82M ONNX model & voice pack (ignored by git)
│
├── scripts/
│   └── kokoro_tts_worker.py           # Persistent offline Kokoro-82M TTS worker (line protocol, atomic WAV write)
│
├── Source/                            # Application source code
│   ├── Main.cpp                       # App entry point & maximised MainWindow
│   ├── MainComponent.h / .cpp         # Header tabs (Library/Live/Settings), action dispatch, routing, Relaunch
│   ├── Common/
│   │   ├── AppTheme.h / .cpp          # LookAndFeel dark theme, tab/page tints, arrowless ComboBox support
│   │   └── MacDockIcon.h / .mm        # macOS dock icon helper
│   ├── Models/
│   │   ├── AvailableSections.h        # Canonical section cue vocabulary
│   │   ├── MidiBinding.h              # ControlAction, MidiBinding (CC/Note/PC), KeyBinding serialization/matching
│   │   ├── Song.h, SongTrack.h        # Song, SongArrangement, SongSection, TempoPoint, SongTrack
│   │   └── Setlist.h, SetlistEntry.h  # Setlist, SetlistEntry (incl. justClick flag), SongTransitionMode
│   ├── Database/
│   │   └── DatabaseService.h / .cpp   # SQLite3 WAL engine, migrations, transactions, targeted updates, cascade deletes
│   ├── Audio/
│   │   ├── AudioEngine.h / .cpp       # Multi-channel engine, streaming, StretchBus DSP, fade-out, TTS, metering
│   │   ├── StreamingAudioTrack.h/.cpp # Chunked 16-bit PCM disk streamer & 4-point cubic Hermite resampler
│   │   ├── Metronome.h / .cpp         # Click subdivisions, 6/8 & 12/8 compound meters, TempoMap, voice cues
│   │   ├── PitchShiftUtils.h          # Semitone math, +/-10 BPM limits, drum/FX pitch exclusions
│   │   ├── SectionJumpUtils.h         # Section spans, direct/queued jumps, transitions, ClickLoopController
│   │   ├── TempoMap.h                 # Closed-form piecewise constant & linear-ramp beat<->frame
│   │   └── MidiTempoImporter.h        # Standard MIDI File (.mid) tempo map importer
│   ├── Services/
│   │   ├── AudioAttachmentService.h / .cpp # WAV/MP3/M4A inspection, duration checks, storage copy
│   │   ├── ControlSurfaceService.h / .cpp  # MIDI device input, MIDI Learn, keyboard shortcut persistence & matching
│   │   └── LibraryBackupService.h / .cpp   # Compressed .zip library backup & restore
│   ├── UI/
│   │   ├── Widgets/
│   │   │   ├── BeatPlayheadComponent.h / .cpp  # Clickable waveform/section timeline, count-in, restart/loop state
│   │   │   ├── CalendarPopupComponent.h / .cpp # Setlist date picker popup
│   │   │   ├── TimePopupComponent.h / .cpp     # Setlist time picker popup
│   │   │   ├── OutputMeterComponent.h / .cpp   # Multi-channel peak meters (30 FPS)
│   │   │   └── TransportComponent.h / .cpp     # Docked transport & Live expanded mixer/faders/buttons
│   │   └── Views/
│   │       ├── LibraryView.h / .cpp   # Auto-saving song library, search bar, arrangement/section/tempo editor
│   │       ├── OpenSetView.h / .cpp   # "Live" tab: set controls, setlist songs, Save-to-Library, searchable song adder
│   │       └── SettingsView.h / .cpp  # Audio device, click/voice/TTS packs, MIDI & keyboard shortcuts, .zip backup
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
├── tts_cache/                         # Cached synthesized TTS WAV files (ignored by git)
├── Backup/                            # Local backup archives (ignored by git)
└── tracks_library.sqlite3             # Local SQLite database + -wal/-shm sidecars (ignored by git)
```

## Building and Testing

### Prerequisites
- macOS with Xcode Command Line Tools (`clang++`, `make`)
- CMake 3.22+ (`.venv/bin/cmake` or system `cmake`)
- Optional for local neural TTS voices: `.venv/bin/python3` with `kokoro-onnx` and model files in `assets/tts/` (`kokoro-v1.0.int8.onnx` and `voices-v1.0.bin`); falls back automatically to macOS `say` if absent.

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

> **Notes:**
> - Because CMake defaults to `CMAKE_BUILD_TYPE=Release`, `Tracks.app` is built at `build/Tracks_artefacts/Release/Tracks.app`. Always copy it to `build/Tracks_artefacts/Tracks.app` after rebuilding so both `open build/Tracks_artefacts/Tracks.app` and the in-app **Relaunch** button pick up the new binary.
> - Release builds define `-DNDEBUG`, which strips `assert(...)` macros. Never place side-effecting code inside `assert()`, and prefer explicit runtime checks (e.g., `require(cond, msg)` throwing `std::runtime_error`) in tests.

## Audio Assets

Bundled application assets live in `assets/`:

- **Click packs** (`assets/clicks/<pack>/`): `tone` (embedded default WAVs: `quarter`, `accent`, `eighth`, `sixteenth`), plus selectable packs such as `Classic` and `Blip` (M4A).
- **Guide voice packs** (`assets/guide/<pack>/`): `female` (embedded default `count-1.wav`–`count-8.wav`), plus `playback_female` (M4A count and spoken section cues like Intro, Verse, Chorus, Bridge, Ending).
- **Icons** (`assets/icon/`): `TracksIcon.png` and `TracksLogo.png` (embedded into `AudioBinaryData`).
- **Neural TTS weights** (`assets/tts/`, untracked): optional Kokoro-82M ONNX model (`kokoro-v1.0.int8.onnx`) and voices (`voices-v1.0.bin`) powering offline Guide-channel song announcements.

Core defaults are embedded at compile time via `juce_add_binary_data` in `CMakeLists.txt`; additional packs under `assets/clicks/` and `assets/guide/` are discovered dynamically at runtime.

## Arrangements & Attached User Audio

Each song can have zero, one, or multiple **Arrangements** (`SongArrangement`), each with its own name, tempo, key, time signature, optional audio stems, **Track Sections**, **Tempo Map**, and remembered 8th/16th click subdivision settings.

- **Trackless arrangements** are first-class: an arrangement may have no audio folder (click/guide-only) while still carrying its own tempo/key/time signature, sections, spoken section cues, tempo automation, and song-end transitions.
- **"Just click" mode** is selectable per song on the **Live** screen (and persisted on `setlist_items.just_click`), ignoring arrangement audio/sections to run pure click-only loop mode with the song's base tempo/key/time signature.
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
- SQLite connections enable `PRAGMA foreign_keys = ON;`, `PRAGMA journal_mode = WAL;`, and `PRAGMA synchronous = NORMAL;` with indexes on child foreign keys and bulk-load queries for fast startup/refresh.
- Database access is centralized in `Source/Database/DatabaseService.h/.cpp`.
- Core tables: `songs`, `arrangements`, `tracks`, `song_sections`, `arrangement_tempo_points`, `setlists`, `setlist_items`, `app_settings`.
- Save methods (`saveSong`, `saveSetlist`) execute inside atomic SQLite transactions (`BEGIN TRANSACTION; ... COMMIT;`).
- `saveSong` upserts arrangements in place (preserving existing `arrangements.id` so `setlist_items.arrangement_id` stays intact) and recreates child `tracks`, `song_sections`, and `arrangement_tempo_points`.
- Targeted in-place updates (`updateSongBaseParams`, `updateArrangementBaseParams`, `updateSongClickSubdivisions`, `updateArrangementClickSubdivisions`, `updateTrackMix`) update columns directly without regenerating child rows or changing `tracks.id`.
- `LibraryBackupService` exports/imports full `.zip` snapshots containing `tracks_library.sqlite3` and `uploaded_tracks/` (`exportSnapshotToFile()` switches the snapshot to `PRAGMA journal_mode = DELETE;` so backups are self-contained and open cleanly with `SQLITE_OPEN_READONLY`).

## Audio Engine, Streaming & Routing

- **Chunked disk streaming & sample-rate synchronization (`StreamingAudioTrack`):**
  - Backing track stems stream on demand from disk in 16-bit PCM chunks (`65,536` frames/chunk) with Chunk 0 kept resident for instant start, background prefetch ahead of the playhead/queued section, and automatic eviction of distant chunks.
  - When a stem's native sample rate differs from the active output device's sample rate (e.g., 44.1 kHz WAV stems on 48 kHz AirPods, speakers, or audio interfaces), each chunk is resampled on the fly with phase-locked 4-point cubic Hermite interpolation (`fileSampleRate / targetSampleRate`) for zero drift and seamless chunk boundaries; matching rates take the zero-overhead direct path.
  - `AudioEngine::audioDeviceAboutToStart()` detects output device sample-rate changes, invalidates stale-rate song caches, and reloads click/guide packs and the active song at the new device rate.
- **Multi-channel routing:**
  - Backing tracks route to their configured `outputChannel` (0-indexed) and `stereoOutput` flag.
  - Stereo tracks route across `[outputChannel, outputChannel + 1]` when available; at channel boundaries or in mono mode, stereo audio folds down cleanly using adaptive correlation-aware mono summing so right-channel content is never lost.
  - Mono tracks route directly to `outputChannel`.
  - Click, Guide, and per-track channel selectors live below each vertical fader on the **Live** screen (Click & Guide channel routing is persisted in `app_settings` and restored on startup/device change).
- **Live Pitch & Tempo Shifting:**
  - Powered by `Signalsmith Stretch`, grouped into `StretchBus` instances by `(outputChannel, routeAsStereo, pitchShiftExcluded)` with `splitComputation = true` for low CPU usage (lazily allocated only when pitch != 0 or playback tempo != recorded tempo).
  - Key changes transpose melodic stems by the shortest interval (`[-6, +6]` semitones) while excluding drum, percussion, FX, click, and guide stems (`shouldExcludeFromPitchShift()` in `PitchShiftUtils.h`).
  - Live tempo changes on track-bearing arrangements are clamped to `±10 BPM` around the arrangement's recorded tempo (unclamped `20–300 BPM` in click-only or trackless arrangements).
- **Metronome, Tempo Map & Count-In:**
  - Clicks route to `clickChannel` with `clickGain`, plus optional independent 8th and 16th subdivision toggles/faders.
  - Spoken count-in and section guide cues route to `guideChannel` with `guideGain`.
  - Compound meters (`6/8`, `12/8`): tempo defines the dotted-quarter duration (3 eighth-note pulses per dotted quarter).
    - `6/8`: accents **beat 1 only** (`beatInBar == 0`); beat 4 plays the normal unaccented main click (even when 8th subdivisions are active, with beats 2/3/5/6 on the 8th click). 2-bar count-in pattern: `"one"`, `[rest]`, `[rest]`, `"two"`, `[rest]`, `[rest]`, `"one"`, `"two"`, `"three"`, `"four"`, `"five"`, `"six"`.
    - `12/8`: accents every 3 pulses (`beatInBar % 3 == 0`).
  - `4/4` 2-bar count-in pattern: `"one"`, `[rest]`, `"two"`, `[rest]`, `"one"`, `"two"`, `"three"`, `"four"`.
  - `TempoMap` supports step and linear-ramp tempo changes (manual or imported from `.mid` files via `MidiTempoImporter`) with proportional scaling when live tempo is adjusted.
- **Section Navigation, Fade-Out Stop, Click Loop & Back to Start:**
  - Structured timelines (track-bearing arrangements with tracks enabled, or trackless arrangements with sections) support pre-play start section selection (via `◀◀`/`▶▶` or clicking a section directly on the timeline, with a 1-bar count-in), live queued section skip/repeat/direct-click jump, and end-of-song transitions (`Stay in current song`, `Cue next song`, `Autoplay next song`, `Crossfade` 1–4 bars with cosine fade-out tail).
  - **Fade-Out Stop** button performs a smooth 4-second raised-cosine fade-out across all channels (clicking again while fading stops immediately).
  - Pure click-only / `"Just click"` mode turns the Repeat button into a **Loop** toggle (active by default; deselecting stops cleanly at the end of the current bar) and displays a **Back to Start** (`↺`) button to the left of Previous Song (`ClickLoopController` in `SectionJumpUtils.h`) that temporarily cancels the loop, jumps sample-accurately to Bar 0 Beat 1 at the end of the current bar to replay the count-in, and reinstates the loop afterward.
- **Guide-Channel Song Announcements (Offline Neural TTS):**
  - When enabled in Settings, selecting or transitioning to a new song announces its title on the Guide channel (suppressed on app startup and tab switches).
  - Uses a persistent local Kokoro-82M Python worker (`scripts/kokoro_tts_worker.py`) with 8 selectable voices, disk cache (`tts_cache/`), RMS loudness matching (`-20 dBFS` active RMS, `-6 dBFS` peak ceiling) to match `playback_female` guide cues, and automatic fallback to macOS `say`.
- **Master output & metering:**
  - All output channels are scaled by `masterGain` and clamped to `[-0.95f, 0.95f]`.
  - Hardware output buffers are zero-filled before mixing to prevent stale-channel bleed.
  - Peak levels are tracked per channel with `0.82` decay for 30 FPS GUI metering (adaptive 30 Hz active / 5 Hz idle timer).

## Control Surface (MIDI Learn & Keyboard Shortcuts)

- Centralized in `Source/Services/ControlSurfaceService.h/.cpp` and `Source/Models/MidiBinding.h`.
- Supports customizable **Keyboard Shortcuts** and **MIDI Learn** (Note On, CC, Program Change) for Play/Pause, Stop, Fade Out, Previous/Next Song, Skip Back/Forward Section, and Repeat/Loop, persisted in `app_settings`.
- Both keyboard and MIDI assignment boxes in `SettingsView` use a consistent click-to-assign box UI with an explicit **Cancel** button (so keys like `Esc` can be bound directly as shortcuts).

## UI & LookAndFeel Conventions

- Uses JUCE 8 components and custom dark `AppTheme`.
- Main window opens maximised to `display->userArea` (keeping macOS menu bar and Dock visible).
- **Attached color-tinted tabs** derived from `TracksLogo.png`:
  - Recessed dark header (`#101014`) with attached top-rounded tabs for **Library** (blue `#3B82F6` / `#171B24`), **Live** (purple `#9D4EFF` / `#1D1826`), and **Settings** (pink `#EC2E8C` / `#231820`), seamlessly merging into subtly tinted page backgrounds.
- **Library view:**
  - Auto-selects the top song on open, includes a live multi-word search bar above the song list, and **auto-saves** all edits (immediate save for dropdowns, arrangements, sections, and tempo maps; 450ms debounced save + flush on blur/switch for text fields) with a `"Saving..."` / `"Saved"` status indicator.
  - **Add / Edit Arrangement** dialog combines audio folder selection (optional, with Clear), tempo/key/time sig, and tabbed **Track Sections** + **Tempo Map** (with MIDI import) editors.
- **Live view (`OpenSetView`):**
  - Left sidebar (25% width) stacks compact set controls (**Setlist** dropdown with small grey date/time subtitles in the popup list, **Date** & **Time** pickers, **New Set**, **Delete Set**) above the setlist song list (orange `*` indicates setlist-specific BPM/Key/Time Signature overrides), searchable **Add song to set...** dropdown (appends at the bottom and scrolls into view without changing selection), and reorder/remove buttons.
  - Right panel hosts the song header with a top-right **Save changes to library** button (greyed out until live BPM/Key/Time Signature changes are made on `"Just click"` or trackless arrangements; persists directly to Library and clears orange `*` overrides), full-height expanded transport (with `"At end:"` heading above the transition dropdown + 1–4 bar crossfade stepper), clickable waveform/section playhead, full-height vertical mixer faders (scroll-wheel disabled, with arrowless centered channel selector buttons via `hideDropdownArrow` property below each fader), and output peak meters.
- **String encoding note:** Avoid raw UTF-8 punctuation (such as `—`) inside plain `const char*` literals passed to `juce::String`; use ASCII `-` or `juce::String::fromUTF8(...)` to prevent Latin-1 mojibake.

## Git Workflow

- Keep ignored: `build/`, `ThirdParty/JUCE/`, `*.sqlite3`, `*.sqlite3-wal`, `*.sqlite3-shm`, `*.sqlite3-journal`, `uploaded_tracks/`, `assets/tts/`, `tts_cache/`, `Backup/`, `Tracks_Backup_*.zip`, `.DS_Store`.
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
