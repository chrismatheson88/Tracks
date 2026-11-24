#pragma once

#include "SetlistEntry.h"
#include <string>
#include <vector>
#include <optional>

namespace tracks {

struct Setlist {
    std::optional<int64_t> id;
    std::string name;
    std::string setDate;
    std::string setTime;
    std::string notes;
    std::vector<SetlistEntry> entries;

    void addSong(int64_t songId) {
        SetlistEntry entry;
        entry.songId = songId;
        entry.orderIndex = static_cast<int>(entries.size());
        entries.push_back(entry);
    }
};

} // namespace tracks
