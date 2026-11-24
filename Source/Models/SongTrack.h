#pragma once

#include <string>
#include <optional>

namespace tracks {

struct SongTrack {
    std::optional<int64_t> id;
    std::string name;
    std::string filePath;
    int outputChannel = 0; // 0-indexed
    bool stereoOutput = true; // true = stereo pair [ch, ch+1] when stereo track; false = mono sum on single ch
    float gain = 1.0f;
    bool muted = false;
    std::string trackKey;
    int numChannels = 0; // 1 = Mono, 2 = Stereo, 0 = Unknown
};

} // namespace tracks
