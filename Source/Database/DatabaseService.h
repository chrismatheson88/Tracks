#pragma once

#include "../Models/Song.h"
#include "../Models/Setlist.h"
#include <string>
#include <vector>
#include <optional>
#include <mutex>

struct sqlite3;

namespace tracks {

class DatabaseService {
public:
    explicit DatabaseService(const std::string& dbPath);
    ~DatabaseService();

    // Disable copy
    DatabaseService(const DatabaseService&) = delete;
    DatabaseService& operator=(const DatabaseService&) = delete;

    void ensureSchema();
    const std::string& getDbPath() const { return dbPath_; }
    void exportSnapshotToFile(const std::string& destPath);
    void replaceDatabaseFromFile(const std::string& srcPath);
    void remapTrackPathsToStorageRoot(const std::string& uploadedTracksRoot);

    // Songs
    Song saveSong(Song song);
    std::vector<Song> listSongs();
    std::optional<Song> loadSong(int64_t songId);
    std::vector<SongTrack> listTracksForSong(int64_t songId);
    void updateTrackMix(int64_t trackId, float gain, int outputChannel, bool stereoOutput = true);
    void updateSongBaseParams(int64_t songId,
                              int tempo,
                              const std::string& key,
                              const std::string& timeSignature);
    void updateArrangementBaseParams(int64_t arrangementId,
                                     int tempo,
                                     const std::string& key,
                                     const std::string& timeSignature);
    void updateSongClickSubdivisions(int64_t songId,
                                     bool clickEighths,
                                     bool clickSixteenths,
                                     float clickEighthsGain,
                                     float clickSixteenthsGain);
    void updateArrangementClickSubdivisions(int64_t arrangementId,
                                            bool clickEighths,
                                            bool clickSixteenths,
                                            float clickEighthsGain,
                                            float clickSixteenthsGain);
    void saveSongSections(int64_t songId, std::vector<SongSection>& sections);
    void saveArrangementSections(int64_t songId, int64_t arrangementId, std::vector<SongSection>& sections);
    void deleteSong(int64_t songId);

    // Setlists
    Setlist saveSetlist(Setlist setlist);
    std::vector<Setlist> listSetlists();
    std::optional<Setlist> loadSetlist(int64_t setlistId);
    std::vector<SetlistEntry> listSetlistEntries(int64_t setlistId);
    void deleteSetlist(int64_t setlistId);

    // App Settings
    std::string getAppSetting(const std::string& key, const std::string& defaultValue = "");
    void setAppSetting(const std::string& key, const std::string& value);

private:
    std::string dbPath_;
    sqlite3* db_ = nullptr;
    std::mutex dbMutex_;

    void connect();
    void close();
    void executeSimple(const char* sql);
    bool columnExists(const std::string& tableName, const std::string& columnName);
};

} // namespace tracks
