#pragma once

#include "SongTrack.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
#include <optional>
#include <sstream>

namespace tracks {

struct SongSection {
    std::optional<int64_t> id;
    std::string name;
    int lengthBars = 4;
    int orderIndex = 0;

    static std::string normalizeSectionName(const std::string& raw) {
        std::string out;
        out.reserve(raw.size());
        bool prevSpace = true;
        for (unsigned char ch : raw) {
            if (std::isalnum(ch)) {
                out.push_back(static_cast<char>(std::tolower(ch)));
                prevSpace = false;
            } else if (std::isspace(ch) || ch == '-' || ch == '_') {
                if (!prevSpace) {
                    out.push_back(' ');
                    prevSpace = true;
                }
            }
        }
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
        return out;
    }

    std::string normalizedKey() const {
        return normalizeSectionName(name);
    }

    bool isCountSection() const {
        return normalizedKey() == "count";
    }
};

struct Song {
    std::optional<int64_t> id;
    std::string title;
    std::string artist;
    int tempo = 120;
    std::string key = "C";
    std::string notes;
    std::string timeSignature = "4/4";
    bool clickEighths = false;
    bool clickSixteenths = false;
    float clickEighthsGain = 1.0f;
    float clickSixteenthsGain = 1.0f;
    std::vector<SongTrack> tracks;
    std::vector<SongSection> sections;

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
