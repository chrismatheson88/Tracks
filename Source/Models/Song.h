#pragma once

#include "SongTrack.h"
#include <string>
#include <vector>
#include <optional>
#include <sstream>

namespace tracks {

struct Song {
    std::optional<int64_t> id;
    std::string title;
    std::string artist;
    int tempo = 120;
    std::string key = "C";
    std::string notes;
    std::string timeSignature = "4/4";
    std::vector<SongTrack> tracks;

    void addTrack(const SongTrack& track) {
        tracks.push_back(track);
    }

    std::string toSummary() const {
        std::ostringstream oss;
        oss << title << " — " << artist << " (" << tempo << " BPM, " << key << ", " << timeSignature << ")";
        return oss.str();
    }
};

} // namespace tracks
