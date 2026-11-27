#pragma once

#include "Song.h"
#include <optional>
#include <string>
#include <memory>

namespace tracks {

enum class SongTransitionMode : int {
    StayInCurrentSong = 0,
    CueNextSong = 1,
    AutoplayNextSong = 2,
    Crossfade = 3
};

inline SongTransitionMode clampTransitionMode(int raw) {
    if (raw == 4) return SongTransitionMode::Crossfade; // Legacy Crossfade2Bar value
    if (raw < 0 || raw > 3) return SongTransitionMode::StayInCurrentSong;
    return static_cast<SongTransitionMode>(raw);
}

inline int clampCrossfadeBars(int bars) {
    return (bars < 1) ? 1 : ((bars > 4) ? 4 : bars);
}

inline int getCrossfadeBarsForTransition(SongTransitionMode mode, int crossfadeBars) {
    if (mode == SongTransitionMode::Crossfade) {
        return clampCrossfadeBars(crossfadeBars);
    }
    return 0;
}

struct SetlistEntry {
    std::optional<int64_t> id;
    std::optional<int64_t> setlistId;
    int64_t songId = 0;
    int orderIndex = 0;
    std::optional<int> overrideTempo;
    std::optional<std::string> overrideKey;
    std::optional<std::string> overrideTimeSignature;
    std::optional<int64_t> arrangementId;
    bool justClick = false;
    SongTransitionMode transitionMode = SongTransitionMode::StayInCurrentSong;
    int transitionCrossfadeBars = 1;
    std::optional<Song> song;

    const SongArrangement* getSelectedArrangement() const {
        if (justClick || !song.has_value()) return nullptr;
        return song->findArrangement(arrangementId);
    }

    int getEffectiveTempo() const {
        if (overrideTempo.has_value() && *overrideTempo > 0)
            return *overrideTempo;
        if (const auto* arr = getSelectedArrangement(); arr != nullptr && arr->tempo > 0)
            return arr->tempo;
        return song ? song->tempo : 120;
    }

    std::string getEffectiveKey() const {
        if (overrideKey.has_value() && !overrideKey->empty())
            return *overrideKey;
        if (const auto* arr = getSelectedArrangement(); arr != nullptr && !arr->key.empty())
            return arr->key;
        return song ? song->key : "C";
    }

    std::string getEffectiveTimeSignature() const {
        if (overrideTimeSignature.has_value() && !overrideTimeSignature->empty())
            return *overrideTimeSignature;
        if (const auto* arr = getSelectedArrangement(); arr != nullptr && !arr->timeSignature.empty())
            return arr->timeSignature;
        return song ? song->timeSignature : "4/4";
    }
};

} // namespace tracks
