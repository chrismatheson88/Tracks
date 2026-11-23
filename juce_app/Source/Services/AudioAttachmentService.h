#pragma once

#include "../Models/SongTrack.h"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace tracks {

struct AudioDirectoryInspectionResult {
    std::vector<SongTrack> tracks;
    std::vector<std::string> mismatches;
    std::vector<std::string> channelDetails;
    std::optional<std::string> error;
};

class AudioAttachmentService {
public:
    static std::filesystem::path getProjectRoot();
    static std::filesystem::path getSongStorageDir(int64_t songId);
    static std::filesystem::path getSongTracksDir(int64_t songId);
    static void removeSongStorage(int64_t songId);
    static void clearSongTrackDirectory(int64_t songId);

    static AudioDirectoryInspectionResult inspectAudioDirectory(const std::string& directoryPath);
    static std::vector<SongTrack> copyTracksToSongDirectory(int64_t songId, const std::vector<SongTrack>& tracks);
};

} // namespace tracks
