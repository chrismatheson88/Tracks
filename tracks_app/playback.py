from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import threading
from typing import List, Optional

import numpy as np

try:
    import sounddevice as sd
except ImportError:  # pragma: no cover - only used when audio dependencies are absent.
    sd = None

try:
    import soundfile as sf
except ImportError:  # pragma: no cover - only used when audio dependencies are absent.
    sf = None

from .models import Song, SongTrack


@dataclass
class LoadedTrack:
    track: SongTrack
    data: np.ndarray
    position: int = 0


def list_audio_devices() -> List[dict]:
    if sd is None:
        return []

    devices: List[dict] = []
    for index, info in enumerate(sd.query_devices()):
        if not isinstance(info, dict):
            continue
        max_output_channels = int(info.get("max_output_channels", 0))
        if max_output_channels <= 0:
            continue
        devices.append(
            {
                "index": index,
                "name": str(info.get("name", f"Device {index}")),
                "max_output_channels": max_output_channels,
                "max_input_channels": int(info.get("max_input_channels", 0)),
            }
        )
    return devices


def load_wav(file_path: str | Path) -> np.ndarray:
    if sf is None:
        raise RuntimeError("soundfile is required to load wav files.")
    array, _ = sf.read(str(file_path), always_2d=True, dtype="float32")
    return array.astype(np.float32, copy=False)


def inspect_wav_directory(directory: str | Path) -> tuple[List[SongTrack], list[str], str | None]:
    """Inspect direct WAV files in a directory and build one track per file."""
    if sf is None:
        return [], [], "soundfile is required to inspect WAV files."

    directory_path = Path(directory)
    if not directory_path.exists() or not directory_path.is_dir():
        return [], [], "Select a valid directory."

    wav_paths = sorted(
        (path for path in directory_path.iterdir() if path.is_file() and path.suffix.lower() == ".wav"),
        key=lambda path: path.name.lower(),
    )
    if not wav_paths:
        return [], [], "No WAV files were found in the selected directory."

    tracks: List[SongTrack] = []
    length_details: list[tuple[str, int, int]] = []
    mismatches: list[str] = []

    for index, wav_path in enumerate(wav_paths):
        try:
            info = sf.info(str(wav_path))
            length_details.append((wav_path.name, int(info.frames), int(info.samplerate)))
            tracks.append(SongTrack(
                name=wav_path.stem,
                file_path=str(wav_path.resolve()),
                output_channel=index,
                gain=1.0,
            ))
        except Exception as exc:
            mismatches.append(f"{wav_path.name}: could not read WAV ({exc})")

    if not tracks:
        return [], mismatches, "No readable WAV files were found in the selected directory."

    durations = {round(frames / max(1, samplerate), 6) for _, frames, samplerate in length_details}
    if len(durations) > 1:
        length_summary = ", ".join(
            f"{name}: {frames / max(1, samplerate):.3f}s"
            for name, frames, samplerate in length_details
        )
        mismatches.insert(0, f"WAV lengths differ — {length_summary}")

    return tracks, mismatches, None


