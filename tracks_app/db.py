from __future__ import annotations

import sqlite3
from pathlib import Path
from typing import List, Optional

from .models import Setlist, SetlistEntry, Song, SongTrack

DEFAULT_DB_PATH = Path(__file__).resolve().parent.parent / "tracks_library.sqlite3"


def _connect(db_path: Path | str = DEFAULT_DB_PATH) -> sqlite3.Connection:
    connection = sqlite3.connect(str(db_path))
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA foreign_keys = ON")
    return connection


def ensure_schema(db_path: Path | str = DEFAULT_DB_PATH) -> None:
    with _connect(db_path) as connection:
        connection.execute(
            """
            CREATE TABLE IF NOT EXISTS songs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                title TEXT NOT NULL,
                artist TEXT NOT NULL,
                tempo INTEGER NOT NULL,
                key TEXT NOT NULL,
                notes TEXT DEFAULT '',
                time_signature TEXT NOT NULL DEFAULT '4/4'
            )
            """
        )
        existing_columns = [row[1] for row in connection.execute("PRAGMA table_info(songs)").fetchall()]
        if "time_signature" not in existing_columns:
            connection.execute(
                "ALTER TABLE songs ADD COLUMN time_signature TEXT NOT NULL DEFAULT '4/4'"
            )
        connection.execute(
            """
            CREATE TABLE IF NOT EXISTS tracks (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                song_id INTEGER NOT NULL,
                name TEXT NOT NULL,
                file_path TEXT NOT NULL,
                output_channel INTEGER NOT NULL,
                gain REAL NOT NULL DEFAULT 1.0,
                muted INTEGER NOT NULL DEFAULT 0,
                FOREIGN KEY(song_id) REFERENCES songs(id) ON DELETE CASCADE
            )
            """
        )
        connection.execute(
            """
            CREATE TABLE IF NOT EXISTS setlists (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL,
                set_date TEXT DEFAULT '',
                set_time TEXT DEFAULT '',
                notes TEXT DEFAULT ''
            )
            """
        )
        setlist_columns = [row[1] for row in connection.execute("PRAGMA table_info(setlists)").fetchall()]
        if "set_date" not in setlist_columns:
            connection.execute("ALTER TABLE setlists ADD COLUMN set_date TEXT DEFAULT ''")
        if "set_time" not in setlist_columns:
            connection.execute("ALTER TABLE setlists ADD COLUMN set_time TEXT DEFAULT ''")
        connection.execute(
            """
            CREATE TABLE IF NOT EXISTS setlist_items (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                setlist_id INTEGER NOT NULL,
                song_id INTEGER NOT NULL,
                order_index INTEGER NOT NULL,
                override_tempo INTEGER,
                override_key TEXT,
                override_time_signature TEXT,
                FOREIGN KEY(setlist_id) REFERENCES setlists(id) ON DELETE CASCADE,
                FOREIGN KEY(song_id) REFERENCES songs(id) ON DELETE CASCADE
            )
            """
        )
        setlist_item_columns = [row[1] for row in connection.execute("PRAGMA table_info(setlist_items)").fetchall()]
        if "override_tempo" not in setlist_item_columns:
            connection.execute("ALTER TABLE setlist_items ADD COLUMN override_tempo INTEGER")
        if "override_key" not in setlist_item_columns:
            connection.execute("ALTER TABLE setlist_items ADD COLUMN override_key TEXT")
        if "override_time_signature" not in setlist_item_columns:
            connection.execute("ALTER TABLE setlist_items ADD COLUMN override_time_signature TEXT")
        connection.execute(
            """
            CREATE TABLE IF NOT EXISTS app_settings (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            )
            """
        )
        connection.commit()


