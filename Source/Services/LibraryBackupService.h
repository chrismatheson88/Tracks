#pragma once

#include "../Database/DatabaseService.h"
#include <filesystem>
#include <functional>
#include <string>

namespace tracks {

struct BackupResult {
    bool success = false;
    std::string message;
    int songCount = 0;
    int setlistCount = 0;
    int audioFileCount = 0;
};

class LibraryBackupService {
public:
    using ProgressCallback = std::function<void(const std::string& status)>;

    // Exports tracks_library.sqlite3, manifest.json, and all uploaded_tracks files
    // into a single compressed ZIP archive at outputZipPath.
    static BackupResult exportBackup(DatabaseService& db,
                                     const std::string& outputZipPath,
                                     const std::filesystem::path& uploadedTracksRoot,
                                     ProgressCallback onProgress = nullptr);

    // Extracts a compressed backup ZIP archive, replaces the local library database
    // and uploaded_tracks directory atomically, and remaps track paths to uploadedTracksRoot.
    static BackupResult importBackup(DatabaseService& db,
                                     const std::string& inputZipPath,
                                     const std::filesystem::path& uploadedTracksRoot,
                                     std::function<void()> onBeforeReplace = nullptr,
                                     ProgressCallback onProgress = nullptr);
};

} // namespace tracks
