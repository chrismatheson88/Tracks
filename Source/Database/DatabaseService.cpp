#include "DatabaseService.h"
#include <sqlite3.h>
#include <filesystem>
#include <set>
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

void DatabaseService::exportSnapshotToFile(const std::string& destPath) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) {
        throw std::runtime_error("Database connection is not open.");
    }

    std::error_code ec;
    std::filesystem::path dst(destPath);
    if (dst.has_parent_path()) {
        std::filesystem::create_directories(dst.parent_path(), ec);
    }
    std::filesystem::remove(dst, ec);

    sqlite3* destDb = nullptr;
    int rc = sqlite3_open(destPath.c_str(), &destDb);
    if (rc != SQLITE_OK || !destDb) {
        std::string err = destDb ? sqlite3_errmsg(destDb) : "Unable to open destination database";
        if (destDb) sqlite3_close(destDb);
        throw std::runtime_error("Failed to create snapshot database: " + err);
    }

    sqlite3_backup* backup = sqlite3_backup_init(destDb, "main", db_, "main");
    if (!backup) {
        std::string err = sqlite3_errmsg(destDb);
        sqlite3_close(destDb);
        throw std::runtime_error("Failed to initialize SQLite backup: " + err);
    }

    rc = sqlite3_backup_step(backup, -1);
    int finishRc = sqlite3_backup_finish(backup);
    sqlite3_close(destDb);

    if (rc != SQLITE_DONE || finishRc != SQLITE_OK) {
        std::filesystem::remove(dst, ec);
        throw std::runtime_error("SQLite backup failed with error code " + std::to_string(rc));
    }
}

void DatabaseService::replaceDatabaseFromFile(const std::string& srcPath) {
    namespace fs = std::filesystem;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        if (!fs::exists(srcPath) || !fs::is_regular_file(srcPath)) {
            throw std::runtime_error("Import database file does not exist.");
        }

        // Validate source file is a readable SQLite database before touching current DB
        sqlite3* testDb = nullptr;
        int testRc = sqlite3_open_v2(srcPath.c_str(), &testDb, SQLITE_OPEN_READONLY, nullptr);
        bool validSchema = false;
        if (testRc == SQLITE_OK && testDb) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(testDb,
                                   "SELECT name FROM sqlite_master WHERE type='table' AND name='songs';",
                                   -1, &stmt, nullptr) == SQLITE_OK) {
                validSchema = (sqlite3_step(stmt) == SQLITE_ROW);
                sqlite3_finalize(stmt);
            }
        }
        if (testDb) sqlite3_close(testDb);
        if (!validSchema) {
            throw std::runtime_error("Backup archive does not contain a valid Tracks library database.");
        }

        close();

        fs::path targetPath(dbPath_);
        fs::path rollbackPath(dbPath_ + ".pre_import_bak");
        std::error_code ec;

        // Clean any SQLite sidecar journal/WAL files
        fs::remove(dbPath_ + "-wal", ec);
        fs::remove(dbPath_ + "-shm", ec);
        fs::remove(dbPath_ + "-journal", ec);

        bool hadOriginal = fs::exists(targetPath, ec);
        if (hadOriginal) {
            fs::remove(rollbackPath, ec);
            fs::copy_file(targetPath, rollbackPath, fs::copy_options::overwrite_existing, ec);
        }

        try {
            fs::copy_file(srcPath, targetPath, fs::copy_options::overwrite_existing);
            int rc = sqlite3_open(dbPath_.c_str(), &db_);
            if (rc != SQLITE_OK || !db_) {
                throw std::runtime_error("Failed to open restored SQLite database.");
            }
            executeSimple("PRAGMA foreign_keys = ON;");
            if (hadOriginal) {
                fs::remove(rollbackPath, ec);
            }
        } catch (...) {
            close();
            if (hadOriginal && fs::exists(rollbackPath, ec)) {
                fs::copy_file(rollbackPath, targetPath, fs::copy_options::overwrite_existing, ec);
                fs::remove(rollbackPath, ec);
            }
            sqlite3_open(dbPath_.c_str(), &db_);
            if (db_) {
                char* errMsg = nullptr;
                sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, &errMsg);
                if (errMsg) sqlite3_free(errMsg);
            }
            throw;
        }
    }

    // Run migrations cleanly on the restored database
    ensureSchema();
}

