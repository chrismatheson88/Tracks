#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../Database/DatabaseService.h"
#include "../../Audio/AudioEngine.h"
#include "../../Services/AudioAttachmentService.h"
#include <vector>
#include <optional>

namespace tracks {

class LibraryView : public juce::Component, public juce::ListBoxModel, private juce::Timer {
public:
    LibraryView(DatabaseService& db, AudioEngine& audioEngine);
    ~LibraryView() override;

    void refreshSongList();
    void reloadAfterImport();
    void flushPendingAutoSave();
    void selectSong(int64_t songId);

    std::function<void(const Song&)> onSongLoadedForPlayback;

    // ListBoxModel overrides
    int getNumRows() override;
    void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
    void selectedRowsChanged(int lastRowSelected) override;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    DatabaseService& db_;
    AudioEngine& audioEngine_;

    std::vector<Song> allSongs_;
    std::vector<Song> songs_;
    std::optional<Song> currentSong_;
    std::vector<SongArrangement> pendingArrangements_;
    std::vector<int64_t> removedArrangementIds_;
    int selectedArrangementIndex_ = -1;
    bool hasPendingAutoSave_ = false;
    bool isLoadingToEditor_ = false;
    bool tracksModified_ = false;

    // Left panel: Song search + list
    juce::TextEditor searchEditor_;
    juce::TextButton searchClearButton_{"x"};
    juce::ListBox songListBox_;
    juce::TextButton newSongButton_{"New Song"};
    juce::TextButton deleteSongButton_{"Delete Song"};

    // Right panel: Editor
    juce::Label headerLabel_{{}, "Edit Song"};
    juce::Label autoSaveStatusLabel_;

    juce::Label titleLabel_{{}, "Title:"};
    juce::TextEditor titleEditor_;

    juce::Label artistLabel_{{}, "Artist:"};
    juce::TextEditor artistEditor_;

    juce::Label tempoLabel_{{}, "Tempo (BPM):"};
    juce::TextEditor tempoEditor_;

    juce::Label keyLabel_{{}, "Key:"};
    juce::ComboBox keyBox_;

    juce::Label timeSigLabel_{{}, "Time Signature:"};
    juce::ComboBox timeSigBox_;

    juce::Label notesLabel_{{}, "Notes:"};
    juce::TextEditor notesEditor_;

    // Arrangements / Backing tracks section
    juce::Label attachmentHeader_{{}, "Arrangements:"};
    juce::ComboBox arrangementPicker_;
    juce::TextButton attachFolderButton_{"Add Arrangement"};
    juce::TextButton editArrangementButton_{"Edit Arrangement"};
    juce::TextButton removeArrangementButton_{"Remove"};
    juce::Label attachmentStatusLabel_;
    juce::TextEditor attachmentWarningBox_;

    // Tracks viewport / component
    juce::Viewport tracksViewport_;
    std::unique_ptr<juce::Component> tracksContainer_;

    std::unique_ptr<juce::FileChooser> fileChooser_;

    void timerCallback() override;
    void applySongFilter(bool allowAutoSelectFirst);
    void loadSongToEditor(const Song& song);
    void clearEditor();
    void scheduleAutoSave();
    void setAutoSaveStatus(const juce::String& text, juce::Colour colour);
    void refreshArrangementPicker();
    void updateAttachmentStatusAndButtons();
    void setAttachmentWarning(const juce::String& warningText);
    void rebuildTrackWidgets();
    void saveCurrentSong(bool refreshTrackPanel = false, bool forceFallbackForEmptyFields = false);
    void openArrangementDialog(std::optional<int> editIndex);
    void handleChooseFolder();
    void handleEditArrangement();
    void handleRemoveArrangement();
};

} // namespace tracks
