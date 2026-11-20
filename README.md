# Tracks

Tracks is a self-contained desktop backing-track library app. It stores songs, tempos, keys, WAV files, set lists, and app settings locally. Desktop playback, click tones, and count-in voices do not depend on a browser or OS speech service.

## Features

- Store songs with title, artist, tempo, key, and time signature.
- Attach multiple WAV files to each song.
- Keep track metadata such as output channel and gain.
- Manage set lists and edit song metadata in the desktop set playback view.
- Use native multi-channel audio output.
- Play every metronome beat with a two-bar count-in.
- Use bundled count-in voice WAV files and generated click tones.

## Native desktop quick start

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
python -m tracks_app
```

## Portable build

The SQLite database, bundled count-in WAV assets, Python source, and installed dependencies can be packaged with PyInstaller. Moving the full source folder to macOS or Windows also allows building a native executable on that device.

## Count-in assets

Bundled count-in voice files are in:

`tracks_app/assets/countin/female/`

No browser or system speech synthesis is used at runtime.