def _song_from_row(row: sqlite3.Row, db_path: Path | str = DEFAULT_DB_PATH) -> Song:
    time_signature = row["time_signature"] if "time_signature" in row.keys() else "4/4"
    song = Song(
        id=row["id"],
        title=row["title"],
        artist=row["artist"],
        tempo=row["tempo"],
        key=row["key"],
        notes=row["notes"],
        time_signature=time_signature,
    )
    song.tracks = list_tracks_for_song(song.id, db_path=db_path)
    return song


def save_song(song: Song, db_path: Path | str = DEFAULT_DB_PATH) -> Song:
    ensure_schema(db_path)
    with _connect(db_path) as connection:
        if song.id is None:
            cursor = connection.execute(
                "INSERT INTO songs (title, artist, tempo, key, notes, time_signature) VALUES (?, ?, ?, ?, ?, ?)",
                (song.title, song.artist, song.tempo, song.key, song.notes, song.time_signature or "4/4"),
            )
            song.id = cursor.lastrowid
        else:
            connection.execute(
                "UPDATE songs SET title = ?, artist = ?, tempo = ?, key = ?, notes = ?, time_signature = ? WHERE id = ?",
                (song.title, song.artist, song.tempo, song.key, song.notes, song.time_signature or "4/4", song.id),
            )
            connection.execute("DELETE FROM tracks WHERE song_id = ?", (song.id,))

        for track in song.tracks:
            connection.execute(
                "INSERT INTO tracks (song_id, name, file_path, output_channel, gain, muted) VALUES (?, ?, ?, ?, ?, ?)",
                (
                    song.id,
                    track.name,
                    track.file_path,
                    track.output_channel,
                    track.gain,
                    int(bool(track.muted)),
                ),
            )

        connection.commit()
    return song


def list_songs(db_path: Path | str = DEFAULT_DB_PATH) -> List[Song]:
    ensure_schema(db_path)
    with _connect(db_path) as connection:
        rows = connection.execute("SELECT * FROM songs ORDER BY title ASC").fetchall()
    return [_song_from_row(row, db_path=db_path) for row in rows]


def load_song(song_id: int, db_path: Path | str = DEFAULT_DB_PATH) -> Optional[Song]:
    ensure_schema(db_path)
    with _connect(db_path) as connection:
        row = connection.execute("SELECT * FROM songs WHERE id = ?", (song_id,)).fetchone()
        if row is None:
            return None
        return _song_from_row(row, db_path=db_path)


def list_tracks_for_song(song_id: int, db_path: Path | str = DEFAULT_DB_PATH) -> List[SongTrack]:
    ensure_schema(db_path)
    with _connect(db_path) as connection:
        rows = connection.execute(
            "SELECT * FROM tracks WHERE song_id = ? ORDER BY output_channel ASC, id ASC",
            (song_id,),
        ).fetchall()

    tracks = []
    for row in rows:
        tracks.append(
            SongTrack(
                id=row["id"],
                name=row["name"],
                file_path=row["file_path"],
                output_channel=row["output_channel"],
                gain=row["gain"],
                muted=bool(row["muted"]),
            )
        )
    return tracks


def delete_song(song_id: int, db_path: Path | str = DEFAULT_DB_PATH) -> None:
    ensure_schema(db_path)
    with _connect(db_path) as connection:
        connection.execute("DELETE FROM songs WHERE id = ?", (song_id,))
        connection.commit()


def _setlist_entry_from_row(row: sqlite3.Row, db_path: Path | str = DEFAULT_DB_PATH) -> SetlistEntry:
    song = load_song(row["song_id"], db_path=db_path)
    return SetlistEntry(
        id=row["id"],
        setlist_id=row["setlist_id"],
        song_id=row["song_id"],
        order_index=row["order_index"],
        override_tempo=row["override_tempo"] if "override_tempo" in row.keys() else None,
        override_key=row["override_key"] if "override_key" in row.keys() else None,
        override_time_signature=(row["override_time_signature"] if "override_time_signature" in row.keys() else None),
        song=song,
    )


