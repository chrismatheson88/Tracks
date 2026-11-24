# Tracks App Agent Guide

## Project overview

Tracks is a high-performance, real-time desktop music library, set list manager, multi-channel backing track player, and metronome/count-in audio application with multi-output routing.

It is implemented as a native C++ desktop application (`Tracks.app`) built with CMake and JUCE 8.

Primary entry points:

- `Source/Main.cpp` — JUCE application and DocumentWindow entry point.
- `Source/MainComponent.cpp` — main navigation layout, view switcher, and docked transport bar.
- `Source/Audio/AudioEngine.cpp` — real-time JUCE audio engine with multi-channel stereo routing, mono downmix fallback, and post-mix peak metering.
- `Source/Database/DatabaseService.cpp` — SQLite3 persistence layer with transactions, migrations, and cascading foreign key deletes.

## Project Structure

```
Tracks/
├── CMakeLists.txt                     # Root CMake project configuration
├── .gitignore                         # Build, database, and local cache ignore rules
├── AGENTS.md                          # Native C++ developer & agent guide
├── README.md                          # User documentation and build instructions
│
├── assets/                            # Canonical audio resources (embedded via CMake BinaryData)
│   ├── clicks/
│   │   ├── tone_normal.wav
│   │   └── tone_accent.wav
│   └── countin/
│       └── female/
│           ├── one.wav ... eight.wav
│
├── Source/                            # Application source code
│   ├── Main.cpp                       # App entry point & MainWindow
│   ├── MainComponent.h / .cpp         # Root layout, navigation, and docked transport
│   ├── Common/
│   │   └── AppTheme.h / .cpp          # LookAndFeel styling (dark palette)
│   ├── Models/
│   │   ├── Song.h, SongTrack.h        # Domain structs
│   │   └── Setlist.h, SetlistEntry.h
│   ├── Database/
│   │   └── DatabaseService.h / .cpp   # SQLite3 engine, migrations, cascade deletes
│   ├── Audio/
│   │   ├── AudioEngine.h / .cpp       # Multi-channel engine, stereo routing, limiter
│   │   └── Metronome.h / .cpp         # Metronome timing, compound meters, speech count-in
│   ├── Services/
│   │   └── AudioAttachmentService.h / .cpp # WAV/MP3 inspection, storage copying
│   ├── UI/
│   │   ├── Widgets/
│   │   │   ├── BeatPlayheadComponent.h / .cpp
│   │   │   ├── OutputMeterComponent.h / .cpp
│   │   │   └── TransportComponent.h / .cpp
│   │   └── Views/
│   │       ├── LibraryView.h / .cpp
│   │       ├── OpenSetView.h / .cpp
│   │       └── SettingsView.h / .cpp
│   └── Tests/                         # Automated test targets
│       ├── DatabaseTests.cpp
│       └── AudioEngineTests.cpp
│
├── ThirdParty/                        # External dependencies
│   └── JUCE/                          # JUCE 8 framework clone/submodule
│
├── build/                             # CMake build directory (ignored by git)
├── uploaded_tracks/                   # Attached user stems (ignored by git)
└── tracks_library.sqlite3             # Local SQLite database (ignored by git)
```

## Building and Testing

### Prerequisites
- macOS with Xcode Command Line Tools (`clang++`, `make`)
- CMake 3.22+ (`.venv/bin/cmake` or system `cmake`)

### Build Commands

```bash
# Configure with CMake
.venv/bin/cmake -B build -S .

# Build Tracks application
.venv/bin/cmake --build build --target Tracks -j 8

# Build and run C++ tests
.venv/bin/cmake --build build --target DatabaseTests AudioEngineTests -j 8
./build/DatabaseTests
./build/AudioEngineTests

# Launch the compiled macOS app
open build/Tracks_artefacts/Tracks.app
```

## Audio Assets

Bundled application assets live in `assets/`:

- Click WAV files: `assets/clicks/` (`tone_normal.wav`, `tone_accent.wav`)
- Count-in voice WAV files: `assets/countin/female/` (`one.wav` through `eight.wav`)

