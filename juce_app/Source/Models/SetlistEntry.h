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
    std::optional<Song> song;

    int getEffectiveTempo() const {
        if (overrideTempo.has_value() && *overrideTempo > 0)
            return *overrideTempo;
        return song ? song->tempo : 120;
    }

    std::string getEffectiveKey() const {
        if (overrideKey.has_value() && !overrideKey->empty())
            return *overrideKey;
        return song ? song->key : "C";
    }

    std::string getEffectiveTimeSignature() const {
        if (overrideTimeSignature.has_value() && !overrideTimeSignature->empty())
            return *overrideTimeSignature;
        return song ? song->timeSignature : "4/4";
    }
};

} // namespace tracks
