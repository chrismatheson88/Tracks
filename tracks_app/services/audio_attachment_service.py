from __future__ import annotations

from pathlib import Path
import shutil

from ..models import SongTrack


SUPPORTED_AUDIO_SUFFIXES = {".wav", ".mp3"}


def copy_tracks_to_song_directory(project_root: Path, song_id: int, tracks: list[SongTrack]) -> list[SongTrack]:
    """Copy pending WAV/MP3 tracks into uploaded_tracks/<song-id>/tracks."""
    app_track_dir = project_root / "uploaded_tracks" / str(song_id) / "tracks"
    app_track_dir.mkdir(parents=True, exist_ok=True)

    for existing in app_track_dir.iterdir():
        if existing.is_file() and existing.suffix.lower() in SUPPORTED_AUDIO_SUFFIXES:
            existing.unlink()

    copied_tracks: list[SongTrack] = []
    for index, track in enumerate(tracks):
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
    app_track_dir = project_root / "uploaded_tracks" / str(song_id) / "tracks"
    if not app_track_dir.exists():
        return
    for existing in app_track_dir.iterdir():
        if existing.is_file() and existing.suffix.lower() in SUPPORTED_AUDIO_SUFFIXES:
            existing.unlink()
