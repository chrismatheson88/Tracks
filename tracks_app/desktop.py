from __future__ import annotations

import math
import sys
from pathlib import Path
import shutil

from PySide6.QtCore import Qt, QTimer, QPoint, QDate, QTime, QSize
from PySide6.QtGui import QColor, QPainter, QPen, QIcon, QPixmap
from PySide6.QtWidgets import (
    QApplication,
    QAbstractItemView,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDateEdit,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QProgressBar,
    QSlider,
    QSplitter,
    QSpinBox,
    QStackedWidget,
    QTextEdit,
    QTimeEdit,
    QVBoxLayout,
    QWidget,
    QFileDialog,
    QCompleter,
)

from .db import (
    delete_setlist,
    delete_song,
    get_app_setting,
    set_app_setting,
    list_setlists,
    list_songs,
    load_setlist,
    save_setlist,
    save_song,
)
from .models import Setlist, SetlistEntry, Song, SongTrack
from .playback import PlaybackEngine, inspect_wav_directory, list_audio_devices
from .widgets.beat_playhead import BeatPlayheadWidget
from .widgets.transport import transport_icon
from .widgets.output_meter import OutputMeterWidget




class TracksDesktopWindow(QMainWindow):
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

        self.views.addWidget(self.build_library_view())
        self.views.addWidget(self.build_sets_view())
        self.views.addWidget(self.build_open_set_view())
        self.views.addWidget(self.build_settings_view())
        self.views.addWidget(self.build_import_click_view())
        self.views.addWidget(self.build_import_voice_view())

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

    def build_library_view(self) -> QWidget:
        view = QWidget()
        layout = QHBoxLayout(view)

        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.addWidget(QLabel("Library"))
        self.library_list = QListWidget()
        self.library_list.setSelectionMode(QAbstractItemView.SingleSelection)
        self.library_list.currentRowChanged.connect(self.select_library_song)
        left_layout.addWidget(self.library_list)

        actions = QHBoxLayout()
        self.new_song_button = QPushButton("New song")
        self.delete_song_button = QPushButton("Delete song")
        actions.addWidget(self.new_song_button)
        actions.addWidget(self.delete_song_button)
        left_layout.addLayout(actions)

        self.library_details = QTextEdit()
        self.library_details.setReadOnly(True)

        self.library_right = QStackedWidget()
        self.library_right.addWidget(self.library_details)

        self.library_editor = QWidget()
        editor_layout = QVBoxLayout(self.library_editor)
        editor_layout.addWidget(QLabel("New song"))

        self.new_title = QLineEdit()
        self.new_artist = QLineEdit("Unknown")
        self.new_tempo = QSpinBox()
        self.new_tempo.setRange(40, 220)
        self.new_tempo.setValue(120)
        self.new_key = QLineEdit("C")
        self.new_time_signature = QComboBox()
        self.new_time_signature.addItems(["4/4", "3/4", "6/8", "12/8", "2/2", "5/4"])
        self.new_notes = QLineEdit()

        form = QFormLayout()
        form.addRow("Title:", self.new_title)
        form.addRow("Artist:", self.new_artist)
        form.addRow("Tempo (BPM):", self.new_tempo)
        form.addRow("Key:", self.new_key)
        form.addRow("Time signature:", self.new_time_signature)
        form.addRow("Notes:", self.new_notes)
        editor_layout.addLayout(form)

        editor_actions = QHBoxLayout()
        save_button = QPushButton("Save song")
        cancel_button = QPushButton("Cancel")
        editor_actions.addWidget(save_button)
        editor_actions.addWidget(cancel_button)
        editor_layout.addLayout(editor_actions)

        save_button.clicked.connect(self.save_new_song)
        cancel_button.clicked.connect(self.cancel_new_song)
        self.library_right.addWidget(self.library_editor)

        self.library_edit_editor = QWidget()
        edit_layout = QVBoxLayout(self.library_edit_editor)
        edit_layout.setContentsMargins(0, 0, 0, 0)
        edit_layout.setSpacing(8)
        edit_layout.setAlignment(Qt.AlignTop)
        edit_layout.addWidget(QLabel("Edit song"))

        self.edit_title = QLineEdit()
        self.edit_artist = QLineEdit()
        self.edit_tempo = QSpinBox()
        self.edit_tempo.setRange(40, 220)
        self.edit_key = QLineEdit()
        self.edit_time_signature = QComboBox()
        self.edit_time_signature.addItems(["4/4", "3/4", "6/8", "12/8", "2/2", "5/4"])
        self.edit_notes = QLineEdit()

        wav_row = QHBoxLayout()
        self.edit_wav_directory = QLabel("No WAV directory attached")
        self.edit_wav_directory.setWordWrap(True)
        browse_wav_button = QPushButton("Browse for WAV directory")
        clear_wav_button = QPushButton("Clear attached WAV files")
        wav_row.addWidget(self.edit_wav_directory, 1)
        wav_row.addWidget(browse_wav_button)
        wav_row.addWidget(clear_wav_button)

        edit_form = QFormLayout()
        edit_form.setFieldGrowthPolicy(QFormLayout.AllNonFixedFieldsGrow)
        edit_form.setFormAlignment(Qt.AlignTop)
        edit_form.addRow("Title:", self.edit_title)
        edit_form.addRow("Artist:", self.edit_artist)
        edit_form.addRow("Tempo (BPM):", self.edit_tempo)
        edit_form.addRow("Key:", self.edit_key)
        edit_form.addRow("Time signature:", self.edit_time_signature)
        edit_form.addRow("Notes:", self.edit_notes)
        edit_form.addRow("Track WAVs:", wav_row)
        edit_layout.addLayout(edit_form)

        browse_wav_button.clicked.connect(self.browse_edit_song_wav_directory)
        clear_wav_button.clicked.connect(self.clear_edit_song_wav_directory)

        edit_actions = QHBoxLayout()
        save_edits_button = QPushButton("Save edits to library")
        cancel_edits_button = QPushButton("Cancel edits")
        edit_actions.addWidget(save_edits_button)
        edit_actions.addWidget(cancel_edits_button)
        edit_layout.addLayout(edit_actions)

        save_edits_button.clicked.connect(self.save_library_edits)
        cancel_edits_button.clicked.connect(self.cancel_library_edits)
        self.library_right.addWidget(self.library_edit_editor)

        layout.addWidget(left, 1)
        layout.addWidget(self.library_right, 2)

        self.new_song_button.clicked.connect(self.create_song)
        self.delete_song_button.clicked.connect(self.delete_selected_song)
        return view

    def build_sets_view(self) -> QWidget:
        view = QWidget()
        layout = QVBoxLayout(view)

        heading = QLabel("Sets")
        heading.setStyleSheet("font-size: 20px; font-weight: 700;")
        layout.addWidget(heading)

        self.set_list = QListWidget()
        self.set_list.itemDoubleClicked.connect(self.open_set_item)
        layout.addWidget(self.set_list)

        actions = QHBoxLayout()
        self.new_set_button = QPushButton("New set")
        self.open_set_button = QPushButton("Open set")
        self.delete_set_button = QPushButton("Delete set")
        actions.addWidget(self.new_set_button)
        actions.addWidget(self.open_set_button)
        actions.addWidget(self.delete_set_button)
        layout.addLayout(actions)

        self.new_set_button.clicked.connect(self.create_set)
        self.open_set_button.clicked.connect(self.open_selected_set)
        self.delete_set_button.clicked.connect(self.delete_selected_set)
        return view

    def build_open_set_view(self) -> QWidget:
        view = QWidget()
        splitter = QSplitter(Qt.Horizontal)

        left = QWidget()
        left_layout = QVBoxLayout(left)
        self.open_set_title = QLabel("Opened set")
        self.open_set_title.setStyleSheet("font-size: 20px; font-weight: 700;")
        left_layout.addWidget(self.open_set_title)

        self.open_set_song_list = QListWidget()
        self.open_set_song_list.setSelectionMode(QAbstractItemView.SingleSelection)
        self.open_set_song_list.setDragEnabled(True)
        self.open_set_song_list.setAcceptDrops(True)
        self.open_set_song_list.setDragDropMode(QAbstractItemView.InternalMove)
        self.open_set_song_list.setDefaultDropAction(Qt.MoveAction)
        self.open_set_song_list.model().rowsMoved.connect(self.persist_open_set_order)
        self.open_set_song_list.currentRowChanged.connect(self.select_set_song)
        left_layout.addWidget(self.open_set_song_list)

        self.add_song_combo = QComboBox()
        self.add_song_combo.setInsertPolicy(QComboBox.NoInsert)
        self.add_song_combo.setEditable(True)
        self.add_song_combo.setDuplicatesEnabled(False)
        self.add_song_combo.setPlaceholderText("Search songs to add")
        self.add_song_combo.lineEdit().setPlaceholderText("Search songs by name")
        completer = self.add_song_combo.completer()
        completer.setCompletionMode(QCompleter.PopupCompletion)
        completer.setFilterMode(Qt.MatchContains)
        completer.setCaseSensitivity(Qt.CaseInsensitive)
        left_layout.addWidget(self.add_song_combo)

        self.add_new_song_to_set_button = QPushButton("Add new song")
        left_layout.addWidget(self.add_new_song_to_set_button)

        remove_song_button = QPushButton("Remove song")
        left_layout.addWidget(remove_song_button)

        transport = QHBoxLayout()
        self.open_set_previous = QPushButton()
        self.open_set_play = QPushButton()
        self.open_set_stop = QPushButton()
        self.open_set_next = QPushButton()

        self.open_set_previous.setIcon(transport_icon("previous", 48))
        self.open_set_play.setIcon(transport_icon("play", 48))
        self.open_set_stop.setIcon(transport_icon("stop", 48))
        self.open_set_next.setIcon(transport_icon("next", 48))

        for transport_button in (
            self.open_set_previous,
            self.open_set_play,
            self.open_set_stop,
            self.open_set_next,
        ):
            transport_button.setIconSize(QSize(48, 48))
            transport_button.setFixedSize(64, 64)

        self.open_set_previous.setToolTip("Previous song")
        self.open_set_play.setToolTip("Play")
        self.open_set_stop.setToolTip("Stop")
        self.open_set_next.setToolTip("Next song")
        self.open_set_play_is_paused = True

        transport.addWidget(self.open_set_previous)
        transport.addWidget(self.open_set_play)
        transport.addWidget(self.open_set_stop)
        transport.addWidget(self.open_set_next)

        volume_layout = QHBoxLayout()
        volume_layout.addWidget(QLabel("Master volume:"))
        self.master_volume_slider = QSlider(Qt.Horizontal)
        self.master_volume_slider.setRange(0, 100)
        self.master_volume_slider.setValue(100)
        volume_layout.addWidget(self.master_volume_slider)


        back_button = QPushButton("Back to Sets")

        right = QWidget()
        right_layout = QVBoxLayout(right)

        set_song_row = QHBoxLayout()
        set_song_row.addWidget(QLabel("Tempo:"))
        self.set_song_tempo = QSpinBox()
        self.set_song_tempo.setRange(40, 260)
        self.set_song_tempo.setFixedWidth(80)
        set_song_row.addWidget(self.set_song_tempo)

        set_song_row.addWidget(QLabel("Key:"))
        self.set_song_key = QLineEdit()
        self.set_song_key.setMaxLength(12)
        self.set_song_key.setFixedWidth(90)
        set_song_row.addWidget(self.set_song_key)

        set_song_row.addWidget(QLabel("Time signature:"))
        self.set_song_time_signature = QComboBox()
        self.set_song_time_signature.addItems(["4/4", "3/4", "6/8", "12/8", "2/2", "5/4"])
        self.set_song_time_signature.setFixedWidth(90)
        set_song_row.addWidget(self.set_song_time_signature)

        self.reset_set_song_button = QPushButton("Reset to library defaults")
        self.save_set_song_to_library_button = QPushButton("Save to library")
        set_song_row.addWidget(self.reset_set_song_button)
        set_song_row.addWidget(self.save_set_song_to_library_button)
        set_song_row.addStretch()
        right_layout.addLayout(set_song_row)

        self.set_song_tempo.valueChanged.connect(self.save_current_set_entry_overrides)
        self.set_song_key.textChanged.connect(self.save_current_set_entry_overrides)
        self.set_song_time_signature.currentIndexChanged.connect(self.save_current_set_entry_overrides)
        self.reset_set_song_button.clicked.connect(self.reset_current_set_entry_to_library_defaults)
        self.save_set_song_to_library_button.clicked.connect(self.save_current_set_entry_values_to_library)

        right_layout.addWidget(QLabel("Playback"))
        right_layout.addLayout(transport)
        self.beat_playhead = BeatPlayheadWidget(self)
        right_layout.addWidget(self.beat_playhead)

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
                # Keep alignment with rows that include a channel selector.
                row.addSpacing(channel_width)
            right_layout.addLayout(row)

            def update_label(value: int) -> None:
                value_label.setText(f"{value:+d} dB")
                set_app_setting(setting_key, str(value))

            slider.valueChanged.connect(update_label)
            return slider, value_label, channel

        self.click_fader, self.click_fader_label, self.click_channel_combo = add_fader("Click", "volume.click_db", with_channel=True)
        self.guide_fader, self.guide_fader_label, self.guide_channel_combo = add_fader("Guide", "volume.guide_db", with_channel=True)
        self.master_fader, self.master_fader_label, _ = add_fader("Master", "volume.master_db", with_channel=False)

        self.click_fader.valueChanged.connect(lambda db: self.set_live_click_db(db))
        self.guide_fader.valueChanged.connect(lambda db: self.set_live_guide_db(db))
        self.master_fader.valueChanged.connect(lambda db: self.set_live_master_db(db))
        self.click_channel_combo.currentIndexChanged.connect(self.set_live_click_channel)
        self.guide_channel_combo.currentIndexChanged.connect(self.set_live_guide_channel)

        meter_title = QLabel("Output meter (post-mix)")
        meter_title.setStyleSheet("font-weight: 600;")
        right_layout.addWidget(meter_title)

        meter_hint = QLabel("Shows what the app sends per output channel.")
        meter_hint.setStyleSheet("color: #64748b; font-size: 12px;")
        right_layout.addWidget(meter_hint)

        self.output_meter_widget = OutputMeterWidget()
        right_layout.addWidget(self.output_meter_widget)
        self.output_meter_widget.ensure_channels(self._get_selected_output_channel_count())

        right_layout.addStretch()

        splitter.addWidget(left)
        splitter.addWidget(right)
        splitter.setSizes([360, 620])

        page_layout = QVBoxLayout(view)
        page_layout.setContentsMargins(0, 0, 0, 0)
        page_layout.addWidget(splitter)

        remove_song_button.clicked.connect(self.remove_song_from_open_set)
        self.add_new_song_to_set_button.clicked.connect(self.add_new_song_from_open_set)
        self.add_song_combo.currentIndexChanged.connect(self.add_song_combo_selected)
        back_button.clicked.connect(self.show_sets_view)
        left_layout.addWidget(back_button)
        self.open_set_previous.clicked.connect(self.previous_set_song)
        self.open_set_play.clicked.connect(self.play_open_set_song)
        self.open_set_stop.clicked.connect(self.stop_playback)
        self.open_set_next.clicked.connect(self.next_set_song)
        self._set_set_song_controls_enabled(False)
        self.playhead_timer.timeout.connect(self.update_beat_playhead)
        self.playhead_timer.start()
        return view

    def build_settings_view(self) -> QWidget:
        view = QWidget()
        layout = QVBoxLayout(view)

        heading = QLabel("Settings")
        heading.setStyleSheet("font-size: 20px; font-weight: 700;")
        layout.addWidget(heading)

        form = QFormLayout()

        click_dir = Path(__file__).resolve().parent / "assets" / "clicks"
        families = sorted({
            path.stem.split("_", 1)[0]
            for path in click_dir.glob("*.wav")
            if "_" in path.stem
        })

        accent_saved = get_app_setting("click.accent_wav", "")
        saved_family = Path(accent_saved).stem.split("_", 1)[0] if accent_saved else ""

        self.click_family = QComboBox()

        for family in families:
            accent_path = click_dir / f"{family}_accent.wav"
            normal_path = click_dir / f"{family}_normal.wav"
            if accent_path.exists() and normal_path.exists():
                self.click_family.addItem(family, (str(accent_path), str(normal_path)))

        saved_index = self.click_family.findText(saved_family)
        if saved_index >= 0:
            self.click_family.setCurrentIndex(saved_index)

        click_row = QHBoxLayout()
        click_row.addWidget(self.click_family)
        import_button = QPushButton("Import click sound")
        import_button.setMinimumWidth(180)
        click_row.addWidget(import_button)
        form.addRow("Click sound:", click_row)
        import_button.clicked.connect(lambda: self.views.setCurrentIndex(4))

        voice_row = QHBoxLayout()
        countin_dir = Path(__file__).resolve().parent / "assets" / "countin"
        voice_names = sorted(path.name for path in countin_dir.iterdir() if path.is_dir())
        saved_voice = get_app_setting("countin.voice", "female")

        self.voice_family = QComboBox()
        for voice_name in voice_names:
            self.voice_family.addItem(voice_name)

        voice_index = self.voice_family.findText(saved_voice)
        if voice_index >= 0:
            self.voice_family.setCurrentIndex(voice_index)

        voice_row.addWidget(self.voice_family)
        import_voice_button = QPushButton("Import voice")
        import_voice_button.setMinimumWidth(180)
        voice_row.addWidget(import_voice_button)
        form.addRow("Count-in voice:", voice_row)

        self.output_device = QComboBox()
        self.refresh_output_device_list()

        form.addRow("Output device:", self.output_device)
        self.output_device.currentIndexChanged.connect(self.save_output_device)

        import_button.clicked.connect(lambda: self.views.setCurrentIndex(4))
        import_voice_button.clicked.connect(lambda: self.views.setCurrentIndex(5))
        self.voice_family.currentIndexChanged.connect(self.save_countin_voice)

        layout.addLayout(form)
        layout.addStretch()

        self.click_family.currentIndexChanged.connect(self.save_click_family)

        return view

    def build_import_click_view(self) -> QWidget:
        view = QWidget()
        layout = QVBoxLayout(view)

        heading = QLabel("Import click sound")
        heading.setStyleSheet("font-size: 20px; font-weight: 700;")
        layout.addWidget(heading)

        form = QFormLayout()
        self.import_name = QLineEdit()
        self.import_accent_path = QLineEdit()
        self.import_normal_path = QLineEdit()

        accent_select = QPushButton("Select accent WAV")
        normal_select = QPushButton("Select normal WAV")
        accent_row = QHBoxLayout()
        accent_row.addWidget(self.import_accent_path)
        accent_row.addWidget(accent_select)
        normal_row = QHBoxLayout()
        normal_row.addWidget(self.import_normal_path)
        normal_row.addWidget(normal_select)

        form.addRow("Name:", self.import_name)
        form.addRow("Accent WAV:", accent_row)
        form.addRow("Normal WAV:", normal_row)
        layout.addLayout(form)

        actions = QHBoxLayout()
        import_button = QPushButton("Import click sound")
        back_button = QPushButton("Back to Settings")
        actions.addWidget(import_button)
        actions.addWidget(back_button)
        layout.addLayout(actions)
        layout.addStretch()

        accent_select.clicked.connect(self.select_import_accent_file)
        normal_select.clicked.connect(self.select_import_normal_file)
        import_button.clicked.connect(self.import_click_sound)
        back_button.clicked.connect(lambda: self.views.setCurrentIndex(3))

        return view

    def build_import_voice_view(self) -> QWidget:
        view = QWidget()
        layout = QVBoxLayout(view)

        heading = QLabel("Import count-in voice")
        heading.setStyleSheet("font-size: 20px; font-weight: 700;")
        layout.addWidget(heading)

        form = QFormLayout()
        self.import_voice_name = QLineEdit()
        self.import_voice_dir = QLineEdit()
        select_dir_button = QPushButton("Select directory")

        dir_row = QHBoxLayout()
        dir_row.addWidget(self.import_voice_dir)
        dir_row.addWidget(select_dir_button)

        form.addRow("Name:", self.import_voice_name)
        form.addRow("Directory:", dir_row)
        layout.addLayout(form)

        required_files_label = QLabel("The selected directory must contain: one.wav, two.wav, three.wav, four.wav, five.wav, six.wav, seven.wav, eight.wav")
        required_files_label.setWordWrap(True)
        required_files_label.setStyleSheet("color: #64748b; font-size: 12px;")
        layout.addWidget(required_files_label)

        actions = QHBoxLayout()
        import_button = QPushButton("Import voice")
        back_button = QPushButton("Back to Settings")
        actions.addWidget(import_button)
        actions.addWidget(back_button)
        layout.addLayout(actions)
        layout.addStretch()

        select_dir_button.clicked.connect(self.select_import_voice_directory)
        import_button.clicked.connect(self.import_countin_voice)
        back_button.clicked.connect(lambda: self.views.setCurrentIndex(3))

        return view


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
        self.views.setCurrentIndex(3)

    def _get_system_default_output_device_index(self) -> int | None:
        try:
            import sounddevice as sd

            default_device = sd.default.device
            if isinstance(default_device, (tuple, list)):
                if len(default_device) > 1:
                    default_output = default_device[1]
                else:
                    default_output = None
            else:
                default_output = default_device

            if default_output is None:
                return None

            default_output_index = int(default_output)
            return default_output_index if default_output_index >= 0 else None
        except Exception:
            return None

    def _get_selected_output_channel_count(self) -> int:
        selected_output = get_app_setting("audio.output_device", "")
        devices = list_audio_devices()

        if selected_output:
            for device in devices:
                if str(device.get("index")) == str(selected_output):
                    return max(1, min(32, int(device.get("max_output_channels", 2))))

        system_output_index = self._get_system_default_output_device_index()
        if system_output_index is not None:
            for device in devices:
                if int(device.get("index", -1)) == int(system_output_index):
                    return max(1, min(32, int(device.get("max_output_channels", 2))))

        return 2

    def refresh_output_device_list(self) -> None:
        if not hasattr(self, "output_device"):
            return

        devices = list_audio_devices()
        saved_output = get_app_setting("audio.output_device", "")
        system_output_index = self._get_system_default_output_device_index()

        self.output_device.blockSignals(True)
        self.output_device.clear()

        system_label = "Use current system output"
        if system_output_index is not None:
            system_device = next(
                (device for device in devices if int(device["index"]) == system_output_index),
                None,
            )
            if system_device is not None:
                system_label = f"Use current system output ({system_device['name']})"

        self.output_device.addItem(system_label, "")
        for device in devices:
            self.output_device.addItem(f"{device['name']} ({device['max_output_channels']} ch)", str(device["index"]))

        selected_output_index = self.output_device.findData(saved_output)
        self.output_device.setCurrentIndex(selected_output_index if selected_output_index >= 0 else 0)
        self.output_device.blockSignals(False)

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
        def sort_key(setlist: Setlist) -> tuple[QDate, QTime, int]:
            date = QDate.fromString(setlist.set_date or "", "dd-MM-yyyy")
            if not date.isValid():
                date = QDate.fromString(setlist.set_date or "", "yyyy-MM-dd")
            if not date.isValid():
                date = QDate(1900, 1, 1)

            time = QTime.fromString(setlist.set_time or "", "HH:mm")
            if not time.isValid():
                time = QTime(0, 0)

            return (date, time, int(setlist.id or 0))

        return sorted(setlists, key=sort_key, reverse=True)

    def _set_picker_list_label(self, setlist: Setlist) -> str:
        parts = [part for part in (self.format_set_date_for_display(setlist.set_date), setlist.set_time) if part]
        if not parts:
            return setlist.name
        return f"{setlist.name} ({' '.join(parts)})"

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
        if not set_date:
            return ""

        date = QDate.fromString(set_date, "dd-MM-yyyy")
        if date.isValid():
            return date.toString("dd-MM-yyyy")

        legacy_date = QDate.fromString(set_date, "yyyy-MM-dd")
        if legacy_date.isValid():
            return legacy_date.toString("dd-MM-yyyy")

        return set_date

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
        self.library_right.setCurrentWidget(self.library_edit_editor)

    def format_song_details(self, song: Song) -> str:
        track_lines = "\n".join(
            f"• {track.name}: channel {track.output_channel}, gain {track.gain:.2f}"
            for track in song.tracks
        ) or "• No backing tracks"
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
            delete_song(self.current_song.id)
            self.current_song = None
            self.library_details.clear()
            self.refresh_library()
            self.refresh_sets()

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

        saved = save_setlist(
            Setlist(
                name=name,
                set_date=date_input.date().toString("dd-MM-yyyy"),
                set_time=time_input.time().toString("HH:mm"),
            )
        )
        self.refresh_sets()
        self.open_set(saved.id)

    def open_selected_set(self) -> None:
        item = self.set_list.currentItem()
        if not item:
            return
        self.open_set(item.data(Qt.UserRole))

    def open_set_item(self, item: QListWidgetItem) -> None:
        self.open_set(item.data(Qt.UserRole))

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

    def add_song_to_open_set(self) -> None:
        if not self.current_set:
            return
        song_id = self.add_song_combo.currentData()
        if song_id is None:
            return
        self.current_set.entries.append(SetlistEntry(song_id=song_id, order_index=len(self.current_set.entries)))
        save_setlist(self.current_set)
        self.open_set(self.current_set.id)
        self.refresh_sets()
        self.add_song_combo.blockSignals(True)
        self.add_song_combo.setCurrentIndex(0)
        self.add_song_combo.lineEdit().clear()
        self.add_song_combo.blockSignals(False)

    def add_new_song_from_open_set(self) -> None:
        if not self.current_set:
            QMessageBox.information(self, "No set selected", "Open a set before adding a new song.")
            return

        dialog = QDialog(self)
        dialog.setWindowTitle("Add new song")
        dialog.setModal(True)

        layout = QVBoxLayout(dialog)
        form = QFormLayout()

        title_input = QLineEdit()
        artist_input = QLineEdit("Unknown")
        tempo_input = QSpinBox()
        tempo_input.setRange(40, 260)
        tempo_input.setValue(120)
        key_input = QLineEdit("C")
        time_signature_input = QComboBox()
        time_signature_input.addItems(["4/4", "3/4", "6/8", "12/8", "2/2", "5/4"])
        notes_input = QLineEdit()
        notes_input.setMaximumHeight(90)

        form.addRow("Title:", title_input)
        form.addRow("Artist:", artist_input)
        form.addRow("Tempo (BPM):", tempo_input)
        form.addRow("Key:", key_input)
        form.addRow("Time signature:", time_signature_input)
        form.addRow("Notes:", notes_input)
        layout.addLayout(form)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        layout.addWidget(buttons)

        title_input.setFocus()
        if dialog.exec() != QDialog.Accepted:
            return

        title = title_input.text().strip()
        if not title:
            QMessageBox.warning(self, "Missing title", "A song title is required.")
            return

        new_song = Song(
            title=title,
            artist=artist_input.text().strip() or "Unknown",
            tempo=int(tempo_input.value()),
            key=key_input.text().strip() or "C",
            time_signature=time_signature_input.currentText(),
            notes=notes_input.text().strip(),
        )
        saved_song = save_song(new_song)

        self.current_set.entries.append(
            SetlistEntry(song_id=saved_song.id, order_index=len(self.current_set.entries))
        )
        save_setlist(self.current_set)

        self.refresh_library()
        self.refresh_sets()
        self.open_set(self.current_set.id)
        self.open_set_song_list.setCurrentRow(self.open_set_song_list.count() - 1)

    def remove_song_from_open_set(self) -> None:
        if not self.current_set:
            return
        row = self.open_set_song_list.currentRow()
        if row < 0:
            return
        self.current_set.entries.pop(row)
        for index, remaining in enumerate(self.current_set.entries):
            remaining.order_index = index
        save_setlist(self.current_set)
        self.open_set(self.current_set.id)
        self.refresh_sets()

    def delete_selected_set(self) -> None:
        item = self.set_list.currentItem()
        if not item:
            return
        set_id = item.data(Qt.UserRole)
        answer = QMessageBox.question(self, "Delete set", f"Delete '{item.text().split(' — ')[0]}'?")
        if answer == QMessageBox.Yes:
            delete_setlist(set_id)
            self.refresh_sets()

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
        self.player = PlaybackEngine(self.current_song, output_channels=output_channels, device_index=selected_device)
        self.player.click_gain = self.db_to_linear(self.click_fader.value())
        self.player.guide_gain = self.db_to_linear(self.guide_fader.value())
        self.player.master_gain = self.db_to_linear(self.master_fader.value())
        try:
            self.player.start()
            self.set_play_pause_state(False)
        except RuntimeError as exc:
            self.player = None
            QMessageBox.critical(self, "Playback error", str(exc))

    def stop_playback(self) -> None:
        if self.player:
            self.player.stop()
            self.player = None
        if hasattr(self, "open_set_play"):
            self.set_play_pause_state(True)
        if hasattr(self, "beat_playhead"):
            self.beat_playhead.update()
        self.update_output_meter()

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
        save_song(self.current_song)

        self.refresh_library()
        self.library_details.setPlainText(self.format_song_details(self.current_song))
        self.library_right.setCurrentWidget(self.library_details)

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
        self.library_right.setCurrentWidget(self.library_details)

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

        reordered_entries = []
        entries_by_id = {entry.id: entry for entry in self.current_set.entries}
        for row in range(self.open_set_song_list.count()):
            item = self.open_set_song_list.item(row)
            entry_id = item.data(Qt.UserRole)
            existing = entries_by_id.get(entry_id)
            if existing:
                existing.order_index = row
                reordered_entries.append(existing)

        self.current_set.entries = reordered_entries
        save_setlist(self.current_set)
        self.refresh_sets()

    @staticmethod
    def db_to_linear(db: int) -> float:
        db = max(-30, min(12, int(db)))
        if db <= -30:
            return 0.0
        return float(10.0 ** (db / 20.0))

    def set_live_click_db(self, db: int) -> None:
        if self.player:
            self.player.click_gain = self.db_to_linear(db)

    def set_live_guide_db(self, db: int) -> None:
        if self.player:
            self.player.guide_gain = self.db_to_linear(db)

    def set_live_master_db(self, db: int) -> None:
        if self.player:
            self.player.master_gain = self.db_to_linear(db)

    def _set_set_song_controls_enabled(self, enabled: bool) -> None:
        self.set_song_tempo.setEnabled(enabled)
        self.set_song_key.setEnabled(enabled)
        self.set_song_time_signature.setEnabled(enabled)
        self.reset_set_song_button.setEnabled(enabled)
        self.save_set_song_to_library_button.setEnabled(enabled)

    def song_with_entry_overrides(self, entry: SetlistEntry) -> Song:
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

    def entry_to_summary(self, entry: SetlistEntry) -> str:
        song = self.song_with_entry_overrides(entry)
        return song.to_summary()

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
        self.current_set_entry.override_time_signature = (
            None if override_time_signature == str(base_song.time_signature) else override_time_signature
        )
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

        # Capture each matching entry's effective values before changing library defaults.
        prior_entry_values: dict[int, tuple[int, str, str]] = {}
        prior_entry_was_edited: dict[int, bool] = {}
        for entry in self.current_set.entries:
            if entry.song_id != library_song.id:
                continue
            key = entry.id if entry.id is not None else id(entry)
            effective_song = self.song_with_entry_overrides(entry)
            prior_entry_values[key] = (
                int(effective_song.tempo),
                str(effective_song.key),
                str(effective_song.time_signature),
            )
            prior_entry_was_edited[key] = any(
                value is not None
                for value in (entry.override_tempo, entry.override_key, entry.override_time_signature)
            )

        library_song.tempo = int(self.set_song_tempo.value())
        library_song.key = self.set_song_key.text().strip() or "C"
        library_song.time_signature = self.set_song_time_signature.currentText().strip() or "4/4"
        save_song(library_song)

        for entry in self.current_set.entries:
            if entry.song_id != library_song.id:
                continue
            key = entry.id if entry.id is not None else id(entry)
            if entry.song:
                entry.song.tempo = library_song.tempo
                entry.song.key = library_song.key
                entry.song.time_signature = library_song.time_signature

            if prior_entry_was_edited.get(key, False):
                prior_tempo, prior_key, prior_time_signature = prior_entry_values[key]
                entry.override_tempo = None if prior_tempo == library_song.tempo else prior_tempo
                entry.override_key = None if prior_key == library_song.key else prior_key
                entry.override_time_signature = (
                    None if prior_time_signature == library_song.time_signature else prior_time_signature
                )
            else:
                entry.override_tempo = None
                entry.override_key = None
                entry.override_time_signature = None

        save_setlist(self.current_set)
        self.load_set_song_controls_from_entry(self.current_set_entry)
        self.current_song = self.song_with_entry_overrides(self.current_set_entry)
        self.refresh_open_set_song_labels()
        self.refresh_library()
        self.beat_playhead.update()
        self._apply_current_song_to_player()

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

    def save_output_device(self, index: int) -> None:
        if index >= 0:
            set_app_setting("audio.output_device", str(self.output_device.itemData(index)))
            if hasattr(self, "output_meter_widget"):
                self.output_meter_widget.ensure_channels(self._get_selected_output_channel_count())
                self.update_output_meter()

    def select_import_accent_file(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Select accent WAV", "", "WAV files (*.wav)")
        if path:
            self.import_accent_path.setText(path)

    def select_import_normal_file(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Select normal WAV", "", "WAV files (*.wav)")
        if path:
            self.import_normal_path.setText(path)

    def select_import_voice_directory(self) -> None:
        directory = QFileDialog.getExistingDirectory(self, "Select count-in voice directory", "")
        if directory:
            self.import_voice_dir.setText(directory)

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

        file_count = len(tracks)
        self.edit_wav_directory.setText(
            f"{self.edit_attached_directory} ({file_count} WAV file{'s' if file_count != 1 else ''})"
        )

    def clear_edit_song_wav_directory(self) -> None:
        self.edit_pending_tracks = []
        self.edit_attached_directory = ""
        self.edit_wav_directory.setText("No WAV files (clear attached tracks)")

    def import_click_sound(self) -> None:
        name = "".join(
            character if character.isalnum() or character in "-_ " else "_"
            for character in self.import_name.text().strip()
        ).strip("-_ ").replace(" ", "_")
        accent_source = Path(self.import_accent_path.text().strip())
        normal_source = Path(self.import_normal_path.text().strip())

        if not name:
            QMessageBox.warning(self, "Missing name", "Enter a name for the click sound.")
            return
        if not accent_source.exists() or accent_source.suffix.lower() != ".wav":
            QMessageBox.warning(self, "Accent WAV required", "Select a valid accent .wav file.")
            return
        if not normal_source.exists() or normal_source.suffix.lower() != ".wav":
            QMessageBox.warning(self, "Normal WAV required", "Select a valid normal .wav file.")
            return

        click_dir = Path(__file__).resolve().parent / "assets" / "clicks"
        click_dir.mkdir(parents=True, exist_ok=True)
        accent_target = click_dir / f"{name}_accent.wav"
        normal_target = click_dir / f"{name}_normal.wav"

        shutil.copy2(accent_source, accent_target)
        shutil.copy2(normal_source, normal_target)

        set_app_setting("click.accent_wav", str(accent_target))
        set_app_setting("click.normal_wav", str(normal_target))

        self.import_name.clear()
        self.import_accent_path.clear()
        self.import_normal_path.clear()

        families = sorted({
            path.stem.split("_", 1)[0]
            for path in click_dir.glob("*.wav")
            if "_" in path.stem
        })
        self.click_family.blockSignals(True)
        self.click_family.clear()
        for family in families:
            accent_path = click_dir / f"{family}_accent.wav"
            normal_path = click_dir / f"{family}_normal.wav"
            if accent_path.exists() and normal_path.exists():
                self.click_family.addItem(family, (str(accent_path), str(normal_path)))
        index = self.click_family.findText(name)
        if index >= 0:
            self.click_family.setCurrentIndex(index)
        self.click_family.blockSignals(False)

        QMessageBox.information(self, "Click sound imported", f"Imported '{name}' into the click sound list.")
        self.views.setCurrentIndex(3)

    def import_countin_voice(self) -> None:
        name = "".join(
            character if character.isalnum() or character in "-_ " else "_"
            for character in self.import_voice_name.text().strip()
        ).strip("-_ ").replace(" ", "_")
        source_dir = Path(self.import_voice_dir.text().strip())

        if not name:
            QMessageBox.warning(self, "Missing name", "Enter a name for the voice.")
            return
        if not source_dir.exists() or not source_dir.is_dir():
            QMessageBox.warning(self, "Directory required", "Select a valid count-in voice directory.")
            return

        required = [f"{word}.wav" for word in ("one", "two", "three", "four", "five", "six", "seven", "eight")]
        missing = [filename for filename in required if not (source_dir / filename).exists()]
        if missing:
            QMessageBox.warning(self, "Missing voice WAV files", "Missing: " + ", ".join(missing))
            return

        countin_dir = Path(__file__).resolve().parent / "assets" / "countin"
        countin_dir.mkdir(parents=True, exist_ok=True)
        target_dir = countin_dir / name
        target_dir.mkdir(parents=True, exist_ok=True)

        for filename in required:
            shutil.copy2(source_dir / filename, target_dir / filename)

        set_app_setting("countin.voice", name)

        self.import_voice_name.clear()
        self.import_voice_dir.clear()

        if hasattr(self, "voice_family"):
            index = self.voice_family.findText(name)
            if index < 0:
                self.voice_family.addItem(name)
                index = self.voice_family.findText(name)
            self.voice_family.setCurrentIndex(index)

        QMessageBox.information(self, "Voice imported", f"Imported '{name}' count-in voice.")
        self.views.setCurrentIndex(3)


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
