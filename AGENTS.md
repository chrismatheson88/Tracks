# Tracks App Agent Guide

## Project overview

Tracks is a PySide6 desktop application for managing a music library, set lists, per-set song overrides, playback, metronome/count-in audio, and multi-output routing.

Primary entry points:

- `tracks_app/__main__.py` — launches the desktop application.
- `tracks_app/desktop.py` — window orchestration, Qt state, and compatibility methods. Keep this file as the smallest practical composition layer.
- `tracks_app/playback.py` — audio loading, mixing, metronome, count-in, and output callback behavior.
- `tracks_app/db.py` — SQLite schema, migrations, and persistence.
- `tracks_app/models.py` — `Song`, `SongTrack`, `Setlist`, and `SetlistEntry` data models.

## UI structure

View builders live in:

- `tracks_app/views/library_view.py`
- `tracks_app/views/open_set_view.py`
- `tracks_app/views/settings_views.py`

Reusable widgets live in:

- `tracks_app/widgets/beat_playhead.py`
- `tracks_app/widgets/output_meter.py`
- `tracks_app/widgets/transport.py`

Shared domain logic lives in:

- `tracks_app/services/set_logic.py`
- `tracks_app/services/audio_controls.py`
- `tracks_app/services/audio_attachment_service.py`
- `tracks_app/services/asset_import_service.py`

When changing UI behavior, prefer the relevant view/mixin module rather than adding large implementations to `desktop.py`.

## Desktop structure rules

- Do not let `tracks_app/desktop.py` become a monolithic file again.
- Target roughly 500 lines or less for `desktop.py`; it should focus on window setup, state, navigation, and compatibility delegation.
- Put reusable Qt widget construction in `tracks_app/views/` or `tracks_app/widgets/`.
- Put database-independent calculations in `tracks_app/services/`.
- Preserve existing public method names initially when refactoring, so existing signals and tests continue to work.
- Move complete methods in one bounded operation and validate immediately; never leave an otherwise invalid truncated file.
- Avoid replacing an entire file from generated or local-history snapshots. Prefer a diff-based edit or restoring from Git only when corruption is confirmed.
- After any large edit, check line count, compile, and run tests before adding another change.

## Audio assets

Bundled application assets:

- Click WAV files: `tracks_app/assets/clicks/`
- Count-in voice WAV files: `tracks_app/assets/countin/<voice>/`

These bundled assets are app code/resources and may be committed to Git when appropriate.

## Attached user audio

User-selected WAV/MP3 directories are not treated as permanent source-directory references only. The app copies accepted files into:

- `tracks_app/uploaded_tracks/<song-id>/tracks/`

The `<song-id>` is the SQLite song row ID. It should be treated as a storage identifier, not as user-visible content.

Important rules:

- WAV and MP3 attachments belong to the saved song, not the global library selection or temporary UI state.
- Copy files only on Save for a song; browsing a directory must not silently copy or modify files.
- Clear attachment should mark the edit as pending dirty state, then remove persisted files/database tracks on Save.
- Cancel attachment changes must restore the selected song's existing track paths and file list.
- Switching away from or back to Library must reload attachment filenames from the selected song's tracks.
- Do not display source absolute directory paths in the Edit Song UI; show attached filenames instead.
- Unsupported or unreadable audio files must be reported and not attached.
- All attached files in a song should be checked for equal length/duration; mismatches may warn but should remain attachable if readable.
- Report mono/stereo/multi-channel status during import separately from length mismatches.
- Preserve source files; copying must not move or delete the user's original directory files.
- Deleting a song should remove or orphan-clean its `uploaded_tracks/<song-id>/tracks/` directory.

## Persistence

SQLite database:

- Default library database: `tracks_library.sqlite3`
- Database files are local state and must remain untracked.
- Do not commit local song data or local absolute audio paths.
- Schema changes should use migrations in `ensure_schema`/database initialization code.
- Song attachment tracks are persisted through `save_song`; track rows are recreated from `song.tracks`.

## Dirty-state behavior

For Edit Song:

- Capture a baseline snapshot after selecting a song.
- Mark `Save edits to library` as dirty/blue on title, artist, tempo, key, time-signature, notes, or attached-track changes.
- Revert dirty state on Save or Cancel.
- Recreate or reset the baseline after switching songs.
- Attachments pending in `edit_pending_tracks` must count as dirty and must not leak to another selected song.

## Audio playback

- Backing tracks route to their configured output channels.
- Metronome and count-in have separate Click and Guide channels.
- Master gain applies after tracks/metronome mixing.
- Callback buffers must be zero-filled before writing to prevent stale-channel or bleed artifacts.
- Output channel count must match the active stream/device channel count.
- Never infer channel routing from UI text; use explicit state and channel indexes.

## Git and local files

Keep ignored:

- `.venv/`
- `.pytest_cache/`
- `__pycache__/`
- `*.sqlite`, `*.sqlite3`, `*.db`
- `uploaded_tracks/`
- `.DS_Store`, editor/IDE folders, `.env` files

