#include "../Database/DatabaseService.h"
#include <cassert>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

void runDatabaseTests() {
    std::string testDbPath = "/tmp/test_tracks_db.sqlite3";
    if (fs::exists(testDbPath)) {
        fs::remove(testDbPath);
    }

    std::cout << "[Test] Initializing DatabaseService..." << std::endl;
    tracks::DatabaseService db(testDbPath);

    // Test 1: Save and load Song
    std::cout << "[Test] Saving Song..." << std::endl;
    tracks::Song s1;
    s1.title = "Amazing Grace";
    s1.artist = "Traditional";
    s1.tempo = 72;
    s1.key = "G";
    s1.timeSignature = "3/4";
    s1.notes = "Solo acoustic intro";

    tracks::SongTrack t1;
    t1.name = "Click";
    t1.filePath = "/path/to/click.wav";
    t1.outputChannel = 0;
    t1.stereoOutput = false;
    t1.gain = 0.8f;
    t1.trackKey = "G";
    t1.numChannels = 1;
    s1.addTrack(t1);

    tracks::SongTrack t2;
    t2.name = "Backing Track";
    t2.filePath = "/path/to/backing.wav";
    t2.outputChannel = 1;
    t2.stereoOutput = true;
    t2.gain = 1.0f;
    t2.trackKey = "G";
    t2.numChannels = 2;
    s1.addTrack(t2);

    tracks::SongSection sec1;
    sec1.name = "Intro";
    sec1.lengthBars = 4;
    sec1.orderIndex = 0;
    s1.sections.push_back(sec1);

    tracks::SongSection sec2;
    sec2.name = "Verse 1";
    sec2.lengthBars = 8;
    sec2.orderIndex = 1;
    s1.sections.push_back(sec2);
    s1.clickEighths = true;
    s1.clickSixteenths = false;
    s1.clickEighthsGain = 0.65f;
    s1.clickSixteenthsGain = 0.5f;

    tracks::Song savedS1 = db.saveSong(s1);
    assert(savedS1.id.has_value());
    int64_t s1Id = *savedS1.id;

    auto loadedS1 = db.loadSong(s1Id);
    assert(loadedS1.has_value());
    assert(loadedS1->title == "Amazing Grace");
    assert(loadedS1->tempo == 72);
    assert(loadedS1->timeSignature == "3/4");
    assert(loadedS1->clickEighths == true);
    assert(loadedS1->clickSixteenths == false);
    assert(std::abs(loadedS1->clickEighthsGain - 0.65f) < 1e-4f);
    assert(std::abs(loadedS1->clickSixteenthsGain - 0.5f) < 1e-4f);

    db.updateSongClickSubdivisions(s1Id, true, true, 0.8f, 0.4f);
    auto updatedClickS1 = db.loadSong(s1Id);
    assert(updatedClickS1.has_value());
    assert(updatedClickS1->clickEighths == true);
    assert(updatedClickS1->clickSixteenths == true);
    assert(std::abs(updatedClickS1->clickEighthsGain - 0.8f) < 1e-4f);
    assert(std::abs(updatedClickS1->clickSixteenthsGain - 0.4f) < 1e-4f);
    loadedS1 = updatedClickS1;
    assert(loadedS1->tracks.size() == 2);
    assert(loadedS1->tracks[0].name == "Click");
    assert(loadedS1->tracks[0].trackKey == "G");
    assert(loadedS1->tracks[0].numChannels == 1);
    assert(loadedS1->tracks[0].stereoOutput == false);
    assert(loadedS1->tracks[1].name == "Backing Track");
    assert(loadedS1->tracks[1].trackKey == "G");
    assert(loadedS1->tracks[1].numChannels == 2);
    assert(loadedS1->tracks[1].stereoOutput == true);
    assert(loadedS1->sections.size() == 2);
    assert(loadedS1->sections[0].name == "Intro");
    assert(loadedS1->sections[0].lengthBars == 4);
    assert(loadedS1->sections[1].name == "Verse 1");
    assert(loadedS1->sections[1].lengthBars == 8);

    // Test 2: Update Song (and verify track replacement)
    std::cout << "[Test] Updating Song..." << std::endl;
    loadedS1->tempo = 75;
    loadedS1->tracks.pop_back(); // Remove backing track, keep only click
    tracks::Song updatedS1 = db.saveSong(*loadedS1);
    assert(*updatedS1.id == s1Id);

    auto reloadedS1 = db.loadSong(s1Id);
    assert(reloadedS1.has_value());
    assert(reloadedS1->tempo == 75);
    assert(reloadedS1->tracks.size() == 1);
    assert(reloadedS1->tracks[0].name == "Click");

    // Test 3: Save and load Setlist with overrides
    std::cout << "[Test] Saving Setlist with overrides..." << std::endl;
    tracks::Setlist set;
    set.name = "Sunday Morning";
    set.setDate = "2026-11-29";
    set.setTime = "10:00 AM";
    set.notes = "Full band";

    tracks::SetlistEntry entry;
    entry.songId = s1Id;
    entry.orderIndex = 0;
    entry.overrideTempo = 80; // Overridden tempo
    entry.overrideKey = "A";   // Overridden key
    entry.overrideTimeSignature = "6/8";
    set.entries.push_back(entry);

    tracks::Setlist savedSet = db.saveSetlist(set);
    assert(savedSet.id.has_value());
    int64_t setId = *savedSet.id;

    auto loadedSet = db.loadSetlist(setId);
    assert(loadedSet.has_value());
    assert(loadedSet->name == "Sunday Morning");
    assert(loadedSet->entries.size() == 1);
    assert(loadedSet->entries[0].songId == s1Id);
    assert(loadedSet->entries[0].overrideTempo.value_or(0) == 80);
    assert(loadedSet->entries[0].overrideKey.value_or("") == "A");
    assert(loadedSet->entries[0].overrideTimeSignature.value_or("") == "6/8");
    assert(loadedSet->entries[0].song.has_value());
    assert(loadedSet->entries[0].song->title == "Amazing Grace");
    assert(loadedSet->entries[0].getEffectiveTempo() == 80);
    assert(loadedSet->entries[0].getEffectiveKey() == "A");
    assert(loadedSet->entries[0].getEffectiveTimeSignature() == "6/8");

    // Test 4: App settings
    std::cout << "[Test] App settings get/set..." << std::endl;
    db.setAppSetting("volume.master_db", "-3.5");
    assert(db.getAppSetting("volume.master_db") == "-3.5");
    db.setAppSetting("volume.master_db", "0.0");
    assert(db.getAppSetting("volume.master_db") == "0.0");
    assert(db.getAppSetting("non_existent", "default_val") == "default_val");

    // Test 5: Cascade delete test
    std::cout << "[Test] Cascade delete test..." << std::endl;
    db.deleteSong(s1Id);
    assert(!db.loadSong(s1Id).has_value());
    assert(db.listTracksForSong(s1Id).empty());
    // The setlist entry for song s1Id should be cascade-deleted by foreign key
    auto entriesAfterSongDelete = db.listSetlistEntries(setId);
    assert(entriesAfterSongDelete.empty());

    // Clean up
    if (fs::exists(testDbPath)) {
        fs::remove(testDbPath);
    }

    std::cout << "[Test] All DatabaseService tests passed successfully!" << std::endl;
}

int main() {
    try {
        runDatabaseTests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Database test failed with error: " << e.what() << std::endl;
        return 1;
    }
}
