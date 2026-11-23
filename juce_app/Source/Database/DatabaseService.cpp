#include "DatabaseService.h"
#include <sqlite3.h>
#include <stdexcept>
#include <iostream>

namespace tracks {

DatabaseService::DatabaseService(const std::string& dbPath)
    : dbPath_(dbPath) {
    connect();
    ensureSchema();
}

DatabaseService::~DatabaseService() {
    close();
}

void DatabaseService::connect() {
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (db_ != nullptr) return;

    int rc = sqlite3_open(dbPath_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "Unknown SQLite error";
        close();
        throw std::runtime_error("Failed to open SQLite database at " + dbPath_ + ": " + err);
    }

    // Always enforce foreign keys
    char* errMsg = nullptr;
    rc = sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "Unknown error";
        sqlite3_free(errMsg);
        throw std::runtime_error("Failed to enable foreign keys: " + err);
    }
}

void DatabaseService::close() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void DatabaseService::executeSimple(const char* sql) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "Unknown error";
        sqlite3_free(errMsg);
        throw std::runtime_error("SQLite error executing: " + std::string(sql) + " - " + err);
    }
}

bool DatabaseService::columnExists(const std::string& tableName, const std::string& columnName) {
    std::string query = "PRAGMA table_info(" + tableName + ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        if (name && columnName == reinterpret_cast<const char*>(name)) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

void DatabaseService::ensureSchema() {
    std::lock_guard<std::mutex> lock(dbMutex_);

    executeSimple(
        "CREATE TABLE IF NOT EXISTS songs ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  title TEXT NOT NULL,"
        "  artist TEXT NOT NULL,"
        "  tempo INTEGER NOT NULL,"
        "  key TEXT NOT NULL,"
        "  notes TEXT DEFAULT '',"
        "  time_signature TEXT NOT NULL DEFAULT '4/4'"
        ");"
    );

    if (!columnExists("songs", "time_signature")) {
        executeSimple("ALTER TABLE songs ADD COLUMN time_signature TEXT NOT NULL DEFAULT '4/4';");
    }

    executeSimple(
        "CREATE TABLE IF NOT EXISTS tracks ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  song_id INTEGER NOT NULL,"
        "  name TEXT NOT NULL,"
        "  file_path TEXT NOT NULL,"
        "  output_channel INTEGER NOT NULL,"
        "  gain REAL NOT NULL DEFAULT 1.0,"
        "  muted INTEGER NOT NULL DEFAULT 0,"
        "  FOREIGN KEY(song_id) REFERENCES songs(id) ON DELETE CASCADE"
        ");"
    );

    executeSimple(
        "CREATE TABLE IF NOT EXISTS setlists ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  set_date TEXT DEFAULT '',"
        "  set_time TEXT DEFAULT '',"
        "  notes TEXT DEFAULT ''"
        ");"
    );

    if (!columnExists("setlists", "set_date")) {
        executeSimple("ALTER TABLE setlists ADD COLUMN set_date TEXT DEFAULT '';");
    }
    if (!columnExists("setlists", "set_time")) {
        executeSimple("ALTER TABLE setlists ADD COLUMN set_time TEXT DEFAULT '';");
    }

    executeSimple(
        "CREATE TABLE IF NOT EXISTS setlist_items ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  setlist_id INTEGER NOT NULL,"
        "  song_id INTEGER NOT NULL,"
        "  order_index INTEGER NOT NULL,"
        "  override_tempo INTEGER,"
        "  override_key TEXT,"
        "  override_time_signature TEXT,"
        "  FOREIGN KEY(setlist_id) REFERENCES setlists(id) ON DELETE CASCADE,"
        "  FOREIGN KEY(song_id) REFERENCES songs(id) ON DELETE CASCADE"
        ");"
    );

    if (!columnExists("setlist_items", "override_tempo")) {
        executeSimple("ALTER TABLE setlist_items ADD COLUMN override_tempo INTEGER;");
    }
    if (!columnExists("setlist_items", "override_key")) {
        executeSimple("ALTER TABLE setlist_items ADD COLUMN override_key TEXT;");
    }
    if (!columnExists("setlist_items", "override_time_signature")) {
        executeSimple("ALTER TABLE setlist_items ADD COLUMN override_time_signature TEXT;");
    }

    executeSimple(
        "CREATE TABLE IF NOT EXISTS app_settings ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ");"
    );
}

