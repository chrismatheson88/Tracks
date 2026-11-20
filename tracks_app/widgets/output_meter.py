from __future__ import annotations

import math

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QHBoxLayout, QLabel, QProgressBar, QVBoxLayout, QWidget


class OutputMeterWidget(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.layout = QHBoxLayout(self)
        self.layout.setContentsMargins(0, 0, 0, 0)
        self.layout.setSpacing(10)
        self.bars: list[QProgressBar] = []
        self.labels: list[QLabel] = []
        self.columns: list[QWidget] = []

    def ensure_channels(self, channel_count: int) -> None:
        channel_count = max(1, min(32, int(channel_count)))
        while len(self.bars) < channel_count:
            column_widget = QWidget()
            column = QVBoxLayout(column_widget)
            column.setContentsMargins(0, 0, 0, 0)
            column.setSpacing(2)

            name = QLabel(f"Ch {len(self.bars) + 1}")
            name.setAlignment(Qt.AlignHCenter)
            bar = QProgressBar()
            bar.setOrientation(Qt.Vertical)
            bar.setRange(0, 1000)
            bar.setTextVisible(False)
            bar.setFixedSize(22, 108)
            value_label = QLabel("-inf dB")
            value_label.setAlignment(Qt.AlignHCenter)
            value_label.setMinimumWidth(52)

            column.addWidget(name)
            column.addWidget(bar, alignment=Qt.AlignHCenter)
            column.addWidget(value_label)
            self.layout.addWidget(column_widget)

            self.bars.append(bar)
            self.labels.append(value_label)
            self.columns.append(column_widget)

        for index, bar in enumerate(self.bars):
            visible = index < channel_count
            bar.setVisible(visible)
            self.labels[index].setVisible(visible)
            self.columns[index].setVisible(visible)

    def set_idle(self, channel_count: int) -> None:
        self.ensure_channels(channel_count)
        for bar, label in zip(self.bars, self.labels):
            bar.setValue(0)
            label.setText("-inf dB")

    def set_peaks(self, peaks) -> None:
        self.ensure_channels(len(peaks))
        for index, peak in enumerate(peaks):
            amplitude = max(0.0, min(1.0, float(peak)))
            self.bars[index].setValue(int(round(amplitude * 1000.0)))
            if amplitude <= 1e-6:
                self.labels[index].setText("-inf dB")
            else:
                level_db = 20.0 * math.log10(amplitude)
                self.labels[index].setText(f"{level_db:5.1f} dB")
