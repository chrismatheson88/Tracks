from __future__ import annotations

from datetime import date, datetime, time

from ..models import Setlist, SetlistEntry, Song


def _parse_date(date_str: str | None) -> date:
    if not date_str:
        return date(1900, 1, 1)
    for fmt in ("%d-%m-%Y", "%Y-%m-%d"):
        try:
            return datetime.strptime(date_str, fmt).date()
        except ValueError:
            pass
    return date(1900, 1, 1)


def _parse_time(time_str: str | None) -> time:
    if not time_str:
        return time(0, 0)
    try:
        return datetime.strptime(time_str, "%H:%M").time()
    except ValueError:
        return time(0, 0)


def setlists_sorted_by_datetime_desc(setlists: list[Setlist]) -> list[Setlist]:
    def sort_key(setlist: Setlist) -> tuple[date, time, int]:
        return (
            _parse_date(setlist.set_date),
            _parse_time(setlist.set_time),
            int(setlist.id or 0),
        )

    return sorted(setlists, key=sort_key, reverse=True)


def format_set_date_for_display(set_date: str) -> str:
    if not set_date:
        return ""

    for fmt in ("%d-%m-%Y", "%Y-%m-%d"):
        try:
            parsed = datetime.strptime(set_date, fmt).date()
            return parsed.strftime("%d-%m-%Y")
        except ValueError:
            pass

    return set_date


def set_picker_list_label(setlist: Setlist) -> str:
    parts = [part for part in (format_set_date_for_display(setlist.set_date), setlist.set_time) if part]
    if not parts:
        return setlist.name
    return f"{setlist.name} ({' '.join(parts)})"


def song_with_entry_overrides(entry: SetlistEntry) -> Song:
    base_song = entry.song
    if base_song is None:
        return Song()
    return Song(
        id=base_song.id,
        title=base_song.title,
        artist=base_song.artist,
        tempo=entry.override_tempo if entry.override_tempo is not None else base_song.tempo,
        key=entry.override_key if entry.override_key else base_song.key,
        notes=base_song.notes,
        time_signature=entry.override_time_signature if entry.override_time_signature else base_song.time_signature,
        tracks=base_song.tracks,
    )


def entry_to_summary(entry: SetlistEntry) -> str:
    return song_with_entry_overrides(entry).to_summary()


def reorder_set_entries(setlist: Setlist, ordered_entry_ids: list[int | None]) -> None:
    entries_by_id = {entry.id: entry for entry in setlist.entries}
    reordered_entries = []

    for order_index, entry_id in enumerate(ordered_entry_ids):
        existing = entries_by_id.get(entry_id)
        if existing is not None:
            existing.order_index = order_index
            reordered_entries.append(existing)

    setlist.entries = reordered_entries