Song DatabaseService::saveSong(Song song) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    executeSimple("BEGIN TRANSACTION;");

    try {
        if (!song.id.has_value()) {
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "INSERT INTO songs (title, artist, tempo, key, notes, time_signature) VALUES (?, ?, ?, ?, ?, ?);";
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
                throw std::runtime_error("Prepare failed for INSERT INTO songs");

            sqlite3_bind_text(stmt, 1, song.title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, song.artist.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, song.tempo);
            sqlite3_bind_text(stmt, 4, song.key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, song.notes.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, song.timeSignature.empty() ? "4/4" : song.timeSignature.c_str(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                sqlite3_finalize(stmt);
                throw std::runtime_error("Step failed for INSERT INTO songs");
            }
            sqlite3_finalize(stmt);
            song.id = sqlite3_last_insert_rowid(db_);
        } else {
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "UPDATE songs SET title = ?, artist = ?, tempo = ?, key = ?, notes = ?, time_signature = ? WHERE id = ?;";
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
                throw std::runtime_error("Prepare failed for UPDATE songs");

            sqlite3_bind_text(stmt, 1, song.title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, song.artist.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, song.tempo);
            sqlite3_bind_text(stmt, 4, song.key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, song.notes.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, song.timeSignature.empty() ? "4/4" : song.timeSignature.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 7, *song.id);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                sqlite3_finalize(stmt);
                throw std::runtime_error("Step failed for UPDATE songs");
            }
            sqlite3_finalize(stmt);

            // Recreate tracks
            sqlite3_stmt* delStmt = nullptr;
            if (sqlite3_prepare_v2(db_, "DELETE FROM tracks WHERE song_id = ?;", -1, &delStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(delStmt, 1, *song.id);
                sqlite3_step(delStmt);
                sqlite3_finalize(delStmt);
            }
        }

        // Insert tracks
        sqlite3_stmt* trackStmt = nullptr;
        const char* insertTrackSql = "INSERT INTO tracks (song_id, name, file_path, output_channel, gain, muted) VALUES (?, ?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(db_, insertTrackSql, -1, &trackStmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("Prepare failed for INSERT INTO tracks");

        for (auto& track : song.tracks) {
            sqlite3_reset(trackStmt);
            sqlite3_bind_int64(trackStmt, 1, *song.id);
            sqlite3_bind_text(trackStmt, 2, track.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(trackStmt, 3, track.filePath.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(trackStmt, 4, track.outputChannel);
            sqlite3_bind_double(trackStmt, 5, static_cast<double>(track.gain));
            sqlite3_bind_int(trackStmt, 6, track.muted ? 1 : 0);

            if (sqlite3_step(trackStmt) == SQLITE_DONE) {
                track.id = sqlite3_last_insert_rowid(db_);
            }
        }
        sqlite3_finalize(trackStmt);

        executeSimple("COMMIT;");
    } catch (...) {
        executeSimple("ROLLBACK;");
        throw;
    }

    return song;
}

std::vector<SongTrack> DatabaseService::listTracksForSong(int64_t songId) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    std::vector<SongTrack> tracks;

    const char* sql = "SELECT id, name, file_path, output_channel, gain, muted FROM tracks WHERE song_id = ? ORDER BY output_channel ASC, id ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return tracks;

    sqlite3_bind_int64(stmt, 1, songId);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SongTrack t;
        t.id = sqlite3_column_int64(stmt, 0);
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        t.name = name ? reinterpret_cast<const char*>(name) : "";
        const unsigned char* fp = sqlite3_column_text(stmt, 2);
        t.filePath = fp ? reinterpret_cast<const char*>(fp) : "";
        t.outputChannel = sqlite3_column_int(stmt, 3);
        t.gain = static_cast<float>(sqlite3_column_double(stmt, 4));
        t.muted = sqlite3_column_int(stmt, 5) != 0;
        tracks.push_back(t);
    }
    sqlite3_finalize(stmt);
    return tracks;
}

