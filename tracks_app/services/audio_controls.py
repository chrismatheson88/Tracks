def db_to_linear(db: int) -> float:
    db = max(-30, min(12, int(db)))
    if db <= -30:
        return 0.0
    return float(10.0 ** (db / 20.0))


def effective_set_entry_values(entry, base_song) -> tuple[int, str, str]:
    tempo = entry.override_tempo if entry.override_tempo is not None else base_song.tempo
    key = entry.override_key if entry.override_key else base_song.key
    time_signature = entry.override_time_signature if entry.override_time_signature else base_song.time_signature
    return int(tempo), str(key or "C"), str(time_signature or "4/4")