These assets are embedded into `AudioBinaryData` at compile time via `juce_add_binary_data` in `CMakeLists.txt`, making the compiled `Tracks.app` binary self-contained.

## Attached User Audio

User-selected WAV/MP3 directories are copied upon Save into:

- `uploaded_tracks/<song-id>/tracks/`

The `<song-id>` is the SQLite song row ID.

Important rules:
- WAV and MP3 attachments belong to the saved song, not temporary UI state.
- Copy files only on Save for a song; browsing a directory must not silently copy or modify files.
- Clear attachment should mark the edit as pending dirty state, then remove persisted files/database tracks on Save.
- Cancel attachment changes must restore the selected song's existing track paths and file list.
- Deleting a song removes its `uploaded_tracks/<song-id>/` directory via `AudioAttachmentService::removeSongStorage()`.
- Unsupported or unreadable audio files must be reported and not attached.
- Audio lengths are checked across files in a song; mismatches warn but remain attachable.

## Persistence & Database Conventions

SQLite database:

- Default library database: `tracks_library.sqlite3` in the project root.
- Database files are local state and must remain untracked.
- SQLite connections must enable `PRAGMA foreign_keys = ON;` so cascading deletes (`ON DELETE CASCADE`) are enforced.
- Database access is centralized in `Source/Database/DatabaseService.h/.cpp`.
- Core tables: `songs`, `tracks`, `setlists`, `setlist_items`, `app_settings`.
- Save methods (`saveSong`, `saveSetlist`) execute inside atomic SQLite transactions (`BEGIN TRANSACTION; ... COMMIT;`).
- Track rows are recreated on `saveSong`; setlist items are recreated on `saveSetlist`.

## Audio Engine & Routing

- Multi-channel routing:
  - Backing tracks route to their configured `outputChannel` (0-indexed).
  - Stereo tracks route across `[outputChannel, outputChannel + 1]` when available.
  - Boundary fallback: if the interface runs out of channels at the boundary, stereo audio automatically folds down to mono `0.5 * (L + R)` on `outputChannel` so right-channel audio is never lost.
  - Mono tracks route directly to `outputChannel`.
- Metronome and count-in:
  - Metronome clicks route to `clickChannel` with `clickGain`.
  - Count-in speech samples route to `guideChannel` with `guideGain`.
  - Compound time signatures (6/8 and 12/8): tempo input defines the 3-eighth-note dotted quarter group duration. Beat frames are divided by 3, with accents every 3 pulses.
  - 4/4 count-in speech pattern: `"one"`, `[rest]`, `"two"`, `[rest]`, `"one"`, `"two"`, `"three"`, `"four"`.
- Master output:
  - All output channels are scaled by `masterGain` and soft-clipped to `[-0.95f, 0.95f]`.
  - Hardware output buffers are zero-filled before mixing to prevent stale-channel or bleed artifacts.
  - Peak levels are tracked per channel with an exponential decay factor of `0.82` for smooth 30 FPS GUI metering.

## UI & LookAndFeel Conventions

- Uses JUCE 8 components and dark theme (`AppTheme`).
- Dark charcoal palette: background `#18181b`, panel `#27272a`, border `#3f3f46`, primary blue `#3b82f6`, accent green `#22c55e`.
- Dirty-state behavior: "Save Edits to Library" button highlights blue when title, artist, tempo, key, time signature, notes, or stems change, and resets on Save, Cancel, or reselection.
- Setlist song overrides: tempo, key, and time signature can be overridden per setlist item. "Save Edits to Library" pushes the overridden parameters back to the master library song.
- Transport bar remains docked and visible across all views.

## Git Workflow

- Keep ignored: `build/`, `ThirdParty/JUCE/`, `*.sqlite3`, `uploaded_tracks/`, `.DS_Store`.
- Before committing:
  ```bash
  git status
  git diff --stat
  ```
- Before pushing:
  ```bash
  .venv/bin/cmake --build build --target DatabaseTests AudioEngineTests -j 8
  ./build/DatabaseTests
  ./build/AudioEngineTests
  ```