std::vector<Song> DatabaseService::listSongs() {
    std::lock_guard<std::mutex> lock(dbMutex_);
    std::vector<Song> songs;

    const char* sql = "SELECT id, title, artist, tempo, key, notes, time_signature FROM songs ORDER BY title ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return songs;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Song s;
        s.id = sqlite3_column_int64(stmt, 0);
        const unsigned char* title = sqlite3_column_text(stmt, 1);
        s.title = title ? reinterpret_cast<const char*>(title) : "";
        const unsigned char* artist = sqlite3_column_text(stmt, 2);
        s.artist = artist ? reinterpret_cast<const char*>(artist) : "";
        s.tempo = sqlite3_column_int(stmt, 3);
        const unsigned char* key = sqlite3_column_text(stmt, 4);
        s.key = key ? reinterpret_cast<const char*>(key) : "";
        const unsigned char* notes = sqlite3_column_text(stmt, 5);
        s.notes = notes ? reinterpret_cast<const char*>(notes) : "";
        const unsigned char* sig = sqlite3_column_text(stmt, 6);
        s.timeSignature = sig ? reinterpret_cast<const char*>(sig) : "4/4";
        songs.push_back(s);
    }
    sqlite3_finalize(stmt);

    // Fetch tracks for each song
    for (auto& s : songs) {
        if (s.id.has_value()) {
            const char* trackSql = "SELECT id, name, file_path, output_channel, gain, muted FROM tracks WHERE song_id = ? ORDER BY output_channel ASC, id ASC;";
            sqlite3_stmt* tStmt = nullptr;
            if (sqlite3_prepare_v2(db_, trackSql, -1, &tStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(tStmt, 1, *s.id);
                while (sqlite3_step(tStmt) == SQLITE_ROW) {
                    SongTrack t;
                    t.id = sqlite3_column_int64(tStmt, 0);
                    const unsigned char* name = sqlite3_column_text(tStmt, 1);
                    t.name = name ? reinterpret_cast<const char*>(name) : "";
                    const unsigned char* fp = sqlite3_column_text(tStmt, 2);
                    t.filePath = fp ? reinterpret_cast<const char*>(fp) : "";
                    t.outputChannel = sqlite3_column_int(tStmt, 3);
                    t.gain = static_cast<float>(sqlite3_column_double(tStmt, 4));
                    t.muted = sqlite3_column_int(tStmt, 5) != 0;
                    s.tracks.push_back(t);
                }
                sqlite3_finalize(tStmt);
            }
        }
    }

    return songs;
}

std::optional<Song> DatabaseService::loadSong(int64_t songId) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    const char* sql = "SELECT id, title, artist, tempo, key, notes, time_signature FROM songs WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_int64(stmt, 1, songId);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    Song s;
    s.id = sqlite3_column_int64(stmt, 0);
    const unsigned char* title = sqlite3_column_text(stmt, 1);
    s.title = title ? reinterpret_cast<const char*>(title) : "";
    const unsigned char* artist = sqlite3_column_text(stmt, 2);
    s.artist = artist ? reinterpret_cast<const char*>(artist) : "";
    s.tempo = sqlite3_column_int(stmt, 3);
    const unsigned char* key = sqlite3_column_text(stmt, 4);
    s.key = key ? reinterpret_cast<const char*>(key) : "";
    const unsigned char* notes = sqlite3_column_text(stmt, 5);
    s.notes = notes ? reinterpret_cast<const char*>(notes) : "";
    const unsigned char* sig = sqlite3_column_text(stmt, 6);
    s.timeSignature = sig ? reinterpret_cast<const char*>(sig) : "4/4";
    sqlite3_finalize(stmt);

    // Fetch tracks
    const char* trackSql = "SELECT id, name, file_path, output_channel, gain, muted FROM tracks WHERE song_id = ? ORDER BY output_channel ASC, id ASC;";
    sqlite3_stmt* tStmt = nullptr;
    if (sqlite3_prepare_v2(db_, trackSql, -1, &tStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(tStmt, 1, songId);
        while (sqlite3_step(tStmt) == SQLITE_ROW) {
            SongTrack t;
            t.id = sqlite3_column_int64(tStmt, 0);
            const unsigned char* name = sqlite3_column_text(tStmt, 1);
            t.name = name ? reinterpret_cast<const char*>(name) : "";
            const unsigned char* fp = sqlite3_column_text(tStmt, 2);
            t.filePath = fp ? reinterpret_cast<const char*>(fp) : "";
            t.outputChannel = sqlite3_column_int(tStmt, 3);
            t.gain = static_cast<float>(sqlite3_column_double(tStmt, 4));
            t.muted = sqlite3_column_int(tStmt, 5) != 0;
            s.tracks.push_back(t);
        }
        sqlite3_finalize(tStmt);
    }

    return s;
}

