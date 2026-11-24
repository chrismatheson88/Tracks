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
    ~OpenSetView() override = default;

    void refreshSetlists();
    void selectSetlist(int64_t setId);
    void syncSongClickSubdivisions(int64_t songId,
                                   bool clickEighths,
                                   bool clickSixteenths,
                                   float clickEighthsGain,
                                   float clickSixteenthsGain);

    std::function<void(const Song&)> onSongSelectedForPlayback;

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
    void selectedRowsChanged(int lastRowSelected) override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    juce::Rectangle<int> getTransportArea() const { return transportArea_; }

private:
    DatabaseService& db_;
    AudioEngine& audioEngine_;

    std::vector<Setlist> setlists_;
    std::optional<Setlist> currentSetlist_;
    int selectedEntryIndex_ = -1;

    // Top set selection, date, and time
    juce::Label setSelectLabel_{{}, "Setlist:"};
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

    juce::ComboBox addSongPicker_;

    // Current song overrides section
    juce::Label overridesHeader_{{}, "Current Song"};
    juce::Label overrideTempoLabel_{{}, "Tempo:"};
    juce::TextEditor overrideTempoEditor_;

    juce::Label overrideKeyLabel_{{}, "Key:"};
    juce::ComboBox overrideKeyBox_;

    juce::Label overrideTimeSigLabel_{{}, "Time Signature:"};
    juce::ComboBox overrideTimeSigBox_;

    juce::TextButton saveOverrideToLibraryButton_{"Save"};
    juce::Rectangle<int> transportArea_;

    void loadSetlistToUI(const Setlist& setlist, bool preserveSelection = false);
    void preloadCurrentSetlist();
    void saveCurrentSetlist();
    void updateOverridesUI();
    void applyOverridesFromUI();
    void saveOverridesToMasterSong();
    void populateAddSongPicker();
    void showCalendarPopup();
    void showTimePopup();
};

} // namespace tracks
