from __future__ import annotations

from pathlib import Path
import shutil


def import_click_sound_files(package_dir: Path, name: str, accent_source: Path, normal_source: Path) -> tuple[Path, Path]:
    click_dir = package_dir / "assets" / "clicks"
    click_dir.mkdir(parents=True, exist_ok=True)
    accent_target = click_dir / f"{name}_accent.wav"
    normal_target = click_dir / f"{name}_normal.wav"
    shutil.copy2(accent_source, accent_target)
    shutil.copy2(normal_source, normal_target)
    return accent_target, normal_target


def import_countin_voice_directory(package_dir: Path, name: str, source_dir: Path) -> Path:
    countin_dir = package_dir / "assets" / "countin"
    countin_dir.mkdir(parents=True, exist_ok=True)
    target_dir = countin_dir / name
    target_dir.mkdir(parents=True, exist_ok=True)

    words = ("one", "two", "three", "four", "five", "six", "seven", "eight")
    for word in words:
        shutil.copy2(source_dir / f"{word}.wav", target_dir / f"{word}.wav")
    return target_dir
