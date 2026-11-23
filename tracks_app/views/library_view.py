from __future__ import annotations

from pathlib import Path
import shutil

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QAbstractItemView,
    QComboBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QSpinBox,
    QStackedWidget,
    QFileDialog,
    QMessageBox,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from ..db import delete_song, list_songs, save_song
from ..models import Song
from ..playback import inspect_wav_directory
from ..services.audio_attachment_service import (
    clear_song_track_directory,
    copy_tracks_to_song_directory,
    remove_song_storage,
)


def build_library_view(window) -> QWidget:
    window.library_edit_baseline = None

    view = QWidget()
    layout = QHBoxLayout(view)

    left = QWidget()
    left_layout = QVBoxLayout(left)
    left_layout.addWidget(QLabel("Library"))
    window.library_list = QListWidget()
    window.library_list.setSelectionMode(QAbstractItemView.SingleSelection)
    window.library_list.currentRowChanged.connect(window.select_library_song)
    left_layout.addWidget(window.library_list)

    actions = QHBoxLayout()
    window.new_song_button = QPushButton("New song")
    window.delete_song_button = QPushButton("Delete song")
    actions.addWidget(window.new_song_button)
    actions.addWidget(window.delete_song_button)
    left_layout.addLayout(actions)

    window.library_details = QTextEdit()
    window.library_details.setReadOnly(True)

    window.library_right = QStackedWidget()
    window.library_right.addWidget(window.library_details)

    window.library_editor = QWidget()
    editor_layout = QVBoxLayout(window.library_editor)
    editor_layout.addWidget(QLabel("New song"))

    window.new_title = QLineEdit()
    window.new_artist = QLineEdit("Unknown")
    window.new_tempo = QSpinBox()
    window.new_tempo.setRange(40, 220)
    window.new_tempo.setValue(120)
    window.new_key = QLineEdit("C")
    window.new_time_signature = QComboBox()
    window.new_time_signature.addItems(["4/4", "3/4", "6/8", "12/8", "2/2", "5/4"])
    window.new_notes = QLineEdit()

    form = QFormLayout()
    form.addRow("Title:", window.new_title)
    form.addRow("Artist:", window.new_artist)
    form.addRow("Tempo (BPM):", window.new_tempo)
    form.addRow("Key:", window.new_key)
    form.addRow("Time signature:", window.new_time_signature)
    form.addRow("Notes:", window.new_notes)
    editor_layout.addLayout(form)

    editor_actions = QHBoxLayout()
    save_button = QPushButton("Save song")
    cancel_button = QPushButton("Cancel")
    editor_actions.addWidget(save_button)
    editor_actions.addWidget(cancel_button)
    editor_layout.addLayout(editor_actions)

    save_button.clicked.connect(window.save_new_song)
    cancel_button.clicked.connect(window.cancel_new_song)
    window.library_right.addWidget(window.library_editor)

    window.library_edit_editor = QWidget()
    edit_layout = QVBoxLayout(window.library_edit_editor)
    edit_layout.setContentsMargins(0, 0, 0, 0)
    edit_layout.setSpacing(8)
    edit_layout.setAlignment(Qt.AlignTop)
    edit_layout.addWidget(QLabel("Edit song"))

    window.edit_title = QLineEdit()
    window.edit_artist = QLineEdit()
    window.edit_tempo = QSpinBox()
    window.edit_tempo.setRange(40, 220)
    window.edit_key = QLineEdit()
    window.edit_time_signature = QComboBox()
    window.edit_time_signature.addItems(["4/4", "3/4", "6/8", "12/8", "2/2", "5/4"])
    window.edit_notes = QLineEdit()

    wav_row = QHBoxLayout()
    window.edit_wav_directory = QLabel("No WAV directory attached")
    window.edit_wav_directory.setWordWrap(True)
    browse_wav_button = QPushButton("Browse for WAV directory")
    clear_wav_button = QPushButton("Clear attached WAV files")
    wav_row.addWidget(window.edit_wav_directory, 1)
    wav_row.addWidget(browse_wav_button)
    wav_row.addWidget(clear_wav_button)

    edit_form = QFormLayout()
    edit_form.setFieldGrowthPolicy(QFormLayout.AllNonFixedFieldsGrow)
    edit_form.setFormAlignment(Qt.AlignTop)
    edit_form.addRow("Title:", window.edit_title)
    edit_form.addRow("Artist:", window.edit_artist)
    edit_form.addRow("Tempo (BPM):", window.edit_tempo)
    edit_form.addRow("Key:", window.edit_key)
    edit_form.addRow("Time signature:", window.edit_time_signature)
    edit_form.addRow("Notes:", window.edit_notes)
    edit_form.addRow("Track WAVs:", wav_row)
    edit_layout.addLayout(edit_form)

    browse_wav_button.clicked.connect(window.browse_edit_song_wav_directory)
    clear_wav_button.clicked.connect(window.clear_edit_song_wav_directory)

    edit_actions = QHBoxLayout()
    save_edits_button = QPushButton("Save edits to library")
    window.save_edits_button = save_edits_button
    window.save_edits_button.setStyleSheet("")
    cancel_edits_button = QPushButton("Cancel edits")
    edit_actions.addWidget(save_edits_button)
    edit_actions.addWidget(cancel_edits_button)
    edit_layout.addLayout(edit_actions)

    save_edits_button.clicked.connect(window.save_library_edits)
    cancel_edits_button.clicked.connect(window.cancel_library_edits)
    window.edit_title.textChanged.connect(window.update_library_edit_dirty_state)
    window.edit_artist.textChanged.connect(window.update_library_edit_dirty_state)
    window.edit_tempo.valueChanged.connect(window.update_library_edit_dirty_state)
    window.edit_key.textChanged.connect(window.update_library_edit_dirty_state)
    window.edit_time_signature.currentTextChanged.connect(window.update_library_edit_dirty_state)
    window.edit_notes.textChanged.connect(window.update_library_edit_dirty_state)
    window.library_right.addWidget(window.library_edit_editor)

    layout.addWidget(left, 1)
    layout.addWidget(window.library_right, 2)

    window.new_song_button.clicked.connect(window.create_song)
    window.delete_song_button.clicked.connect(window.delete_selected_song)
    return view


