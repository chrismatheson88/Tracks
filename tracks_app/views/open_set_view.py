from __future__ import annotations

from PySide6.QtCore import Qt, QSize, QDate, QTime
from PySide6.QtWidgets import (
    QAbstractItemView,
    QCompleter,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QSlider,
    QSpinBox,
    QSplitter,
    QVBoxLayout,
    QWidget,
    QMessageBox,
    QDateEdit,
    QFormLayout,
    QTimeEdit,
)

from ..db import (
    delete_setlist,
    get_app_setting,
    list_setlists,
    load_setlist,
    save_setlist,
    save_song,
    set_app_setting,
)
from ..models import Setlist, SetlistEntry, Song
from ..playback import PlaybackEngine, list_audio_devices
from ..services.set_logic import entry_to_summary, reorder_set_entries, song_with_entry_overrides
from ..widgets.beat_playhead import BeatPlayheadWidget
from ..widgets.output_meter import OutputMeterWidget
from ..widgets.transport import transport_icon


class SetViewMixin:
    def previous_set_song(self) -> None:
        row = self.open_set_song_list.currentRow()
        if row > 0:
            was_playing = not self.open_set_play_is_paused
            self.stop_playback()
            self.open_set_song_list.setCurrentRow(row - 1)
            if was_playing:
                self.play_open_set_song()

    def next_set_song(self) -> None:
        row = self.open_set_song_list.currentRow()
        if row >= 0 and row + 1 < self.open_set_song_list.count():
            was_playing = not self.open_set_play_is_paused
            self.stop_playback()
            self.open_set_song_list.setCurrentRow(row + 1)
            if was_playing:
                self.play_open_set_song()

    def set_play_pause_state(self, paused: bool) -> None:
        self.open_set_play_is_paused = paused
        if paused:
            self.open_set_play.setIcon(transport_icon("play", 48))
            self.open_set_play.setToolTip("Play")
        else:
            self.open_set_play.setIcon(transport_icon("pause", 48))
            self.open_set_play.setToolTip("Pause")

    def stop_playback(self) -> None:
        if self.player:
            self.player.stop()
            self.player = None
        if hasattr(self, "open_set_play"):
            self.set_play_pause_state(True)
        if hasattr(self, "beat_playhead"):
            self.beat_playhead.update()
        self.update_output_meter()

    def play_open_set_song(self) -> None:
        if self.player:
            if not self.open_set_play_is_paused:
                self.player.pause()
                self.set_play_pause_state(True)
            else:
                self.player.resume()
                self.set_play_pause_state(False)
            return

        if not self.current_song:
            QMessageBox.information(self, "No song selected", "Select a song in this set first.")
            return

        saved_output = get_app_setting("audio.output_device", "")
        selected_device = int(saved_output) if saved_output else self._get_system_default_output_device_index()

        output_channels = 2
        try:
            import sounddevice as sd
            if selected_device is not None:
                device_info = sd.query_devices(selected_device)
                output_channels = max(1, min(32, int(device_info.get("max_output_channels", 2))))
            else:
                default_output = sd.default.device[1]
                if default_output is not None:
                    device_info = sd.query_devices(default_output)
                    output_channels = max(1, min(32, int(device_info.get("max_output_channels", 2))))
        except Exception:
            output_channels = 2

        try:
            self.player = PlaybackEngine(self.current_song, output_channels=output_channels, device_index=selected_device)
            self.player.click_gain = self.db_to_linear(self.click_fader.value())
            self.player.guide_gain = self.db_to_linear(self.guide_fader.value())
            self.player.master_gain = self.db_to_linear(self.master_fader.value())
            self.player.start()
            self.set_play_pause_state(False)
        except Exception as exc:
            self.player = None
            QMessageBox.critical(self, "Playback error", str(exc))

    def add_song_combo_selected(self, index: int) -> None:
        if index <= 0:
            return
        song_id = self.add_song_combo.itemData(index)
        if song_id is None:
            return
        self.add_song_to_open_set()

    def persist_open_set_order(self, *args) -> None:
        if not self.current_set:
            return

        ordered_entry_ids = []
        for row in range(self.open_set_song_list.count()):
            item = self.open_set_song_list.item(row)
            ordered_entry_ids.append(item.data(Qt.UserRole))
        reorder_set_entries(self.current_set, ordered_entry_ids)
        save_setlist(self.current_set)
        self.refresh_sets()

    def delete_current_open_set(self) -> None:
        if not self.current_set:
            QMessageBox.information(self, "No set selected", "Open a set before deleting.")
            return

        answer = QMessageBox.question(self, "Delete set", f"Delete '{self.current_set.name}'?")
        if answer != QMessageBox.Yes:
            return

        delete_setlist(self.current_set.id)
        self.current_set = None
        self.current_set_entry = None
        self.current_song = None
        self.refresh_open_set_datetime()
        self.refresh_sets()
        self.show_sets_view()

    def edit_current_set_info(self) -> None:
        if not self.current_set:
            QMessageBox.information(self, "No set selected", "Open a set before editing set info.")
            return

        dialog = QDialog(self)
        dialog.setWindowTitle("Edit set info")
        dialog.setModal(True)
        layout = QVBoxLayout(dialog)
        form = QFormLayout()

        name_input = QLineEdit(self.current_set.name)
        parsed_date = QDate.fromString(self.current_set.set_date, "dd-MM-yyyy")
        if not parsed_date.isValid():
            parsed_date = QDate.fromString(self.current_set.set_date, "yyyy-MM-dd")
        if not parsed_date.isValid():
            parsed_date = QDate.currentDate()

        parsed_time = QTime.fromString(self.current_set.set_time, "HH:mm")
        if not parsed_time.isValid():
            parsed_time = QTime.currentTime()

        date_input = QDateEdit(parsed_date)
        date_input.setCalendarPopup(True)
        date_input.setDisplayFormat("dd-MM-yyyy")
        time_input = QTimeEdit(parsed_time)
        time_input.setDisplayFormat("HH:mm")

        form.addRow("Set name:", name_input)
        form.addRow("Date:", date_input)
        form.addRow("Time:", time_input)
        layout.addLayout(form)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        layout.addWidget(buttons)

        name_input.setFocus()
        if dialog.exec() != QDialog.Accepted:
            return

        name = name_input.text().strip()
        if not name:
            QMessageBox.warning(self, "Missing name", "A set name is required.")
            return

        self.current_set.name = name
        self.current_set.set_date = date_input.date().toString("dd-MM-yyyy")
        self.current_set.set_time = time_input.time().toString("HH:mm")
        save_setlist(self.current_set)

        self.open_set_title.setText(self.current_set.name)
        self.refresh_sets()
        self.refresh_open_set_datetime()

    def create_set(self) -> None:
        dialog = QDialog(self)
        dialog.setWindowTitle("Add new set")
        dialog.setModal(True)
        layout = QVBoxLayout(dialog)
        form = QFormLayout()

        name_input = QLineEdit()
        date_input = QDateEdit(QDate.currentDate())
        date_input.setCalendarPopup(True)
        date_input.setDisplayFormat("dd-MM-yyyy")
        time_input = QTimeEdit(QTime.currentTime())
        time_input.setDisplayFormat("HH:mm")

        form.addRow("Set name:", name_input)
        form.addRow("Date:", date_input)
        form.addRow("Time:", time_input)
        layout.addLayout(form)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        layout.addWidget(buttons)

        name_input.setFocus()
        if dialog.exec() != QDialog.Accepted:
            return

        name = name_input.text().strip()
        if not name:
            QMessageBox.warning(self, "Missing name", "A set name is required.")
            return

        saved = save_setlist(Setlist(
            name=name,
            set_date=date_input.date().toString("dd-MM-yyyy"),
            set_time=time_input.time().toString("HH:mm"),
        ))
        self.refresh_sets()
        self.open_set(saved.id)

    def open_selected_set(self) -> None:
        item = self.set_list.currentItem()
        if not item:
            return
        self.open_set(item.data(Qt.UserRole))

    def open_set_item(self, item) -> None:
        self.open_set(item.data(Qt.UserRole))

    def refresh_sets(self) -> None:
        self.set_list.clear()
        for setlist in list_setlists():
            label = setlist.name
            if setlist.entries:
                label += " — " + ", ".join(entry.song.title for entry in setlist.entries if entry.song)
            item = QListWidgetItem(label)
            item.setData(Qt.UserRole, setlist.id)
            self.set_list.addItem(item)
        self.refresh_open_set_picker()

    def open_set_from_picker(self, index: int) -> None:
        if index < 0:
            return
        set_id = self.open_set_picker.itemData(index)
        if set_id is None:
            return
        set_id_int = int(set_id)
        self._update_open_set_picker_collapsed_text()
        if self.current_set and self.current_set.id == set_id_int:
            return
        self.open_set(set_id_int)

    def _set_set_song_controls_enabled(self, enabled: bool) -> None:
        self.set_song_tempo.setEnabled(enabled)
        self.set_song_key.setEnabled(enabled)
        self.set_song_time_signature.setEnabled(enabled)
        self.reset_set_song_button.setEnabled(enabled)
        self.save_set_song_to_library_button.setEnabled(enabled)

    def song_with_entry_overrides(self, entry: SetlistEntry) -> Song:
        return song_with_entry_overrides(entry)

    def entry_to_summary(self, entry: SetlistEntry) -> str:
        return entry_to_summary(entry)

    def load_set_song_controls_from_entry(self, entry: SetlistEntry) -> None:
        song = entry.song
        if song is None:
            return
        tempo_value = entry.override_tempo if entry.override_tempo is not None else song.tempo
        key_value = entry.override_key if entry.override_key else song.key
        time_signature_value = entry.override_time_signature if entry.override_time_signature else song.time_signature

        self.set_song_tempo.blockSignals(True)
        self.set_song_key.blockSignals(True)
        self.set_song_time_signature.blockSignals(True)
        self.set_song_tempo.setValue(int(tempo_value))
        self.set_song_key.setText(str(key_value or "C"))
        self.set_song_time_signature.setCurrentText(str(time_signature_value or "4/4"))
        self.set_song_tempo.blockSignals(False)
        self.set_song_key.blockSignals(False)
        self.set_song_time_signature.blockSignals(False)

    def save_current_set_entry_overrides(self, *_args) -> None:
        if not self.current_set or not self.current_set_entry or not self.current_set_entry.song:
            return

        base_song = self.current_set_entry.song
        override_tempo = int(self.set_song_tempo.value())
        override_key = self.set_song_key.text().strip() or "C"
        override_time_signature = self.set_song_time_signature.currentText().strip() or "4/4"

        self.current_set_entry.override_tempo = None if override_tempo == int(base_song.tempo) else override_tempo
        self.current_set_entry.override_key = None if override_key == str(base_song.key) else override_key
        self.current_set_entry.override_time_signature = None if override_time_signature == str(base_song.time_signature) else override_time_signature
        self.beat_playhead.update()

        save_setlist(self.current_set)
        row = self.open_set_song_list.currentRow()
        if 0 <= row < self.open_set_song_list.count():
            self.open_set_song_list.item(row).setText(self.entry_to_summary(self.current_set_entry))

        self.current_song = self.song_with_entry_overrides(self.current_set_entry)
        self._apply_current_song_to_player()

    def _apply_current_song_to_player(self) -> None:
        if self.player and self.current_song:
            self.player.song.tempo = int(self.current_song.tempo)
            self.player.song.key = str(self.current_song.key)
            self.player.song.time_signature = str(self.current_song.time_signature)

    def refresh_open_set_song_labels(self) -> None:
        if not self.current_set:
            return
        for row, entry in enumerate(self.current_set.entries):
            if row < self.open_set_song_list.count():
                self.open_set_song_list.item(row).setText(self.entry_to_summary(entry))

    def reset_current_set_entry_to_library_defaults(self) -> None:
        if not self.current_set or not self.current_set_entry or not self.current_set_entry.song:
            return
        self.current_set_entry.override_tempo = None
        self.current_set_entry.override_key = None
        self.current_set_entry.override_time_signature = None
        save_setlist(self.current_set)
        self.load_set_song_controls_from_entry(self.current_set_entry)
        self.current_song = self.song_with_entry_overrides(self.current_set_entry)
        self.refresh_open_set_song_labels()
        self.beat_playhead.update()
        self._apply_current_song_to_player()

    def save_current_set_entry_values_to_library(self) -> None:
        if not self.current_set or not self.current_set_entry or not self.current_set_entry.song:
            return

        library_song = self.current_set_entry.song
        prior_entry_values: dict[int, tuple[int, str, str]] = {}
        prior_entry_was_edited: dict[int, bool] = {}
        for entry in self.current_set.entries:
            if entry.song_id != library_song.id:
                continue
            key = entry.id if entry.id is not None else id(entry)
            effective_song = self.song_with_entry_overrides(entry)
            prior_entry_values[key] = (int(effective_song.tempo), str(effective_song.key), str(effective_song.time_signature))
            prior_entry_was_edited[key] = any(value is not None for value in (entry.override_tempo, entry.override_key, entry.override_time_signature))

        library_song.tempo = int(self.set_song_tempo.value())
        library_song.key = self.set_song_key.text().strip() or "C"
        library_song.time_signature = self.set_song_time_signature.currentText().strip() or "4/4"
        save_song(library_song)
        save_setlist(self.current_set)
        if hasattr(self, "refresh_library"):
            self.refresh_library()

    def set_live_click_db(self, db: int) -> None:
        if self.player:
            self.player.click_gain = self.db_to_linear(db)

    def set_live_guide_db(self, db: int) -> None:
        if self.player:
            self.player.guide_gain = self.db_to_linear(db)

    def set_live_master_db(self, db: int) -> None:
        if self.player:
            self.player.master_gain = self.db_to_linear(db)

    def set_live_click_channel(self, index: int) -> None:
        if index < 0:
            return
        channel = int(self.click_channel_combo.itemData(index))
        set_app_setting("volume.channel.click_db", str(channel))
        if self.player:
            self.player.click_channel = channel

    def set_live_guide_channel(self, index: int) -> None:
        if index < 0:
            return
        channel = int(self.guide_channel_combo.itemData(index))
        set_app_setting("volume.channel.guide_db", str(channel))
        if self.player:
            self.player.guide_channel = channel


