from __future__ import annotations

import pytest
import numpy as np
import soundfile as sf

from tracks_app.db import list_setlists, list_songs, save_setlist, save_song
from tracks_app.models import Setlist, SetlistEntry, Song, SongTrack
from tracks_app.playback import PlaybackEngine, inspect_wav_directory


@pytest.fixture(autouse=True)
def isolated_test_database(tmp_path, monkeypatch):
    """Keep test data isolated and remove known test records from the real library."""
    import sqlite3

    test_db = tmp_path / "test_library.sqlite3"

    import tracks_app.db as db_module
    import tracks_app.web as web_module

    real_db = db_module.DEFAULT_DB_PATH

    def cleanup_real_test_records():
        if not real_db.exists():
            return
        test_song_titles = (
            "Time Signature Test",
            "Opening Song",
            "Alpha",
            "Beta",
            "Editable Song",
        )
        test_set_names = ("Set Two", "Set Picker", "Edit Set")
        placeholders_songs = ",".join("?" for _ in test_song_titles)
        placeholders_sets = ",".join("?" for _ in test_set_names)

        with sqlite3.connect(str(real_db)) as connection:
            set_rows = connection.execute(
                f"SELECT id FROM setlists WHERE name IN ({placeholders_sets})",
                test_set_names,
            ).fetchall()
            for (setlist_id,) in set_rows:
                connection.execute("DELETE FROM setlist_items WHERE setlist_id = ?", (setlist_id,))
                connection.execute("DELETE FROM setlists WHERE id = ?", (setlist_id,))

            song_rows = connection.execute(
                f"SELECT id FROM songs WHERE title IN ({placeholders_songs})",
                test_song_titles,
            ).fetchall()
            for (song_id,) in song_rows:
                connection.execute("DELETE FROM tracks WHERE song_id = ?", (song_id,))
                connection.execute("DELETE FROM songs WHERE id = ?", (song_id,))

            if "app_settings" in [row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type='table'")]:
                connection.execute("DELETE FROM app_settings WHERE key = ?", ("voice.test",))
            connection.commit()

    cleanup_real_test_records()

    monkeypatch.setattr(db_module, "DEFAULT_DB_PATH", test_db)
    monkeypatch.setattr(web_module, "DEFAULT_DB_PATH", test_db)
    web_module.app.config.update(TESTING=True, DB_PATH=str(test_db))

    yield

    cleanup_real_test_records()


def test_save_and_load_song_round_trip(tmp_path):
    db_path = tmp_path / "library.sqlite3"
    song = Song(title="Northbound", artist="The Session", tempo=96, key="D", time_signature="4/4")
    song.add_track(SongTrack(name="Drums", file_path="/tmp/drums.wav", output_channel=0, gain=0.8))
    song.add_track(SongTrack(name="Bass", file_path="/tmp/bass.wav", output_channel=2, gain=0.9))

    saved = save_song(song, db_path)
    loaded = list_songs(db_path)[0]

    assert saved.id == loaded.id
    assert loaded.title == "Northbound"
    assert loaded.time_signature == "4/4"
    assert [track.name for track in loaded.tracks] == ["Drums", "Bass"]
    assert loaded.tracks[1].output_channel == 2


def test_render_chunk_routes_audio_to_output_channels(tmp_path):
    track_one = tmp_path / "drums.wav"
    track_two = tmp_path / "bass.wav"

    sf.write(track_one, np.full((512, 1), 0.3, dtype=np.float32), 44100)
    sf.write(track_two, np.full((512, 1), 0.7, dtype=np.float32), 44100)

    song = Song(title="Demo", artist="App", tempo=120, key="A")
    song.add_track(SongTrack(name="Drums", file_path=str(track_one), output_channel=0, gain=1.0))
    song.add_track(SongTrack(name="Bass", file_path=str(track_two), output_channel=3, gain=1.0))

    engine = PlaybackEngine(song, output_channels=6)
    rendered = engine.render_chunk(512)

    assert rendered.shape == (512, 6)
    assert np.any(rendered[:, 0] > 0.2)
    assert np.any(rendered[:, 3] > 0.5)
    assert np.all(rendered[:, 1] == 0)


def test_save_and_load_setlist_round_trip(tmp_path):
    db_path = tmp_path / "library.sqlite3"
    song_one = save_song(Song(title="Song One", artist="Band", tempo=84, key="D"), db_path)
    song_two = save_song(Song(title="Song Two", artist="Band", tempo=112, key="E"), db_path)

    setlist = Setlist(name="Main Set", set_date="2026-11-19", set_time="19:30")
    setlist.entries = [
        SetlistEntry(song_id=song_one.id, order_index=0),
        SetlistEntry(song_id=song_two.id, order_index=1),
    ]

    saved_setlist = save_setlist(setlist, db_path)
    loaded_setlist = list_setlists(db_path)[0]

    assert saved_setlist.id == loaded_setlist.id
    assert loaded_setlist.set_date == "2026-11-19"
    assert loaded_setlist.set_time == "19:30"
    assert [entry.song_id for entry in loaded_setlist.entries] == [song_one.id, song_two.id]
    assert [entry.order_index for entry in loaded_setlist.entries] == [0, 1]


def test_song_can_appear_in_multiple_setlists(tmp_path):
    db_path = tmp_path / "library.sqlite3"
    song = save_song(Song(title="Shared Song", artist="Band", tempo=100, key="G"), db_path)

    first_set = save_setlist(Setlist(name="Set One"), db_path)
    second_set = save_setlist(Setlist(name="Set Two"), db_path)

    first_set.entries = [SetlistEntry(song_id=song.id, order_index=0)]
    second_set.entries = [SetlistEntry(song_id=song.id, order_index=0)]

    save_setlist(first_set, db_path)
    save_setlist(second_set, db_path)

    setlists = list_setlists(db_path)
    song_ids = sorted(entry.song_id for setlist in setlists for entry in setlist.entries)

    assert song_ids == [song.id, song.id]
    assert [song.title for song in list_songs(db_path)] == ["Shared Song"]
    assert len(setlists) == 2


def test_song_can_be_added_more_than_once_to_set(tmp_path):
    db_path = tmp_path / "library.sqlite3"
    song = save_song(Song(title="Repeat Song", artist="Band", tempo=100, key="G"), db_path)

    setlist = Setlist(name="Repeated Set")
    setlist.entries = [
        SetlistEntry(song_id=song.id, order_index=0),
        SetlistEntry(song_id=song.id, order_index=1),
        SetlistEntry(song_id=song.id, order_index=2),
    ]

    saved = save_setlist(setlist, db_path)
    loaded = list_setlists(db_path)[0]

    assert saved.id == loaded.id
    assert [entry.song_id for entry in loaded.entries] == [song.id, song.id, song.id]
    assert [entry.order_index for entry in loaded.entries] == [0, 1, 2]


def test_duplicate_song_entries_can_store_independent_overrides(tmp_path):
    db_path = tmp_path / "library.sqlite3"
    song = save_song(Song(title="Repeat Song", artist="Band", tempo=100, key="G", time_signature="4/4"), db_path)

    setlist = Setlist(name="Override Set")
    setlist.entries = [
        SetlistEntry(song_id=song.id, order_index=0, override_tempo=92, override_key="F", override_time_signature="3/4"),
        SetlistEntry(song_id=song.id, order_index=1, override_tempo=120, override_key="A", override_time_signature="6/8"),
    ]

    save_setlist(setlist, db_path)
    loaded = list_setlists(db_path)[0]

    assert len(loaded.entries) == 2
    assert loaded.entries[0].song_id == song.id
    assert loaded.entries[1].song_id == song.id
    assert loaded.entries[0].override_tempo == 92
    assert loaded.entries[0].override_key == "F"
    assert loaded.entries[0].override_time_signature == "3/4"
    assert loaded.entries[1].override_tempo == 120
    assert loaded.entries[1].override_key == "A"
    assert loaded.entries[1].override_time_signature == "6/8"


def test_home_page_has_library_and_sets_links():
    from tracks_app.web import app

    client = app.test_client()
    response = client.get("/")

    assert response.status_code == 200
    page = response.get_data(as_text=True)
    assert 'href="/library"' in page
    assert 'href="/sets"' in page
    assert "Library" in page
    assert "Sets" in page


def test_song_form_accepts_time_signature_and_persists_it():
    from tracks_app.web import app

    client = app.test_client()
    response = client.post(
        "/songs",
        data={
            "title": "Time Signature Test",
            "artist": "Band",
            "tempo": "118",
            "key": "G",
            "time_signature": "6/8",
            "notes": "Test track",
        },
        follow_redirects=False,
    )

    assert response.status_code == 302
    songs = list_songs(app.config.get("DB_PATH", "tracks_library.sqlite3"))
    assert any(song.title == "Time Signature Test" and song.time_signature == "6/8" for song in songs)


def test_open_set_page_renders_set_details_and_song_selection():
    from tracks_app.web import app

    from tracks_app.db import save_setlist, save_song
    from tracks_app.models import Setlist, SetlistEntry, Song

    song = save_song(Song(title="Opening Song", artist="Band", tempo=90, key="D"), db_path=app.config.get("DB_PATH", "tracks_library.sqlite3"))
    setlist = save_setlist(Setlist(name="Set Two", entries=[]), db_path=app.config.get("DB_PATH", "tracks_library.sqlite3"))
    setlist.entries = [SetlistEntry(song_id=song.id, order_index=0)]
    save_setlist(setlist, db_path=app.config.get("DB_PATH", "tracks_library.sqlite3"))

    client = app.test_client()
    response = client.get(f"/sets/{setlist.id}")

    assert response.status_code == 200
    page = response.get_data(as_text=True)
    assert "Open set" in page or "Playback" in page
    assert "Opening Song" in page
    assert "songSelect" not in page
    assert "set-song-list" in page
    assert "Add song to set" in page
    assert "Add from library" in page
    assert "countInBars = 2" in page
    assert "window.setTimeout" in page
    assert "firstBarCountPattern" in page
    assert "voice-select" not in page
    assert "tracks.defaultEnglishVoice" in page


def test_add_song_picker_lists_titles_only_and_returns_to_set():
    from tracks_app.web import app

    from tracks_app.db import save_setlist, save_song
    from tracks_app.models import Setlist, Song

    save_song(Song(title="Alpha", artist="Band", tempo=80, key="A"), db_path=app.config.get("DB_PATH", "tracks_library.sqlite3"))
    save_song(Song(title="Beta", artist="Band", tempo=100, key="B"), db_path=app.config.get("DB_PATH", "tracks_library.sqlite3"))
    setlist = save_setlist(Setlist(name="Set Picker"), db_path=app.config.get("DB_PATH", "tracks_library.sqlite3"))

    client = app.test_client()
    response = client.get(f"/sets/{setlist.id}/add-song")

    assert response.status_code == 200
    page = response.get_data(as_text=True)
    assert "Alpha" in page
    assert "Beta" in page
    assert "BPM" not in page

    add_response = client.post("/setlists/add-song", data={"song_id": "1", "setlist_id": str(setlist.id)}, follow_redirects=False)
    assert add_response.status_code == 302
    assert add_response.headers["Location"].endswith(f"/sets/{setlist.id}")


def test_set_page_has_editable_song_form_and_updates_library():
    from tracks_app.web import app

    from tracks_app.db import save_setlist, save_song
    from tracks_app.models import Setlist, Song

    song = save_song(Song(title="Editable Song", artist="Band", tempo=92, key="F", time_signature="4/4"), db_path=app.config.get("DB_PATH", "tracks_library.sqlite3"))
    setlist = save_setlist(Setlist(name="Edit Set", entries=[]), db_path=app.config.get("DB_PATH", "tracks_library.sqlite3"))
    setlist.entries = [SetlistEntry(song_id=song.id, order_index=0)]
    save_setlist(setlist, db_path=app.config.get("DB_PATH", "tracks_library.sqlite3"))

    client = app.test_client()
    get_response = client.get(f"/sets/{setlist.id}")
    assert get_response.status_code == 200
    page = get_response.get_data(as_text=True)
    assert 'name="tempo"' in page
    assert 'name="time_signature"' in page

    post_response = client.post(
        f"/songs/{song.id}/edit",
        data={
            "title": "Editable Song",
            "artist": "Band",
            "tempo": "104",
            "key": "E",
            "time_signature": "6/8",
            "notes": "Updated",
            "setlist_id": str(setlist.id),
        },
        follow_redirects=False,
    )
    assert post_response.status_code == 302
    updated = next(item for item in list_songs(app.config.get("DB_PATH", "tracks_library.sqlite3")) if item.id == song.id)
    assert updated.tempo == 104
    assert updated.key == "E"
    assert updated.time_signature == "6/8"
    assert updated.notes == "Updated"


def test_settings_page_lists_voice_setting_and_persistence_key():
    from tracks_app.web import app

    client = app.test_client()
    response = client.get("/settings")

    assert response.status_code == 200
    page = response.get_data(as_text=True)
    assert 'id="voice-select"' in page
    assert "tracks.defaultEnglishVoice" in page


def test_native_countin_assets_are_bundled():
    from pathlib import Path

    asset_dir = Path("tracks_app/assets/countin/female")
    assert all((asset_dir / f"{word}.wav").exists() for word in ("one", "two", "three", "four", "five", "six", "seven", "eight"))


def test_native_metronome_render_produces_click_and_voice():
    song = Song(title="Native Click", artist="Band", tempo=120, key="C", time_signature="4/4")
    engine = PlaybackEngine(song, output_channels=2)

    rendered = engine.render_metronome(44100)

    assert rendered.shape == (44100, 2)
    assert np.any(rendered[:, 0] != 0)


def test_app_settings_round_trip():
    from tracks_app.db import get_app_setting, set_app_setting

    set_app_setting("voice.test", "Samantha")
    assert get_app_setting("voice.test") == "Samantha"


def test_main_entry_point_uses_desktop_app():
    from pathlib import Path

    main_source = Path("tracks_app/__main__.py").read_text()
    assert "launch_desktop_app" in main_source
    assert "launch_app" not in main_source


def test_bundled_accent_and_normal_click_wavs_exist_and_load():
    from pathlib import Path

    from tracks_app.models import Song
    from tracks_app.playback import PlaybackEngine

    click_dir = Path("tracks_app/assets/clicks")
    assert (click_dir / "tone_accent.wav").exists()
    assert (click_dir / "tone_normal.wav").exists()

    engine = PlaybackEngine(Song(title="Clicks", tempo=120, time_signature="4/4"), output_channels=2)
    assert engine._accent_click is not None
    assert engine._normal_click is not None


def test_click_and_voice_wavs_continue_across_callbacks():

    from tracks_app.models import Song
    from tracks_app.playback import PlaybackEngine

    engine = PlaybackEngine(Song(title="WAV tails", tempo=120, time_signature="4/4"), output_channels=2)
    block = 441

    rendered = []
    for _ in range(40):
        out = np.zeros((block, 2), dtype=np.float32)
        engine._callback(out, block, None, None)
        rendered.append(out)

    combined = np.concatenate(rendered, axis=0)
    assert np.any(combined != 0)
    assert len(engine._accent_click) > block


def test_volume_fader_db_conversion():
    from tracks_app.desktop import TracksDesktopWindow

    assert TracksDesktopWindow.db_to_linear(0) == 1.0
    assert TracksDesktopWindow.db_to_linear(-30) == 0.0
    assert abs(TracksDesktopWindow.db_to_linear(12) - 3.9810717055349722) < 1e-12

    class DummyWindow:
        db_to_linear = TracksDesktopWindow.db_to_linear

    assert DummyWindow().db_to_linear(0) == 1.0
    assert DummyWindow().db_to_linear(-30) == 0.0


def test_playback_engine_gain_fields_mutate_live():
    from tracks_app.models import Song
    from tracks_app.playback import PlaybackEngine

    engine = PlaybackEngine(Song(title="Gain test", tempo=120, time_signature="4/4"), output_channels=2)
    engine.click_gain = 0.5
    engine.guide_gain = 0.25
    engine.master_gain = 2.0

    assert engine.click_gain == 0.5
    assert engine.guide_gain == 0.25
    assert engine.master_gain == 2.0


def test_countin_voice_directories_are_discoverable():
    from pathlib import Path

    countin_dir = Path("tracks_app/assets/countin")
    voices = sorted(path.name for path in countin_dir.iterdir() if path.is_dir())
    assert "female" in voices

    for voice_name in voices:
        voice_dir = countin_dir / voice_name
        assert all((voice_dir / f"{word}.wav").exists() for word in ("one", "two", "three", "four", "five", "six", "seven", "eight"))


def test_next_previous_restart_logic_is_wired():
    from tracks_app.views.open_set_view import SetViewMixin

    assert hasattr(SetViewMixin, "previous_set_song")
    assert hasattr(SetViewMixin, "next_set_song")
    assert hasattr(SetViewMixin, "stop_playback")


def test_compound_tempo_click_spacing_is_three_quavers():
    song = Song(title="Compound", tempo=120, time_signature="6/8")
    engine = PlaybackEngine(song, output_channels=2)
    engine.sample_rate = 44100

    group_frames = int(round(engine.sample_rate * 60.0 / 120))
    expected_click_frames = max(1, int(round(group_frames / 3.0)))

    assert expected_click_frames == 7350


def test_inspect_wav_directory_rejects_directory_with_no_wavs(tmp_path):
    (tmp_path / "notes.txt").write_text("not a wav")

    tracks, mismatches, error = inspect_wav_directory(tmp_path)

    assert tracks == []
    assert mismatches == []
    assert error == "No WAV or MP3 files were found in the selected directory."


def test_inspect_wav_directory_accepts_matching_lengths(tmp_path):
    sf.write(tmp_path / "01_drums.wav", np.zeros((512, 1), dtype=np.float32), 44100)
    sf.write(tmp_path / "02_bass.wav", np.ones((512, 1), dtype=np.float32) * 0.5, 44100)

    tracks, mismatches, error = inspect_wav_directory(tmp_path)

    assert error is None
    assert mismatches == []
    assert [track.output_channel for track in tracks] == [0, 1]
    assert [track.name for track in tracks] == ["01_drums", "02_bass"]


def test_inspect_wav_directory_reports_mismatched_lengths(tmp_path):
    sf.write(tmp_path / "01_drums.wav", np.zeros((512, 1), dtype=np.float32), 44100)
    sf.write(tmp_path / "02_bass.wav", np.ones((1024, 1), dtype=np.float32) * 0.5, 44100)

    tracks, mismatches, error = inspect_wav_directory(tmp_path)

    assert error is None
    assert len(tracks) == 2
    assert any("Audio lengths differ" in mismatch for mismatch in mismatches)


def test_inspect_wav_directory_reports_mono_and_stereo_files(tmp_path):
    sf.write(tmp_path / "01_mono.wav", np.zeros((512, 1), dtype=np.float32), 44100)
    sf.write(tmp_path / "02_stereo.wav", np.zeros((512, 2), dtype=np.float32), 44100)

    tracks, mismatches, error = inspect_wav_directory(tmp_path)
    from tracks_app.playback import inspect_audio_channels
    details = inspect_audio_channels(tmp_path)

    assert error is None
    assert len(tracks) == 2
    assert mismatches == []
    assert any("01_mono.wav: mono" in item for item in details)
    assert any("02_stereo.wav: stereo" in item for item in details)


def test_inspect_audio_channels_reports_mono_and_stereo(tmp_path):
    from tracks_app.playback import inspect_audio_channels

    sf.write(tmp_path / "01_mono.wav", np.zeros((512, 1), dtype=np.float32), 44100)
    sf.write(tmp_path / "02_stereo.wav", np.zeros((512, 2), dtype=np.float32), 44100)

    details = inspect_audio_channels(tmp_path)

    assert "01_mono.wav: mono" in details
    assert "02_stereo.wav: stereo" in details


def test_attached_track_paths_persist_after_save_and_reload(tmp_path):
    import tracks_app.db as db_module

    audio_file = tmp_path / "01_drums.wav"
    sf.write(audio_file, np.zeros((512, 1), dtype=np.float32), 44100)

    song = db_module.save_song(
        Song(title="Attached", tracks=[SongTrack(name="01_drums", file_path=str(audio_file), output_channel=0)]),
        tmp_path / "library.sqlite3",
    )
    loaded = db_module.load_song(song.id, tmp_path / "library.sqlite3")

    assert loaded is not None
    assert len(loaded.tracks) == 1
    assert loaded.tracks[0].file_path == str(audio_file)


def test_compound_12_8_metronome_render_does_not_crash():
    song = Song(title="Compound 12/8", tempo=120, time_signature="12/8")
    engine = PlaybackEngine(song, output_channels=2)
    rendered = engine.render_metronome(44100 * 2)
    assert rendered.shape == (44100 * 2, 2)


def test_foreign_keys_cascade_delete_song_and_tracks(tmp_path):
    from tracks_app.db import _connect, delete_song, save_song

    db_path = tmp_path / "fk_test.sqlite3"
    song = save_song(Song(title="Cascade Song", tracks=[SongTrack(name="T1", file_path="t1.wav")]), db_path)
    assert song.id is not None
    with _connect(db_path) as conn:
        assert len(conn.execute("SELECT * FROM tracks WHERE song_id = ?", (song.id,)).fetchall()) == 1
    delete_song(song.id, db_path)
    with _connect(db_path) as conn:
        assert len(conn.execute("SELECT * FROM tracks WHERE song_id = ?", (song.id,)).fetchall()) == 0


def test_playback_engine_missing_file_skips_gracefully():
    song = Song(title="Missing File", tracks=[SongTrack(name="Ghost", file_path="/nonexistent/ghost.wav")])
    engine = PlaybackEngine(song, output_channels=2)
    assert len(engine.tracks) == 0


def test_render_chunk_routes_stereo_to_dual_channels_when_available(tmp_path):
    track_stereo = tmp_path / "stereo.wav"
    data = np.zeros((512, 2), dtype=np.float32)
    data[:, 0] = 0.35  # Left channel
    data[:, 1] = 0.75  # Right channel
    sf.write(track_stereo, data, 44100)

    song = Song(title="Stereo Demo", tracks=[SongTrack(name="Synth", file_path=str(track_stereo), output_channel=0)])
    engine = PlaybackEngine(song, output_channels=4)
    rendered = engine.render_chunk(512)

    assert rendered.shape == (512, 4)
    # Channel 0 has Left channel
    assert np.allclose(rendered[:, 0], 0.35, atol=1e-4)
    # Channel 1 has Right channel
    assert np.allclose(rendered[:, 1], 0.75, atol=1e-4)
    # Other channels remain zero
    assert np.all(rendered[:, 2:] == 0)


def test_render_chunk_downmixes_stereo_when_adjacent_channel_unavailable(tmp_path):
    track_stereo = tmp_path / "stereo.wav"
    data = np.zeros((512, 2), dtype=np.float32)
    data[:, 0] = 0.2
    data[:, 1] = 0.8
    sf.write(track_stereo, data, 44100)

    song = Song(title="Stereo Downmix", tracks=[SongTrack(name="Synth", file_path=str(track_stereo), output_channel=0)])
    engine = PlaybackEngine(song, output_channels=1)
    rendered = engine.render_chunk(512)

    assert rendered.shape == (512, 1)
    # Downmixed average (0.2 + 0.8) / 2 = 0.5
    assert np.allclose(rendered[:, 0], 0.5, atol=1e-4)


def test_inspect_wav_directory_assigns_stereo_channel_offset(tmp_path):
    sf.write(tmp_path / "01_stereo.wav", np.zeros((512, 2), dtype=np.float32), 44100)
    sf.write(tmp_path / "02_mono.wav", np.zeros((512, 1), dtype=np.float32), 44100)

    tracks, mismatches, error = inspect_wav_directory(tmp_path)

    assert error is None
    assert len(tracks) == 2
    # Stereo track 1 gets channel 0 (occupies 0 and 1)
    assert tracks[0].output_channel == 0
    # Mono track 2 gets channel 2
    assert tracks[1].output_channel == 2


def test_web_create_song_uploads_tracks_to_canonical_directory():
    import io
    from pathlib import Path
    from tracks_app.web import app
    from tracks_app.db import list_songs
    from tracks_app.services.audio_attachment_service import get_song_tracks_dir, remove_song_storage

    client = app.test_client()
    response = client.post(
        "/songs",
        data={
            "title": "Web Upload Test",
            "artist": "Web Band",
            "tempo": "120",
            "key": "A",
            "time_signature": "4/4",
            "notes": "Testing canonical upload",
            "tracks": (io.BytesIO(b"RIFFdummydataWAVE"), "test_track.wav"),
        },
        content_type="multipart/form-data",
        follow_redirects=False,
    )
    assert response.status_code == 302
    db_path = app.config.get("DB_PATH", "tracks_library.sqlite3")
    songs = list_songs(db_path)
    created = next(s for s in songs if s.title == "Web Upload Test")
    assert len(created.tracks) == 1

    tracks_dir = get_song_tracks_dir(created.id)
    saved_file = tracks_dir / "test_track.wav"
    try:
        assert saved_file.exists()
        assert Path(created.tracks[0].file_path).resolve() == saved_file.resolve()

        # Test serving uploaded file
        clean_path = created.tracks[0].file_path.lstrip('/')
        serve_resp = client.get(f"/uploads/{clean_path}")
        assert serve_resp.status_code == 200
        assert serve_resp.data == b"RIFFdummydataWAVE"

        # Test deleting song removes storage
        del_resp = client.post(f"/songs/{created.id}/delete", follow_redirects=False)
        assert del_resp.status_code == 302
        assert not tracks_dir.exists()
    finally:
        remove_song_storage(created.id)


def test_set_logic_sorting_and_formatting():
    from tracks_app.models import Setlist
    from tracks_app.services.set_logic import (
        format_set_date_for_display,
        set_picker_list_label,
        setlists_sorted_by_datetime_desc,
    )

    assert format_set_date_for_display("20-11-2026") == "20-11-2026"
    assert format_set_date_for_display("2026-11-20") == "20-11-2026"
    assert format_set_date_for_display("") == ""
    assert format_set_date_for_display("not-a-date") == "not-a-date"

    s1 = Setlist(id=1, name="Earliest", set_date="10-11-2026", set_time="19:00")
    s2 = Setlist(id=2, name="Latest", set_date="20-11-2026", set_time="20:00")
    s3 = Setlist(id=3, name="Middle", set_date="2026-11-15", set_time="18:00")
    s_undated = Setlist(id=4, name="Undated", set_date="", set_time="")

    sorted_sets = setlists_sorted_by_datetime_desc([s1, s_undated, s3, s2])
    assert [s.id for s in sorted_sets] == [2, 3, 1, 4]

    assert set_picker_list_label(s2) == "Latest (20-11-2026 20:00)"
    assert set_picker_list_label(s_undated) == "Undated"



