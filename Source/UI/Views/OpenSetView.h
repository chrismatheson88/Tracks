#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../Database/DatabaseService.h"
#include "../../Audio/AudioEngine.h"
#include <vector>
#include <optional>

namespace tracks {

class OpenSetView : public juce::Component, public juce::ListBoxModel {
public:
    OpenSetView(DatabaseService& db, AudioEngine& audioEngine);
    ~OpenSetView() override;

    void refreshSetlists();
    void selectSetlist(int64_t setId);
    void selectPreviousSong();
    void selectNextSong();
    bool canSelectPreviousSong() const;
    bool canSelectNextSong() const;
    SongTransitionMode getCurrentSongTransitionMode() const;
    int getCurrentSongCrossfadeBars() const;
    void setCurrentSongTransition(SongTransitionMode mode, int crossfadeBars);
    void executeSongTransition(AudioEngine::TransitionTrigger trigger);
    void selectArrangementForCurrentSong(int64_t arrangementId);
    void applySongParameters(int tempo, const std::string& key, const std::string& timeSignature);
    void syncSongClickSubdivisions(int64_t songId,
                                   std::optional<int64_t> arrangementId,
                                   bool clickEighths,
                                   bool clickSixteenths,
                                   float clickEighthsGain,
                                   float clickSixteenthsGain);
    std::optional<Song> getClickOnlyBaselineForCurrentSong() const;

    std::function<void(const Song&)> onSongSelectedForPlayback;
    std::function<void(bool, bool)> onSongNavigationStateChanged;

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
    void selectedRowsChanged(int lastRowSelected) override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    juce::Rectangle<int> getTransportArea() const { return transportArea_; }

private:
    struct LibrarySongChoice {
        int64_t songId = 0;
        std::string title;
        std::string artist;
        std::string displayLabel;
    };

    class SongFilterEditor;
    class SongDropdownToggle;
    class SongDropdownOverlay;
    class SetlistPickerLookAndFeel;

    DatabaseService& db_;
    AudioEngine& audioEngine_;

    std::vector<Setlist> setlists_;
    std::optional<Setlist> currentSetlist_;
    int selectedEntryIndex_ = -1;

    // Top set selection, date, and time
    juce::Label setSelectLabel_{{}, "Setlist:"};
    std::unique_ptr<SetlistPickerLookAndFeel> setPickerLookAndFeel_;
    juce::ComboBox setPicker_;
    juce::Label dateLabel_{{}, "Date:"};
    juce::TextEditor dateEditor_;
    juce::TextButton datePickerButton_{"Pick"};
    juce::Label timeLabel_{{}, "Time:"};
    juce::TextEditor timeEditor_;
    juce::TextButton timePickerButton_{"Pick"};
    juce::TextButton newSetButton_{"New Set"};
    juce::TextButton deleteSetButton_{"Delete Set"};

    // Set songs table
    juce::ListBox entryListBox_;
    juce::TextButton moveUpButton_{"Move Up"};
    juce::TextButton moveDownButton_{"Move Down"};
    juce::TextButton removeSongButton_{"Remove"};

    std::vector<LibrarySongChoice> librarySongChoices_;
    std::unique_ptr<SongFilterEditor> addSongFilterEditor_;
    std::unique_ptr<SongDropdownToggle> addSongDropdownButton_;
    std::unique_ptr<SongDropdownOverlay> addSongDropdownOverlay_;
    juce::Rectangle<int> addSongBarBounds_;
    juce::Rectangle<int> transportArea_;
    int setHeaderBottomY_ = 0;

    void loadSetlistToUI(const Setlist& setlist, bool preserveSelection = false);
    Song buildEffectiveSongForEntry(const SetlistEntry& entry) const;
    void preloadCurrentSetlist();
    void saveCurrentSetlist();
    void populateSetPickerItems();
    void notifySongNavigationState();
    void populateAddSongPicker();
    void openAddSongDropdown();
    void hideAddSongDropdown();
    void positionAddSongDropdown();
    void updateFilteredAddSongList();
    void addSongToCurrentSet(int64_t songId);
    void showCalendarPopup();
    void showTimePopup();
};

} // namespace tracks
