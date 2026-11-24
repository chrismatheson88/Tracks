#pragma once

#include <string>
#include <optional>

namespace tracks {

struct SongTrack {
    std::optional<int64_t> id;
    std::string name;
    std::string filePath;
    int outputChannel = 0; // 0-indexed
    float gain = 1.0f;
    bool muted = false;
};

} // namespace tracks