Before staging, inspect status and ensure no virtual environment, database, cache, or copied user audio is accidentally added.

## Validation workflow

After any code change:

1. Run `python3 -m compileall -q tracks_app tests`.
2. Run `python3 -m pytest -q`.
3. Smoke-test desktop window construction/offscreen startup when UI code changes.
4. Check `desktop.py` line count and diff size after structural edits.
5. Confirm no public method names used by Qt signal connections disappeared.

For desktop view changes, also verify:

- Library selection/dirty-state behavior
- Add/Edit/Cancel/Delete song flows
- Set open/create/edit/delete flows
- Playback transport icon state
- Output-device/meter channel counts
- Attached WAV/MP3 filenames and persistence across view changes

## Setup and common commands

The project uses Python and PySide6. A virtual environment is normally used locally:

```bash
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
```

Typical dependency packages include PySide6, numpy, sounddevice, soundfile, Flask, pytest, and Werkzeug.

Run the desktop application:

```bash
python3 -m tracks_app
```

Run the optional Flask web application:

```bash
PORT=5001 python3 -m tracks_app.web
```

Useful validation commands:

```bash
python3 -m compileall -q tracks_app tests
python3 -m pytest -q
QT_QPA_PLATFORM=offscreen python3 - <<'PY'
from PySide6.QtWidgets import QApplication
from tracks_app.desktop import TracksDesktopWindow

app = QApplication.instance() or QApplication([])
window = TracksDesktopWindow()
window.show()
app.processEvents()
print(window.views.count())
window.close()
PY
```

## Database and persistence conventions

- `tracks_library.sqlite3` is default local state; never commit it.
- Database access is centralized in `tracks_app/db.py`.
- `ensure_schema` performs schema initialization and migrations.
- The core tables include `songs`, `tracks`, `setlists`, and `setlist_entries`; exact columns should be checked in `ensure_schema` before schema work.
- Existing SQLite row IDs are important: `Song.id` and `Setlist.id` identify persisted records.
- `SongTrack.output_channel` is zero-based for routing, while UI channel selectors normally display it as one-based.
- `save_song` recreates/updates track rows from `song.tracks`; do not assume omitted tracks remain untouched.
- Set-entry overrides are persisted separately from library-song fields.
- User audio paths must be app-local after save: store paths inside `tracks_app/uploaded_tracks/<song-id>/tracks/` rather than temporary source-directory paths.
- Song deletion should remove or orphan-clean uploaded-track storage for that song ID.
- If adopting `AUTOINCREMENT`, use a migration strategy rather than changing table creation assumptions silently.

## Application state to preserve

Important window state includes:

- `current_song` — currently selected library or effective set song.
- `current_set` — opened set.
- `current_set_entry` — selected set entry with overrides.
- `player` — active `PlaybackEngine`, or `None`.
- `edit_pending_tracks` — uncommitted attachment state for Edit Song.
- `edit_attached_directory` — pending/source directory label state; should not leak between songs.
- `open_set_play_is_paused` — transport icon/play-state model; do not infer playing state from button text.
- `open_set_picker` — set selector; use `Qt.UserRole` for set IDs and `Qt.UserRole + 1` for collapsed names.
- `output_meter_widget` — live post-mix channel meter widget.
- `beat_playhead` — count-in/loop visual state.

When switching songs or sets:

- Stop playback when appropriate.
- Reset pending attachment state.
- Recreate Edit Song baseline snapshots.
- Restore existing track filenames from persisted `Song.tracks`.
- Never leak pending attachments, overrides, or current-song state into another selection.

## Testing conventions

The main test suite is `tests/test_library.py`. It uses an isolated temporary SQLite database through an autouse fixture.

Tests cover:

- Song/set persistence and round trips
- Set ordering and override behavior
- Compound-time tempo spacing
- Audio loading and callback tails
- WAV directory inspection
- Mono/stereo reporting
- Desktop public compatibility methods
- Refactor wiring and public method names

Use temporary paths and temporary databases for new persistence tests. Do not create test records in `tracks_library.sqlite3`.

For WAV/MP3 tests:

- Write small WAV fixtures with `soundfile`.
- Assert frame/duration mismatch behavior separately from channel reporting.
- Verify MP3 support only when an actual readable decoder is available in the environment.
- Test copy-on-Save and persistence across reload.

## Audio implementation notes

- `sounddevice` supplies playback callbacks; `soundfile` loads WAV/MP3-readable audio.
- `render_chunk` mixes backing tracks into one internal frame matrix.
- `render_metronome` adds click and count-in/guide channels.
- Master gain is applied after track/metronome mixing.
- Callback buffers must be filled/zeroed before writing.
- Opening an output stream must use the actual device channel count and no channel fold-down assumptions.
- A selected explicit output device must not silently fall back to another device.
- Live channel selectors must update engine state without recreating/restarting playback where possible.
- Compound time 6/8 and 12/8 use the tempo input as the set-of-three-quavers duration.
- Mismatched backing-track lengths may warn but must remain attachable.
- Channel assertions separate mono/stereo/multi-channel status from duration mismatch status.

