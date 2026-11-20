from __future__ import annotations

import os
from pathlib import Path

from flask import Flask, flash, redirect, render_template, request, send_from_directory, url_for
from werkzeug.utils import secure_filename

from .db import (
    DEFAULT_DB_PATH,
    delete_setlist,
    delete_song,
    list_setlists,
    list_songs,
    load_setlist,
    load_song,
    save_setlist,
    save_song,
)
from .models import Setlist, SetlistEntry, Song, SongTrack

PROJECT_ROOT = Path(__file__).resolve().parent.parent
UPLOAD_DIR = PROJECT_ROOT / "uploaded_tracks"
UPLOAD_DIR.mkdir(exist_ok=True, parents=True)

app = Flask(__name__)
app.secret_key = "tracks-demo-secret"


@app.get("/")
def home():
    return render_template("home.html")


@app.get("/library")
def index():
    songs = list_songs(DEFAULT_DB_PATH)
    setlists = list_setlists(DEFAULT_DB_PATH)
    return render_template("index.html", songs=songs, setlists=setlists)


@app.get("/sets")
def sets_page():
    setlists = list_setlists(DEFAULT_DB_PATH)
    return render_template("sets.html", setlists=setlists)


@app.get("/sets/<int:setlist_id>")
def set_detail_route(setlist_id: int):
    setlist = load_setlist(setlist_id, DEFAULT_DB_PATH)
    if setlist is None:
        flash("Set not found.", "error")
        return redirect(url_for("sets_page"))
    songs = list_songs(DEFAULT_DB_PATH)
    return render_template("set_detail.html", setlist=setlist, songs=songs)


@app.get("/sets/<int:setlist_id>/add-song")
def add_song_to_set_page(setlist_id: int):
    setlist = load_setlist(setlist_id, DEFAULT_DB_PATH)
    if setlist is None:
        flash("Set not found.", "error")
        return redirect(url_for("sets_page"))
    songs = list_songs(DEFAULT_DB_PATH)
    return render_template("set_add_song.html", setlist=setlist, songs=songs)


@app.post("/songs")
def create_song():
    title = (request.form.get("title") or "").strip()
    artist = (request.form.get("artist") or "Unknown").strip()
    tempo = request.form.get("tempo") or "120"
    key = (request.form.get("key") or "C").strip()
    time_signature = (request.form.get("time_signature") or "4/4").strip() or "4/4"
    notes = (request.form.get("notes") or "").strip()

    if not title:
        flash("Song title is required.", "error")
        return redirect(url_for("index"))

    try:
        tempo_value = int(tempo)
    except ValueError:
        flash("Tempo must be a whole number in BPM.", "error")
        return redirect(url_for("index"))

    song = Song(title=title, artist=artist, tempo=tempo_value, key=key, notes=notes, time_signature=time_signature)
    save_song(song, DEFAULT_DB_PATH)

    uploaded = request.files.getlist("tracks")
    if uploaded:
        song_dir = UPLOAD_DIR / str(song.id)
        song_dir.mkdir(exist_ok=True, parents=True)
        for index, upload in enumerate(uploaded):
            if not upload or upload.filename == "":
                continue
            safe_name = secure_filename(upload.filename)
            save_path = song_dir / safe_name
            upload.save(save_path)
            relative_path = f"uploaded_tracks/{song.id}/{safe_name}"
            song.add_track(
                SongTrack(
                    name=Path(safe_name).stem,
                    file_path=relative_path,
                    output_channel=index,
                    gain=1.0,
                )
            )
        save_song(song, DEFAULT_DB_PATH)

    flash("Song added to the library.", "success")
    return redirect(url_for("index"))


