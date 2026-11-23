from __future__ import annotations

from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QColor, QPainter, QPen
from PySide6.QtWidgets import QWidget


class BeatPlayheadWidget(QWidget):
    """Visual timeline for count-in beats and the looping backing track."""

    def __init__(self, window) -> None:
        super().__init__()
        self.window = window
        self.setMinimumHeight(72)
        self.setMaximumHeight(72)

    def paintEvent(self, event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        width = max(1, self.width())
        height = self.height()
        painter.fillRect(0, 0, width, height, QColor("#f8fafc"))

        player = self.window.player
        song = self.window.current_song

        if song is None:
            painter.setPen(QColor("#94a3b8"))
            painter.drawText(12, 24, "COUNT-IN  /  LOOP")
            painter.setPen(QPen(QColor("#cbd5e1"), 1))
            painter.drawRect(1, 8, width - 2, height - 17)
            painter.end()
            return

        try:
            time_signature_parts = str(song.time_signature).split("/", 1)
            beats_per_bar = int(time_signature_parts[0])
            beat_division = int(time_signature_parts[1]) if len(time_signature_parts) > 1 else 4
        except (TypeError, ValueError):
            beats_per_bar = 4
            beat_division = 4
        beats_per_bar = max(1, beats_per_bar)
        beat_division = max(1, beat_division)
        is_compound = beat_division == 8 and beats_per_bar in (6, 12)
        pulse_count_per_bar = beats_per_bar

        sample_rate = int(player.sample_rate) if player is not None else 44100
        if is_compound:
            group_frames = max(1, int(round(sample_rate * 60.0 / max(1, int(song.tempo)))))
            beat_frames = max(1, int(round(group_frames / 3.0)))
        else:
            beat_frames = max(1, int(round(sample_rate * 60.0 / max(1, int(song.tempo)))))

        countin_frames = beat_frames * pulse_count_per_bar * 2
        elapsed_frames = max(0, int(player._metronome_frame)) if player is not None else 0
        loop_frames = max(beat_frames, int(getattr(player, "loop_frames", beat_frames))) if player is not None else beat_frames

        countin_ratio = 0.40
        countin_width = int(width * countin_ratio)
        loop_x = countin_width
        loop_width = width - loop_x

        painter.setPen(QPen(QColor("#cbd5e1"), 1))
        painter.drawRect(1, 8, width - 2, height - 17)

        painter.fillRect(1, 9, countin_width - 2, height - 19, QColor("#dbeafe"))
        countin_beats = pulse_count_per_bar * 2
        for beat in range(countin_beats + 1):
            x = 1 + int(countin_width * beat / countin_beats)
            accent = beat % 3 == 0 if is_compound else beat % beats_per_bar == 0
            painter.setPen(QPen(QColor("#1d4ed8" if accent else "#60a5fa"), 2 if accent else 1))
            painter.drawLine(x, 10, x, height - 10)

        painter.fillRect(loop_x + 1, 9, loop_width - 2, height - 19, QColor("#f1f5f9"))
        loop_beats = pulse_count_per_bar
        for beat in range(loop_beats + 1):
            x = loop_x + int(loop_width * beat / loop_beats)
            accent = beat % 3 == 0 if is_compound else beat % beats_per_bar == 0
            painter.setPen(QPen(QColor("#334155" if accent else "#94a3b8"), 2 if accent else 1))
            painter.drawLine(x, 10, x, height - 10)

        painter.setPen(QColor("#1e3a8a"))
        painter.drawText(8, 24, "COUNT-IN")
        painter.setPen(QColor("#334155"))
        painter.drawText(loop_x + 8, 24, "LOOP")

        if player is None:
            playhead_x = 1
        elif elapsed_frames < countin_frames:
            playhead_x = int(countin_width * elapsed_frames / countin_frames)
        else:
            bar_frames = beat_frames * pulse_count_per_bar
            loop_elapsed = (elapsed_frames - countin_frames) % bar_frames
            playhead_x = loop_x + int(loop_width * loop_elapsed / bar_frames)

        playhead_x = max(1, min(width - 2, playhead_x))
        painter.setPen(QPen(QColor("#ef4444"), 3))
        painter.drawLine(playhead_x, 5, playhead_x, height - 5)
        painter.setBrush(QColor("#ef4444"))
        painter.setPen(Qt.NoPen)
        painter.drawPolygon([
            QPoint(playhead_x - 5, 5),
            QPoint(playhead_x + 5, 5),
            QPoint(playhead_x, 13),
        ])
        painter.end()
