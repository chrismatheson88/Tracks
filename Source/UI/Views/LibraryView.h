#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../Database/DatabaseService.h"
#include "../../Audio/AudioEngine.h"
#include "../../Services/AudioAttachmentService.h"
#include <vector>
#include <optional>

namespace tracks {

class LibraryView : public juce::Component, public juce::ListBoxModel {
public:
    LibraryView(DatabaseService& db, AudioEngine& audioEngine);
    ~LibraryView() override = default;

    void refreshSongList();
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

    std::vector<Song> songs_;
    std::optional<Song> currentSong_;
    std::vector<SongTrack> pendingTracks_;
    bool isDirty_ = false;

    // Left panel: Song list
    juce::ListBox songListBox_;
    juce::TextButton newSongButton_{"+ New Song"};
    juce::TextButton deleteSongButton_{"Delete Song"};

    // Right panel: Editor
    juce::Label headerLabel_{{}, "Edit Song"};

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

    // Attachment section
    juce::Label attachmentHeader_{{}, "Audio Stems / Backing Tracks:"};
    juce::TextButton attachFolderButton_{"📁 Select Audio Folder..."};
    juce::Label attachmentStatusLabel_;

    // Tracks viewport / component
    juce::Viewport tracksViewport_;
    std::unique_ptr<juce::Component> tracksContainer_;

    juce::TextButton saveButton_{"Save Edits to Library"};
    juce::TextButton cancelButton_{"Cancel"};

    std::unique_ptr<juce::FileChooser> fileChooser_;

    void loadSongToEditor(const Song& song);
    void clearEditor();
    void markDirty(bool dirty);
    void rebuildTrackWidgets();
    void saveCurrentSong();
    void handleChooseFolder();
};

} // namespace tracks
