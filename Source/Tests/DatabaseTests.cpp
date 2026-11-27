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
    assert(loadedS1->arrangements.size() == 1);
    loadedS1->arrangements[0].tracks.pop_back(); // Remove backing track, keep only click
    tracks::Song updatedS1 = db.saveSong(*loadedS1);
    assert(*updatedS1.id == s1Id);

    auto reloadedS1 = db.loadSong(s1Id);
    assert(reloadedS1.has_value());
    assert(reloadedS1->tempo == 75);
    assert(reloadedS1->tracks.size() == 1);
    assert(reloadedS1->tracks[0].name == "Click");

    // Test 2b: Multiple Arrangements per Song (independent tempo, key, time sig, tracks, sections, click subdivisions)
    std::cout << "[Test] Multiple Arrangements per Song..." << std::endl;
    tracks::Song multiSong;
    multiSong.title = "House of the Lord";
    multiSong.artist = "Phil Wickham";
    multiSong.tempo = 86;
    multiSong.key = "Bb";
    multiSong.timeSignature = "4/4";

    tracks::SongArrangement loudArr;
    loudArr.name = "Loud Final Chorus";
    loudArr.tempo = 86;
    loudArr.key = "Bb";
    loudArr.timeSignature = "4/4";
    tracks::SongTrack loudDrums;
    loudDrums.name = "Drums";
    loudDrums.filePath = "/tmp/loud_drums.wav";
    loudDrums.outputChannel = 0;
    loudDrums.gain = 1.0f;
    loudDrums.trackKey = "Bb";
    loudDrums.numChannels = 2;
    loudDrums.stereoOutput = true;
    loudArr.tracks.push_back(loudDrums);
    loudArr.sections.push_back({ std::nullopt, "Intro", 4, 0 });
    loudArr.sections.push_back({ std::nullopt, "Big Chorus", 8, 1 });

    tracks::SongArrangement quietArr;
    quietArr.name = "Quiet Final Chorus";
    quietArr.tempo = 82;
    quietArr.key = "G";
    quietArr.timeSignature = "6/8";
    tracks::SongTrack quietKeys;
    quietKeys.name = "Keys";
    quietKeys.filePath = "/tmp/quiet_keys.wav";
    quietKeys.outputChannel = 0;
    quietKeys.gain = 0.9f;
    quietKeys.trackKey = "G";
    quietKeys.numChannels = 2;
    quietKeys.stereoOutput = true;
    quietArr.tracks.push_back(quietKeys);
    quietArr.sections.push_back({ std::nullopt, "Soft Intro", 2, 0 });
    quietArr.tempoPoints.push_back({ std::nullopt, 5, 1.0, 64.0, 2.0, 0 }); // Ritardando from Bar 5 over 2 bars
    quietArr.tempoPoints.push_back({ std::nullopt, 9, 1.0, 72.0, 0.0, 1 }); // Instant step at Bar 9

    multiSong.arrangements.push_back(loudArr);
    multiSong.arrangements.push_back(quietArr);

    auto savedMulti = db.saveSong(multiSong);
    assert(savedMulti.id.has_value());
    assert(savedMulti.arrangements.size() == 2);
    assert(savedMulti.arrangements[0].id.has_value());
    assert(savedMulti.arrangements[1].id.has_value());
    int64_t loudId = *savedMulti.arrangements[0].id;
    int64_t quietId = *savedMulti.arrangements[1].id;

    // Default click subdivisions per arrangement are quarter notes only
    assert(!savedMulti.arrangements[0].clickEighths && !savedMulti.arrangements[0].clickSixteenths);
    assert(!savedMulti.arrangements[1].clickEighths && !savedMulti.arrangements[1].clickSixteenths);

    // Update click subdivisions on Quiet Final Chorus only (as remembered from a set)
    db.updateArrangementClickSubdivisions(quietId, true, false, 0.7f, 1.0f);

    // Save a setlist selecting Quiet Final Chorus and verify arrangementId & metadata persist
    tracks::Setlist arrSet;
    arrSet.name = "Evening Acoustic";
    tracks::SetlistEntry arrEntry;
    arrEntry.songId = *savedMulti.id;
    arrEntry.orderIndex = 0;
    arrEntry.arrangementId = quietId;
    arrSet.entries.push_back(arrEntry);
    auto savedArrSet = db.saveSetlist(arrSet);
    auto loadedArrSet = db.loadSetlist(*savedArrSet.id);
    assert(loadedArrSet.has_value());
    assert(loadedArrSet->entries.size() == 1);
    assert(loadedArrSet->entries[0].arrangementId.value_or(0) == quietId);
    assert(loadedArrSet->entries[0].getEffectiveTempo() == 82);
    assert(loadedArrSet->entries[0].getEffectiveKey() == "G");
    assert(loadedArrSet->entries[0].getEffectiveTimeSignature() == "6/8");
    const auto* pickedArr = loadedArrSet->entries[0].getSelectedArrangement();
    assert(pickedArr != nullptr && pickedArr->name == "Quiet Final Chorus");
    assert(pickedArr->clickEighths == true && pickedArr->clickSixteenths == false);
    assert(std::abs(pickedArr->clickEighthsGain - 0.7f) < 1e-4f);
    assert(pickedArr->tracks.size() == 1 && pickedArr->tracks[0].name == "Keys");
    assert(pickedArr->sections.size() == 1 && pickedArr->sections[0].name == "Soft Intro");
    assert(pickedArr->tempoPoints.size() == 2);
    assert(pickedArr->tempoPoints[0].bar == 5 && std::abs(pickedArr->tempoPoints[0].bpm - 64.0) < 1e-4 &&
           std::abs(pickedArr->tempoPoints[0].rampBars - 2.0) < 1e-4);
    assert(pickedArr->tempoPoints[1].bar == 9 && std::abs(pickedArr->tempoPoints[1].bpm - 72.0) < 1e-4 &&
           std::abs(pickedArr->tempoPoints[1].rampBars - 0.0) < 1e-4);

    // Re-saving the song preserves existing arrangement IDs so setlist_items.arrangement_id stays linked
    auto loadedMulti = db.loadSong(*savedMulti.id);
    assert(loadedMulti.has_value());
    loadedMulti->notes = "Updated notes";
    auto resavedMulti = db.saveSong(*loadedMulti);
    assert(*resavedMulti.arrangements[0].id == loudId);
    assert(*resavedMulti.arrangements[1].id == quietId);
    auto reloadedArrSet = db.loadSetlist(*savedArrSet.id);
    assert(reloadedArrSet->entries[0].arrangementId.value_or(0) == quietId);

    // Trackless arrangement (click/guide-only with sections and tempo map)
    tracks::SongArrangement tracklessArr;
    tracklessArr.name = "Acoustic Click Only";
    tracklessArr.tempo = 76;
    tracklessArr.key = "D";
    tracklessArr.timeSignature = "3/4";
    tracklessArr.sections.push_back({std::nullopt, "Verse 1", 8, 0});
    tracklessArr.sections.push_back({std::nullopt, "Chorus", 8, 1});
    tracklessArr.tempoPoints.push_back({std::nullopt, 15, 1.0, 62.0, 2.0, 0});
    resavedMulti.arrangements.push_back(tracklessArr);
    auto savedWithTrackless = db.saveSong(resavedMulti);
    assert(savedWithTrackless.arrangements.size() == 3);
    assert(savedWithTrackless.arrangements[2].id.has_value());
    assert(savedWithTrackless.arrangements[2].tracks.empty());
    assert(savedWithTrackless.arrangements[2].sections.size() == 2);
    assert(savedWithTrackless.arrangements[2].tempoPoints.size() == 1);

    tracks::SetlistEntry tracklessEntry;
    tracklessEntry.songId = *savedWithTrackless.id;
    tracklessEntry.arrangementId = savedWithTrackless.arrangements[2].id;
    tracklessEntry.song = savedWithTrackless;
    assert(tracklessEntry.getEffectiveTempo() == 76);
    assert(tracklessEntry.getEffectiveKey() == "D");
    assert(tracklessEntry.getEffectiveTimeSignature() == "3/4");

    // "Just click" mode persisted on a setlist entry ignores arrangements and uses song base settings
    tracks::Setlist justClickSet;
    justClickSet.name = "Click Only Set";
    tracks::SetlistEntry jcEntry;
    jcEntry.songId = *savedWithTrackless.id;
    jcEntry.orderIndex = 0;
    jcEntry.arrangementId = loudId;
    jcEntry.justClick = true;
    justClickSet.entries.push_back(jcEntry);
    auto savedJcSet = db.saveSetlist(justClickSet);
    auto loadedJcSet = db.loadSetlist(*savedJcSet.id);
    assert(loadedJcSet.has_value());
    assert(loadedJcSet->entries.size() == 1);
    assert(loadedJcSet->entries[0].justClick == true);
    assert(loadedJcSet->entries[0].getSelectedArrangement() == nullptr);
    assert(loadedJcSet->entries[0].getEffectiveTempo() == savedWithTrackless.tempo);
    assert(loadedJcSet->entries[0].getEffectiveKey() == savedWithTrackless.key);

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
    entry.transitionMode = tracks::SongTransitionMode::Crossfade;
    entry.transitionCrossfadeBars = 3;
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
    assert(loadedSet->entries[0].transitionMode == tracks::SongTransitionMode::Crossfade);
    assert(loadedSet->entries[0].transitionCrossfadeBars == 3);
    assert(loadedSet->entries[0].song.has_value());
    assert(loadedSet->entries[0].song->title == "Amazing Grace");
    assert(loadedSet->entries[0].getEffectiveTempo() == 80);
    assert(loadedSet->entries[0].getEffectiveKey() == "A");
    assert(loadedSet->entries[0].getEffectiveTimeSignature() == "6/8");

    // Test 3b: Targeted Library base parameter updates (Save changes to library)
    {
        auto requireDb = [](bool cond, const char* msg) {
            if (!cond) {
                throw std::runtime_error(std::string("DatabaseTest failed: ") + msg);
            }
        };
        db.updateSongBaseParams(s1Id, 82, "Bb", "6/8");
        auto s1AfterParams = db.loadSong(s1Id);
        requireDb(s1AfterParams.has_value(), "updated song loaded");
        requireDb(s1AfterParams->tempo == 82, "song tempo updated");
        requireDb(s1AfterParams->key == "Bb", "song key updated");
        requireDb(s1AfterParams->timeSignature == "6/8", "song time signature updated");
        requireDb(s1AfterParams->tracks.size() == 1, "song tracks preserved intact");

        db.updateArrangementBaseParams(*savedWithTrackless.arrangements[2].id, 66, "Eb", "6/8");
        auto arrSongAfterParams = db.loadSong(*savedWithTrackless.id);
        requireDb(arrSongAfterParams.has_value(), "updated arrangement song loaded");
        requireDb(arrSongAfterParams->arrangements[2].tempo == 66, "arrangement tempo updated");
        requireDb(arrSongAfterParams->arrangements[2].key == "Eb", "arrangement key updated");
        requireDb(arrSongAfterParams->arrangements[2].timeSignature == "6/8", "arrangement time signature updated");
        requireDb(arrSongAfterParams->arrangements[2].sections.size() == 2, "arrangement sections preserved intact");
        requireDb(arrSongAfterParams->arrangements[2].tempoPoints.size() == 1, "arrangement tempo points preserved intact");
    }

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