@app.post("/songs/<int:song_id>/edit")
def update_song_route(song_id: int):
    song = load_song(song_id, DEFAULT_DB_PATH)
    if song is None:
        flash("Song not found.", "error")
        return redirect(url_for("index"))

    title = (request.form.get("title") or "").strip()
    artist = (request.form.get("artist") or "Unknown").strip()
    tempo = request.form.get("tempo") or "120"
    key = (request.form.get("key") or "C").strip()
    time_signature = (request.form.get("time_signature") or "4/4").strip() or "4/4"
    notes = (request.form.get("notes") or "").strip()

    setlist_id = request.form.get("setlist_id")
    if not title:
        flash("Song title is required.", "error")
        if setlist_id:
            return redirect(url_for("set_detail_route", setlist_id=int(setlist_id)))
        return redirect(url_for("index"))

    try:
        tempo_value = int(tempo)
    except ValueError:
        flash("Tempo must be a whole number in BPM.", "error")
        if setlist_id:
            return redirect(url_for("set_detail_route", setlist_id=int(setlist_id)))
        return redirect(url_for("index"))

    song.title = title
    song.artist = artist
    song.tempo = tempo_value
    song.key = key
    song.time_signature = time_signature
    song.notes = notes
    save_song(song, DEFAULT_DB_PATH)

    if setlist_id:
        return redirect(url_for("set_detail_route", setlist_id=int(setlist_id)))
    return redirect(url_for("index"))


@app.post("/songs/<int:song_id>/delete")
def delete_song_route(song_id: int):
    delete_song(song_id, DEFAULT_DB_PATH)
    flash("Song removed.", "success")
    return redirect(url_for("index"))


@app.post("/setlists")
def create_setlist_route():
    name = (request.form.get("setlist_name") or "").strip()
    if not name:
        flash("Set list name is required.", "error")
        return redirect(url_for("index"))

    save_setlist(Setlist(name=name), DEFAULT_DB_PATH)
    flash("Set list created.", "success")
    return redirect(url_for("index"))


@app.post("/setlists/add-song")
def add_song_to_setlist_route():
    song_id = request.form.get("song_id")
    setlist_id = request.form.get("setlist_id")
    if not song_id or not setlist_id:
        flash("Choose a set list and a song.", "error")
        return redirect(url_for("index"))

    setlist_id_int = int(setlist_id)
    setlist = load_setlist(setlist_id_int, DEFAULT_DB_PATH)
    if setlist is None:
        flash("Set list not found.", "error")
        return redirect(url_for("index"))

    song_id_int = int(song_id)
    if not any(entry.song_id == song_id_int for entry in setlist.entries):
        setlist.entries.append(SetlistEntry(song_id=song_id_int, order_index=len(setlist.entries)))
        save_setlist(setlist, DEFAULT_DB_PATH)
        flash("Song added to the set list.", "success")
    else:
        flash("Song is already in that set list.", "info")

    return redirect(url_for("set_detail_route", setlist_id=setlist_id_int))


@app.post("/setlists/<int:setlist_id>/songs/<int:song_id>/remove")
def remove_song_from_setlist_route(setlist_id: int, song_id: int):
    setlist = load_setlist(setlist_id, DEFAULT_DB_PATH)
    if setlist is None:
        flash("Set list not found.", "error")
        return redirect(url_for("index"))

    setlist.entries = [entry for entry in setlist.entries if entry.song_id != song_id]
    for index, entry in enumerate(setlist.entries):
        entry.order_index = index
    save_setlist(setlist, DEFAULT_DB_PATH)
    flash("Song removed from set list.", "success")
    return redirect(url_for("index"))


@app.post("/setlists/<int:setlist_id>/delete")
def delete_setlist_route(setlist_id: int):
    delete_setlist(setlist_id, DEFAULT_DB_PATH)
    flash("Set list deleted.", "success")
    return redirect(url_for("index"))


@app.get("/uploads/<path:relative_path>")
def serve_uploaded(relative_path: str):
    return send_from_directory(PROJECT_ROOT, relative_path)


@app.get("/settings")
def settings_page():
    return render_template("settings.html")


def launch_web_app() -> None:
    app.run(host="0.0.0.0", port=int(os.environ.get("PORT", "5001")), debug=True)
