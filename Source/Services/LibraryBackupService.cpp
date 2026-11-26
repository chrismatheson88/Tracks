#include "LibraryBackupService.h"
#include "AudioAttachmentService.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace tracks {

namespace {

bool isSafeRelativeZipPath(const std::string& pathStr) {
    if (pathStr.empty()) return false;
    if (pathStr[0] == '/' || pathStr[0] == '\\') return false;
    if (pathStr.size() >= 2 && pathStr[1] == ':') return false; // Windows drive letter

    std::string normalized = pathStr;
    for (char& ch : normalized) {
        if (ch == '\\') ch = '/';
    }

    size_t pos = 0;
    while (pos < normalized.size()) {
        size_t next = normalized.find('/', pos);
        std::string segment = (next == std::string::npos) ? normalized.substr(pos) : normalized.substr(pos, next - pos);
        if (segment == "..") return false;
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return true;
}

int compressionLevelForFile(const fs::path& filePath) {
    std::string ext = filePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // Already-compressed formats gain nothing from deflate; store directly for speed
    if (ext == ".mp3" || ext == ".m4a" || ext == ".aac" || ext == ".ogg" || ext == ".flac") {
        return 0;
    }
    if (ext == ".wav" || ext == ".aif" || ext == ".aiff") {
        return 4;
    }
    return 6;
}

fs::path createUniqueTempDir(const std::string& prefix) {
    auto base = fs::path(juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName().toStdString());
    auto nowNs = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = base / (prefix + "_" + std::to_string(nowNs));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

void moveOrCopyDirectory(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::rename(src, dst, ec);
    if (!ec) return;

    // Cross-filesystem fallback (e.g. /tmp on a different volume from workspace)
    fs::create_directories(dst);
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    fs::remove_all(src, ec);
}

} // namespace

BackupResult LibraryBackupService::exportBackup(DatabaseService& db,
                                                const std::string& outputZipPath,
                                                const fs::path& uploadedTracksRoot,
                                                ProgressCallback onProgress) {
    BackupResult result;
    if (outputZipPath.empty()) {
        result.message = "No destination file selected.";
        return result;
    }

    fs::path stagingDir = createUniqueTempDir("tracks_backup_export");
    auto cleanupStaging = [&]() {
        std::error_code ec;
        fs::remove_all(stagingDir, ec);
    };

    try {
        if (onProgress) onProgress("Creating consistent database snapshot...");

        const fs::path snapshotDbPath = stagingDir / "tracks_library.sqlite3";
        db.exportSnapshotToFile(snapshotDbPath.string());

        const auto songs = db.listSongs();
        const auto setlists = db.listSetlists();
        result.songCount = static_cast<int>(songs.size());
        result.setlistCount = static_cast<int>(setlists.size());

        // Gather all files under uploaded_tracks/
        if (onProgress) onProgress("Scanning attached audio tracks...");
        struct TrackZipItem {
            fs::path absPath;
            std::string zipEntryName;
            int compressionLevel = 4;
        };
        std::vector<TrackZipItem> trackItems;
        std::error_code ec;
        if (fs::exists(uploadedTracksRoot, ec) && fs::is_directory(uploadedTracksRoot, ec)) {
            for (const auto& entry : fs::recursive_directory_iterator(uploadedTracksRoot, ec)) {
                if (!entry.is_regular_file()) continue;
                std::string fname = entry.path().filename().string();
                if (fname.empty() || fname[0] == '.') continue; // Skip .DS_Store and hidden files

                fs::path rel = fs::relative(entry.path(), uploadedTracksRoot, ec);
                if (ec || rel.empty()) continue;

                std::string zipName = "uploaded_tracks/" + rel.generic_string();
                trackItems.push_back({ entry.path(), std::move(zipName), compressionLevelForFile(entry.path()) });
            }
        }
        std::sort(trackItems.begin(), trackItems.end(), [](const TrackZipItem& a, const TrackZipItem& b) {
            return a.zipEntryName < b.zipEntryName;
        });
        result.audioFileCount = static_cast<int>(trackItems.size());

        // Write manifest.json
        const fs::path manifestPath = stagingDir / "manifest.json";
        {
            std::ofstream mf(manifestPath);
            juce::Time now = juce::Time::getCurrentTime();
            mf << "{\n"
               << "  \"format\": \"tracks-library-backup\",\n"
               << "  \"version\": 1,\n"
               << "  \"createdAt\": \"" << now.toISO8601(true).toStdString() << "\",\n"
               << "  \"songCount\": " << result.songCount << ",\n"
               << "  \"setlistCount\": " << result.setlistCount << ",\n"
               << "  \"audioFileCount\": " << result.audioFileCount << "\n"
               << "}\n";
        }

        if (onProgress) {
            onProgress("Compressing library and " + std::to_string(result.audioFileCount) + " audio file(s)...");
        }

        juce::ZipFile::Builder builder;
        builder.addFile(juce::File(manifestPath.string()), 6, "manifest.json");
        builder.addFile(juce::File(snapshotDbPath.string()), 6, "tracks_library.sqlite3");
        for (const auto& item : trackItems) {
            builder.addFile(juce::File(item.absPath.string()), item.compressionLevel, item.zipEntryName);
        }

        fs::path outPath(outputZipPath);
        if (outPath.has_parent_path()) {
            fs::create_directories(outPath.parent_path(), ec);
        }
        fs::path tmpOutPath = outPath.string() + ".tmp";
        fs::remove(tmpOutPath, ec);

        {
            juce::FileOutputStream outStream(juce::File(tmpOutPath.string()));
            if (!outStream.openedOk()) {
                cleanupStaging();
                result.message = "Could not open destination file for writing: " + outputZipPath;
                return result;
            }

            double progress = 0.0;
            bool ok = builder.writeToStream(outStream, &progress);
            outStream.flush();
            if (!ok) {
                fs::remove(tmpOutPath, ec);
                cleanupStaging();
                result.message = "Failed while writing compressed backup archive.";
                return result;
            }
        }

        fs::remove(outPath, ec);
        fs::rename(tmpOutPath, outPath, ec);
        if (ec) {
            fs::copy_file(tmpOutPath, outPath, fs::copy_options::overwrite_existing, ec);
            fs::remove(tmpOutPath, ec);
            if (ec) {
                cleanupStaging();
                result.message = "Could not finalize backup file: " + ec.message();
                return result;
            }
        }

        cleanupStaging();
        result.success = true;
        std::ostringstream oss;
        oss << "Backup saved: " << result.songCount << (result.songCount == 1 ? " song, " : " songs, ")
            << result.setlistCount << (result.setlistCount == 1 ? " set, " : " sets, ")
            << result.audioFileCount << (result.audioFileCount == 1 ? " audio file." : " audio files.");
        result.message = oss.str();
        return result;
    } catch (const std::exception& e) {
        cleanupStaging();
        result.success = false;
        result.message = std::string("Backup failed: ") + e.what();
        return result;
    }
}

BackupResult LibraryBackupService::importBackup(DatabaseService& db,
                                                const std::string& inputZipPath,
                                                const fs::path& uploadedTracksRoot,
                                                std::function<void()> onBeforeReplace,
                                                ProgressCallback onProgress) {
    BackupResult result;
    if (inputZipPath.empty() || !fs::exists(inputZipPath)) {
        result.message = "Selected backup file does not exist.";
        return result;
    }

    fs::path stagingDir = createUniqueTempDir("tracks_backup_import");
    auto cleanupStaging = [&]() {
        std::error_code ec;
        fs::remove_all(stagingDir, ec);
    };

    try {
        if (onProgress) onProgress("Inspecting backup archive...");

        juce::File zipFile(inputZipPath);
        juce::ZipFile zip(zipFile);
        const int numEntries = zip.getNumEntries();
        if (numEntries <= 0) {
            cleanupStaging();
            result.message = "Selected file is not a valid ZIP archive.";
            return result;
        }

        int dbEntryIdx = -1;
        std::vector<int> trackEntryIndices;
        for (int i = 0; i < numEntries; ++i) {
            const auto* entry = zip.getEntry(i);
            if (!entry) continue;
            std::string name = entry->filename.toStdString();
            for (char& ch : name) {
                if (ch == '\\') ch = '/';
            }
            if (!isSafeRelativeZipPath(name)) {
                cleanupStaging();
                result.message = "Backup archive contains an unsafe file path: " + name;
                return result;
            }
            if (!name.empty() && name.back() == '/') continue; // directory entry

            if (name == "tracks_library.sqlite3") {
                dbEntryIdx = i;
            } else if (name.rfind("uploaded_tracks/", 0) == 0) {
                trackEntryIndices.push_back(i);
            }
        }

        if (dbEntryIdx < 0) {
            cleanupStaging();
            result.message = "Selected archive is not a Tracks backup (missing tracks_library.sqlite3).";
            return result;
        }

        // Extract database to staging directory
        if (onProgress) onProgress("Extracting library database...");
        auto extractSingleEntry = [&](int entryIdx, const fs::path& destFile) -> bool {
            std::error_code ec;
            if (destFile.has_parent_path()) {
                fs::create_directories(destFile.parent_path(), ec);
            }
            std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(entryIdx));
            if (!in) return false;
            juce::FileOutputStream out(juce::File(destFile.string()));
            if (!out.openedOk()) return false;
            out.writeFromInputStream(*in, -1);
            out.flush();
            return true;
        };

        const fs::path stagedDbPath = stagingDir / "tracks_library.sqlite3";
        if (!extractSingleEntry(dbEntryIdx, stagedDbPath)) {
            cleanupStaging();
            result.message = "Failed to extract library database from backup.";
            return result;
        }

        // Extract all audio track files to staging/uploaded_tracks/...
        const fs::path stagedTracksRoot = stagingDir / "uploaded_tracks";
        std::error_code ec;
        fs::create_directories(stagedTracksRoot, ec);

        int extractedCount = 0;
        const int totalTracks = static_cast<int>(trackEntryIndices.size());
        for (int idx : trackEntryIndices) {
            const auto* entry = zip.getEntry(idx);
            std::string name = entry->filename.toStdString();
            for (char& ch : name) {
                if (ch == '\\') ch = '/';
            }
            std::string rel = name.substr(std::string("uploaded_tracks/").size());
            if (rel.empty()) continue;

            if (onProgress && (extractedCount % 5 == 0 || extractedCount + 1 == totalTracks)) {
                onProgress("Extracting audio tracks (" + std::to_string(extractedCount + 1) + "/" +
                           std::to_string(totalTracks) + ")...");
            }

            if (!extractSingleEntry(idx, stagedTracksRoot / fs::path(rel))) {
                cleanupStaging();
                result.message = "Failed to extract audio file: " + rel;
                return result;
            }
            ++extractedCount;
        }

        if (onProgress) onProgress("Loading imported library into Tracks...");

        // Stop playback and flush audio cache so no files inside uploaded_tracks/ are in use
        if (onBeforeReplace) {
            onBeforeReplace();
        }

        // Swap uploaded_tracks directory safely with rollback protection
        fs::path rollbackTracksDir = uploadedTracksRoot;
        rollbackTracksDir += ".pre_import_bak";
        fs::remove_all(rollbackTracksDir, ec);

        bool hadOldTracksDir = fs::exists(uploadedTracksRoot, ec);
        if (hadOldTracksDir) {
            fs::rename(uploadedTracksRoot, rollbackTracksDir, ec);
            if (ec) {
                cleanupStaging();
                result.message = "Could not move existing uploaded_tracks folder aside: " + ec.message();
                return result;
            }
        }

        try {
            if (uploadedTracksRoot.has_parent_path()) {
                fs::create_directories(uploadedTracksRoot.parent_path(), ec);
            }
            moveOrCopyDirectory(stagedTracksRoot, uploadedTracksRoot);

            // Replace SQLite database (validates schema & rolls back DB on failure)
            db.replaceDatabaseFromFile(stagedDbPath.string());

            // Rewrite track file_paths so backups are 100% portable across directories & machines
            db.remapTrackPathsToStorageRoot(uploadedTracksRoot.string());

            // Success -> discard rollback backup of old tracks
            if (hadOldTracksDir) {
                fs::remove_all(rollbackTracksDir, ec);
            }
        } catch (...) {
            // Roll back uploaded_tracks folder if database restore failed
            fs::remove_all(uploadedTracksRoot, ec);
            if (hadOldTracksDir && fs::exists(rollbackTracksDir, ec)) {
                fs::rename(rollbackTracksDir, uploadedTracksRoot, ec);
            }
            throw;
        }

        cleanupStaging();

        result.songCount = static_cast<int>(db.listSongs().size());
        result.setlistCount = static_cast<int>(db.listSetlists().size());
        result.audioFileCount = extractedCount;
        result.success = true;

        std::ostringstream oss;
        oss << "Import complete: " << result.songCount << (result.songCount == 1 ? " song, " : " songs, ")
            << result.setlistCount << (result.setlistCount == 1 ? " set, " : " sets, ")
            << result.audioFileCount << (result.audioFileCount == 1 ? " audio file." : " audio files.");
        result.message = oss.str();
        return result;
    } catch (const std::exception& e) {
        cleanupStaging();
        result.success = false;
        result.message = std::string("Import failed: ") + e.what();
        return result;
    }
}

} // namespace tracks