def build_sets_view(window) -> QWidget:
    view = QWidget()
    layout = QVBoxLayout(view)

    heading = QLabel("Sets")
    heading.setStyleSheet("font-size: 20px; font-weight: 700;")
    layout.addWidget(heading)

    window.set_list = QListWidget()
    window.set_list.itemDoubleClicked.connect(window.open_set_item)
    layout.addWidget(window.set_list)

    actions = QHBoxLayout()
    window.new_set_button = QPushButton("New set")
    window.open_set_button = QPushButton("Open set")
    window.delete_set_button = QPushButton("Delete set")
    actions.addWidget(window.new_set_button)
    actions.addWidget(window.open_set_button)
    actions.addWidget(window.delete_set_button)
    layout.addLayout(actions)

    window.new_set_button.clicked.connect(window.create_set)
    window.open_set_button.clicked.connect(window.open_selected_set)
    window.delete_set_button.clicked.connect(window.delete_selected_set)
    return view


class LibraryViewMixin:
    def format_song_details(self, song: Song) -> str:
        track_lines = "\n".join(
            f"• {Path(track.file_path).name if track.file_path else track.name}: channel {track.output_channel}, gain {track.gain:.2f}"
            for track in song.tracks
        ) or "• No attached audio files"
        return (
            f"Title: {song.title}\nArtist: {song.artist}\nTempo: {song.tempo} BPM\n"
            f"Key: {song.key}\nTime signature: {song.time_signature}\n"
            f"Notes: {song.notes or 'None'}\n\nTracks:\n{track_lines}"
        )

    def create_song(self) -> None:
        self.new_title.clear()
        self.new_artist.setText("Unknown")
        self.new_tempo.setValue(120)
        self.new_key.setText("C")
        self.new_time_signature.setCurrentText("4/4")
        self.new_notes.clear()
        self.library_right.setCurrentWidget(self.library_editor)
        self.new_title.setFocus()

    def save_new_song(self) -> None:
        title = self.new_title.text().strip()
        if not title:
            QMessageBox.warning(self, "Missing title", "A song title is required.")
            return

        song = Song(
            title=title,
            artist=self.new_artist.text().strip() or "Unknown",
            tempo=self.new_tempo.value(),
            key=self.new_key.text().strip() or "C",
            time_signature=self.new_time_signature.currentText(),
            notes=self.new_notes.text().strip(),
        )
        save_song(song)
        self.refresh_library()
        self.library_right.setCurrentWidget(self.library_details)
        self.library_details.setPlainText(self.format_song_details(song))

    def cancel_new_song(self) -> None:
        self.library_right.setCurrentWidget(self.library_details)

    def delete_selected_song(self) -> None:
        if not self.current_song:
            return
        answer = QMessageBox.question(self, "Delete song", f"Delete '{self.current_song.title}' from the library?")
        if answer == QMessageBox.Yes:
            song_id = self.current_song.id
            delete_song(song_id)
            if song_id is not None:
                remove_song_storage(int(song_id))
            self.current_song = None
            self.library_details.clear()
            self.refresh_library()
            self.refresh_sets()

    def select_library_song(self, row: int) -> None:
        if row < 0:
            return
        item = self.library_list.item(row)
        song_id = item.data(Qt.UserRole)
        songs = list_songs()
        self.current_song = next((song for song in songs if song.id == song_id), None)
        if not self.current_song:
            return

        song = self.current_song
        self.edit_title.setText(song.title)
        self.edit_artist.setText(song.artist)
        self.edit_tempo.setValue(song.tempo)
        self.edit_key.setText(song.key)
        self.edit_time_signature.setCurrentText(song.time_signature or "4/4")
        self.edit_notes.setText(song.notes)
        self.edit_pending_tracks = None
        self.edit_attached_directory = ""

        file_names = [Path(track.file_path).name for track in song.tracks if track.file_path]
        self.edit_wav_directory.setText("\n".join(file_names) if file_names else "No WAV or MP3 files attached")
        self.library_right.setCurrentWidget(self.library_edit_editor)

        self.reset_library_edit_dirty_state()

    def save_library_edits(self) -> None:
        if not self.current_song:
            return

        title = self.edit_title.text().strip()
        if not title:
            QMessageBox.warning(self, "Missing title", "A song title is required.")
            return

        self.current_song.title = title
        self.current_song.artist = self.edit_artist.text().strip() or "Unknown"
        self.current_song.tempo = self.edit_tempo.value()
        self.current_song.key = self.edit_key.text().strip() or "C"
        self.current_song.time_signature = self.edit_time_signature.currentText()
        self.current_song.notes = self.edit_notes.text().strip()

        if getattr(self, "edit_pending_tracks", None) is not None:
            if self.edit_pending_tracks:
                project_root = Path(__file__).resolve().parent.parent
                copied = copy_tracks_to_song_directory(project_root, int(self.current_song.id), self.edit_pending_tracks)
                self.current_song.tracks = copied
            else:
                self.current_song.tracks = []
                clear_song_track_directory(Path(__file__).resolve().parent.parent, int(self.current_song.id))

        save_song(self.current_song)
        self.edit_pending_tracks = None
        file_names = [Path(track.file_path).name for track in self.current_song.tracks if track.file_path]
        self.edit_wav_directory.setText("\n".join(file_names) if file_names else "No WAV or MP3 files attached")

        self.refresh_library()
        self.library_details.setPlainText(self.format_song_details(self.current_song))
        self.library_right.setCurrentWidget(self.library_details)
        self.reset_library_edit_dirty_state()

    def cancel_library_edits(self) -> None:
        if not self.current_song:
            return
        song = self.current_song
        self.edit_title.setText(song.title)
        self.edit_artist.setText(song.artist)
        self.edit_tempo.setValue(song.tempo)
        self.edit_key.setText(song.key)
        self.edit_time_signature.setCurrentText(song.time_signature or "4/4")
        self.edit_notes.setText(song.notes)
        self.edit_pending_tracks = None
        self.edit_attached_directory = ""
        file_names = [Path(track.file_path).name for track in song.tracks if track.file_path]
        self.edit_wav_directory.setText("\n".join(file_names) if file_names else "No WAV or MP3 files attached")
        self.library_right.setCurrentWidget(self.library_details)
        self.reset_library_edit_dirty_state()

    def browse_edit_song_wav_directory(self) -> None:
        directory = QFileDialog.getExistingDirectory(self, "Select WAV directory", "")
        if not directory:
            return

        tracks, mismatches, error = inspect_wav_directory(directory)
        if error:
            QMessageBox.critical(self, "Cannot attach WAV directory", error)
            return

        self.edit_pending_tracks = tracks
        self.edit_attached_directory = str(Path(directory).resolve())
        if mismatches:
            QMessageBox.warning(
                self,
                "WAV lengths do not match",
                "The WAV files in this directory do not have matching lengths.\n\n"
                + "\n".join(mismatches)
                + "\n\nThey will be attached anyway and may not play or loop properly.",
            )

        file_names = [Path(track.file_path).name for track in tracks if track.file_path]
        self.edit_wav_directory.setText("\n".join(file_names) if file_names else "No WAV or MP3 files attached")
        self.update_library_edit_dirty_state()

    def clear_edit_song_wav_directory(self) -> None:
        self.edit_pending_tracks = []
        self.edit_attached_directory = ""
        self.edit_wav_directory.setText("No WAV or MP3 files attached")
        self.update_library_edit_dirty_state()

    def refresh_library(self) -> None:
        self.library_list.blockSignals(True)
        self.library_list.clear()
        for song in list_songs():
            item = QListWidgetItem(song.to_summary())
            item.setData(Qt.UserRole, song.id)
            self.library_list.addItem(item)
        self.library_list.blockSignals(False)

        self.add_song_combo.blockSignals(True)
        self.add_song_combo.clear()
        self.add_song_combo.addItem("Add song", None)
        for song in list_songs():
            self.add_song_combo.addItem(song.to_summary(), song.id)
        self.add_song_combo.blockSignals(False)

    def reset_library_edit_dirty_state(self) -> None:
        if not hasattr(self, "save_edits_button"):
            return
        self.save_edits_button.setStyleSheet("")

        if not self.current_song:
            self.library_edit_baseline = None
            return

        self.library_edit_baseline = (
            self.edit_title.text(),
            self.edit_artist.text(),
            int(self.edit_tempo.value()),
            self.edit_key.text(),
            self.edit_time_signature.currentText(),
            self.edit_notes.text(),
            tuple(Path(track.file_path).name for track in self.current_song.tracks if track.file_path),
        )

    def update_library_edit_dirty_state(self) -> None:
        if not hasattr(self, "save_edits_button") or not hasattr(self, "library_edit_baseline"):
            return

        pending_names = None
        if getattr(self, "edit_pending_tracks", None) is not None:
            pending_names = tuple(Path(track.file_path).name for track in self.edit_pending_tracks if track.file_path)

        current = (
            self.edit_title.text(),
            self.edit_artist.text(),
            int(self.edit_tempo.value()),
            self.edit_key.text(),
            self.edit_time_signature.currentText(),
            self.edit_notes.text(),
            pending_names if pending_names is not None else tuple(
                Path(track.file_path).name
                for track in (self.current_song.tracks if self.current_song else [])
                if track.file_path
            ),
        )

        dirty = current != self.library_edit_baseline
        self.save_edits_button.setStyleSheet(
            "background-color: #2563eb; color: white; font-weight: 700;"
            if dirty else ""
        )