void DatabaseService::remapTrackPathsToStorageRoot(const std::string& uploadedTracksRoot) {
    namespace fs = std::filesystem;
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_ || uploadedTracksRoot.empty()) return;

    fs::path rootPath = fs::absolute(fs::path(uploadedTracksRoot));

    std::vector<std::pair<int64_t, std::string>> updates;
    sqlite3_stmt* selStmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id, file_path FROM tracks;", -1, &selStmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(selStmt) == SQLITE_ROW) {
            int64_t id = sqlite3_column_int64(selStmt, 0);
            const unsigned char* txt = sqlite3_column_text(selStmt, 1);
            if (!txt) continue;
            std::string oldPath(reinterpret_cast<const char*>(txt));
            if (oldPath.empty()) continue;

            // Normalize slashes to find "uploaded_tracks/" marker for cross-machine portability
            std::string normalized = oldPath;
            for (char& ch : normalized) {
                if (ch == '\\') ch = '/';
            }
            const std::string marker = "uploaded_tracks/";
            size_t pos = normalized.rfind(marker);
            if (pos != std::string::npos) {
                std::string rel = normalized.substr(pos + marker.size());
                if (!rel.empty()) {
                    fs::path mapped = rootPath / fs::path(rel);
                    std::string mappedStr = mapped.string();
                    if (mappedStr != oldPath) {
                        updates.emplace_back(id, std::move(mappedStr));
                    }
                }
            }
        }
        sqlite3_finalize(selStmt);
    }

    if (updates.empty()) return;

    executeSimple("BEGIN TRANSACTION;");
    try {
        sqlite3_stmt* updStmt = nullptr;
        if (sqlite3_prepare_v2(db_, "UPDATE tracks SET file_path = ? WHERE id = ?;", -1, &updStmt, nullptr) == SQLITE_OK) {
            for (const auto& [id, newPath] : updates) {
                sqlite3_reset(updStmt);
                sqlite3_clear_bindings(updStmt);
                sqlite3_bind_text(updStmt, 1, newPath.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(updStmt, 2, id);
                sqlite3_step(updStmt);
            }
            sqlite3_finalize(updStmt);
        }
        executeSimple("COMMIT;");
    } catch (...) {
        executeSimple("ROLLBACK;");
        throw;
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
        "  time_signature TEXT NOT NULL DEFAULT '4/4',"
        "  click_eighths INTEGER NOT NULL DEFAULT 0,"
        "  click_sixteenths INTEGER NOT NULL DEFAULT 0,"
        "  click_eighths_gain REAL NOT NULL DEFAULT 1.0,"
        "  click_sixteenths_gain REAL NOT NULL DEFAULT 1.0"
        ");"
    );

    if (!columnExists("songs", "time_signature")) {
        executeSimple("ALTER TABLE songs ADD COLUMN time_signature TEXT NOT NULL DEFAULT '4/4';");
    }
    if (!columnExists("songs", "click_eighths")) {
        executeSimple("ALTER TABLE songs ADD COLUMN click_eighths INTEGER NOT NULL DEFAULT 0;");
    }
    if (!columnExists("songs", "click_sixteenths")) {
        executeSimple("ALTER TABLE songs ADD COLUMN click_sixteenths INTEGER NOT NULL DEFAULT 0;");
    }
    if (!columnExists("songs", "click_eighths_gain")) {
        executeSimple("ALTER TABLE songs ADD COLUMN click_eighths_gain REAL NOT NULL DEFAULT 1.0;");
    }
    if (!columnExists("songs", "click_sixteenths_gain")) {
        executeSimple("ALTER TABLE songs ADD COLUMN click_sixteenths_gain REAL NOT NULL DEFAULT 1.0;");
    }

    executeSimple(
        "CREATE TABLE IF NOT EXISTS arrangements ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  song_id INTEGER NOT NULL,"
        "  name TEXT NOT NULL DEFAULT 'Main',"
        "  tempo INTEGER NOT NULL DEFAULT 120,"
        "  key TEXT NOT NULL DEFAULT 'C',"
        "  time_signature TEXT NOT NULL DEFAULT '4/4',"
        "  click_eighths INTEGER NOT NULL DEFAULT 0,"
        "  click_sixteenths INTEGER NOT NULL DEFAULT 0,"
        "  click_eighths_gain REAL NOT NULL DEFAULT 1.0,"
        "  click_sixteenths_gain REAL NOT NULL DEFAULT 1.0,"
        "  order_index INTEGER NOT NULL DEFAULT 0,"
        "  FOREIGN KEY(song_id) REFERENCES songs(id) ON DELETE CASCADE"
        ");"
    );

    executeSimple(
        "CREATE TABLE IF NOT EXISTS tracks ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  song_id INTEGER NOT NULL,"
        "  arrangement_id INTEGER,"
        "  name TEXT NOT NULL,"
        "  file_path TEXT NOT NULL,"
        "  output_channel INTEGER NOT NULL,"
        "  gain REAL NOT NULL DEFAULT 1.0,"
        "  muted INTEGER NOT NULL DEFAULT 0,"
        "  track_key TEXT DEFAULT '',"
        "  num_channels INTEGER NOT NULL DEFAULT 0,"
        "  stereo_output INTEGER NOT NULL DEFAULT 1,"
        "  FOREIGN KEY(song_id) REFERENCES songs(id) ON DELETE CASCADE,"
        "  FOREIGN KEY(arrangement_id) REFERENCES arrangements(id) ON DELETE CASCADE"
        ");"
    );

    if (!columnExists("tracks", "arrangement_id")) {
        executeSimple("ALTER TABLE tracks ADD COLUMN arrangement_id INTEGER REFERENCES arrangements(id) ON DELETE CASCADE;");
    }
    if (!columnExists("tracks", "track_key")) {
        executeSimple("ALTER TABLE tracks ADD COLUMN track_key TEXT DEFAULT '';");
    }
    if (!columnExists("tracks", "num_channels")) {
        executeSimple("ALTER TABLE tracks ADD COLUMN num_channels INTEGER NOT NULL DEFAULT 0;");
    }
    if (!columnExists("tracks", "stereo_output")) {
        executeSimple("ALTER TABLE tracks ADD COLUMN stereo_output INTEGER NOT NULL DEFAULT 1;");
    }

    executeSimple(
        "CREATE TABLE IF NOT EXISTS song_sections ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  song_id INTEGER NOT NULL,"
        "  arrangement_id INTEGER,"
        "  name TEXT NOT NULL,"
        "  length_bars INTEGER NOT NULL DEFAULT 4,"
        "  order_index INTEGER NOT NULL DEFAULT 0,"
        "  FOREIGN KEY(song_id) REFERENCES songs(id) ON DELETE CASCADE,"
        "  FOREIGN KEY(arrangement_id) REFERENCES arrangements(id) ON DELETE CASCADE"
        ");"
    );

    if (!columnExists("song_sections", "arrangement_id")) {
        executeSimple("ALTER TABLE song_sections ADD COLUMN arrangement_id INTEGER REFERENCES arrangements(id) ON DELETE CASCADE;");
    }

    executeSimple(
        "CREATE TABLE IF NOT EXISTS arrangement_tempo_points ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  song_id INTEGER NOT NULL,"
        "  arrangement_id INTEGER NOT NULL,"
        "  bar INTEGER NOT NULL DEFAULT 1,"
        "  beat REAL NOT NULL DEFAULT 1.0,"
        "  bpm REAL NOT NULL DEFAULT 120.0,"
        "  ramp_bars REAL NOT NULL DEFAULT 0.0,"
        "  order_index INTEGER NOT NULL DEFAULT 0,"
        "  FOREIGN KEY(song_id) REFERENCES songs(id) ON DELETE CASCADE,"
        "  FOREIGN KEY(arrangement_id) REFERENCES arrangements(id) ON DELETE CASCADE"
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
        "  arrangement_id INTEGER,"
        "  transition_mode INTEGER NOT NULL DEFAULT 0,"
        "  transition_crossfade_bars INTEGER NOT NULL DEFAULT 1,"
        "  FOREIGN KEY(setlist_id) REFERENCES setlists(id) ON DELETE CASCADE,"
        "  FOREIGN KEY(song_id) REFERENCES songs(id) ON DELETE CASCADE,"
        "  FOREIGN KEY(arrangement_id) REFERENCES arrangements(id) ON DELETE SET NULL"
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
    if (!columnExists("setlist_items", "arrangement_id")) {
        executeSimple("ALTER TABLE setlist_items ADD COLUMN arrangement_id INTEGER REFERENCES arrangements(id) ON DELETE SET NULL;");
    }
    if (!columnExists("setlist_items", "transition_mode")) {
        executeSimple("ALTER TABLE setlist_items ADD COLUMN transition_mode INTEGER NOT NULL DEFAULT 0;");
    }
    if (!columnExists("setlist_items", "transition_crossfade_bars")) {
        executeSimple("ALTER TABLE setlist_items ADD COLUMN transition_crossfade_bars INTEGER NOT NULL DEFAULT 1;");
    }

    executeSimple(
        "CREATE TABLE IF NOT EXISTS app_settings ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ");"
    );

    // One-time automatic migration: attach any legacy song tracks/sections with arrangement_id IS NULL
    // to a default "Main" arrangement inheriting that song's tempo, key, time signature, and click subdivisions.
    std::vector<int64_t> legacySongIds;
    {
        const char* findLegacySql =
            "SELECT DISTINCT song_id FROM ("
            "  SELECT song_id FROM tracks WHERE arrangement_id IS NULL "
            "  UNION "
            "  SELECT song_id FROM song_sections WHERE arrangement_id IS NULL"
            ");";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, findLegacySql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                legacySongIds.push_back(sqlite3_column_int64(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }
    }

    for (int64_t songId : legacySongIds) {
        int64_t arrId = 0;
        {
            sqlite3_stmt* checkStmt = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT id FROM arrangements WHERE song_id = ? ORDER BY order_index ASC, id ASC LIMIT 1;", -1, &checkStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(checkStmt, 1, songId);
                if (sqlite3_step(checkStmt) == SQLITE_ROW) {
                    arrId = sqlite3_column_int64(checkStmt, 0);
                }
                sqlite3_finalize(checkStmt);
            }
        }

        if (arrId == 0) {
            int tempo = 120;
            std::string key = "C";
            std::string sig = "4/4";
            int c8 = 0, c16 = 0;
            double g8 = 1.0, g16 = 1.0;

            sqlite3_stmt* sStmt = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT tempo, key, time_signature, click_eighths, click_sixteenths, click_eighths_gain, click_sixteenths_gain FROM songs WHERE id = ?;", -1, &sStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(sStmt, 1, songId);
                if (sqlite3_step(sStmt) == SQLITE_ROW) {
                    tempo = sqlite3_column_int(sStmt, 0);
                    const unsigned char* k = sqlite3_column_text(sStmt, 1);
                    if (k) key = reinterpret_cast<const char*>(k);
                    const unsigned char* ts = sqlite3_column_text(sStmt, 2);
                    if (ts) sig = reinterpret_cast<const char*>(ts);
                    c8 = sqlite3_column_int(sStmt, 3);
                    c16 = sqlite3_column_int(sStmt, 4);
                    g8 = sqlite3_column_double(sStmt, 5);
                    g16 = sqlite3_column_double(sStmt, 6);
                }
                sqlite3_finalize(sStmt);
            }

            // Prefer non-empty track_key from existing tracks if present
            sqlite3_stmt* tkStmt = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT track_key FROM tracks WHERE song_id = ? AND track_key != '' LIMIT 1;", -1, &tkStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(tkStmt, 1, songId);
                if (sqlite3_step(tkStmt) == SQLITE_ROW) {
                    const unsigned char* tk = sqlite3_column_text(tkStmt, 0);
                    if (tk && tk[0] != '\0') key = reinterpret_cast<const char*>(tk);
                }
                sqlite3_finalize(tkStmt);
            }

            sqlite3_stmt* insStmt = nullptr;
            const char* insSql = "INSERT INTO arrangements (song_id, name, tempo, key, time_signature, click_eighths, click_sixteenths, click_eighths_gain, click_sixteenths_gain, order_index) VALUES (?, 'Main', ?, ?, ?, ?, ?, ?, ?, 0);";
            if (sqlite3_prepare_v2(db_, insSql, -1, &insStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(insStmt, 1, songId);
                sqlite3_bind_int(insStmt, 2, tempo);
                sqlite3_bind_text(insStmt, 3, key.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insStmt, 4, sig.empty() ? "4/4" : sig.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(insStmt, 5, c8);
                sqlite3_bind_int(insStmt, 6, c16);
                sqlite3_bind_double(insStmt, 7, g8);
                sqlite3_bind_double(insStmt, 8, g16);
                if (sqlite3_step(insStmt) == SQLITE_DONE) {
                    arrId = sqlite3_last_insert_rowid(db_);
                }
                sqlite3_finalize(insStmt);
            }
        }

        if (arrId > 0) {
            sqlite3_stmt* upT = nullptr;
            if (sqlite3_prepare_v2(db_, "UPDATE tracks SET arrangement_id = ? WHERE song_id = ? AND arrangement_id IS NULL;", -1, &upT, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(upT, 1, arrId);
                sqlite3_bind_int64(upT, 2, songId);
                sqlite3_step(upT);
                sqlite3_finalize(upT);
            }
            sqlite3_stmt* upS = nullptr;
            if (sqlite3_prepare_v2(db_, "UPDATE song_sections SET arrangement_id = ? WHERE song_id = ? AND arrangement_id IS NULL;", -1, &upS, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(upS, 1, arrId);
                sqlite3_bind_int64(upS, 2, songId);
                sqlite3_step(upS);
                sqlite3_finalize(upS);
            }
        }
    }
}

Song DatabaseService::saveSong(Song song) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    executeSimple("BEGIN TRANSACTION;");

    try {
        if (!song.id.has_value()) {
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "INSERT INTO songs (title, artist, tempo, key, notes, time_signature, click_eighths, click_sixteenths, click_eighths_gain, click_sixteenths_gain) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
                throw std::runtime_error("Prepare failed for INSERT INTO songs");

            sqlite3_bind_text(stmt, 1, song.title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, song.artist.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, song.tempo);
            sqlite3_bind_text(stmt, 4, song.key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, song.notes.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, song.timeSignature.empty() ? "4/4" : song.timeSignature.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 7, song.clickEighths ? 1 : 0);
            sqlite3_bind_int(stmt, 8, song.clickSixteenths ? 1 : 0);
            sqlite3_bind_double(stmt, 9, static_cast<double>(song.clickEighthsGain));
            sqlite3_bind_double(stmt, 10, static_cast<double>(song.clickSixteenthsGain));

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                sqlite3_finalize(stmt);
                throw std::runtime_error("Step failed for INSERT INTO songs");
            }
            sqlite3_finalize(stmt);
            song.id = sqlite3_last_insert_rowid(db_);
        } else {
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "UPDATE songs SET title = ?, artist = ?, tempo = ?, key = ?, notes = ?, time_signature = ?, click_eighths = ?, click_sixteenths = ?, click_eighths_gain = ?, click_sixteenths_gain = ? WHERE id = ?;";
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
                throw std::runtime_error("Prepare failed for UPDATE songs");

            sqlite3_bind_text(stmt, 1, song.title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, song.artist.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, song.tempo);
            sqlite3_bind_text(stmt, 4, song.key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, song.notes.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, song.timeSignature.empty() ? "4/4" : song.timeSignature.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 7, song.clickEighths ? 1 : 0);
            sqlite3_bind_int(stmt, 8, song.clickSixteenths ? 1 : 0);
            sqlite3_bind_double(stmt, 9, static_cast<double>(song.clickEighthsGain));
            sqlite3_bind_double(stmt, 10, static_cast<double>(song.clickSixteenthsGain));
            sqlite3_bind_int64(stmt, 11, *song.id);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                sqlite3_finalize(stmt);
                throw std::runtime_error("Step failed for UPDATE songs");
            }
            sqlite3_finalize(stmt);

            // Recreate tracks & sections (re-inserted per arrangement below)
            sqlite3_stmt* delStmt = nullptr;
            if (sqlite3_prepare_v2(db_, "DELETE FROM tracks WHERE song_id = ?;", -1, &delStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(delStmt, 1, *song.id);
                sqlite3_step(delStmt);
                sqlite3_finalize(delStmt);
            }

            sqlite3_stmt* delSecStmt = nullptr;
            if (sqlite3_prepare_v2(db_, "DELETE FROM song_sections WHERE song_id = ?;", -1, &delSecStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(delSecStmt, 1, *song.id);
                sqlite3_step(delSecStmt);
                sqlite3_finalize(delSecStmt);
            }

            sqlite3_stmt* delTempoStmt = nullptr;
            if (sqlite3_prepare_v2(db_, "DELETE FROM arrangement_tempo_points WHERE song_id = ?;", -1, &delTempoStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(delTempoStmt, 1, *song.id);
                sqlite3_step(delTempoStmt);
                sqlite3_finalize(delTempoStmt);
            }
        }

        // Backwards compatibility: if caller populated legacy song.tracks / song.sections without arrangements,
        // wrap them into a default "Main" arrangement so everything is stored uniformly.
        if (song.arrangements.empty() && (!song.tracks.empty() || !song.sections.empty() || !song.tempoPoints.empty())) {
            SongArrangement mainArr;
            mainArr.name = "Main";
            mainArr.tempo = song.tempo;
            mainArr.key = song.key;
            mainArr.timeSignature = song.timeSignature.empty() ? "4/4" : song.timeSignature;
            mainArr.clickEighths = song.clickEighths;
            mainArr.clickSixteenths = song.clickSixteenths;
            mainArr.clickEighthsGain = song.clickEighthsGain;
            mainArr.clickSixteenthsGain = song.clickSixteenthsGain;
            mainArr.tracks = song.tracks;
            mainArr.sections = song.sections;
            mainArr.tempoPoints = song.tempoPoints;
            song.arrangements.push_back(std::move(mainArr));
        }

        // Upsert arrangements while preserving existing IDs so setlist_items.arrangement_id stays intact
        std::set<int64_t> keptArrangementIds;
        sqlite3_stmt* insArrStmt = nullptr;
        const char* insArrSql = "INSERT INTO arrangements (song_id, name, tempo, key, time_signature, click_eighths, click_sixteenths, click_eighths_gain, click_sixteenths_gain, order_index) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(db_, insArrSql, -1, &insArrStmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("Prepare failed for INSERT INTO arrangements");

        sqlite3_stmt* updArrStmt = nullptr;
        const char* updArrSql = "UPDATE arrangements SET name = ?, tempo = ?, key = ?, time_signature = ?, click_eighths = ?, click_sixteenths = ?, click_eighths_gain = ?, click_sixteenths_gain = ?, order_index = ? WHERE id = ? AND song_id = ?;";
        if (sqlite3_prepare_v2(db_, updArrSql, -1, &updArrStmt, nullptr) != SQLITE_OK) {
            sqlite3_finalize(insArrStmt);
            throw std::runtime_error("Prepare failed for UPDATE arrangements");
        }

        for (size_t i = 0; i < song.arrangements.size(); ++i) {
            auto& arr = song.arrangements[i];
            std::string arrName = arr.name.empty() ? "Main" : arr.name;
            std::string arrKey = arr.key.empty() ? (song.key.empty() ? "C" : song.key) : arr.key;
            std::string arrSig = arr.timeSignature.empty() ? (song.timeSignature.empty() ? "4/4" : song.timeSignature) : arr.timeSignature;
            int arrTempo = (arr.tempo > 0) ? arr.tempo : song.tempo;

            bool updatedExisting = false;
            if (arr.id.has_value()) {
                sqlite3_reset(updArrStmt);
                sqlite3_bind_text(updArrStmt, 1, arrName.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(updArrStmt, 2, arrTempo);
                sqlite3_bind_text(updArrStmt, 3, arrKey.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(updArrStmt, 4, arrSig.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(updArrStmt, 5, arr.clickEighths ? 1 : 0);
                sqlite3_bind_int(updArrStmt, 6, arr.clickSixteenths ? 1 : 0);
                sqlite3_bind_double(updArrStmt, 7, static_cast<double>(arr.clickEighthsGain));
                sqlite3_bind_double(updArrStmt, 8, static_cast<double>(arr.clickSixteenthsGain));
                sqlite3_bind_int(updArrStmt, 9, static_cast<int>(i));
                sqlite3_bind_int64(updArrStmt, 10, *arr.id);
                sqlite3_bind_int64(updArrStmt, 11, *song.id);
                if (sqlite3_step(updArrStmt) == SQLITE_DONE && sqlite3_changes(db_) > 0) {
                    updatedExisting = true;
                    keptArrangementIds.insert(*arr.id);
                }
            }

            if (!updatedExisting) {
                sqlite3_reset(insArrStmt);
                sqlite3_bind_int64(insArrStmt, 1, *song.id);
                sqlite3_bind_text(insArrStmt, 2, arrName.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(insArrStmt, 3, arrTempo);
                sqlite3_bind_text(insArrStmt, 4, arrKey.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insArrStmt, 5, arrSig.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(insArrStmt, 6, arr.clickEighths ? 1 : 0);
                sqlite3_bind_int(insArrStmt, 7, arr.clickSixteenths ? 1 : 0);
                sqlite3_bind_double(insArrStmt, 8, static_cast<double>(arr.clickEighthsGain));
                sqlite3_bind_double(insArrStmt, 9, static_cast<double>(arr.clickSixteenthsGain));
                sqlite3_bind_int(insArrStmt, 10, static_cast<int>(i));
                if (sqlite3_step(insArrStmt) == SQLITE_DONE) {
                    arr.id = sqlite3_last_insert_rowid(db_);
                    keptArrangementIds.insert(*arr.id);
                }
            }
        }
        sqlite3_finalize(insArrStmt);
        sqlite3_finalize(updArrStmt);

        // Delete arrangements that were removed from this song
        {
            std::vector<int64_t> existingIds;
            sqlite3_stmt* qStmt = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT id FROM arrangements WHERE song_id = ?;", -1, &qStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(qStmt, 1, *song.id);
                while (sqlite3_step(qStmt) == SQLITE_ROW) {
                    existingIds.push_back(sqlite3_column_int64(qStmt, 0));
                }
                sqlite3_finalize(qStmt);
            }
            sqlite3_stmt* delArrStmt = nullptr;
            if (sqlite3_prepare_v2(db_, "DELETE FROM arrangements WHERE id = ?;", -1, &delArrStmt, nullptr) == SQLITE_OK) {
                for (int64_t existingId : existingIds) {
                    if (keptArrangementIds.find(existingId) == keptArrangementIds.end()) {
                        sqlite3_reset(delArrStmt);
                        sqlite3_bind_int64(delArrStmt, 1, existingId);
                        sqlite3_step(delArrStmt);
                    }
                }
                sqlite3_finalize(delArrStmt);
            }
        }

        // Insert tracks and sections for every arrangement
        sqlite3_stmt* trackStmt = nullptr;
        const char* insertTrackSql = "INSERT INTO tracks (song_id, arrangement_id, name, file_path, output_channel, gain, muted, track_key, num_channels, stereo_output) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(db_, insertTrackSql, -1, &trackStmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("Prepare failed for INSERT INTO tracks");

        sqlite3_stmt* secStmt = nullptr;
        const char* insertSecSql = "INSERT INTO song_sections (song_id, arrangement_id, name, length_bars, order_index) VALUES (?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(db_, insertSecSql, -1, &secStmt, nullptr) != SQLITE_OK) {
            sqlite3_finalize(trackStmt);
            throw std::runtime_error("Prepare failed for INSERT INTO song_sections");
        }

        sqlite3_stmt* tempoStmt = nullptr;
        const char* insertTempoSql = "INSERT INTO arrangement_tempo_points (song_id, arrangement_id, bar, beat, bpm, ramp_bars, order_index) VALUES (?, ?, ?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(db_, insertTempoSql, -1, &tempoStmt, nullptr) != SQLITE_OK) {
            sqlite3_finalize(trackStmt);
            sqlite3_finalize(secStmt);
            throw std::runtime_error("Prepare failed for INSERT INTO arrangement_tempo_points");
        }

        for (auto& arr : song.arrangements) {
            if (!arr.id.has_value()) continue;

            for (auto& track : arr.tracks) {
                sqlite3_reset(trackStmt);
                sqlite3_bind_int64(trackStmt, 1, *song.id);
                sqlite3_bind_int64(trackStmt, 2, *arr.id);
                sqlite3_bind_text(trackStmt, 3, track.name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(trackStmt, 4, track.filePath.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(trackStmt, 5, track.outputChannel);
                sqlite3_bind_double(trackStmt, 6, static_cast<double>(track.gain));
                sqlite3_bind_int(trackStmt, 7, track.muted ? 1 : 0);
                sqlite3_bind_text(trackStmt, 8, track.trackKey.empty() ? arr.key.c_str() : track.trackKey.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(trackStmt, 9, track.numChannels);
                sqlite3_bind_int(trackStmt, 10, track.stereoOutput ? 1 : 0);

                if (sqlite3_step(trackStmt) == SQLITE_DONE) {
                    track.id = sqlite3_last_insert_rowid(db_);
                }
            }

            for (size_t i = 0; i < arr.sections.size(); ++i) {
                auto& sec = arr.sections[i];
                sec.orderIndex = static_cast<int>(i);
                sqlite3_reset(secStmt);
                sqlite3_bind_int64(secStmt, 1, *song.id);
                sqlite3_bind_int64(secStmt, 2, *arr.id);
                sqlite3_bind_text(secStmt, 3, sec.name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(secStmt, 4, std::max(1, sec.lengthBars));
                sqlite3_bind_int(secStmt, 5, sec.orderIndex);

                if (sqlite3_step(secStmt) == SQLITE_DONE) {
                    sec.id = sqlite3_last_insert_rowid(db_);
                }
            }

            for (size_t i = 0; i < arr.tempoPoints.size(); ++i) {
                auto& pt = arr.tempoPoints[i];
                pt.orderIndex = static_cast<int>(i);
                sqlite3_reset(tempoStmt);
                sqlite3_bind_int64(tempoStmt, 1, *song.id);
                sqlite3_bind_int64(tempoStmt, 2, *arr.id);
                sqlite3_bind_int(tempoStmt, 3, std::max(1, pt.bar));
                sqlite3_bind_double(tempoStmt, 4, std::max(1.0, pt.beat));
                sqlite3_bind_double(tempoStmt, 5, std::clamp(pt.bpm, 20.0, 300.0));
                sqlite3_bind_double(tempoStmt, 6, std::max(0.0, pt.rampBars));
                sqlite3_bind_int(tempoStmt, 7, pt.orderIndex);

                if (sqlite3_step(tempoStmt) == SQLITE_DONE) {
                    pt.id = sqlite3_last_insert_rowid(db_);
                }
            }
        }

        sqlite3_finalize(trackStmt);
        sqlite3_finalize(secStmt);
        sqlite3_finalize(tempoStmt);

        // Keep legacy top-level mirrors in sync with first arrangement
        if (!song.arrangements.empty()) {
            song.tracks = song.arrangements.front().tracks;
            song.sections = song.arrangements.front().sections;
            song.tempoPoints = song.arrangements.front().tempoPoints;
        } else {
            song.tracks.clear();
            song.sections.clear();
            song.tempoPoints.clear();
        }

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

    const char* sql = "SELECT id, name, file_path, output_channel, gain, muted, track_key, num_channels, stereo_output FROM tracks WHERE song_id = ? ORDER BY output_channel ASC, id ASC;";
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
        const unsigned char* tk = sqlite3_column_text(stmt, 6);
        t.trackKey = tk ? reinterpret_cast<const char*>(tk) : "";
        t.numChannels = sqlite3_column_int(stmt, 7);
        t.stereoOutput = sqlite3_column_int(stmt, 8) != 0;
        tracks.push_back(t);
    }
    sqlite3_finalize(stmt);
    return tracks;
}

void DatabaseService::updateTrackMix(int64_t trackId, float gain, int outputChannel, bool stereoOutput) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    const char* sql = "UPDATE tracks SET gain = ?, output_channel = ?, stereo_output = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_double(stmt, 1, static_cast<double>(gain));
        sqlite3_bind_int(stmt, 2, outputChannel);
        sqlite3_bind_int(stmt, 3, stereoOutput ? 1 : 0);
        sqlite3_bind_int64(stmt, 4, trackId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void DatabaseService::updateSongClickSubdivisions(int64_t songId,
                                                  bool clickEighths,
                                                  bool clickSixteenths,
                                                  float clickEighthsGain,
                                                  float clickSixteenthsGain) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    const char* sql = "UPDATE songs SET click_eighths = ?, click_sixteenths = ?, click_eighths_gain = ?, click_sixteenths_gain = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, clickEighths ? 1 : 0);
        sqlite3_bind_int(stmt, 2, clickSixteenths ? 1 : 0);
        sqlite3_bind_double(stmt, 3, static_cast<double>(clickEighthsGain));
        sqlite3_bind_double(stmt, 4, static_cast<double>(clickSixteenthsGain));
        sqlite3_bind_int64(stmt, 5, songId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void DatabaseService::updateArrangementClickSubdivisions(int64_t arrangementId,
                                                         bool clickEighths,
                                                         bool clickSixteenths,
                                                         float clickEighthsGain,
                                                         float clickSixteenthsGain) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    const char* sql = "UPDATE arrangements SET click_eighths = ?, click_sixteenths = ?, click_eighths_gain = ?, click_sixteenths_gain = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, clickEighths ? 1 : 0);
        sqlite3_bind_int(stmt, 2, clickSixteenths ? 1 : 0);
        sqlite3_bind_double(stmt, 3, static_cast<double>(clickEighthsGain));
        sqlite3_bind_double(stmt, 4, static_cast<double>(clickSixteenthsGain));
        sqlite3_bind_int64(stmt, 5, arrangementId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void DatabaseService::saveArrangementSections(int64_t songId, int64_t arrangementId, std::vector<SongSection>& sections) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    executeSimple("BEGIN TRANSACTION;");
    try {
        sqlite3_stmt* delStmt = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM song_sections WHERE arrangement_id = ?;", -1, &delStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(delStmt, 1, arrangementId);
            sqlite3_step(delStmt);
            sqlite3_finalize(delStmt);
        }

        sqlite3_stmt* secStmt = nullptr;
        const char* insertSecSql = "INSERT INTO song_sections (song_id, arrangement_id, name, length_bars, order_index) VALUES (?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(db_, insertSecSql, -1, &secStmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("Prepare failed for INSERT INTO song_sections");

        for (size_t i = 0; i < sections.size(); ++i) {
            auto& sec = sections[i];
            sec.orderIndex = static_cast<int>(i);
            sqlite3_reset(secStmt);
            sqlite3_bind_int64(secStmt, 1, songId);
            sqlite3_bind_int64(secStmt, 2, arrangementId);
            sqlite3_bind_text(secStmt, 3, sec.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(secStmt, 4, std::max(1, sec.lengthBars));
            sqlite3_bind_int(secStmt, 5, sec.orderIndex);

            if (sqlite3_step(secStmt) == SQLITE_DONE) {
                sec.id = sqlite3_last_insert_rowid(db_);
            }
        }
        sqlite3_finalize(secStmt);

        executeSimple("COMMIT;");
    } catch (...) {
        executeSimple("ROLLBACK;");
        throw;
    }
}

void DatabaseService::saveSongSections(int64_t songId, std::vector<SongSection>& sections) {
    // Route to the song's first arrangement if one exists, otherwise store on song_id directly
    int64_t firstArrId = 0;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT id FROM arrangements WHERE song_id = ? ORDER BY order_index ASC, id ASC LIMIT 1;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, songId);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                firstArrId = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }
    if (firstArrId > 0) {
        saveArrangementSections(songId, firstArrId, sections);
        return;
    }

    std::lock_guard<std::mutex> lock(dbMutex_);
    executeSimple("BEGIN TRANSACTION;");
    try {
        sqlite3_stmt* delStmt = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM song_sections WHERE song_id = ?;", -1, &delStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(delStmt, 1, songId);
            sqlite3_step(delStmt);
            sqlite3_finalize(delStmt);
        }

        sqlite3_stmt* secStmt = nullptr;
        const char* insertSecSql = "INSERT INTO song_sections (song_id, name, length_bars, order_index) VALUES (?, ?, ?, ?);";
        if (sqlite3_prepare_v2(db_, insertSecSql, -1, &secStmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("Prepare failed for INSERT INTO song_sections");

        for (size_t i = 0; i < sections.size(); ++i) {
            auto& sec = sections[i];
            sec.orderIndex = static_cast<int>(i);
            sqlite3_reset(secStmt);
            sqlite3_bind_int64(secStmt, 1, songId);
            sqlite3_bind_text(secStmt, 2, sec.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(secStmt, 3, std::max(1, sec.lengthBars));
            sqlite3_bind_int(secStmt, 4, sec.orderIndex);

            if (sqlite3_step(secStmt) == SQLITE_DONE) {
                sec.id = sqlite3_last_insert_rowid(db_);
            }
        }
        sqlite3_finalize(secStmt);

        executeSimple("COMMIT;");
    } catch (...) {
        executeSimple("ROLLBACK;");
        throw;
    }
}

std::vector<Song> DatabaseService::listSongs() {
    std::lock_guard<std::mutex> lock(dbMutex_);
    std::vector<Song> songs;

    const char* sql = "SELECT id, title, artist, tempo, key, notes, time_signature, click_eighths, click_sixteenths, click_eighths_gain, click_sixteenths_gain FROM songs ORDER BY title ASC;";
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
        s.clickEighths = sqlite3_column_int(stmt, 7) != 0;
        s.clickSixteenths = sqlite3_column_int(stmt, 8) != 0;
        s.clickEighthsGain = static_cast<float>(sqlite3_column_double(stmt, 9));
        s.clickSixteenthsGain = static_cast<float>(sqlite3_column_double(stmt, 10));
        songs.push_back(s);
    }
    sqlite3_finalize(stmt);

    auto populateSongChildren = [this](Song& s) {
        if (!s.id.has_value()) return;
        const int64_t songId = *s.id;

        const char* arrSql = "SELECT id, name, tempo, key, time_signature, click_eighths, click_sixteenths, click_eighths_gain, click_sixteenths_gain FROM arrangements WHERE song_id = ? ORDER BY order_index ASC, id ASC;";
        sqlite3_stmt* aStmt = nullptr;
        if (sqlite3_prepare_v2(db_, arrSql, -1, &aStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(aStmt, 1, songId);
            while (sqlite3_step(aStmt) == SQLITE_ROW) {
                SongArrangement arr;
                arr.id = sqlite3_column_int64(aStmt, 0);
                const unsigned char* name = sqlite3_column_text(aStmt, 1);
                arr.name = name ? reinterpret_cast<const char*>(name) : "Main";
                arr.tempo = sqlite3_column_int(aStmt, 2);
                const unsigned char* k = sqlite3_column_text(aStmt, 3);
                arr.key = k ? reinterpret_cast<const char*>(k) : "C";
                const unsigned char* ts = sqlite3_column_text(aStmt, 4);
                arr.timeSignature = ts ? reinterpret_cast<const char*>(ts) : "4/4";
                arr.clickEighths = sqlite3_column_int(aStmt, 5) != 0;
                arr.clickSixteenths = sqlite3_column_int(aStmt, 6) != 0;
                arr.clickEighthsGain = static_cast<float>(sqlite3_column_double(aStmt, 7));
                arr.clickSixteenthsGain = static_cast<float>(sqlite3_column_double(aStmt, 8));
                s.arrangements.push_back(std::move(arr));
            }
            sqlite3_finalize(aStmt);
        }

        const char* trackSql = "SELECT id, name, file_path, output_channel, gain, muted, track_key, num_channels, stereo_output, arrangement_id FROM tracks WHERE song_id = ? ORDER BY output_channel ASC, id ASC;";
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
                const unsigned char* tk = sqlite3_column_text(tStmt, 6);
                t.trackKey = tk ? reinterpret_cast<const char*>(tk) : "";
                t.numChannels = sqlite3_column_int(tStmt, 7);
                t.stereoOutput = sqlite3_column_int(tStmt, 8) != 0;
                bool placed = false;
                if (sqlite3_column_type(tStmt, 9) != SQLITE_NULL) {
                    int64_t arrId = sqlite3_column_int64(tStmt, 9);
                    for (auto& arr : s.arrangements) {
                        if (arr.id.has_value() && *arr.id == arrId) {
                            arr.tracks.push_back(t);
                            placed = true;
                            break;
                        }
                    }
                }
                if (!placed) {
                    if (!s.arrangements.empty()) s.arrangements.front().tracks.push_back(t);
                    else s.tracks.push_back(t);
                }
            }
            sqlite3_finalize(tStmt);
        }

        const char* secSql = "SELECT id, name, length_bars, order_index, arrangement_id FROM song_sections WHERE song_id = ? ORDER BY order_index ASC, id ASC;";
        sqlite3_stmt* sStmt = nullptr;
        if (sqlite3_prepare_v2(db_, secSql, -1, &sStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(sStmt, 1, songId);
            while (sqlite3_step(sStmt) == SQLITE_ROW) {
                SongSection sec;
                sec.id = sqlite3_column_int64(sStmt, 0);
                const unsigned char* name = sqlite3_column_text(sStmt, 1);
                sec.name = name ? reinterpret_cast<const char*>(name) : "";
                sec.lengthBars = std::max(1, sqlite3_column_int(sStmt, 2));
                sec.orderIndex = sqlite3_column_int(sStmt, 3);
                bool placed = false;
                if (sqlite3_column_type(sStmt, 4) != SQLITE_NULL) {
                    int64_t arrId = sqlite3_column_int64(sStmt, 4);
                    for (auto& arr : s.arrangements) {
                        if (arr.id.has_value() && *arr.id == arrId) {
                            arr.sections.push_back(sec);
                            placed = true;
                            break;
                        }
                    }
                }
                if (!placed) {
                    if (!s.arrangements.empty()) s.arrangements.front().sections.push_back(sec);
                    else s.sections.push_back(sec);
                }
            }
            sqlite3_finalize(sStmt);
        }

        const char* tempoSql = "SELECT id, bar, beat, bpm, ramp_bars, order_index, arrangement_id FROM arrangement_tempo_points WHERE song_id = ? ORDER BY bar ASC, beat ASC, order_index ASC, id ASC;";
        sqlite3_stmt* tpStmt = nullptr;
        if (sqlite3_prepare_v2(db_, tempoSql, -1, &tpStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(tpStmt, 1, songId);
            while (sqlite3_step(tpStmt) == SQLITE_ROW) {
                TempoPoint pt;
                pt.id = sqlite3_column_int64(tpStmt, 0);
                pt.bar = std::max(1, sqlite3_column_int(tpStmt, 1));
                pt.beat = std::max(1.0, sqlite3_column_double(tpStmt, 2));
                pt.bpm = std::clamp(sqlite3_column_double(tpStmt, 3), 20.0, 300.0);
                pt.rampBars = std::max(0.0, sqlite3_column_double(tpStmt, 4));
                pt.orderIndex = sqlite3_column_int(tpStmt, 5);
                int64_t arrId = sqlite3_column_int64(tpStmt, 6);
                for (auto& arr : s.arrangements) {
                    if (arr.id.has_value() && *arr.id == arrId) {
                        arr.tempoPoints.push_back(pt);
                        break;
                    }
                }
            }
            sqlite3_finalize(tpStmt);
        }

        if (!s.arrangements.empty()) {
            s.tracks = s.arrangements.front().tracks;
            s.sections = s.arrangements.front().sections;
            s.tempoPoints = s.arrangements.front().tempoPoints;
        }
    };

    for (auto& s : songs) {
        populateSongChildren(s);
    }

    return songs;
}

std::optional<Song> DatabaseService::loadSong(int64_t songId) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    const char* sql = "SELECT id, title, artist, tempo, key, notes, time_signature, click_eighths, click_sixteenths, click_eighths_gain, click_sixteenths_gain FROM songs WHERE id = ?;";
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
    s.clickEighths = sqlite3_column_int(stmt, 7) != 0;
    s.clickSixteenths = sqlite3_column_int(stmt, 8) != 0;
    s.clickEighthsGain = static_cast<float>(sqlite3_column_double(stmt, 9));
    s.clickSixteenthsGain = static_cast<float>(sqlite3_column_double(stmt, 10));
    sqlite3_finalize(stmt);

    // Fetch arrangements
    const char* arrSql = "SELECT id, name, tempo, key, time_signature, click_eighths, click_sixteenths, click_eighths_gain, click_sixteenths_gain FROM arrangements WHERE song_id = ? ORDER BY order_index ASC, id ASC;";
    sqlite3_stmt* aStmt = nullptr;
    if (sqlite3_prepare_v2(db_, arrSql, -1, &aStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(aStmt, 1, songId);
        while (sqlite3_step(aStmt) == SQLITE_ROW) {
            SongArrangement arr;
            arr.id = sqlite3_column_int64(aStmt, 0);
            const unsigned char* name = sqlite3_column_text(aStmt, 1);
            arr.name = name ? reinterpret_cast<const char*>(name) : "Main";
            arr.tempo = sqlite3_column_int(aStmt, 2);
            const unsigned char* k = sqlite3_column_text(aStmt, 3);
            arr.key = k ? reinterpret_cast<const char*>(k) : "C";
            const unsigned char* ts = sqlite3_column_text(aStmt, 4);
            arr.timeSignature = ts ? reinterpret_cast<const char*>(ts) : "4/4";
            arr.clickEighths = sqlite3_column_int(aStmt, 5) != 0;
            arr.clickSixteenths = sqlite3_column_int(aStmt, 6) != 0;
            arr.clickEighthsGain = static_cast<float>(sqlite3_column_double(aStmt, 7));
            arr.clickSixteenthsGain = static_cast<float>(sqlite3_column_double(aStmt, 8));
            s.arrangements.push_back(std::move(arr));
        }
        sqlite3_finalize(aStmt);
    }

    // Fetch tracks
    const char* trackSql = "SELECT id, name, file_path, output_channel, gain, muted, track_key, num_channels, stereo_output, arrangement_id FROM tracks WHERE song_id = ? ORDER BY output_channel ASC, id ASC;";
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
            const unsigned char* tk = sqlite3_column_text(tStmt, 6);
            t.trackKey = tk ? reinterpret_cast<const char*>(tk) : "";
            t.numChannels = sqlite3_column_int(tStmt, 7);
            t.stereoOutput = sqlite3_column_int(tStmt, 8) != 0;
            bool placed = false;
            if (sqlite3_column_type(tStmt, 9) != SQLITE_NULL) {
                int64_t arrId = sqlite3_column_int64(tStmt, 9);
                for (auto& arr : s.arrangements) {
                    if (arr.id.has_value() && *arr.id == arrId) {
                        arr.tracks.push_back(t);
                        placed = true;
                        break;
                    }
                }
            }
            if (!placed) {
                if (!s.arrangements.empty()) s.arrangements.front().tracks.push_back(t);
                else s.tracks.push_back(t);
            }
        }
        sqlite3_finalize(tStmt);
    }

    // Fetch sections
    const char* secSql = "SELECT id, name, length_bars, order_index, arrangement_id FROM song_sections WHERE song_id = ? ORDER BY order_index ASC, id ASC;";
    sqlite3_stmt* sStmt = nullptr;
    if (sqlite3_prepare_v2(db_, secSql, -1, &sStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(sStmt, 1, songId);
        while (sqlite3_step(sStmt) == SQLITE_ROW) {
            SongSection sec;
            sec.id = sqlite3_column_int64(sStmt, 0);
            const unsigned char* name = sqlite3_column_text(sStmt, 1);
            sec.name = name ? reinterpret_cast<const char*>(name) : "";
            sec.lengthBars = std::max(1, sqlite3_column_int(sStmt, 2));
            sec.orderIndex = sqlite3_column_int(sStmt, 3);
            bool placed = false;
            if (sqlite3_column_type(sStmt, 4) != SQLITE_NULL) {
                int64_t arrId = sqlite3_column_int64(sStmt, 4);
                for (auto& arr : s.arrangements) {
                    if (arr.id.has_value() && *arr.id == arrId) {
                        arr.sections.push_back(sec);
                        placed = true;
                        break;
                    }
                }
            }
            if (!placed) {
                if (!s.arrangements.empty()) s.arrangements.front().sections.push_back(sec);
                else s.sections.push_back(sec);
            }
        }
        sqlite3_finalize(sStmt);
    }

    // Fetch tempo points
    const char* tempoSql = "SELECT id, bar, beat, bpm, ramp_bars, order_index, arrangement_id FROM arrangement_tempo_points WHERE song_id = ? ORDER BY bar ASC, beat ASC, order_index ASC, id ASC;";
    sqlite3_stmt* tpStmt = nullptr;
    if (sqlite3_prepare_v2(db_, tempoSql, -1, &tpStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(tpStmt, 1, songId);
        while (sqlite3_step(tpStmt) == SQLITE_ROW) {
            TempoPoint pt;
            pt.id = sqlite3_column_int64(tpStmt, 0);
            pt.bar = std::max(1, sqlite3_column_int(tpStmt, 1));
            pt.beat = std::max(1.0, sqlite3_column_double(tpStmt, 2));
            pt.bpm = std::clamp(sqlite3_column_double(tpStmt, 3), 20.0, 300.0);
            pt.rampBars = std::max(0.0, sqlite3_column_double(tpStmt, 4));
            pt.orderIndex = sqlite3_column_int(tpStmt, 5);
            int64_t arrId = sqlite3_column_int64(tpStmt, 6);
            for (auto& arr : s.arrangements) {
                if (arr.id.has_value() && *arr.id == arrId) {
                    arr.tempoPoints.push_back(pt);
                    break;
                }
            }
        }
        sqlite3_finalize(tpStmt);
    }

    if (!s.arrangements.empty()) {
        s.tracks = s.arrangements.front().tracks;
        s.sections = s.arrangements.front().sections;
        s.tempoPoints = s.arrangements.front().tempoPoints;
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
        const char* insertItemSql = "INSERT INTO setlist_items (setlist_id, song_id, order_index, override_tempo, override_key, override_time_signature, arrangement_id, transition_mode, transition_crossfade_bars) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
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

            if (entry.arrangementId.has_value())
                sqlite3_bind_int64(itemStmt, 7, *entry.arrangementId);
            else
                sqlite3_bind_null(itemStmt, 7);

            sqlite3_bind_int(itemStmt, 8, static_cast<int>(entry.transitionMode));
            sqlite3_bind_int(itemStmt, 9, clampCrossfadeBars(entry.transitionCrossfadeBars));

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
        const char* sql = "SELECT id, setlist_id, song_id, order_index, override_tempo, override_key, override_time_signature, arrangement_id, transition_mode, transition_crossfade_bars FROM setlist_items WHERE setlist_id = ? ORDER BY order_index ASC, id ASC;";
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
                if (sqlite3_column_type(stmt, 7) != SQLITE_NULL)
                    e.arrangementId = sqlite3_column_int64(stmt, 7);
                int rawMode = 0;
                if (sqlite3_column_type(stmt, 8) != SQLITE_NULL)
                    rawMode = sqlite3_column_int(stmt, 8);
                e.transitionMode = clampTransitionMode(rawMode);
                int rawXf = (sqlite3_column_type(stmt, 9) != SQLITE_NULL) ? sqlite3_column_int(stmt, 9) : 1;
                if (rawMode == 4 && rawXf == 1) rawXf = 2; // Preserve legacy Crossfade2Bar rows
                e.transitionCrossfadeBars = clampCrossfadeBars(rawXf);
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
