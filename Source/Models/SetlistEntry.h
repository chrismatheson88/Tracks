#pragma once

#include "Song.h"
#include <optional>
#include <string>
#include <memory>

namespace tracks {

struct SetlistEntry {
    std::optional<int64_t> id;
    std::optional<int64_t> setlistId;
    int64_t songId = 0;
    int orderIndex = 0;
    std::optional<int> overrideTempo;
    std::optional<std::string> overrideKey;
    std::optional<std::string> overrideTimeSignature;
    std::optional<int64_t> arrangementId;
    std::optional<Song> song;

    const SongArrangement* getSelectedArrangement() const {
        if (!song.has_value()) return nullptr;
        return song->findArrangement(arrangementId);
    }

    int getEffectiveTempo() const {
        if (overrideTempo.has_value() && *overrideTempo > 0)
            return *overrideTempo;
        if (const auto* arr = getSelectedArrangement(); arr != nullptr && !arr->tracks.empty())
            return arr->tempo;
        return song ? song->tempo : 120;
    }

    std::string getEffectiveKey() const {
        if (overrideKey.has_value() && !overrideKey->empty())
            return *overrideKey;
        if (const auto* arr = getSelectedArrangement(); arr != nullptr && !arr->tracks.empty() && !arr->key.empty())
            return arr->key;
        return song ? song->key : "C";
    }

    std::string getEffectiveTimeSignature() const {
        if (overrideTimeSignature.has_value() && !overrideTimeSignature->empty())
            return *overrideTimeSignature;
        if (const auto* arr = getSelectedArrangement(); arr != nullptr && !arr->tracks.empty() && !arr->timeSignature.empty())
            return arr->timeSignature;
        return song ? song->timeSignature : "4/4";
    }
};

} // namespace tracks
