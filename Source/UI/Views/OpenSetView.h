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

    std::function<void(const Song&)> onSongSelectedForPlayback;

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
    void selectedRowsChanged(int lastRowSelected) override;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    DatabaseService& db_;
    AudioEngine& audioEngine_;

    std::vector<Setlist> setlists_;
    std::optional<Setlist> currentSetlist_;
    int selectedEntryIndex_ = -1;

    // Top set selection
    juce::Label setSelectLabel_{{}, "Setlist:"};
    juce::ComboBox setPicker_;
    juce::TextButton newSetButton_{"New Set"};
    juce::TextButton deleteSetButton_{"Delete Set"};

    // Set metadata
    juce::Label nameLabel_{{}, "Name:"};
    juce::TextEditor nameEditor_;

    juce::Label dateLabel_{{}, "Date:"};
    juce::TextEditor dateEditor_;
    juce::TextButton datePickerButton_{"Pick"};

    juce::Label timeLabel_{{}, "Time:"};
    juce::TextEditor timeEditor_;

    juce::Label notesLabel_{{}, "Notes:"};
    juce::TextEditor notesEditor_;

    // Set songs table
    juce::ListBox entryListBox_;
    juce::TextButton moveUpButton_{"Move Up"};
    juce::TextButton moveDownButton_{"Move Down"};
    juce::TextButton removeSongButton_{"Remove Song"};

    juce::ComboBox addSongPicker_;
    juce::TextButton addSongButton_{"Add to Set"};

    // Overrides section
    juce::Label overridesHeader_{{}, "Song Overrides in This Set:"};
    juce::Label overrideTempoLabel_{{}, "Override Tempo:"};
    juce::TextEditor overrideTempoEditor_;

    juce::Label overrideKeyLabel_{{}, "Override Key:"};
    juce::ComboBox overrideKeyBox_;

    juce::Label overrideTimeSigLabel_{{}, "Override Time Signature:"};
    juce::ComboBox overrideTimeSigBox_;

    juce::TextButton saveOverrideToLibraryButton_{"Save Edits to Library"};
    juce::TextButton saveSetButton_{"Save Setlist"};

    void loadSetlistToUI(const Setlist& setlist);
    void saveCurrentSetlist();
    void updateOverridesUI();
    void applyOverridesFromUI();
    void saveOverridesToMasterSong();
    void populateAddSongPicker();
    void showCalendarPopup();
};

} // namespace tracks
