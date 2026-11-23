from __future__ import annotations

import math
import sys
from pathlib import Path
import shutil

from PySide6.QtCore import Qt, QTimer, QPoint, QDate, QTime, QSize
from PySide6.QtGui import QColor, QPainter, QPen, QIcon, QPixmap
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from .db import (
    get_app_setting,
    set_app_setting,
    list_setlists,
    load_setlist,
)
from .models import Setlist, SetlistEntry, Song
from .playback import PlaybackEngine, inspect_wav_directory, list_audio_devices
from .widgets.beat_playhead import BeatPlayheadWidget
from .widgets.transport import transport_icon
from .widgets.output_meter import OutputMeterWidget
from .views.library_view import LibraryViewMixin, build_library_view, build_sets_view
from .views.open_set_view import SetViewMixin, build_open_set_view
from .views.settings_views import SettingsViewMixin, build_settings_view, build_import_click_view, build_import_voice_view
from .services.audio_controls import db_to_linear, effective_set_entry_values
from .services.set_logic import (
    entry_to_summary,
    format_set_date_for_display,
    reorder_set_entries,
    set_picker_list_label,
    setlists_sorted_by_datetime_desc,
    song_with_entry_overrides,
)




class TracksDesktopWindow(LibraryViewMixin, SetViewMixin, SettingsViewMixin, QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Tracks")
        self.resize(1100, 720)
        self.current_song: Song | None = None
        self.current_set: Setlist | None = None
        self.current_set_entry: SetlistEntry | None = None
        self.player: PlaybackEngine | None = None
        self.playhead_timer = QTimer(self)
        self.playhead_timer.setInterval(30)
        self.edit_pending_tracks = None
        self.edit_attached_directory = ""

        shell = QWidget(self)
        shell_layout = QVBoxLayout(shell)
        shell_layout.setContentsMargins(18, 18, 18, 18)

        title = QLabel("Tracks")
        title.setStyleSheet("font-size: 28px; font-weight: 700;")
        shell_layout.addWidget(title)

        nav = QHBoxLayout()
        self.library_nav = QPushButton("Library")
        self.sets_nav = QPushButton("Sets")
        self.settings_nav = QPushButton("Settings")
        nav.addWidget(self.library_nav)
        nav.addWidget(self.sets_nav)
        nav.addWidget(self.settings_nav)

        self.set_controls_group = QGroupBox("Set controls")
        set_controls_row = QHBoxLayout(self.set_controls_group)
        set_controls_row.setContentsMargins(8, 8, 8, 8)
        set_controls_row.addWidget(QLabel("Set:"))
        self.open_set_picker = QComboBox()
        self.open_set_picker.setMinimumWidth(260)
        self.open_set_picker.setEditable(True)
        self.open_set_picker.lineEdit().setReadOnly(True)
        set_controls_row.addWidget(self.open_set_picker)
        self.open_set_datetime_label = QLabel("Date/time: -")
        set_controls_row.addWidget(self.open_set_datetime_label)
        self.open_set_edit_info_button = QPushButton("Edit set info")
        set_controls_row.addWidget(self.open_set_edit_info_button)
        self.open_set_add_button = QPushButton("Add new set")
        self.open_set_delete_button = QPushButton("Delete set")
        set_controls_row.addWidget(self.open_set_add_button)
        set_controls_row.addWidget(self.open_set_delete_button)
        nav.addWidget(self.set_controls_group)

        nav.addStretch()
        shell_layout.addLayout(nav)

        self.views = QStackedWidget()
        shell_layout.addWidget(self.views)

        self.views.addWidget(build_library_view(self))
        self.views.addWidget(build_sets_view(self))
        self.views.addWidget(build_open_set_view(self))
        self.views.addWidget(build_settings_view(self))
        self.views.addWidget(build_import_click_view(self))
        self.views.addWidget(build_import_voice_view(self))

        self.library_nav.clicked.connect(lambda: self.views.setCurrentIndex(0))
        self.sets_nav.clicked.connect(self.show_sets_view)
        self.settings_nav.clicked.connect(self.show_settings_view)
        self.open_set_picker.currentIndexChanged.connect(self.open_set_from_picker)
        self.open_set_edit_info_button.clicked.connect(self.edit_current_set_info)
        self.open_set_add_button.clicked.connect(self.create_set)
        self.open_set_delete_button.clicked.connect(self.delete_current_open_set)
        self.views.currentChanged.connect(self.update_set_controls_visibility)
        self.setCentralWidget(shell)

        self.refresh_library()
        self.refresh_sets()
        self.update_set_controls_visibility(self.views.currentIndex())

    def update_set_controls_visibility(self, view_index: int) -> None:
        # Hide set-level controls on the Library page.
        self.set_controls_group.setVisible(view_index != 0)

    def update_beat_playhead(self) -> None:
        if hasattr(self, "beat_playhead"):
            self.beat_playhead.update()
        self.update_output_meter()

    def _ensure_output_meter_rows(self, channel_count: int) -> None:
        if hasattr(self, "output_meter_widget"):
            self.output_meter_widget.ensure_channels(channel_count)

    def update_output_meter(self) -> None:
        if not hasattr(self, "output_meter_widget"):
            return
        if not self.player:
            self.output_meter_widget.set_idle(self._get_selected_output_channel_count())
            return
        self.output_meter_widget.set_peaks(self.player.get_output_peaks())

    def show_sets_view(self) -> None:
        if self.views.currentIndex() == 2:
            return
        self.show_last_opened_set_view()

    def _get_last_opened_set_id(self) -> int | None:
        raw_value = get_app_setting("sets.last_opened_set_id", "")
        try:
            set_id = int(raw_value)
            return set_id if set_id > 0 else None
        except (TypeError, ValueError):
            return None

    def _set_last_opened_set_id(self, set_id: int | None) -> None:
        set_app_setting("sets.last_opened_set_id", str(set_id) if set_id is not None else "")

    def show_last_opened_set_view(self) -> None:
        self.stop_playback()
        self.refresh_sets()

        setlists = list_setlists()
        if not setlists:
            self._set_last_opened_set_id(None)
            self.current_set = None
            self.current_set_entry = None
            self.current_song = None
            self.open_set_title.setText("Opened set")
            self.open_set_song_list.clear()
            self._set_set_song_controls_enabled(False)
            self.set_play_pause_state(True)
            self.refresh_open_set_datetime()
            if hasattr(self, "beat_playhead"):
                self.beat_playhead.update()
            self.views.setCurrentIndex(2)
            return

        last_opened_set_id = self._get_last_opened_set_id()
        selected_set = next((setlist for setlist in setlists if setlist.id == last_opened_set_id), None)
        if selected_set is None:
            selected_set = setlists[-1]
        self.open_set(selected_set.id)

    def show_settings_view(self) -> None:
        self.refresh_output_device_list()
        self.refresh_click_families()
        self.refresh_voice_families()
        self.views.setCurrentIndex(3)

    def refresh_open_set_picker(self) -> None:
        if not hasattr(self, "open_set_picker"):
            return

        selected_set_id = self.current_set.id if self.current_set else None
        self.open_set_picker.blockSignals(True)
        self.open_set_picker.clear()
        for setlist in self._setlists_sorted_by_datetime_desc(list_setlists()):
            list_label = self._set_picker_list_label(setlist)
            self.open_set_picker.addItem(list_label, setlist.id)
            row_index = self.open_set_picker.count() - 1
            self.open_set_picker.setItemData(row_index, setlist.name, Qt.UserRole + 1)

        if selected_set_id is not None:
            selected_index = self.open_set_picker.findData(selected_set_id)
            if selected_index >= 0:
                self.open_set_picker.setCurrentIndex(selected_index)
            else:
                last_opened_set_id = self._get_last_opened_set_id()
                last_opened_index = self.open_set_picker.findData(last_opened_set_id)
                if last_opened_index >= 0:
                    self.open_set_picker.setCurrentIndex(last_opened_index)
        self._update_open_set_picker_collapsed_text()
        self.open_set_picker.blockSignals(False)
        self.refresh_open_set_datetime()

    def _setlists_sorted_by_datetime_desc(self, setlists: list[Setlist]) -> list[Setlist]:
        return setlists_sorted_by_datetime_desc(setlists)

    def _set_picker_list_label(self, setlist: Setlist) -> str:
        return set_picker_list_label(setlist)

    def _update_open_set_picker_collapsed_text(self) -> None:
        if not hasattr(self, "open_set_picker"):
            return
        if self.open_set_picker.count() == 0:
            self.open_set_picker.setEditText("")
            return

        current_index = self.open_set_picker.currentIndex()
        if current_index < 0:
            self.open_set_picker.setEditText("")
            return

        name_only = self.open_set_picker.itemData(current_index, Qt.UserRole + 1)
        if not name_only:
            name_only = self.open_set_picker.itemText(current_index)
        self.open_set_picker.setEditText(str(name_only))

    def refresh_open_set_datetime(self) -> None:
        if not hasattr(self, "open_set_datetime_label"):
            return
        if not self.current_set:
            self.open_set_datetime_label.setText("Date/time: -")
            return

        formatted_date = self.format_set_date_for_display(self.current_set.set_date)
        parts = [part for part in (formatted_date, self.current_set.set_time) if part]
        date_time_text = " ".join(parts) if parts else "-"
        self.open_set_datetime_label.setText(f"Date/time: {date_time_text}")

    @staticmethod
    def format_set_date_for_display(set_date: str) -> str:
        return format_set_date_for_display(set_date)

    def open_set(self, set_id: int) -> None:
        setlist = load_setlist(set_id)
        if setlist is None:
            return
        self.stop_playback()
        self.current_set = setlist
        self._set_last_opened_set_id(set_id)
        self.current_set_entry = None
        self.refresh_open_set_datetime()
        self.refresh_open_set_picker()
        selected_index = self.open_set_picker.findData(set_id)
        if selected_index >= 0:
            self.open_set_picker.blockSignals(True)
            self.open_set_picker.setCurrentIndex(selected_index)
            self._update_open_set_picker_collapsed_text()
            self.open_set_picker.blockSignals(False)
        self.open_set_title.setText(setlist.name)
        self.open_set_song_list.blockSignals(True)
        self.open_set_song_list.clear()
        for entry in setlist.entries:
            if entry.song:
                item = QListWidgetItem(self.entry_to_summary(entry))
                item.setData(Qt.UserRole, entry.id)
                self.open_set_song_list.addItem(item)
        self.open_set_song_list.blockSignals(False)
        if self.open_set_song_list.count():
            self.open_set_song_list.setCurrentRow(0)
            self.select_set_song(0)
        else:
            self.current_set_entry = None
            self.current_song = None
            self._set_set_song_controls_enabled(False)
            self.set_play_pause_state(True)
            if hasattr(self, "beat_playhead"):
                self.beat_playhead.update()
        self.views.setCurrentIndex(2)

    def select_set_song(self, row: int) -> None:
        if row < 0 or not self.current_set or row >= len(self.current_set.entries):
            self.current_set_entry = None
            self.current_song = None
            self._set_set_song_controls_enabled(False)
            if hasattr(self, "beat_playhead"):
                self.beat_playhead.update()
            return
        entry = self.current_set.entries[row]
        if not entry.song:
            self.current_set_entry = None
            self.current_song = None
            self._set_set_song_controls_enabled(False)
            if hasattr(self, "beat_playhead"):
                self.beat_playhead.update()
            return

        self.current_set_entry = entry
        self.current_song = self.song_with_entry_overrides(entry)
        self.load_set_song_controls_from_entry(entry)
        self._set_set_song_controls_enabled(True)
        if hasattr(self, "beat_playhead"):
            self.beat_playhead.update()

    @staticmethod
    def db_to_linear(*args) -> float:
        return db_to_linear(args[-1])

    def song_with_entry_overrides(self, entry: SetlistEntry) -> Song:
        return song_with_entry_overrides(entry)

    def entry_to_summary(self, entry: SetlistEntry) -> str:
        return entry_to_summary(entry)

    def save_click_family(self, index: int) -> None:
        if index >= 0:
            paths = self.click_family.itemData(index)
            if isinstance(paths, tuple) and len(paths) == 2:
                set_app_setting("click.accent_wav", paths[0])
                set_app_setting("click.normal_wav", paths[1])

    def save_countin_voice(self, index: int) -> None:
        if index >= 0:
            voice = self.voice_family.itemText(index)
            set_app_setting("countin.voice", voice)

    def set_play_pause_state(self, paused: bool) -> None:
        self.open_set_play_is_paused = paused
        if paused:
            self.open_set_play.setIcon(transport_icon("play", 48))
            self.open_set_play.setToolTip("Play")
        else:
            self.open_set_play.setIcon(transport_icon("pause", 48))
            self.open_set_play.setToolTip("Pause")


def launch_desktop_app() -> int:
    app = QApplication.instance() or QApplication(sys.argv)
    window = TracksDesktopWindow()
    window.show()
    window.raise_()
    window.activateWindow()
    return app.exec()