def build_open_set_view(window) -> QWidget:
    view = QWidget()
    splitter = QSplitter(Qt.Horizontal)

    left = QWidget()
    left_layout = QVBoxLayout(left)
    window.open_set_title = QLabel("Opened set")
    window.open_set_title.setStyleSheet("font-size: 20px; font-weight: 700;")
    left_layout.addWidget(window.open_set_title)

    window.open_set_song_list = QListWidget()
    window.open_set_song_list.setSelectionMode(QAbstractItemView.SingleSelection)
    window.open_set_song_list.setDragEnabled(True)
    window.open_set_song_list.setAcceptDrops(True)
    window.open_set_song_list.setDragDropMode(QAbstractItemView.InternalMove)
    window.open_set_song_list.setDefaultDropAction(Qt.MoveAction)
    window.open_set_song_list.model().rowsMoved.connect(window.persist_open_set_order)
    window.open_set_song_list.currentRowChanged.connect(window.select_set_song)
    left_layout.addWidget(window.open_set_song_list)

    window.add_song_combo = QComboBox()
    window.add_song_combo.setInsertPolicy(QComboBox.NoInsert)
    window.add_song_combo.setEditable(True)
    window.add_song_combo.setDuplicatesEnabled(False)
    window.add_song_combo.setPlaceholderText("Search songs to add")
    window.add_song_combo.lineEdit().setPlaceholderText("Search songs by name")
    completer = window.add_song_combo.completer()
    completer.setCompletionMode(QCompleter.PopupCompletion)
    completer.setFilterMode(Qt.MatchContains)
    completer.setCaseSensitivity(Qt.CaseInsensitive)
    left_layout.addWidget(window.add_song_combo)

    window.add_new_song_to_set_button = QPushButton("Add new song")
    left_layout.addWidget(window.add_new_song_to_set_button)

    remove_song_button = QPushButton("Remove song")
    left_layout.addWidget(remove_song_button)

    transport = QHBoxLayout()
    window.open_set_previous = QPushButton()
    window.open_set_play = QPushButton()
    window.open_set_stop = QPushButton()
    window.open_set_next = QPushButton()

    window.open_set_previous.setIcon(transport_icon("previous", 48))
    window.open_set_play.setIcon(transport_icon("play", 48))
    window.open_set_stop.setIcon(transport_icon("stop", 48))
    window.open_set_next.setIcon(transport_icon("next", 48))

    for transport_button in (window.open_set_previous, window.open_set_play, window.open_set_stop, window.open_set_next):
        transport_button.setIconSize(QSize(48, 48))
        transport_button.setFixedSize(64, 64)

    window.open_set_previous.setToolTip("Previous song")
    window.open_set_play.setToolTip("Play")
    window.open_set_stop.setToolTip("Stop")
    window.open_set_next.setToolTip("Next song")
    window.open_set_play_is_paused = True

    transport.addWidget(window.open_set_previous)
    transport.addWidget(window.open_set_play)
    transport.addWidget(window.open_set_stop)
    transport.addWidget(window.open_set_next)

    back_button = QPushButton("Back to Sets")

    right = QWidget()
    right_layout = QVBoxLayout(right)

    set_song_row = QHBoxLayout()
    set_song_row.addWidget(QLabel("Tempo:"))
    window.set_song_tempo = QSpinBox()
    window.set_song_tempo.setRange(40, 260)
    window.set_song_tempo.setFixedWidth(80)
    set_song_row.addWidget(window.set_song_tempo)

    set_song_row.addWidget(QLabel("Key:"))
    window.set_song_key = QLineEdit()
    window.set_song_key.setMaxLength(12)
    window.set_song_key.setFixedWidth(90)
    set_song_row.addWidget(window.set_song_key)

    set_song_row.addWidget(QLabel("Time signature:"))
    window.set_song_time_signature = QComboBox()
    window.set_song_time_signature.addItems(["4/4", "3/4", "6/8", "12/8", "2/2", "5/4"])
    window.set_song_time_signature.setFixedWidth(90)
    set_song_row.addWidget(window.set_song_time_signature)

    window.reset_set_song_button = QPushButton("Reset to library defaults")
    window.save_set_song_to_library_button = QPushButton("Save to library")
    set_song_row.addWidget(window.reset_set_song_button)
    set_song_row.addWidget(window.save_set_song_to_library_button)
    set_song_row.addStretch()
    right_layout.addLayout(set_song_row)

    window.set_song_tempo.valueChanged.connect(window.save_current_set_entry_overrides)
    window.set_song_key.textChanged.connect(window.save_current_set_entry_overrides)
    window.set_song_time_signature.currentIndexChanged.connect(window.save_current_set_entry_overrides)
    window.reset_set_song_button.clicked.connect(window.reset_current_set_entry_to_library_defaults)
    window.save_set_song_to_library_button.clicked.connect(window.save_current_set_entry_values_to_library)

    right_layout.addWidget(QLabel("Playback"))
    right_layout.addLayout(transport)
    window.beat_playhead = BeatPlayheadWidget(window)
    right_layout.addWidget(window.beat_playhead)

    def add_fader(label_text: str, setting_key: str, with_channel: bool = True) -> tuple[QSlider, QLabel, QComboBox | None]:
        row = QHBoxLayout()
        label = QLabel(label_text)
        label.setFixedWidth(70)
        slider = QSlider(Qt.Horizontal)
        slider.setRange(-30, 12)
        slider.setValue(int(get_app_setting(setting_key, "0") or "0"))
        slider.setTickPosition(QSlider.TicksBelow)
        slider.setTickInterval(12)
        value_label = QLabel(f"{slider.value():+d} dB")
        row.addWidget(label)
        row.addWidget(slider)
        row.addWidget(value_label)

        channel = None
        channel_width = 65
        if with_channel:
            channel = QComboBox()
            channel.setMinimumWidth(channel_width)
            selected_output = get_app_setting("audio.output_device", "")
            channel_count = 2
            if selected_output:
                for device in list_audio_devices():
                    if str(device["index"]) == selected_output:
                        channel_count = max(1, int(device["max_output_channels"]))
                        break
            for channel_number in range(1, channel_count + 1):
                channel.addItem(str(channel_number), channel_number - 1)

            channel_key = setting_key.replace("volume.", "volume.channel.")
            saved_channel = int(get_app_setting(channel_key, "0") or "0")
            channel_index = channel.findData(saved_channel)
            if channel_index >= 0:
                channel.setCurrentIndex(channel_index)
            row.addWidget(channel)
        else:
            row.addSpacing(channel_width)
        right_layout.addLayout(row)

        def update_label(value: int) -> None:
            value_label.setText(f"{value:+d} dB")
            set_app_setting(setting_key, str(value))

        slider.valueChanged.connect(update_label)
        return slider, value_label, channel

    window.click_fader, window.click_fader_label, window.click_channel_combo = add_fader("Click", "volume.click_db", with_channel=True)
    window.guide_fader, window.guide_fader_label, window.guide_channel_combo = add_fader("Guide", "volume.guide_db", with_channel=True)
    window.master_fader, window.master_fader_label, _ = add_fader("Master", "volume.master_db", with_channel=False)

    window.click_fader.valueChanged.connect(lambda db: window.set_live_click_db(db))
    window.guide_fader.valueChanged.connect(lambda db: window.set_live_guide_db(db))
    window.master_fader.valueChanged.connect(lambda db: window.set_live_master_db(db))
    window.click_channel_combo.currentIndexChanged.connect(window.set_live_click_channel)
    window.guide_channel_combo.currentIndexChanged.connect(window.set_live_guide_channel)

    right_layout.addWidget(QLabel("Output meter (post-mix)"))
    window.output_meter_widget = OutputMeterWidget()
    right_layout.addWidget(window.output_meter_widget)
    window.output_meter_widget.ensure_channels(window._get_selected_output_channel_count())
    right_layout.addStretch()

    splitter.addWidget(left)
    splitter.addWidget(right)
    splitter.setSizes([360, 620])

    page_layout = QVBoxLayout(view)
    page_layout.setContentsMargins(0, 0, 0, 0)
    page_layout.addWidget(splitter)

    remove_song_button.clicked.connect(window.remove_song_from_open_set)
    window.add_new_song_to_set_button.clicked.connect(window.add_new_song_from_open_set)
    window.add_song_combo.currentIndexChanged.connect(window.add_song_combo_selected)
    back_button.clicked.connect(window.show_sets_view)
    left_layout.addWidget(back_button)
    window.open_set_previous.clicked.connect(window.previous_set_song)
    window.open_set_play.clicked.connect(window.play_open_set_song)
    window.open_set_stop.clicked.connect(window.stop_playback)
    window.open_set_next.clicked.connect(window.next_set_song)
    window._set_set_song_controls_enabled(False)
    window.playhead_timer.timeout.connect(window.update_beat_playhead)
    window.playhead_timer.start()
    return view
