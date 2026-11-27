#!/usr/bin/env python3
"""Persistent local Kokoro-82M neural TTS worker for Tracks.

Protocol (UTF-8 line-based over stdin/stdout):
  Startup -> prints "READY" once the ONNX model is loaded.
  Request -> "<voice>\\t<out_wav_path>\\t<text>\\n"
  Reply   -> "OK\\n" or "ERR <message>\\n"
"""

import argparse
import os
import sys
import wave
import numpy as np


def write_wav_int16(path: str, samples: np.ndarray, sample_rate: int) -> None:
    arr = np.asarray(samples, dtype=np.float32)
    if arr.ndim > 1:
        arr = arr.mean(axis=1)
    arr = np.clip(arr, -1.0, 1.0)
    pcm = np.rint(arr * 32767.0).astype("<i2")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp_path = path + ".tmp"
    with wave.open(tmp_path, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(int(sample_rate))
        wf.writeframes(pcm.tobytes())
    os.replace(tmp_path, path)


def lang_for_voice(voice: str) -> str:
    if voice.startswith(("bf_", "bm_")):
        return "en-gb"
    return "en-us"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--voices", required=True)
    args = parser.parse_args()

    try:
        from kokoro_onnx import Kokoro
        kokoro = Kokoro(args.model, args.voices)
    except Exception as exc:
        sys.stdout.write(f"ERR init: {exc}\n")
        sys.stdout.flush()
        return 1

    sys.stdout.write("READY\n")
    sys.stdout.flush()

    for raw_line in sys.stdin:
        line = raw_line.rstrip("\r\n")
        if not line:
            continue
        if line == "QUIT":
            break
        parts = line.split("\t", 2)
        if len(parts) != 3:
            sys.stdout.write("ERR malformed request\n")
            sys.stdout.flush()
            continue

        voice, out_path, text = parts
        try:
            samples, sr = kokoro.create(
                text,
                voice=voice,
                speed=1.0,
                lang=lang_for_voice(voice),
            )
            write_wav_int16(out_path, samples, sr)
            sys.stdout.write("OK\n")
            sys.stdout.flush()
        except Exception as exc:
            sys.stdout.write(f"ERR {exc}\n")
            sys.stdout.flush()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