void DatabaseService::deleteSong(int64_t songId) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    const char* sql = "DELETE FROM songs WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, songId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

Setlist DatabaseService::saveSetlist(Setlist setlist) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    executeSimple("BEGIN TRANSACTION;");

    try {
        if (!setlist.id.has_value()) {
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "INSERT INTO setlists (name, set_date, set_time, notes) VALUES (?, ?, ?, ?);";
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
                throw std::runtime_error("Prepare failed for INSERT INTO setlists");

            sqlite3_bind_text(stmt, 1, setlist.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, setlist.setDate.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, setlist.setTime.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, setlist.notes.c_str(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                sqlite3_finalize(stmt);
                throw std::runtime_error("Step failed for INSERT INTO setlists");
            }
            sqlite3_finalize(stmt);
            setlist.id = sqlite3_last_insert_rowid(db_);
        } else {
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "UPDATE setlists SET name = ?, set_date = ?, set_time = ?, notes = ? WHERE id = ?;";
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
                throw std::runtime_error("Prepare failed for UPDATE setlists");

            sqlite3_bind_text(stmt, 1, setlist.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, setlist.setDate.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, setlist.setTime.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, setlist.notes.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 5, *setlist.id);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                sqlite3_finalize(stmt);
                throw std::runtime_error("Step failed for UPDATE setlists");
            }
            sqlite3_finalize(stmt);

            // Recreate entries
            sqlite3_stmt* delStmt = nullptr;
            if (sqlite3_prepare_v2(db_, "DELETE FROM setlist_items WHERE setlist_id = ?;", -1, &delStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(delStmt, 1, *setlist.id);
                sqlite3_step(delStmt);
                sqlite3_finalize(delStmt);
            }
        }

        // Insert items
        sqlite3_stmt* itemStmt = nullptr;
        const char* insertItemSql = "INSERT INTO setlist_items (setlist_id, song_id, order_index, override_tempo, override_key, override_time_signature) VALUES (?, ?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(db_, insertItemSql, -1, &itemStmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("Prepare failed for INSERT INTO setlist_items");

        for (auto& entry : setlist.entries) {
            sqlite3_reset(itemStmt);
            sqlite3_bind_int64(itemStmt, 1, *setlist.id);
            sqlite3_bind_int64(itemStmt, 2, entry.songId);
            sqlite3_bind_int(itemStmt, 3, entry.orderIndex);

            if (entry.overrideTempo.has_value())
                sqlite3_bind_int(itemStmt, 4, *entry.overrideTempo);
            else
                sqlite3_bind_null(itemStmt, 4);

            if (entry.overrideKey.has_value())
                sqlite3_bind_text(itemStmt, 5, entry.overrideKey->c_str(), -1, SQLITE_TRANSIENT);
            else
                sqlite3_bind_null(itemStmt, 5);

            if (entry.overrideTimeSignature.has_value())
                sqlite3_bind_text(itemStmt, 6, entry.overrideTimeSignature->c_str(), -1, SQLITE_TRANSIENT);
            else
                sqlite3_bind_null(itemStmt, 6);

            if (sqlite3_step(itemStmt) == SQLITE_DONE) {
                entry.id = sqlite3_last_insert_rowid(db_);
                entry.setlistId = *setlist.id;
            }
        }
        sqlite3_finalize(itemStmt);

        executeSimple("COMMIT;");
    } catch (...) {
        executeSimple("ROLLBACK;");
        throw;
    }

    return setlist;
}

