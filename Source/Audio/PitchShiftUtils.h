#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace tracks {

inline std::optional<int> parseKeyToPitchClass(const std::string& rawKey) {
    // Trim whitespace
    size_t start = 0;
    while (start < rawKey.size() && std::isspace(static_cast<unsigned char>(rawKey[start]))) ++start;
    size_t end = rawKey.size();
    while (end > start && std::isspace(static_cast<unsigned char>(rawKey[end - 1]))) --end;
    if (start >= end) return std::nullopt;

    std::string k = rawKey.substr(start, end - start);
    char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(k[0])));

    int base = -1;
    switch (letter) {
        case 'C': base = 0; break;
        case 'D': base = 2; break;
        case 'E': base = 4; break;
        case 'F': base = 5; break;
        case 'G': base = 7; break;
        case 'A': base = 9; break;
        case 'B': base = 11; break;
        default: return std::nullopt;
    }

    if (k.size() >= 2) {
        char acc = k[1];
        if (acc == '#') {
            base = (base + 1) % 12;
        } else if (acc == 'b') {
            base = (base + 11) % 12;
        }
    }
    return base;
}

inline bool isMinorKey(const std::string& rawKey) {
    size_t start = 0;
    while (start < rawKey.size() && std::isspace(static_cast<unsigned char>(rawKey[start]))) ++start;
    size_t end = rawKey.size();
    while (end > start && std::isspace(static_cast<unsigned char>(rawKey[end - 1]))) --end;
    if (start >= end) return false;

    std::string k = rawKey.substr(start, end - start);
    size_t pos = 1; // Skip root letter (A-G)
    if (pos < k.size() && (k[pos] == '#' || k[pos] == 'b')) {
        ++pos;
    }
    while (pos < k.size() && std::isspace(static_cast<unsigned char>(k[pos]))) {
        ++pos;
    }
    if (pos >= k.size()) return false;

    std::string suffix = k.substr(pos);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (suffix.rfind("maj", 0) == 0) return false;
    return suffix[0] == 'm';
}

inline int computeSemitoneShift(const std::string& fromKey, const std::string& toKey) {
    auto fromPc = parseKeyToPitchClass(fromKey);
    auto toPc = parseKeyToPitchClass(toKey);
    if (!fromPc.has_value() || !toPc.has_value()) return 0;

    int diff = (*toPc - *fromPc) % 12;
    if (diff < 0) diff += 12;
    // Wrap to shortest musical transposition interval [-6, +6]
    if (diff > 6) diff -= 12;
    return diff;
}

inline bool shouldExcludeFromPitchShift(const std::string& trackName) {
    if (trackName.empty()) return false;

    std::string lower = trackName;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Unambiguous drum / percussion / FX / click-guide substrings
    static const char* kExcludedSubstrings[] = {
        "drum", "kick", "snare", "hihat", "cymbal", "percussion", "perc",
        "tambourine", "shaker", "cajon", "timpani", "conga", "bongo",
        "maraca", "cowbell", "clap", "overhead", "click", "guide",
        "sfx", "effect", "riser", "sweep", "impact", "crowd", "ambience", "noise"
    };
    for (const char* sub : kExcludedSubstrings) {
        if (lower.find(sub) != std::string::npos) {
            return true;
        }
    }

    // Tokenize into alphabetic words (splitting on non-letters so "FX1", "FX_2", "Hi-Hat", "OH_L" cleanly produce "fx", "hi", "hat", "oh")
    std::vector<std::string> tokens;
    std::string cur;
    for (unsigned char ch : lower) {
        if (std::isalpha(ch)) {
            cur.push_back(static_cast<char>(ch));
        } else if (!cur.empty()) {
            tokens.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) tokens.push_back(cur);

    static const char* kExcludedTokens[] = {
        "fx", "hat", "hats", "hh", "tom", "toms", "oh", "kit", "rim",
        "bd", "sd", "tam", "cue", "cues", "swell", "swells", "hit", "hits",
        "boom", "booms", "subdrop"
    };
    for (const auto& tok : tokens) {
        for (const char* ex : kExcludedTokens) {
            if (tok == ex) return true;
        }
    }

    return false;
}

} // namespace tracks