def _setlist_from_row(row: sqlite3.Row, db_path: Path | str = DEFAULT_DB_PATH) -> Setlist:
    setlist = Setlist(
        id=row["id"],
        name=row["name"],
        set_date=row["set_date"] if "set_date" in row.keys() else "",
        set_time=row["set_time"] if "set_time" in row.keys() else "",
        notes=row["notes"],
    )
    setlist.entries = list_setlist_entries(setlist.id, db_path=db_path)
    return setlist


def save_setlist(setlist: Setlist, db_path: Path | str = DEFAULT_DB_PATH) -> Setlist:
    ensure_schema(db_path)
    with _connect(db_path) as connection:
        if setlist.id is None:
            cursor = connection.execute(
                "INSERT INTO setlists (name, set_date, set_time, notes) VALUES (?, ?, ?, ?)",
                (setlist.name, setlist.set_date, setlist.set_time, setlist.notes),
            )
            setlist.id = cursor.lastrowid
        else:
            connection.execute(
                "UPDATE setlists SET name = ?, set_date = ?, set_time = ?, notes = ? WHERE id = ?",
                (setlist.name, setlist.set_date, setlist.set_time, setlist.notes, setlist.id),
            )
            connection.execute("DELETE FROM setlist_items WHERE setlist_id = ?", (setlist.id,))

        for entry in setlist.entries:
            if entry.song_id is None:
                continue
            connection.execute(
                (
                    "INSERT INTO setlist_items "
                    "(setlist_id, song_id, order_index, override_tempo, override_key, override_time_signature) "
                    "VALUES (?, ?, ?, ?, ?, ?)"
                ),
                (
                    setlist.id,
                    entry.song_id,
                    entry.order_index,
                    entry.override_tempo,
                    entry.override_key,
                    entry.override_time_signature,
                ),
            )
        connection.commit()
    return setlist


def list_setlists(db_path: Path | str = DEFAULT_DB_PATH) -> List[Setlist]:
    ensure_schema(db_path)
    with _connect(db_path) as connection:
        rows = connection.execute("SELECT * FROM setlists ORDER BY id ASC").fetchall()
    return [_setlist_from_row(row, db_path=db_path) for row in rows]


def list_setlist_entries(setlist_id: int, db_path: Path | str = DEFAULT_DB_PATH) -> List[SetlistEntry]:
    ensure_schema(db_path)
    with _connect(db_path) as connection:
        rows = connection.execute(
            "SELECT * FROM setlist_items WHERE setlist_id = ? ORDER BY order_index ASC, id ASC",
            (setlist_id,),
        ).fetchall()

    return [_setlist_entry_from_row(row, db_path=db_path) for row in rows]


def load_setlist(setlist_id: int, db_path: Path | str = DEFAULT_DB_PATH) -> Optional[Setlist]:
    ensure_schema(db_path)
    with _connect(db_path) as connection:
        row = connection.execute("SELECT * FROM setlists WHERE id = ?", (setlist_id,)).fetchone()
        if row is None:
            return None
        return _setlist_from_row(row, db_path=db_path)


def delete_setlist(setlist_id: int, db_path: Path | str = DEFAULT_DB_PATH) -> None:
    ensure_schema(db_path)
    with _connect(db_path) as connection:
        connection.execute("DELETE FROM setlists WHERE id = ?", (setlist_id,))
        connection.commit()


def get_app_setting(key: str, default: str = "") -> str:
    ensure_schema(DEFAULT_DB_PATH)
    with _connect(DEFAULT_DB_PATH) as connection:
        row = connection.execute("SELECT value FROM app_settings WHERE key = ?", (key,)).fetchone()
    return row["value"] if row else default


def set_app_setting(key: str, value: str) -> None:
    ensure_schema(DEFAULT_DB_PATH)
    with _connect(DEFAULT_DB_PATH) as connection:
        connection.execute(
            "INSERT INTO app_settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value",
            (key, value),
        )
        connection.commit()
