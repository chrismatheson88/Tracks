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

struct TempoPoint {
    std::optional<int64_t> id;
    int bar = 1;            // 1-indexed musical bar in the arrangement's audio timeline
    double beat = 1.0;      // 1-indexed beat/pulse within the bar (1.0 .. beatsPerBar)
    double bpm = 120.0;     // Target BPM (20.0 .. 300.0)
    double rampBars = 0.0;  // 0.0 = instant Step at (bar, beat); > 0 = linear ramp starting at (bar, beat) over rampBars bars
    int orderIndex = 0;

    bool isRamp() const { return rampBars > 1e-4; }
};

struct SongArrangement {
    std::optional<int64_t> id;
    std::string name = "Main";
    int tempo = 120;
    std::string key = "C";
    std::string timeSignature = "4/4";
    bool clickEighths = false;
    bool clickSixteenths = false;
    float clickEighthsGain = 1.0f;
    float clickSixteenthsGain = 1.0f;
    std::vector<SongTrack> tracks;
    std::vector<SongSection> sections;
    std::vector<TempoPoint> tempoPoints;

    std::string toDisplayLabel() const {
        std::ostringstream oss;
        oss << (name.empty() ? "Main" : name)
            << " (" << tempo << " BPM, " << (key.empty() ? "C" : key)
            << ", " << (timeSignature.empty() ? "4/4" : timeSignature) << ")";
        return oss.str();
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
    std::vector<TempoPoint> tempoPoints;
    std::vector<SongArrangement> arrangements;
    std::optional<int64_t> activeArrangementId; // Runtime selection for playback & caching
    bool justClickMode = false;                 // Runtime flag when "Just click" is chosen in Live/Transport

    void addTrack(const SongTrack& track) {
        tracks.push_back(track);
    }

    const SongArrangement* findArrangement(std::optional<int64_t> arrId) const {
        if (arrangements.empty()) return nullptr;
        if (arrId.has_value()) {
            for (const auto& arr : arrangements) {
                if (arr.id.has_value() && *arr.id == *arrId) {
                    return &arr;
                }
            }
        }
        return &arrangements.front();
    }

    SongArrangement* findArrangement(std::optional<int64_t> arrId) {
        if (arrangements.empty()) return nullptr;
        if (arrId.has_value()) {
            for (auto& arr : arrangements) {
                if (arr.id.has_value() && *arr.id == *arrId) {
                    return &arr;
                }
            }
        }
        return &arrangements.front();
    }

    bool hasAnyTracks() const {
        for (const auto& arr : arrangements) {
            if (!arr.tracks.empty()) return true;
        }
        return !tracks.empty();
    }

    std::string toSummary() const {
        std::ostringstream oss;
        oss << title << " - " << artist << " (" << tempo << " BPM, " << key << ", " << timeSignature << ")";
        return oss.str();
    }
};

} // namespace tracks