std::vector<SetlistEntry> DatabaseService::listSetlistEntries(int64_t setlistId) {
    // Note: this function does not lock dbMutex_ directly if loadSong locks it, but we lock inside.
    // To allow loadSong to run cleanly, let's read the items, then unlock, then fetch songs.
    std::vector<SetlistEntry> entries;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        const char* sql = "SELECT id, setlist_id, song_id, order_index, override_tempo, override_key, override_time_signature FROM setlist_items WHERE setlist_id = ? ORDER BY order_index ASC, id ASC;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, setlistId);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                SetlistEntry e;
                e.id = sqlite3_column_int64(stmt, 0);
                e.setlistId = sqlite3_column_int64(stmt, 1);
                e.songId = sqlite3_column_int64(stmt, 2);
                e.orderIndex = sqlite3_column_int(stmt, 3);
                if (sqlite3_column_type(stmt, 4) != SQLITE_NULL)
                    e.overrideTempo = sqlite3_column_int(stmt, 4);
                if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
                    const unsigned char* k = sqlite3_column_text(stmt, 5);
                    e.overrideKey = k ? reinterpret_cast<const char*>(k) : "";
                }
                if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
                    const unsigned char* s = sqlite3_column_text(stmt, 6);
                    e.overrideTimeSignature = s ? reinterpret_cast<const char*>(s) : "";
                }
                entries.push_back(e);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Now populate the song for each entry
    for (auto& entry : entries) {
        entry.song = loadSong(entry.songId);
    }

    return entries;
}

std::vector<Setlist> DatabaseService::listSetlists() {
    std::vector<Setlist> setlists;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        const char* sql = "SELECT id, name, set_date, set_time, notes FROM setlists ORDER BY id ASC;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                Setlist s;
                s.id = sqlite3_column_int64(stmt, 0);
                const unsigned char* name = sqlite3_column_text(stmt, 1);
                s.name = name ? reinterpret_cast<const char*>(name) : "";
                const unsigned char* dt = sqlite3_column_text(stmt, 2);
                s.setDate = dt ? reinterpret_cast<const char*>(dt) : "";
                const unsigned char* tm = sqlite3_column_text(stmt, 3);
                s.setTime = tm ? reinterpret_cast<const char*>(tm) : "";
                const unsigned char* notes = sqlite3_column_text(stmt, 4);
                s.notes = notes ? reinterpret_cast<const char*>(notes) : "";
                setlists.push_back(s);
            }
            sqlite3_finalize(stmt);
        }
    }

    for (auto& s : setlists) {
        if (s.id.has_value()) {
            s.entries = listSetlistEntries(*s.id);
        }
    }

    return setlists;
}

std::optional<Setlist> DatabaseService::loadSetlist(int64_t setlistId) {
    Setlist s;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        const char* sql = "SELECT id, name, set_date, set_time, notes FROM setlists WHERE id = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
            return std::nullopt;

        sqlite3_bind_int64(stmt, 1, setlistId);
        if (sqlite3_step(stmt) != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return std::nullopt;
        }

        s.id = sqlite3_column_int64(stmt, 0);
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        s.name = name ? reinterpret_cast<const char*>(name) : "";
        const unsigned char* dt = sqlite3_column_text(stmt, 2);
        s.setDate = dt ? reinterpret_cast<const char*>(dt) : "";
        const unsigned char* tm = sqlite3_column_text(stmt, 3);
        s.setTime = tm ? reinterpret_cast<const char*>(tm) : "";
        const unsigned char* notes = sqlite3_column_text(stmt, 4);
        s.notes = notes ? reinterpret_cast<const char*>(notes) : "";
        sqlite3_finalize(stmt);
    }

    s.entries = listSetlistEntries(setlistId);
    return s;
}

void DatabaseService::deleteSetlist(int64_t setlistId) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    const char* sql = "DELETE FROM setlists WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, setlistId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::string DatabaseService::getAppSetting(const std::string& key, const std::string& defaultValue) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    const char* sql = "SELECT value FROM app_settings WHERE key = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return defaultValue;

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string val = defaultValue;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        if (text) val = reinterpret_cast<const char*>(text);
    }
    sqlite3_finalize(stmt);
    return val;
}

void DatabaseService::setAppSetting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    const char* sql = "INSERT INTO app_settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

} // namespace tracks
