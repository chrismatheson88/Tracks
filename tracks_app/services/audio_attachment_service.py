from __future__ import annotations

from pathlib import Path
import shutil

from ..models import SongTrack


SUPPORTED_AUDIO_SUFFIXES = {".wav", ".mp3"}
PACKAGE_ROOT = Path(__file__).resolve().parent.parent
CANONICAL_UPLOAD_DIR = PACKAGE_ROOT / "uploaded_tracks"


def get_song_storage_dir(song_id: int, project_root: Path = PACKAGE_ROOT) -> Path:
    """Return the base storage directory for a song: uploaded_tracks/<song-id>"""
    return project_root / "uploaded_tracks" / str(song_id)


def get_song_tracks_dir(song_id: int, project_root: Path = PACKAGE_ROOT) -> Path:
    """Return the tracks directory for a song: uploaded_tracks/<song-id>/tracks"""
    return get_song_storage_dir(song_id, project_root) / "tracks"


def remove_song_storage(song_id: int, project_root: Path = PACKAGE_ROOT) -> None:
    """Completely remove uploaded_tracks/<song-id> directory when a song is deleted."""
    song_dir = get_song_storage_dir(song_id, project_root)
    if song_dir.exists():
        shutil.rmtree(song_dir, ignore_errors=True)


def copy_tracks_to_song_directory(project_root: Path, song_id: int, tracks: list[SongTrack]) -> list[SongTrack]:
    """Copy pending WAV/MP3 tracks into uploaded_tracks/<song-id>/tracks."""
    app_track_dir = get_song_tracks_dir(song_id, project_root)
    app_track_dir.mkdir(parents=True, exist_ok=True)

    for existing in app_track_dir.iterdir():
        if existing.is_file() and existing.suffix.lower() in SUPPORTED_AUDIO_SUFFIXES:
            existing.unlink()

    copied_tracks: list[SongTrack] = []
    for track in tracks:
        source = Path(track.file_path)
        if not source.is_file():
            continue
        target = app_track_dir / source.name
        shutil.copy2(source, target)
        copied_tracks.append(SongTrack(
            name=source.stem,
            file_path=str(target.resolve()),
            output_channel=track.output_channel,
            gain=track.gain,
            muted=track.muted,
        ))
    return copied_tracks


def clear_song_track_directory(project_root: Path, song_id: int) -> None:
    app_track_dir = get_song_tracks_dir(song_id, project_root)
    if not app_track_dir.exists():
        return
    for existing in app_track_dir.iterdir():
        if existing.is_file() and existing.suffix.lower() in SUPPORTED_AUDIO_SUFFIXES:
            existing.unlink()
