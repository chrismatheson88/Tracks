from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QColor, QIcon, QPainter, QPixmap


def transport_icon(kind: str, size: int = 48) -> QIcon:
    pixmap = QPixmap(size, size)
    pixmap.fill(Qt.transparent)
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.Antialiasing)
    painter.setPen(Qt.NoPen)
    painter.setBrush(QColor("#1e293b"))

    if kind == "play":
        painter.drawPolygon([
            QPoint(size * 32 // 100, size * 18 // 100),
            QPoint(size * 82 // 100, size * 50 // 100),
            QPoint(size * 32 // 100, size * 82 // 100),
        ])
    elif kind == "pause":
        painter.drawRect(size * 27 // 100, size * 18 // 100, size * 15 // 100, size * 64 // 100)
        painter.drawRect(size * 58 // 100, size * 18 // 100, size * 15 // 100, size * 64 // 100)
    elif kind == "stop":
        painter.drawRect(size * 22 // 100, size * 22 // 100, size * 56 // 100, size * 56 // 100)
    elif kind == "previous":
        painter.drawRect(size * 16 // 100, size * 22 // 100, size * 8 // 100, size * 56 // 100)
        painter.drawPolygon([
            QPoint(size * 74 // 100, size * 22 // 100),
            QPoint(size * 26 // 100, size * 50 // 100),
            QPoint(size * 74 // 100, size * 78 // 100),
        ])
    elif kind == "next":
        painter.drawRect(size * 76 // 100, size * 22 // 100, size * 8 // 100, size * 56 // 100)
        painter.drawPolygon([
            QPoint(size * 26 // 100, size * 22 // 100),
            QPoint(size * 74 // 100, size * 50 // 100),
            QPoint(size * 26 // 100, size * 78 // 100),
        ])

    painter.end()
    return QIcon(pixmap)
