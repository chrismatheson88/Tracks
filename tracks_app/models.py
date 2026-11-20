from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional


@dataclass
class SongTrack:
    id: Optional[int] = None
    name: str = ""
    file_path: str = ""
    output_channel: int = 0
    gain: float = 1.0
    muted: bool = False


@dataclass
class Song:
    id: Optional[int] = None
    title: str = ""
    artist: str = ""
    tempo: int = 120
    key: str = "C"
    notes: str = ""
    time_signature: str = "4/4"
    tracks: List[SongTrack] = field(default_factory=list)

    def add_track(self, track: SongTrack) -> None:
        self.tracks.append(track)

    def to_summary(self) -> str:
        return f"{self.title} — {self.artist} ({self.tempo} BPM, {self.key}, {self.time_signature})"


@dataclass
class SetlistEntry:
    id: Optional[int] = None
    setlist_id: Optional[int] = None
    song_id: int = 0
    order_index: int = 0
    override_tempo: Optional[int] = None
    override_key: Optional[str] = None
    override_time_signature: Optional[str] = None
    song: Optional[Song] = None


@dataclass
class Setlist:
    id: Optional[int] = None
    name: str = ""
    set_date: str = ""
    set_time: str = ""
    notes: str = ""
    entries: List[SetlistEntry] = field(default_factory=list)

    def add_song(self, song_id: int) -> None:
        self.entries.append(SetlistEntry(song_id=song_id, order_index=len(self.entries)))