class PlaybackEngine:
    def __init__(self, song: Song, output_channels: int = 8, device_index: Optional[int] = None):
        self.song = song
        self.output_channels = max(1, int(output_channels))
        self.device_index = device_index
        self.stream = None
        self.sample_rate = 44100
        self.tracks: List[LoadedTrack] = []
        self.loop_frames = 0
        self._metronome_frame = 0
        self._peak_lock = threading.Lock()
        self._output_peaks = np.zeros(self.output_channels, dtype=np.float32)

        loaded_accent = self._load_click_wav("accent")
        loaded_normal = self._load_click_wav("normal")
        self._accent_click = loaded_accent if loaded_accent is not None else np.zeros(1, dtype=np.float32)
        self._normal_click = loaded_normal if loaded_normal is not None else np.zeros(1, dtype=np.float32)

        self._countin_words = self._load_countin_words()
        self._load_song()

        self._active_click_samples = []
        self._active_voice_samples = []

        try:
            from .db import get_app_setting
            self.click_gain = self._db_to_linear(float(get_app_setting("volume.click_db", "0")))
            self.guide_gain = self._db_to_linear(float(get_app_setting("volume.guide_db", "0")))
            self.master_gain = self._db_to_linear(float(get_app_setting("volume.master_db", "0")))
            self.click_channel = int(get_app_setting("volume.channel.click_db", "0") or "0")
            self.guide_channel = int(get_app_setting("volume.channel.guide_db", "0") or "0")
            self.master_channel = int(get_app_setting("volume.channel.master_db", "0") or "0")
        except Exception:
            self.click_gain = 1.0
            self.guide_gain = 1.0
            self.master_gain = 1.0
            self.click_channel = 0
            self.guide_channel = 0
            self.master_channel = 0

    @staticmethod
    def _db_to_linear(db: float) -> float:
        db = max(-30.0, min(12.0, float(db)))
        if db <= -30.0:
            return 0.0
        return float(10.0 ** (db / 20.0))

    def _load_click_wav(self, click_type: str) -> np.ndarray | None:
        try:
            from .db import get_app_setting

            setting_key = f"click.{click_type}_wav"
            configured_path = get_app_setting(setting_key, "")
            if configured_path:
                path = Path(configured_path)
                if path.exists():
                    data, source_rate = sf.read(str(path), always_2d=True, dtype="float32")
                    if data.shape[1] > 1:
                        data = np.mean(data, axis=1)
                    else:
                        data = data[:, 0]
                    if data.size:
                        return self._resample_wav(data, int(source_rate))
        except Exception:
            pass

        bundled_filename = "tone_accent.wav" if click_type == "accent" else "tone_normal.wav"
        bundled_path = Path(__file__).resolve().parent / "assets" / "clicks" / bundled_filename
        if bundled_path.exists():
            try:
                data, source_rate = sf.read(str(bundled_path), always_2d=True, dtype="float32")
                if data.shape[1] > 1:
                    data = np.mean(data, axis=1)
                else:
                    data = data[:, 0]
                return self._resample_wav(data, int(source_rate))
            except Exception:
                pass
        return None

    def _load_countin_words(self) -> dict[str, np.ndarray]:
        words: dict[str, np.ndarray] = {}
        try:
            from .db import get_app_setting
            voice_name = get_app_setting("countin.voice", "female")
        except Exception:
            voice_name = "female"
        asset_dir = Path(__file__).resolve().parent / "assets" / "countin" / voice_name
        for word in ("one", "two", "three", "four", "five", "six", "seven", "eight"):
            path = asset_dir / f"{word}.wav"
            if path.exists():
                data, source_rate = sf.read(str(path), always_2d=True, dtype="float32")
                data = np.mean(data, axis=1) if data.shape[1] > 1 else data[:, 0]
                words[word] = self._resample_wav(data, int(source_rate))
        return words

    def _load_song(self) -> None:
        self.tracks = []
        for track in self.song.tracks:
            if track.muted or not track.file_path:
                continue
            data = load_wav(track.file_path)
            if data.ndim == 1:
                data = data.reshape(-1, 1)
            loaded_track = LoadedTrack(track=track, data=data.astype(np.float32, copy=False))
            self.tracks.append(loaded_track)
            self.loop_frames = max(self.loop_frames, len(loaded_track.data))

    def render_chunk(self, frames: int) -> np.ndarray:
        output = np.zeros((frames, self.output_channels), dtype=np.float32)
        for TrackState in self.tracks:
            if TrackState.track.muted:
                continue
            data = TrackState.data
            if data.size == 0:
                continue

            channel_a = TrackState.track.output_channel
            audio_length = len(data)
            available = min(frames, max(0, audio_length - TrackState.position))
            if available <= 0:
                TrackState.position = 0
                available = min(frames, audio_length)

            window = data[TrackState.position:TrackState.position + available]
            if available < frames:
                remainder = min(frames - available, audio_length)
                if remainder > 0:
                    window = np.concatenate([window, data[:remainder]], axis=0)

            if window.shape[0] == 0:
                continue

            # Strict isolation: each backing track writes to one destination
            # channel only (its configured output channel). For multi-channel
            # source files we take the first channel to avoid cross-content
            # from source-channel summing.
            if data.shape[1] == 1:
                channel_values = window[:, 0]
            else:
                channel_values = window[:, 0]

            if channel_a < self.output_channels:
                output[: len(channel_values), channel_a] += channel_values * TrackState.track.gain

            TrackState.position = (TrackState.position + frames) % audio_length

        return output

    def render_metronome(self, frames: int) -> np.ndarray:
        output = np.zeros((frames, self.output_channels), dtype=np.float32)

        try:
            time_signature_parts = str(self.song.time_signature).split("/", 1)
            beats_per_bar = int(time_signature_parts[0])
            beat_division = int(time_signature_parts[1]) if len(time_signature_parts) > 1 else 4
        except (TypeError, ValueError):
            beats_per_bar = 4
            beat_division = 4
        beats_per_bar = max(1, beats_per_bar)
        beat_division = max(1, beat_division)

        is_compound = beat_division == 8 and beats_per_bar in (6, 12)
        if is_compound:
            # For compound time, the tempo input defines each set of 3 eighths.
            group_frames = max(1, int(round(self.sample_rate * 60.0 / max(1, int(self.song.tempo)))))
            beat_frames = max(1, int(round(group_frames / 3.0)))
            pulse_count_per_bar = beats_per_bar // 3 * 3
        else:
            beat_frames = max(1, int(round(self.sample_rate * 60.0 / max(1, int(self.song.tempo)))))
            pulse_count_per_bar = beats_per_bar
        block_start = self._metronome_frame
        block_end = block_start + frames

        for beat_index in range(max(0, block_start // beat_frames), (block_end + beat_frames - 1) // beat_frames + 1):
            beat_start = beat_index * beat_frames
            if beat_start < block_start or beat_start >= block_end:
                continue

            beat_in_bar = beat_index % pulse_count_per_bar
            if is_compound:
                is_accent = beat_in_bar % 3 == 0
            else:
                is_accent = beat_in_bar == 0
            click = self._accent_click if is_accent else self._normal_click
            if click is not None and len(click) > 0:
                self._active_click_samples.append([click, beat_start])

            if beat_index < pulse_count_per_bar * 2:
                if pulse_count_per_bar == 4:
                    speech_pattern = ["one", None, "two", None, "one", "two", "three", "four"]
                    word = speech_pattern[beat_index]
                else:
                    words = ("one", "two", "three", "four", "five", "six", "seven", "eight")
                    word = words[beat_index % pulse_count_per_bar]

                if word:
                    word_data = self._countin_words.get(word)
                    if word_data is not None and len(word_data) > 0:
                        self._active_voice_samples.append([word_data, beat_start])

        still_active_clicks = []
        for click_data, start_frame in self._active_click_samples:
            overlap_start = max(block_start, start_frame)
            overlap_end = min(block_end, start_frame + len(click_data))
            if overlap_end > overlap_start:
                out_start = overlap_start - block_start
                out_end = overlap_end - block_start
                wav_start = overlap_start - start_frame
                wav_end = overlap_end - start_frame
                if self.click_channel < output.shape[1]:
                    output[out_start:out_end, self.click_channel] = np.clip(output[out_start:out_end, self.click_channel] + click_data[wav_start:wav_end] * self.click_gain, -0.95, 0.95)
            if start_frame + len(click_data) > block_end:
                still_active_clicks.append([click_data, start_frame])
        self._active_click_samples = still_active_clicks

        still_active_voices = []
        for voice_data, start_frame in self._active_voice_samples:
            overlap_start = max(block_start, start_frame)
            overlap_end = min(block_end, start_frame + len(voice_data))
            if overlap_end > overlap_start:
                out_start = overlap_start - block_start
                out_end = overlap_end - block_start
                wav_start = overlap_start - start_frame
                wav_end = overlap_end - start_frame
                if self.guide_channel < output.shape[1]:
                    output[out_start:out_end, self.guide_channel] = np.clip(output[out_start:out_end, self.guide_channel] + voice_data[wav_start:wav_end] * self.guide_gain, -0.95, 0.95)
            if start_frame + len(voice_data) > block_end:
                still_active_voices.append([voice_data, start_frame])
        self._active_voice_samples = still_active_voices

        self._metronome_frame = block_end
        return output

    def _callback(self, outdata: np.ndarray, frames: int, time, status) -> None:  # pragma: no cover - integration path
        if status:
            pass
        mixed = self.render_chunk(frames)
        metronome = self.render_metronome(frames)
        mixed += metronome
        mixed *= self.master_gain
        np.clip(mixed, -0.95, 0.95, out=mixed)
        # Always clear the full hardware buffer first, otherwise channels that
        # are not written in this callback can leak stale audio.
        outdata.fill(0.0)
        if mixed.shape[1] == outdata.shape[1]:
            outdata[:] = mixed
        elif outdata.shape[1] == 1:
            outdata[:, 0] = np.mean(mixed, axis=1)
        else:
            output_channels = outdata.shape[1]
            if mixed.shape[1] >= output_channels:
                outdata[:] = mixed[:, :output_channels]
            else:
                outdata[:, :mixed.shape[1]] = mixed

        peaks = np.max(np.abs(outdata), axis=0).astype(np.float32, copy=False)
        with self._peak_lock:
            if len(self._output_peaks) != outdata.shape[1]:
                self._output_peaks = np.zeros(outdata.shape[1], dtype=np.float32)
            self._output_peaks = np.maximum(self._output_peaks * 0.82, peaks)

    def get_output_peaks(self) -> np.ndarray:
        with self._peak_lock:
            return self._output_peaks.copy()

    def start(self) -> None:
        if sd is None:
            raise RuntimeError("sounddevice is required to play audio.")

        explicit_device_selected = self.device_index is not None
        candidate_devices = []
        if self.device_index is not None:
            candidate_devices.append(self.device_index)
        else:
            try:
                default_device = sd.default.device
                if isinstance(default_device, (tuple, list)):
                    default_output = default_device[1] if len(default_device) > 1 else None
                else:
                    default_output = default_device
                if default_output is not None and int(default_output) >= 0:
                    candidate_devices.append(int(default_output))
            except Exception:
                pass

        if not explicit_device_selected:
            devices = list_audio_devices()
            preferred = next((device for device in devices if "speaker" in device["name"].lower()), None)
            if preferred:
                candidate_devices.append(preferred["index"])
            candidate_devices.extend(device["index"] for device in devices)

        last_error = None
        for index in dict.fromkeys(candidate_devices):
            try:
                info = sd.query_devices(index)
                channels = min(max(1, int(info.get("max_output_channels", 1))), max(1, int(self.output_channels)))
                sample_rates = (44100, int(info.get("default_samplerate", 44100)), 48000)
                for sample_rate in dict.fromkeys(sample_rates):
                    try:
                        self.stream = sd.OutputStream(
                            samplerate=sample_rate,
                            channels=channels,
                            device=index,
                            callback=self._callback,
                        )
                        self.stream.start()
                        self.device_index = index
                        self.output_channels = channels
                        with self._peak_lock:
                            self._output_peaks = np.zeros(self.output_channels, dtype=np.float32)
                        self.sample_rate = sample_rate
                        loaded_accent = self._load_click_wav("accent")
                        loaded_normal = self._load_click_wav("normal")
                        self._accent_click = loaded_accent if loaded_accent is not None else np.zeros(1, dtype=np.float32)
                        self._normal_click = loaded_normal if loaded_normal is not None else np.zeros(1, dtype=np.float32)
                        self._countin_words = self._load_countin_words()
                        self._metronome_frame = 0
                        self._active_click_samples = []
                        self._active_voice_samples = []
                        return
                    except Exception as exc:
                        last_error = exc
                        self.stream = None
            except Exception as exc:
                last_error = exc

        if last_error is not None:
            raise RuntimeError(f"No working output device was found: {last_error}")
        raise RuntimeError("No output audio devices are available.")

    def stop(self) -> None:
        if self.stream is not None:
            try:
                self.stream.stop()
            except Exception:
                pass
            try:
                self.stream.close()
            except Exception:
                pass
            self.stream = None
        self._metronome_frame = 0
        self._active_click_samples = []
        self._active_voice_samples = []
        with self._peak_lock:
            self._output_peaks = np.zeros(self.output_channels, dtype=np.float32)

    def pause(self) -> None:
        if self.stream is not None:
            try:
                if getattr(self.stream, "active", False):
                    self.stream.stop()
            except Exception:
                pass

    def resume(self) -> None:
        if self.stream is not None:
            try:
                if not getattr(self.stream, "active", False):
                    self.stream.start()
            except Exception:
                pass

    def _normalize_wav(self, data: np.ndarray, target_peak: float = 0.85) -> np.ndarray:
        if data.ndim > 1:
            data = np.mean(data, axis=1)
        data = data.astype(np.float32, copy=False)
        peak = float(np.max(np.abs(data))) if data.size else 0.0
        if peak > 0:
            data = data * (target_peak / peak)
        return data

    def _resample_wav(self, data: np.ndarray, source_rate: int) -> np.ndarray:
        data = np.asarray(data, dtype=np.float32)
        if source_rate == self.sample_rate:
            return data
        target_length = max(1, int(round(len(data) * self.sample_rate / source_rate)))
        source_positions = np.linspace(0, len(data) - 1, num=len(data), dtype=np.float64)
        target_positions = np.linspace(0, len(data) - 1, num=target_length, dtype=np.float64)
        return np.interp(target_positions, source_positions, data).astype(np.float32)

    def __del__(self):
        try:
            self.stop()
        except Exception:
            pass