## UI and Qt conventions

- Use PySide6 Qt widgets and signals; avoid browser/web UI assumptions inside desktop code.
- Preserve public method names for existing Qt signal connections when refactoring.
- External view modules should receive the window and attach callbacks back to it where practical.
- Keep large Qt builder functions out of `desktop.py`.
- Dirty buttons should use Qt stylesheets and reset on Save, Cancel, or song reselection.
- Use one-line `QLineEdit` for compact Edit Song fields where required; use `QTextEdit` only for multiline content.
- Use tooltips and fixed icon sizes for transport controls instead of word buttons.
- Refresh output-device lists when opening Settings and default to system output unless an explicit device is stored.
- Keep the output meter below volume faders and use actual post-mix callback peaks.

## Refactor status and boundaries

Desktop currently includes 500+ compatibility and orchestration methods and should continue shrinking, but it is not currently safe to replace the whole file from generated snapshots.

Extract further into:

- `tracks_app/views/library_view.py` — Library and Edit Song UI/handlers.
- `tracks_app/views/open_set_view.py` — Open Set UI, set controls, transport, and set overrides.
- `tracks_app/views/settings_views.py` — Settings, device, click, and voice import UI.
- `tracks_app/services/set_service.py` — Set CRUD and override persistence.
- `tracks_app/services/audio_attachment_service.py` — WAV/MP3 validation/copy/clear behavior.
- `tracks_app/widgets/` — reusable visual widgets and controls.

Before moving a method:

1. Identify every signal or direct caller for that public name.
2. Move complete bounded methods, not fragments.
3. Compile immediately.
4. Run tests immediately.
5. Smoke-test the relevant desktop view.
6. Check line count/diff size.

Do not use local VS Code history or generated snapshots as a normal source of truth.

## Known safety and cleanup needs

- Ensure deletion of a song removes or orphan-cleans its uploaded-track directory.
- Consider `AUTOINCREMENT` or explicit cleanup before reusing SQLite song IDs.
- Avoid duplicate `uploaded_tracks/` locations; the active desktop location is `tracks_app/uploaded_tracks/<song-id>/tracks/`.
- Clear attachment is pending until Save; do not silently delete copied files on Cancel or browse.
- Verify `save_song` recreates tracks and does not preserve stale track rows unexpectedly.
- Monitor callback channel width and stale buffer data after device changes.
- When changing output device, refresh channel selectors and meter channel count consistently.
- Preserve source files; copying must never move/delete source directories or files.
- Do not commit `.venv`, SQLite database files, caches, or copied user audio.

## Git workflow

- Default branch: `main`.
- GitHub remote may use HTTPS for credential-helper authentication or SSH if keys are loaded.
- Before staging:

```bash
git status
git diff --stat
git diff --stat HEAD
```

- Before committing, review staged names and contents:

```bash
git diff --cached --stat
git diff --cached --name-only
```

- Unstage local/generated data instead of deleting it from disk:

```bash
git rm -r --cached -f .venv
```

- Push current branch:

```bash
git add .
git diff --cached --stat
git commit -m "Describe change"
git push
```

Keep bundled assets and source code commit-worthy; keep databases, virtual environments, caches, and copied user audio ignored.

## Troubleshooting

If the app fails at startup:

1. Run `python3 -m compileall -q tracks_app tests`.
2. Import `TracksDesktopWindow` and construct it offscreen.
3. Verify `launch_desktop_app` is defined at module scope in `desktop.py`.
4. Inspect missing mixin/public method names and Qt signal connections.
5. Check that view modules export builder and mixin names used by imports.

If Sets do not open:

- Verify `load_setlist`/set list loading.
- Verify `select_set_song` and `load_set_song_controls_from_entry` are complete.
- Confirm `tempo_value`, `key_value`, and `time_signature_value` are assigned before widget updates.

If audio is silent or leaking:

- Check callback buffer clearing.
- Check selected device and stream channel count.
- Check track `output_channel` and gain values.
- Check click/guide channel indexes.
- Inspect post-mix meter values before blaming hardware/interface routing.
- Verify source files still exist and persist in app-local copied directories.

If attachment files disappear in UI:

- Confirm Save copied tracks to `SongTrack.file_path` app-local paths.
- Confirm Save calls `save_song` after copying.
- Confirm `select_library_song` resets pending tracks and reloads `song.tracks` filenames.
- Confirm Cancel does not persist pending source tracks.

If `desktop.py` grows or truncates:

- Stop and inspect `git diff --stat`/`wc -l` immediately.
- Restore only from Git when a confirmed truncation is found.
- Reapply small bounded edits, not a full-file generated replacement.
- Prefer extracting a complete method/class into an existing view or service module.
- Re-run compile/tests after every extraction.
