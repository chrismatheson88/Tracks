from __future__ import annotations

from pathlib import Path

from PySide6.QtWidgets import (
    QComboBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QVBoxLayout,
    QWidget,
    QFileDialog,
    QMessageBox,
)

from ..db import get_app_setting, set_app_setting
from ..playback import list_audio_devices
from ..services.asset_import_service import import_click_sound_files, import_countin_voice_directory


def build_settings_view(window) -> QWidget:
    view = QWidget()
    layout = QVBoxLayout(view)

    heading = QLabel("Settings")
    heading.setStyleSheet("font-size: 20px; font-weight: 700;")
    layout.addWidget(heading)

    form = QFormLayout()

    click_dir = Path(__file__).resolve().parent.parent / "assets" / "clicks"
    families = sorted({
        path.stem.split("_", 1)[0]
        for path in click_dir.glob("*.wav")
        if "_" in path.stem
    })

    accent_saved = get_app_setting("click.accent_wav", "")
    saved_family = Path(accent_saved).stem.split("_", 1)[0] if accent_saved else ""
    window.click_family = QComboBox()

    for family in families:
        accent_path = click_dir / f"{family}_accent.wav"
        normal_path = click_dir / f"{family}_normal.wav"
        if accent_path.exists() and normal_path.exists():
            window.click_family.addItem(family, (str(accent_path), str(normal_path)))

    saved_index = window.click_family.findText(saved_family)
    if saved_index >= 0:
        window.click_family.setCurrentIndex(saved_index)

    click_row = QHBoxLayout()
    click_row.addWidget(window.click_family)
    import_button = QPushButton("Import click sound")
    import_button.setMinimumWidth(180)
    click_row.addWidget(import_button)
    form.addRow("Click sound:", click_row)

    countin_dir = Path(__file__).resolve().parent.parent / "assets" / "countin"
    voice_names = sorted(path.name for path in countin_dir.iterdir() if path.is_dir())
    saved_voice = get_app_setting("countin.voice", "female")
    window.voice_family = QComboBox()
    for voice_name in voice_names:
        window.voice_family.addItem(voice_name)
    voice_index = window.voice_family.findText(saved_voice)
    if voice_index >= 0:
        window.voice_family.setCurrentIndex(voice_index)

    voice_row = QHBoxLayout()
    voice_row.addWidget(window.voice_family)
    import_voice_button = QPushButton("Import voice")
    import_voice_button.setMinimumWidth(180)
    voice_row.addWidget(import_voice_button)
    form.addRow("Count-in voice:", voice_row)

    window.output_device = QComboBox()
    window.refresh_output_device_list()
    form.addRow("Output device:", window.output_device)

    layout.addLayout(form)
    layout.addStretch()

    import_button.clicked.connect(lambda: window.views.setCurrentIndex(4))
    import_voice_button.clicked.connect(lambda: window.views.setCurrentIndex(5))
    window.output_device.currentIndexChanged.connect(window.save_output_device)
    window.voice_family.currentIndexChanged.connect(window.save_countin_voice)
    window.click_family.currentIndexChanged.connect(window.save_click_family)
    return view


def build_import_click_view(window) -> QWidget:
    view = QWidget()
    layout = QVBoxLayout(view)
    layout.addWidget(QLabel("Import click sound"))

    form = QFormLayout()
    window.import_name = QLineEdit()
    window.import_accent_path = QLineEdit()
    window.import_normal_path = QLineEdit()

    accent_select = QPushButton("Select accent WAV")
    normal_select = QPushButton("Select normal WAV")
    accent_row = QHBoxLayout()
    accent_row.addWidget(window.import_accent_path)
    accent_row.addWidget(accent_select)
    normal_row = QHBoxLayout()
    normal_row.addWidget(window.import_normal_path)
    normal_row.addWidget(normal_select)

    form.addRow("Name:", window.import_name)
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

    accent_select.clicked.connect(window.select_import_accent_file)
    normal_select.clicked.connect(window.select_import_normal_file)
    import_button.clicked.connect(window.import_click_sound)
    back_button.clicked.connect(lambda: window.views.setCurrentIndex(3))
    return view


def build_import_voice_view(window) -> QWidget:
    view = QWidget()
    layout = QVBoxLayout(view)
    layout.addWidget(QLabel("Import count-in voice"))

    form = QFormLayout()
    window.import_voice_name = QLineEdit()
    window.import_voice_dir = QLineEdit()
    select_dir_button = QPushButton("Select directory")

    dir_row = QHBoxLayout()
    dir_row.addWidget(window.import_voice_dir)
    dir_row.addWidget(select_dir_button)
    form.addRow("Name:", window.import_voice_name)
    form.addRow("Directory:", dir_row)
    layout.addLayout(form)

    required = QLabel("The selected directory must contain: one.wav, two.wav, three.wav, four.wav, five.wav, six.wav, seven.wav, eight.wav")
    required.setWordWrap(True)
    layout.addWidget(required)

    actions = QHBoxLayout()
    import_button = QPushButton("Import voice")
    back_button = QPushButton("Back to Settings")
    actions.addWidget(import_button)
    actions.addWidget(back_button)
    layout.addLayout(actions)
    layout.addStretch()

    select_dir_button.clicked.connect(window.select_import_voice_directory)
    import_button.clicked.connect(window.import_countin_voice)
    back_button.clicked.connect(lambda: window.views.setCurrentIndex(3))
    return view


class SettingsViewMixin:
    def _get_system_default_output_device_index(self) -> int | None:
        try:
            import sounddevice as sd
            default_device = sd.default.device
            if isinstance(default_device, (tuple, list)):
                default_output = default_device[1] if len(default_device) > 1 else None
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
            system_device = next((device for device in devices if int(device["index"])==system_output_index), None)
            if system_device is not None:
                system_label = f"Use current system output ({system_device['name']})"

        self.output_device.addItem(system_label, "")
        for device in devices:
            self.output_device.addItem(f"{device['name']} ({device['max_output_channels']} ch)", str(device["index"]))

        selected_output_index = self.output_device.findData(saved_output)
        self.output_device.setCurrentIndex(selected_output_index if selected_output_index >= 0 else 0)
        self.output_device.blockSignals(False)

    def save_output_device(self, index: int) -> None:
        if index >= 0:
            set_app_setting("audio.output_device", str(self.output_device.itemData(index)))

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

        package_dir = Path(__file__).resolve().parent.parent
        accent_target, normal_target = import_click_sound_files(package_dir, name, accent_source, normal_source)
        set_app_setting("click.accent_wav", str(accent_target))
        set_app_setting("click.normal_wav", str(normal_target))

        self.import_name.clear()
        self.import_accent_path.clear()
        self.import_normal_path.clear()
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

        package_dir = Path(__file__).resolve().parent.parent
        import_countin_voice_directory(package_dir, name, source_dir)
        set_app_setting("countin.voice", name)

        self.import_voice_name.clear()
        self.import_voice_dir.clear()
        QMessageBox.information(self, "Voice imported", f"Imported '{name}' count-in voice.")
        self.views.setCurrentIndex(3)
