from __future__ import annotations

import sys
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, simpledialog, ttk

from .db import DEFAULT_DB_PATH, delete_song, list_songs, save_song
from .models import Song, SongTrack
from .playback import PlaybackEngine


class SongEditorDialog(tk.Toplevel):
    def __init__(self, parent: tk.Tk, song: Song | None = None, on_save=None):
        super().__init__(parent)
        self.title("Edit Song" if song else "New Song")
        self.geometry("780x520")
        self.transient(parent)
        self.grab_set()

        self.song = song or Song()
        self.on_save = on_save

        main = ttk.Frame(self, padding=12)
        main.pack(fill="both", expand=True)

        fields = ttk.Frame(main)
        fields.pack(fill="x", pady=(0, 10))

        ttk.Label(fields, text="Title").grid(row=0, column=0, sticky="w", padx=(0, 8), pady=4)
        self.title_var = tk.StringVar(value=self.song.title)
        ttk.Entry(fields, textvariable=self.title_var, width=30).grid(row=0, column=1, sticky="ew", padx=(0, 12))

        ttk.Label(fields, text="Artist").grid(row=0, column=2, sticky="w", padx=(0, 8), pady=4)
        self.artist_var = tk.StringVar(value=self.song.artist)
        ttk.Entry(fields, textvariable=self.artist_var, width=30).grid(row=0, column=3, sticky="ew")

        ttk.Label(fields, text="Tempo").grid(row=1, column=0, sticky="w", padx=(0, 8), pady=4)
        self.tempo_var = tk.StringVar(value=str(self.song.tempo))
        ttk.Entry(fields, textvariable=self.tempo_var, width=12).grid(row=1, column=1, sticky="w", padx=(0, 12))

        ttk.Label(fields, text="Key").grid(row=1, column=2, sticky="w", padx=(0, 8), pady=4)
        self.key_var = tk.StringVar(value=self.song.key)
        ttk.Entry(fields, textvariable=self.key_var, width=12).grid(row=1, column=3, sticky="w")

        ttk.Label(fields, text="Time sig").grid(row=2, column=0, sticky="w", padx=(0, 8), pady=4)
        self.time_signature_var = tk.StringVar(value=self.song.time_signature)
        ttk.Entry(fields, textvariable=self.time_signature_var, width=12).grid(row=2, column=1, sticky="w", padx=(0, 12))

        ttk.Label(fields, text="Notes").grid(row=3, column=0, sticky="nw", padx=(0, 8), pady=4)
        self.notes_var = tk.StringVar(value=self.song.notes)
        ttk.Entry(fields, textvariable=self.notes_var, width=80).grid(row=3, column=1, columnspan=3, sticky="ew")

        fields.columnconfigure(1, weight=1)
        fields.columnconfigure(3, weight=1)

        buttons = ttk.Frame(main)
        buttons.pack(fill="x", pady=(0, 8))
        ttk.Button(buttons, text="Add Tracks", command=self.add_tracks).pack(side="left", padx=(0, 6))
        ttk.Button(buttons, text="Edit Selected Track", command=self.edit_selected_track).pack(side="left", padx=6)
        ttk.Button(buttons, text="Remove Selected Track", command=self.remove_selected_track).pack(side="left", padx=6)

        self.track_table = ttk.Treeview(main, columns=("name", "file", "channel", "gain", "muted"), show="headings")
        self.track_table.heading("name", text="Name")
        self.track_table.heading("file", text="File")
        self.track_table.heading("channel", text="Channel")
        self.track_table.heading("gain", text="Gain")
        self.track_table.heading("muted", text="Muted")
        self.track_table.column("name", width=120)
        self.track_table.column("file", width=280)
        self.track_table.column("channel", width=70)
        self.track_table.column("gain", width=70)
        self.track_table.column("muted", width=60)
        self.track_table.pack(fill="both", expand=True)

        save_bar = ttk.Frame(main)
        save_bar.pack(fill="x", pady=(8, 0))
        ttk.Button(save_bar, text="Save", command=self.save_song).pack(side="right")
        ttk.Button(save_bar, text="Cancel", command=self.destroy).pack(side="right", padx=(0, 8))

        self.refresh_tracks()

    def refresh_tracks(self) -> None:
        for item in self.track_table.get_children():
            self.track_table.delete(item)
        for index, track in enumerate(self.song.tracks):
            self.track_table.insert(
                "",
                "end",
                iid=str(index),
                values=(track.name, Path(track.file_path).name if track.file_path else "", track.output_channel, f"{track.gain:.2f}", "Yes" if track.muted else "No"),
            )

    def add_tracks(self) -> None:
        selected_files = filedialog.askopenfilenames(
            title="Select backing track wav files",
            filetypes=[("WAV files", "*.wav"), ("All files", "*.*")],
        )
        if not selected_files:
            return

        next_channel = max((track.output_channel for track in self.song.tracks), default=-1) + 1
        for file_path in selected_files:
            path = Path(file_path)
            self.song.add_track(
                SongTrack(
                    name=path.stem,
                    file_path=str(path),
                    output_channel=next_channel,
                    gain=1.0,
                )
            )
            next_channel += 1
        self.refresh_tracks()

    def edit_selected_track(self) -> None:
        selection = self.track_table.selection()
        if not selection:
            return

        track = self.song.tracks[int(selection[0])]
        name = simpledialog.askstring("Track name", "Track name:", initialvalue=track.name)
        if name is not None:
            track.name = name

        channel = simpledialog.askinteger("Output channel", "Output channel:", initialvalue=track.output_channel, minvalue=0, maxvalue=31)
        if channel is not None:
            track.output_channel = channel

        gain = simpledialog.askfloat("Gain", "Gain level:", initialvalue=track.gain, minvalue=0.0, maxvalue=3.0)
        if gain is not None:
            track.gain = gain

        if messagebox.askyesno("Mute track", "Mute this track?", default=track.muted):
            track.muted = True
        else:
            track.muted = False

        self.refresh_tracks()

    def remove_selected_track(self) -> None:
        selection = self.track_table.selection()
        if not selection:
            return
        index = int(selection[0])
        del self.song.tracks[index]
        self.refresh_tracks()

    def save_song(self) -> None:
        title = self.title_var.get().strip()
        artist = self.artist_var.get().strip() or "Unknown"
        tempo = self.tempo_var.get().strip() or "120"
        key = self.key_var.get().strip() or "C"
        time_signature = (self.time_signature_var.get().strip() or "4/4").strip() or "4/4"
        notes = self.notes_var.get().strip()

        if not title:
            messagebox.showerror("Missing title", "A song title is required.")
            return

        try:
            tempo_value = int(tempo)
        except ValueError:
            messagebox.showerror("Invalid tempo", "Tempo must be an integer number of BPM.")
            return

        self.song.title = title
        self.song.artist = artist
        self.song.tempo = tempo_value
        self.song.key = key
        self.song.time_signature = time_signature
        self.song.notes = notes

        save_song(self.song, DEFAULT_DB_PATH)
        if self.on_save:
            self.on_save()
        self.destroy()


class TracksApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("Tracks")
        self.root.geometry("980x620")
        self.root.minsize(840, 520)

        self.current_song = None
        self.player = None

        left = ttk.Frame(root, padding=10)
        left.pack(side="left", fill="y")
        ttk.Label(left, text="Library").pack(anchor="w")

        self.library_list = tk.Listbox(left, width=38, height=28, exportselection=False)
        self.library_list.bind("<<ListboxSelect>>", self.on_select_song)
        self.library_list.pack(fill="both", expand=True)

        controls = ttk.Frame(left)
        controls.pack(fill="x", pady=(10, 0))
        ttk.Button(controls, text="New Song", command=self.new_song).pack(side="left", padx=(0, 6))
        ttk.Button(controls, text="Edit Song", command=self.edit_selected_song).pack(side="left", padx=6)
        ttk.Button(controls, text="Delete", command=self.delete_selected_song).pack(side="left", padx=6)

        right = ttk.Frame(root, padding=10)
        right.pack(side="left", fill="both", expand=True)

        toolbar = ttk.Frame(right)
        toolbar.pack(fill="x", pady=(0, 10))
        ttk.Button(toolbar, text="Play", command=self.play_selected_song).pack(side="left", padx=(0, 6))
        ttk.Button(toolbar, text="Stop", command=self.stop_playback).pack(side="left", padx=6)

        self.info_text = tk.Text(right, wrap="word", state="disabled", width=60, height=18)
        self.info_text.pack(fill="both", expand=True)

        self.refresh_library()

    def refresh_library(self) -> None:
        self.library_list.delete(0, tk.END)
        for song in list_songs(DEFAULT_DB_PATH):
            self.library_list.insert(tk.END, song.to_summary())

    def on_select_song(self, _event) -> None:
        selection = self.library_list.curselection()
        if not selection:
            return
        index = selection[0]
        songs = list_songs(DEFAULT_DB_PATH)
        if index >= len(songs):
            return
        self.current_song = songs[index]
        self.info_text.config(state="normal")
        self.info_text.delete("1.0", tk.END)
        self.info_text.insert(
            tk.END,
            f"Title: {self.current_song.title}\n"
            f"Artist: {self.current_song.artist}\n"
            f"Tempo: {self.current_song.tempo} BPM\n"
            f"Key: {self.current_song.key}\n"
            f"Time signature: {self.current_song.time_signature}\n\n"
            f"Notes: {self.current_song.notes or 'None'}\n\n"
            f"Tracks:\n"
            + "\n".join(
                f"- {track.name}: {Path(track.file_path).name} -> channel {track.output_channel}, gain {track.gain:.2f}{' (muted)' if track.muted else ''}"
                for track in self.current_song.tracks
            )
            if self.current_song.tracks
            else "- No tracks attached yet",
        )
        self.info_text.config(state="disabled")

    def new_song(self) -> None:
        SongEditorDialog(self.root, on_save=self.refresh_library).wait_window()

    def edit_selected_song(self) -> None:
        if self.current_song is None:
            messagebox.showinfo("No song selected", "Select a song from the library first.")
            return
        current_id = self.current_song.id
        SongEditorDialog(self.root, song=self.current_song, on_save=self.refresh_library).wait_window()
        songs = list_songs(DEFAULT_DB_PATH)
        self.current_song = next((song for song in songs if song.id == current_id), songs[0] if songs else None)

    def play_selected_song(self) -> None:
        if self.current_song is None:
            messagebox.showinfo("No song selected", "Select a song from the library first.")
            return
        self.stop_playback()
        output_channels = max(2, max((track.output_channel for track in self.current_song.tracks), default=0) + 1)
        self.player = PlaybackEngine(self.current_song, output_channels=output_channels)
        try:
            self.player.start()
        except RuntimeError as exc:
            messagebox.showerror("Playback error", str(exc))

    def stop_playback(self) -> None:
        if self.player is not None:
            self.player.stop()
            self.player = None

    def delete_selected_song(self) -> None:
        if self.current_song is None:
            return
        if messagebox.askyesno("Delete song", f"Delete {self.current_song.title} from the library?"):
            delete_song(self.current_song.id, DEFAULT_DB_PATH)
            self.current_song = None
            self.stop_playback()
            self.refresh_library()


def launch_app() -> None:
    try:
        root = tk.Tk()
    except tk.TclError as exc:
        print(
            "Tkinter could not open a desktop window. This app needs a local desktop session "
            "(not a headless or remote terminal).",
            file=sys.stderr,
        )
        raise SystemExit(1) from exc

    root.title("Tracks")
    root.geometry("980x620")
    root.minsize(840, 520)
    TracksApp(root)
    root.update_idletasks()
    root.deiconify()
    root.attributes("-topmost", True)
    root.lift()
    root.focus_force()
    root.after(250, lambda: root.attributes("-topmost", False))
    root.mainloop()
