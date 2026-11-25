#pragma once

#include "Song.h"
#include <string>
#include <vector>

namespace tracks {

struct AvailableSectionGroup {
    std::string groupName;
    std::vector<std::string> sections;
};

// Canonical list of available track sections, referenced from the playback_female
// guide cue set plus "Count". Used by the Track Sections editor to constrain section
// names to those with guide cues.
inline const std::vector<AvailableSectionGroup>& getAvailableSectionGroups() {
    static const std::vector<AvailableSectionGroup> kGroups = {
        { "Count", { "Count" } },
        { "Intro", { "Intro" } },
        { "Verse", {
            "Verse", "Verse 1", "Verse 2", "Verse 3", "Verse 4",
            "Verse 5", "Verse 6", "Verse 7", "Verse 8"
        } },
        { "Pre Chorus", {
            "Pre Chorus", "Pre Chorus 1", "Pre Chorus 2", "Pre Chorus 3", "Pre Chorus 4"
        } },
        { "Chorus", {
            "Chorus", "Chorus 1", "Chorus 2", "Chorus 3", "Chorus 4",
            "Chorus 5", "Chorus 6", "Chorus 7", "Chorus 8"
        } },
        { "Post Chorus", {
            "Post Chorus", "Post Chorus 1", "Post Chorus 2", "Post Chorus 3", "Post Chorus 4"
        } },
        { "Bridge", {
            "Bridge", "Bridge 1", "Bridge 2", "Bridge 3", "Bridge 4",
            "Bridge 5", "Bridge 6", "Bridge 7", "Bridge 8"
        } },
        { "Refrain", {
            "Refrain", "Refrain 1", "Refrain 2", "Refrain 3", "Refrain 4"
        } },
        { "Tag", {
            "Tag", "Tag 1", "Tag 2", "Tag 3", "Tag 4"
        } },
        { "Turnaround", { "Turnaround" } },
        { "Interlude", { "Interlude" } },
        { "Instrumental", { "Instrumental" } },
        { "Solo", { "Solo" } },
        { "Breakdown", { "Breakdown" } },
        { "Acapella", { "Acapella" } },
        { "Rap", { "Rap" } },
        { "Exhortation", { "Exhortation" } },
        { "Vamp", {
            "Vamp", "Vamp Verse", "Vamp Pre Chorus", "Vamp Chorus", "Vamp Bridge",
            "Vamp on 1", "Vamp on 1 and 1sus", "Vamp on 4",
            "Vamp Ending on 1", "Vamp Ending on 4"
        } },
        { "Outro", { "Outro" } },
        { "Ending", { "Ending" } }
    };
    return kGroups;
}

inline const std::vector<std::string>& getAvailableSections() {
    static const std::vector<std::string> kAllSections = []() {
        std::vector<std::string> flat;
        for (const auto& group : getAvailableSectionGroups()) {
            for (const auto& sec : group.sections) {
                flat.push_back(sec);
            }
        }
        return flat;
    }();
    return kAllSections;
}

inline std::string findCanonicalSectionName(const std::string& name) {
    const std::string norm = SongSection::normalizeSectionName(name);
    if (norm.empty()) return {};
    for (const auto& sec : getAvailableSections()) {
        if (SongSection::normalizeSectionName(sec) == norm) {
            return sec;
        }
    }
    return {};
}

inline bool isAvailableSectionName(const std::string& name) {
    return !findCanonicalSectionName(name).empty();
}

// Filters available sections by case-insensitive query tokens, ranking prefix matches first.
inline std::vector<std::string> filterAvailableSections(const std::string& query) {
    const std::string normQuery = SongSection::normalizeSectionName(query);
    const auto& all = getAvailableSections();
    if (normQuery.empty()) {
        return all;
    }

    // Split normalized query into space-separated tokens
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < normQuery.size()) {
        size_t next = normQuery.find(' ', pos);
        if (next == std::string::npos) {
            tokens.push_back(normQuery.substr(pos));
            break;
        }
        if (next > pos) {
            tokens.push_back(normQuery.substr(pos, next - pos));
        }
        pos = next + 1;
    }

    std::vector<std::string> exactMatches;
    std::vector<std::string> prefixMatches;
    std::vector<std::string> containsMatches;

    for (const auto& sec : all) {
        const std::string normSec = SongSection::normalizeSectionName(sec);
        if (normSec == normQuery) {
            exactMatches.push_back(sec);
            continue;
        }
        if (normSec.rfind(normQuery, 0) == 0) {
            prefixMatches.push_back(sec);
            continue;
        }
        bool allTokensFound = true;
        for (const auto& tok : tokens) {
            if (normSec.find(tok) == std::string::npos) {
                allTokensFound = false;
                break;
            }
        }
        if (allTokensFound) {
            containsMatches.push_back(sec);
        }
    }

    std::vector<std::string> result;
    result.reserve(exactMatches.size() + prefixMatches.size() + containsMatches.size());
    result.insert(result.end(), exactMatches.begin(), exactMatches.end());
    result.insert(result.end(), prefixMatches.begin(), prefixMatches.end());
    result.insert(result.end(), containsMatches.begin(), containsMatches.end());
    return result;
}

} // namespace tracks
