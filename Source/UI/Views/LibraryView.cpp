#include "LibraryView.h"
#include "../../Common/AppTheme.h"
#include "../../Models/AvailableSections.h"
#include <algorithm>

namespace tracks {

LibraryView::LibraryView(DatabaseService& db, AudioEngine& audioEngine)
    : db_(db), audioEngine_(audioEngine) {
    setOpaque(true);

    // Song List
    songListBox_.setModel(this);
    songListBox_.setRowHeight(36);
    addAndMakeVisible(songListBox_);

    newSongButton_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
    newSongButton_.onClick = [this]() {
        clearEditor();
        songListBox_.deselectAllRows();
        markDirty(true);
    };
    addAndMakeVisible(newSongButton_);

    deleteSongButton_.setColour(juce::TextButton::buttonColourId, AppTheme::dangerRed);
    deleteSongButton_.onClick = [this]() {
        if (currentSong_ && currentSong_->id.has_value()) {
            int64_t id = *currentSong_->id;
            audioEngine_.invalidateCachedSong(id);
            db_.deleteSong(id);
            AudioAttachmentService::removeSongStorage(id);
            clearEditor();
            refreshSongList();
        }
    };
    addAndMakeVisible(deleteSongButton_);

    // Editor fields
    headerLabel_.setFont(juce::Font(18.0f, juce::Font::bold));
    headerLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
    addAndMakeVisible(headerLabel_);

    auto setupField = [this](juce::Label& lbl, juce::TextEditor& ed) {
        lbl.setFont(juce::Font(12.0f, juce::Font::bold));
        lbl.setColour(juce::Label::textColourId, AppTheme::textMuted);
        addAndMakeVisible(lbl);

        ed.onTextChange = [this]() { markDirty(true); };
        addAndMakeVisible(ed);
    };

    setupField(titleLabel_, titleEditor_);
    setupField(artistLabel_, artistEditor_);
    setupField(tempoLabel_, tempoEditor_);
    setupField(notesLabel_, notesEditor_);
    notesEditor_.setMultiLine(true);
    notesEditor_.setReturnKeyStartsNewLine(true);

    // Key ComboBox
    keyLabel_.setFont(juce::Font(12.0f, juce::Font::bold));
    keyLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
    addAndMakeVisible(keyLabel_);

    const char* keys[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B",
                           "Am", "Bbm", "Bm", "Cm", "C#m", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "Abm" };
    int kid = 1;
    for (const char* k : keys) {
        keyBox_.addItem(k, kid++);
    }
    keyBox_.onChange = [this]() { markDirty(true); };
    addAndMakeVisible(keyBox_);

    // Time Signature ComboBox
    timeSigLabel_.setFont(juce::Font(12.0f, juce::Font::bold));
    timeSigLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
    addAndMakeVisible(timeSigLabel_);

    const char* sigs[] = { "4/4", "3/4", "6/8", "12/8", "2/4", "5/4", "7/8" };
    int sid = 1;
    for (const char* s : sigs) {
        timeSigBox_.addItem(s, sid++);
    }
    timeSigBox_.onChange = [this]() { markDirty(true); };
    addAndMakeVisible(timeSigBox_);

    // Arrangements header & controls
    attachmentHeader_.setFont(juce::Font(14.0f, juce::Font::bold));
    attachmentHeader_.setColour(juce::Label::textColourId, AppTheme::textLight);
    addAndMakeVisible(attachmentHeader_);

    arrangementPicker_.setTextWhenNoChoicesAvailable("No arrangements");
    arrangementPicker_.setTextWhenNothingSelected("No arrangements");
    arrangementPicker_.onChange = [this]() {
        int idx = arrangementPicker_.getSelectedItemIndex();
        if (idx >= 0 && idx < static_cast<int>(pendingArrangements_.size())) {
            selectedArrangementIndex_ = idx;
            updateAttachmentStatusAndButtons();
            rebuildTrackWidgets();
        }
    };
    addAndMakeVisible(arrangementPicker_);

    attachFolderButton_.onClick = [this]() { handleChooseFolder(); };
    addAndMakeVisible(attachFolderButton_);

    editArrangementButton_.setEnabled(false);
    editArrangementButton_.onClick = [this]() { handleEditArrangement(); };
    addAndMakeVisible(editArrangementButton_);

    addSectionsButton_.setEnabled(false);
    addSectionsButton_.onClick = [this]() { handleAddSections(); };
    addAndMakeVisible(addSectionsButton_);

    removeArrangementButton_.setColour(juce::TextButton::buttonColourId, AppTheme::dangerRed);
    removeArrangementButton_.setEnabled(false);
    removeArrangementButton_.onClick = [this]() { handleRemoveArrangement(); };
    addAndMakeVisible(removeArrangementButton_);

    attachmentStatusLabel_.setFont(juce::Font(11.0f, juce::Font::plain));
    attachmentStatusLabel_.setColour(juce::Label::textColourId, AppTheme::warningAmber);
    addAndMakeVisible(attachmentStatusLabel_);

    // Tracks viewport
    tracksViewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(tracksViewport_);

    // Save and Cancel buttons
    saveButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
    saveButton_.onClick = [this]() { saveCurrentSong(); };
    addAndMakeVisible(saveButton_);

    cancelButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
    cancelButton_.onClick = [this]() {
        if (currentSong_) loadSongToEditor(*currentSong_);
        else clearEditor();
        markDirty(false);
    };
    addAndMakeVisible(cancelButton_);

    refreshSongList();
}

void LibraryView::refreshSongList() {
    songs_ = db_.listSongs();
    songListBox_.updateContent();
    songListBox_.repaint();
}

int LibraryView::getNumRows() {
    return static_cast<int>(songs_.size());
}

void LibraryView::paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) {
    if (rowNumber < 0 || rowNumber >= static_cast<int>(songs_.size())) return;

    if (rowIsSelected) {
        g.setColour(AppTheme::primaryBlue.withAlpha(0.25f));
        g.fillRect(0, 0, width, height);
        g.setColour(AppTheme::primaryBlue);
        g.drawVerticalLine(0, 0.0f, static_cast<float>(height));
    }

    const auto& s = songs_[static_cast<size_t>(rowNumber)];
    g.setFont(juce::Font(13.0f, juce::Font::bold));
    g.setColour(AppTheme::textLight);
    g.drawText(s.title, 12, 4, width - 24, 16, juce::Justification::centredLeft, true);

    g.setFont(juce::Font(11.0f, juce::Font::plain));
    g.setColour(AppTheme::textMuted);
    juce::String subtitle = juce::String(s.artist) + " (" + juce::String(s.tempo) + " BPM, " + juce::String(s.key) + ", " + juce::String(s.timeSignature) + ")";
    g.drawText(subtitle, 12, 20, width - 24, 14, juce::Justification::centredLeft, true);

    g.setColour(AppTheme::borderCol);
    g.drawHorizontalLine(height - 1, 0.0f, static_cast<float>(width));
}

void LibraryView::selectedRowsChanged(int lastRowSelected) {
    if (lastRowSelected >= 0 && lastRowSelected < static_cast<int>(songs_.size())) {
        currentSong_ = songs_[static_cast<size_t>(lastRowSelected)];
        loadSongToEditor(*currentSong_);
    }
}

void LibraryView::loadSongToEditor(const Song& song) {
    headerLabel_.setText("Edit Song: " + song.title, juce::dontSendNotification);
    titleEditor_.setText(song.title, false);
    artistEditor_.setText(song.artist, false);
    tempoEditor_.setText(juce::String(song.tempo), false);

    keyBox_.setText(song.key, juce::dontSendNotification);
    timeSigBox_.setText(song.timeSignature, juce::dontSendNotification);
    notesEditor_.setText(song.notes, false);

    pendingArrangements_ = song.arrangements;
    if (pendingArrangements_.empty() && (!song.tracks.empty() || !song.sections.empty())) {
        SongArrangement fallback;
        fallback.name = "Main";
        fallback.tempo = song.tempo;
        fallback.key = song.key;
        fallback.timeSignature = song.timeSignature;
        fallback.tracks = song.tracks;
        fallback.sections = song.sections;
        pendingArrangements_.push_back(std::move(fallback));
    }
    removedArrangementIds_.clear();
    selectedArrangementIndex_ = pendingArrangements_.empty() ? -1 : 0;
    tracksModified_ = false;
    refreshArrangementPicker();
    updateAttachmentStatusAndButtons();
    rebuildTrackWidgets();
    markDirty(false);
}

void LibraryView::clearEditor() {
    currentSong_ = std::nullopt;
    headerLabel_.setText("Create New Song", juce::dontSendNotification);
    titleEditor_.setText("", false);
    artistEditor_.setText("", false);
    tempoEditor_.setText("120", false);
    keyBox_.setText("C", juce::dontSendNotification);
    timeSigBox_.setText("4/4", juce::dontSendNotification);
    notesEditor_.setText("", false);
    pendingArrangements_.clear();
    removedArrangementIds_.clear();
    selectedArrangementIndex_ = -1;
    tracksModified_ = false;
    refreshArrangementPicker();
    updateAttachmentStatusAndButtons();
    rebuildTrackWidgets();
    markDirty(false);
}

void LibraryView::refreshArrangementPicker() {
    arrangementPicker_.clear(juce::dontSendNotification);
    for (size_t i = 0; i < pendingArrangements_.size(); ++i) {
        arrangementPicker_.addItem(pendingArrangements_[i].toDisplayLabel(), static_cast<int>(i + 1));
    }
    if (pendingArrangements_.empty()) {
        selectedArrangementIndex_ = -1;
        arrangementPicker_.setSelectedId(0, juce::dontSendNotification);
        arrangementPicker_.setEnabled(false);
    } else {
        if (selectedArrangementIndex_ < 0 || selectedArrangementIndex_ >= static_cast<int>(pendingArrangements_.size())) {
            selectedArrangementIndex_ = 0;
        }
        arrangementPicker_.setEnabled(true);
        arrangementPicker_.setSelectedId(selectedArrangementIndex_ + 1, juce::dontSendNotification);
    }
}

void LibraryView::updateAttachmentStatusAndButtons() {
    const bool hasSelection = (selectedArrangementIndex_ >= 0 &&
                               selectedArrangementIndex_ < static_cast<int>(pendingArrangements_.size()));
    editArrangementButton_.setEnabled(hasSelection);
    removeArrangementButton_.setEnabled(hasSelection);

    if (!hasSelection) {
        addSectionsButton_.setEnabled(false);
        attachmentStatusLabel_.setText("", juce::dontSendNotification);
        return;
    }

    const auto& arr = pendingArrangements_[static_cast<size_t>(selectedArrangementIndex_)];
    const bool hasTracks = !arr.tracks.empty();
    addSectionsButton_.setEnabled(hasTracks);

    juce::String status = juce::String(arr.tracks.size()) + " audio file(s)  |  " +
                          juce::String(arr.tempo) + " BPM, Key: " + juce::String(arr.key.empty() ? "C" : arr.key) +
                          ", " + juce::String(arr.timeSignature.empty() ? "4/4" : arr.timeSignature);
    if (!arr.sections.empty()) {
        int totalBars = 0;
        for (const auto& sec : arr.sections) {
            totalBars += std::max(1, sec.lengthBars);
        }
        status += "  |  " + juce::String(arr.sections.size()) +
                  (arr.sections.size() == 1 ? " section (" : " sections (") +
                  juce::String(totalBars) + " bars)";
    }
    attachmentStatusLabel_.setText(status, juce::dontSendNotification);
}

void LibraryView::handleRemoveArrangement() {
    if (selectedArrangementIndex_ < 0 ||
        selectedArrangementIndex_ >= static_cast<int>(pendingArrangements_.size())) {
        return;
    }
    const auto& removed = pendingArrangements_[static_cast<size_t>(selectedArrangementIndex_)];
    if (removed.id.has_value()) {
        removedArrangementIds_.push_back(*removed.id);
    }
    pendingArrangements_.erase(pendingArrangements_.begin() + selectedArrangementIndex_);
    if (selectedArrangementIndex_ >= static_cast<int>(pendingArrangements_.size())) {
        selectedArrangementIndex_ = static_cast<int>(pendingArrangements_.size()) - 1;
    }
    tracksModified_ = true;
    refreshArrangementPicker();
    updateAttachmentStatusAndButtons();
    rebuildTrackWidgets();
    markDirty(true);
}

void LibraryView::markDirty(bool dirty) {
    isDirty_ = dirty;
    saveButton_.setColour(juce::TextButton::buttonColourId,
                          isDirty_ ? AppTheme::primaryBlue : AppTheme::bgPanel);
}

void LibraryView::handleChooseFolder() {
    openArrangementDialog(std::nullopt);
}

void LibraryView::handleEditArrangement() {
    if (selectedArrangementIndex_ < 0 ||
        selectedArrangementIndex_ >= static_cast<int>(pendingArrangements_.size())) {
        return;
    }
    openArrangementDialog(selectedArrangementIndex_);
}

void LibraryView::openArrangementDialog(std::optional<int> editIndex) {
    const bool isEditMode = editIndex.has_value() &&
                            *editIndex >= 0 &&
                            *editIndex < static_cast<int>(pendingArrangements_.size());

    class ArrangementDialogContent : public juce::Component {
    public:
        ArrangementDialogContent(bool editMode,
                                 const SongArrangement& initialArr,
                                 const juce::String& initialDir,
                                 std::function<void(SongArrangement, bool, const juce::String&)> onConfirm)
            : isEditMode_(editMode),
              originalArr_(initialArr),
              initialDir_(initialDir),
              onConfirm_(std::move(onConfirm)) {
            setSize(500, 250);

            auto setupLbl = [this](juce::Label& l) {
                l.setFont(juce::Font(12.0f, juce::Font::bold));
                l.setColour(juce::Label::textColourId, AppTheme::textLight);
                addAndMakeVisible(l);
            };

            setupLbl(nameLabel_);
            nameEditor_.setJustification(juce::Justification::centredLeft);
            nameEditor_.setText(initialArr.name, false);
            nameEditor_.setTextToShowWhenEmpty("e.g. Loud Final Chorus, Quiet Ending...", AppTheme::textMuted.withAlpha(0.6f));
            addAndMakeVisible(nameEditor_);

            setupLbl(dirLabel_);
            dirEditor_.setJustification(juce::Justification::centredLeft);
            dirEditor_.setText(initialDir, false);
            dirEditor_.setTextToShowWhenEmpty(
                editMode ? "Keep current audio files, or browse to replace..."
                         : "Select a folder with WAV, MP3, or M4A files...",
                AppTheme::textMuted.withAlpha(0.6f));
            dirEditor_.onTextChange = [this]() {
                if (isEditMode_) {
                    confirmButton_.setEnabled(true);
                } else {
                    confirmButton_.setEnabled(dirEditor_.getText().trim().isNotEmpty());
                }
            };
            addAndMakeVisible(dirEditor_);

            browseButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
            browseButton_.onClick = [this]() { openFolderChooser(); };
            addAndMakeVisible(browseButton_);

            setupLbl(tempoLabel_);
            tempoEditor_.setJustification(juce::Justification::centred);
            tempoEditor_.setInputRestrictions(3, "0123456789");
            tempoEditor_.setText(juce::String(initialArr.tempo > 0 ? initialArr.tempo : 120), false);
            addAndMakeVisible(tempoEditor_);

            setupLbl(keyLabel_);
            const char* keys[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B",
                                   "Am", "Bbm", "Bm", "Cm", "C#m", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "Abm" };
            int kid = 1;
            for (const char* k : keys) {
                keyBox_.addItem(k, kid++);
            }
            if (!initialArr.key.empty()) {
                keyBox_.setText(initialArr.key, juce::dontSendNotification);
            } else {
                keyBox_.setSelectedId(1, juce::dontSendNotification);
            }
            addAndMakeVisible(keyBox_);

            setupLbl(timeSigLabel_);
            const char* sigs[] = { "4/4", "3/4", "6/8", "12/8", "2/4", "5/4", "7/8" };
            int sid = 1;
            for (const char* s : sigs) {
                timeSigBox_.addItem(s, sid++);
            }
            if (!initialArr.timeSignature.empty()) {
                timeSigBox_.setText(initialArr.timeSignature, juce::dontSendNotification);
            } else {
                timeSigBox_.setSelectedId(1, juce::dontSendNotification);
            }
            addAndMakeVisible(timeSigBox_);

            statusLabel_.setFont(juce::Font(11.5f, juce::Font::plain));
            statusLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
            if (isEditMode_ && !originalArr_.tracks.empty()) {
                statusLabel_.setText(juce::String(originalArr_.tracks.size()) + " existing audio file(s) attached (browse to replace)",
                                     juce::dontSendNotification);
            }
            addAndMakeVisible(statusLabel_);

            cancelButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
            cancelButton_.onClick = [this]() { closeDialog(); };
            addAndMakeVisible(cancelButton_);

            confirmButton_.setButtonText(isEditMode_ ? "Save Changes" : "Add Arrangement");
            confirmButton_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
            confirmButton_.setEnabled(isEditMode_);
            confirmButton_.onClick = [this]() { confirmSelection(); };
            addAndMakeVisible(confirmButton_);
        }

        void paint(juce::Graphics& g) override {
            g.fillAll(AppTheme::bgDark);
        }

        void resized() override {
            auto area = getLocalBounds().reduced(16);

            auto row0 = area.removeFromTop(28);
            nameLabel_.setBounds(row0.removeFromLeft(96));
            nameEditor_.setBounds(row0);

            area.removeFromTop(12);

            auto row1 = area.removeFromTop(28);
            dirLabel_.setBounds(row1.removeFromLeft(96));
            browseButton_.setBounds(row1.removeFromRight(84));
            row1.removeFromRight(8);
            dirEditor_.setBounds(row1);

            area.removeFromTop(12);

            auto row2 = area.removeFromTop(28);
            tempoLabel_.setBounds(row2.removeFromLeft(96));
            tempoEditor_.setBounds(row2.removeFromLeft(66));
            row2.removeFromLeft(14);
            keyLabel_.setBounds(row2.removeFromLeft(36));
            keyBox_.setBounds(row2.removeFromLeft(78));
            row2.removeFromLeft(14);
            timeSigLabel_.setBounds(row2.removeFromLeft(70));
            timeSigBox_.setBounds(row2);

            area.removeFromTop(12);
            statusLabel_.setBounds(area.removeFromTop(32));

            auto bottomRow = area.removeFromBottom(32);
            confirmButton_.setBounds(bottomRow.removeFromRight(128));
            bottomRow.removeFromRight(8);
            cancelButton_.setBounds(bottomRow.removeFromRight(84));
        }

    private:
        bool isEditMode_ = false;
        SongArrangement originalArr_;
        juce::String initialDir_;

        juce::Label nameLabel_{{}, "Arrangement:"};
        juce::TextEditor nameEditor_;

        juce::Label dirLabel_{{}, "Audio Folder:"};
        juce::TextEditor dirEditor_;
        juce::TextButton browseButton_{"Browse..."};

        juce::Label tempoLabel_{{}, "Tempo (BPM):"};
        juce::TextEditor tempoEditor_;

        juce::Label keyLabel_{{}, "Key:"};
        juce::ComboBox keyBox_;

        juce::Label timeSigLabel_{{}, "Time Sig:"};
        juce::ComboBox timeSigBox_;

        juce::Label statusLabel_;
        juce::TextButton cancelButton_{"Cancel"};
        juce::TextButton confirmButton_{"Add Arrangement"};

        std::unique_ptr<juce::FileChooser> chooser_;
        std::function<void(SongArrangement, bool, const juce::String&)> onConfirm_;

        void openFolderChooser() {
            auto startDir = dirEditor_.getText().trim().isNotEmpty()
                                ? juce::File(dirEditor_.getText().trim())
                                : juce::File::getSpecialLocation(juce::File::userHomeDirectory);
            if (!startDir.isDirectory()) {
                startDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
            }

            chooser_ = std::make_unique<juce::FileChooser>("Select Audio Folder with Stems", startDir);
            auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;

            juce::Component::SafePointer<ArrangementDialogContent> safeThis(this);
            chooser_->launchAsync(flags, [safeThis](const juce::FileChooser& fc) {
                if (safeThis == nullptr) return;
                auto result = fc.getResult();
                if (!result.isDirectory()) return;

                safeThis->dirEditor_.setText(result.getFullPathName(), true);
                auto insp = AudioAttachmentService::inspectAudioDirectory(result.getFullPathName().toStdString());
                if (insp.error.has_value()) {
                    safeThis->statusLabel_.setColour(juce::Label::textColourId, AppTheme::dangerRed);
                    safeThis->statusLabel_.setText(*insp.error, juce::dontSendNotification);
                    safeThis->confirmButton_.setEnabled(false);
                } else {
                    int monoCount = 0;
                    int stereoCount = 0;
                    for (const auto& t : insp.tracks) {
                        if (t.numChannels == 1) ++monoCount;
                        else if (t.numChannels == 2) ++stereoCount;
                    }
                    juce::String msg = juce::String(insp.tracks.size()) + " audio file(s) found";
                    if (monoCount > 0 || stereoCount > 0) {
                        msg += " (";
                        if (stereoCount > 0) msg += juce::String(stereoCount) + " stereo";
                        if (stereoCount > 0 && monoCount > 0) msg += ", ";
                        if (monoCount > 0) msg += juce::String(monoCount) + " mono";
                        msg += ")";
                    }
                    if (!insp.mismatches.empty()) {
                        msg += " - " + juce::String(insp.mismatches[0]);
                        safeThis->statusLabel_.setColour(juce::Label::textColourId, AppTheme::warningAmber);
                    } else {
                        safeThis->statusLabel_.setColour(juce::Label::textColourId, AppTheme::accentGreen);
                    }
                    safeThis->statusLabel_.setText(msg, juce::dontSendNotification);
                    safeThis->confirmButton_.setEnabled(true);
                }
            });
        }

        void confirmSelection() {
            auto dirText = dirEditor_.getText().trim();
            const bool dirChanged = (!isEditMode_) || (dirText.isNotEmpty() && dirText != initialDir_);

            SongArrangement arr = isEditMode_ ? originalArr_ : SongArrangement{};
            arr.name = nameEditor_.getText().trim().toStdString();
            if (arr.name.empty()) arr.name = "Main";
            int parsedTempo = tempoEditor_.getText().trim().getIntValue();
            arr.tempo = (parsedTempo > 0) ? std::clamp(parsedTempo, 20, 300) : 120;
            arr.key = keyBox_.getText().toStdString();
            if (arr.key.empty()) arr.key = "C";
            arr.timeSignature = timeSigBox_.getText().toStdString();
            if (arr.timeSignature.empty()) arr.timeSignature = "4/4";

            juce::String status;
            bool filesReplacedOrRenamed = (isEditMode_ && arr.name != originalArr_.name);

            if (dirChanged) {
                auto insp = AudioAttachmentService::inspectAudioDirectory(dirText.toStdString());
                if (insp.error.has_value()) {
                    statusLabel_.setColour(juce::Label::textColourId, AppTheme::dangerRed);
                    statusLabel_.setText(*insp.error, juce::dontSendNotification);
                    return;
                }
                for (auto& track : insp.tracks) {
                    track.trackKey = arr.key;
                }
                arr.tracks = std::move(insp.tracks);
                filesReplacedOrRenamed = true;

                status = juce::String(arr.tracks.size()) + " audio file(s) ready for \"" + juce::String(arr.name) + "\"";
                if (!insp.mismatches.empty()) {
                    status += " — " + juce::String(insp.mismatches[0]);
                }
            } else {
                for (auto& track : arr.tracks) {
                    track.trackKey = arr.key;
                }
            }

            if (onConfirm_) {
                onConfirm_(std::move(arr), filesReplacedOrRenamed, status);
            }
            closeDialog();
        }

        void closeDialog() {
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
                dw->exitModalState(0);
            }
        }
    };

    SongArrangement initialArr;
    juce::String initialDir;
    if (isEditMode) {
        initialArr = pendingArrangements_[static_cast<size_t>(*editIndex)];
        if (!initialArr.tracks.empty() && !initialArr.tracks[0].filePath.empty()) {
            initialDir = juce::File(initialArr.tracks[0].filePath).getParentDirectory().getFullPathName();
        }
    } else {
        initialArr.name = pendingArrangements_.empty()
                              ? "Main"
                              : ("Arrangement " + std::to_string(pendingArrangements_.size() + 1));
        int defaultTempo = tempoEditor_.getText().trim().getIntValue();
        initialArr.tempo = (defaultTempo > 0) ? defaultTempo : 120;
        initialArr.key = keyBox_.getText().toStdString();
        if (initialArr.key.empty()) initialArr.key = "C";
        initialArr.timeSignature = timeSigBox_.getText().toStdString();
        if (initialArr.timeSignature.empty()) initialArr.timeSignature = "4/4";
    }

    juce::Component::SafePointer<LibraryView> safeView(this);
    auto* content = new ArrangementDialogContent(
        isEditMode,
        initialArr,
        initialDir,
        [safeView, editIndex, isEditMode](SongArrangement updatedArr, bool filesChanged, const juce::String& status) {
            if (safeView == nullptr) return;
            if (isEditMode && *editIndex >= 0 && *editIndex < static_cast<int>(safeView->pendingArrangements_.size())) {
                safeView->pendingArrangements_[static_cast<size_t>(*editIndex)] = std::move(updatedArr);
                safeView->selectedArrangementIndex_ = *editIndex;
            } else {
                safeView->pendingArrangements_.push_back(std::move(updatedArr));
                safeView->selectedArrangementIndex_ = static_cast<int>(safeView->pendingArrangements_.size()) - 1;
            }
            if (filesChanged || !isEditMode) {
                safeView->tracksModified_ = true;
            }
            safeView->refreshArrangementPicker();
            safeView->updateAttachmentStatusAndButtons();
            if (status.isNotEmpty()) {
                safeView->attachmentStatusLabel_.setText(status, juce::dontSendNotification);
            }
            safeView->rebuildTrackWidgets();
            safeView->markDirty(true);
        });

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = isEditMode ? "Edit Arrangement" : "Add Arrangement";
    options.content.setOwned(content);
    options.componentToCentreAround = getTopLevelComponent();
    options.dialogBackgroundColour = AppTheme::bgDark;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void LibraryView::handleAddSections() {
    if (selectedArrangementIndex_ < 0 ||
        selectedArrangementIndex_ >= static_cast<int>(pendingArrangements_.size())) {
        return;
    }
    if (pendingArrangements_[static_cast<size_t>(selectedArrangementIndex_)].tracks.empty()) {
        return;
    }

    class AddSectionsDialogContent : public juce::Component {
    public:
        AddSectionsDialogContent(std::vector<SongSection> initialSections,
                                 std::function<void(std::vector<SongSection>)> onConfirm)
            : sections_(std::move(initialSections)), onConfirm_(std::move(onConfirm)) {
            setSize(520, 440);

            auto setupColHdr = [this](juce::Label& l, juce::Justification just) {
                l.setFont(juce::Font(11.0f, juce::Font::bold));
                l.setColour(juce::Label::textColourId, AppTheme::textMuted);
                l.setJustificationType(just);
                addAndMakeVisible(l);
            };
            setupColHdr(colNumLabel_, juce::Justification::centredLeft);
            setupColHdr(colNameLabel_, juce::Justification::centredLeft);
            setupColHdr(colBarsLabel_, juce::Justification::centredLeft);

            rowsContent_ = std::make_unique<RowsContainer>(*this);
            viewport_.setViewedComponent(rowsContent_.get(), false);
            viewport_.setScrollBarsShown(true, false);
            viewport_.setScrollBarThickness(8);
            viewport_.onScrolled = [this]() {
                if (isDropdownVisible()) {
                    positionDropdown();
                }
            };
            addAndMakeVisible(viewport_);

            errorLabel_.setFont(juce::Font(11.0f, juce::Font::plain));
            errorLabel_.setColour(juce::Label::textColourId, AppTheme::dangerRed);
            addAndMakeVisible(errorLabel_);

            moveUpButton_.onClick = [this]() {
                hideDropdown();
                if (selectedRow_ <= 0 || selectedRow_ >= static_cast<int>(sections_.size())) return;
                std::swap(sections_[static_cast<size_t>(selectedRow_)], sections_[static_cast<size_t>(selectedRow_ - 1)]);
                reindexSections();
                selectedRow_--;
                refreshRowsLayout(false);
                updateSummary();
            };
            addAndMakeVisible(moveUpButton_);

            moveDownButton_.onClick = [this]() {
                hideDropdown();
                if (selectedRow_ < 0 || selectedRow_ >= static_cast<int>(sections_.size()) - 1) return;
                std::swap(sections_[static_cast<size_t>(selectedRow_)], sections_[static_cast<size_t>(selectedRow_ + 1)]);
                reindexSections();
                selectedRow_++;
                refreshRowsLayout(false);
                updateSummary();
            };
            addAndMakeVisible(moveDownButton_);

            removeButton_.setColour(juce::TextButton::buttonColourId, AppTheme::dangerRed);
            removeButton_.onClick = [this]() {
                hideDropdown();
                if (selectedRow_ < 0 || selectedRow_ >= static_cast<int>(sections_.size())) return;
                sections_.erase(sections_.begin() + selectedRow_);
                reindexSections();
                selectedRow_ = -1;
                refreshRowsLayout(false);
                updateSummary();
            };
            addAndMakeVisible(removeButton_);

            summaryLabel_.setFont(juce::Font(11.5f, juce::Font::bold));
            summaryLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
            summaryLabel_.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(summaryLabel_);

            cancelButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
            cancelButton_.onClick = [this]() { closeDialog(); };
            addAndMakeVisible(cancelButton_);

            doneButton_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
            doneButton_.onClick = [this]() {
                // If user picked a valid section on the inline next row and clicked Done before Add, include it too
                std::string pendingName = findCanonicalSectionName(rowsContent_->getFilterText());
                int pendingBars = rowsContent_->getBarsValue();
                if (!pendingName.empty() && pendingBars > 0) {
                    SongSection sec;
                    sec.name = pendingName;
                    sec.lengthBars = pendingBars;
                    sec.orderIndex = static_cast<int>(sections_.size());
                    sections_.push_back(std::move(sec));
                }

                reindexSections();
                if (onConfirm_) {
                    onConfirm_(sections_);
                }
                closeDialog();
            };
            addAndMakeVisible(doneButton_);

            dropdownOverlay_ = std::make_unique<DropdownOverlay>(*this);
            dropdownOverlay_->setAlwaysOnTop(true);
            addChildComponent(dropdownOverlay_.get());

            updateSummary();
            refreshRowsLayout(true);
        }

        void paint(juce::Graphics& g) override {
            g.fillAll(AppTheme::bgDark);

            // Subtle frame around the section list viewport
            if (!listFrameBounds_.isEmpty()) {
                auto r = listFrameBounds_.toFloat().reduced(0.5f);
                g.setColour(AppTheme::bgPanel);
                g.fillRoundedRectangle(r, 4.0f);
                g.setColour(AppTheme::borderCol);
                g.drawRoundedRectangle(r, 4.0f, 1.0f);
            }
        }

        void mouseDown(const juce::MouseEvent&) override {
            hideDropdown();
        }

        void resized() override {
            auto area = getLocalBounds().reduced(16);

            auto colRow = area.removeFromTop(18);
            colNumLabel_.setBounds(colRow.removeFromLeft(34));
            colNameLabel_.setBounds(colRow.removeFromLeft(252));
            colRow.removeFromLeft(8);
            colBarsLabel_.setBounds(colRow);

            area.removeFromTop(4);

            auto bottomRow = area.removeFromBottom(32);
            doneButton_.setBounds(bottomRow.removeFromRight(92));
            bottomRow.removeFromRight(8);
            cancelButton_.setBounds(bottomRow.removeFromRight(82));
            summaryLabel_.setBounds(bottomRow);

            area.removeFromBottom(8);

            auto editRow = area.removeFromBottom(26);
            moveUpButton_.setBounds(editRow.removeFromLeft(82));
            editRow.removeFromLeft(6);
            moveDownButton_.setBounds(editRow.removeFromLeft(90));
            editRow.removeFromLeft(6);
            removeButton_.setBounds(editRow.removeFromLeft(78));

            area.removeFromBottom(6);
            errorLabel_.setBounds(area.removeFromBottom(16));
            area.removeFromBottom(6);

            listFrameBounds_ = area;
            viewport_.setBounds(area.reduced(1));
            refreshRowsLayout(false);
            if (dropdownOverlay_->isVisible()) {
                positionDropdown();
            }
        }

    private:
        enum { kRowHeight = 34 };

        class SectionListViewport : public juce::Viewport {
        public:
            std::function<void()> onScrolled;
            void visibleAreaChanged(const juce::Rectangle<int>&) override {
                if (onScrolled) onScrolled();
            }
        };

        class FilterTextEditor : public juce::TextEditor {
        public:
            std::function<bool(const juce::KeyPress&)> onKeyInterceptor;
            std::function<void()> onMouseClick;

            bool keyPressed(const juce::KeyPress& key) override {
                if (onKeyInterceptor && onKeyInterceptor(key)) {
                    return true;
                }
                return juce::TextEditor::keyPressed(key);
            }

            void mouseDown(const juce::MouseEvent& e) override {
                juce::TextEditor::mouseDown(e);
                if (onMouseClick) onMouseClick();
            }
        };

        class DropdownToggleButton : public juce::Button {
        public:
            DropdownToggleButton() : juce::Button("Dropdown") {
                setWantsKeyboardFocus(false);
            }

            void paintButton(juce::Graphics& g, bool highlighted, bool down) override {
                auto r = getLocalBounds().toFloat().reduced(0.5f);
                juce::Colour bg = AppTheme::bgInput;
                if (down) bg = bg.darker(0.15f);
                else if (highlighted) bg = bg.brighter(0.12f);
                g.setColour(bg);
                g.fillRoundedRectangle(r, 3.0f);
                g.setColour(AppTheme::borderCol);
                g.drawRoundedRectangle(r, 3.0f, 1.0f);

                const float cx = r.getCentreX();
                const float cy = r.getCentreY();
                juce::Path tri;
                tri.addTriangle(cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.0f);
                g.setColour(AppTheme::textLight);
                g.fillPath(tri);
            }
        };

        class DropdownOverlay : public juce::Component, public juce::ListBoxModel {
        public:
            explicit DropdownOverlay(AddSectionsDialogContent& owner) : owner_(owner) {
                setWantsKeyboardFocus(false);
                list_.setModel(this);
                list_.setRowHeight(24);
                list_.setWantsKeyboardFocus(false);
                list_.setColour(juce::ListBox::backgroundColourId, AppTheme::bgPanel.darker(0.15f));
                addAndMakeVisible(list_);
            }

            void setItems(std::vector<std::string> items, const std::string& highlightItem = "") {
                items_ = std::move(items);
                highlightedRow_ = items_.empty() ? -1 : 0;
                if (!highlightItem.empty()) {
                    for (size_t i = 0; i < items_.size(); ++i) {
                        if (items_[i] == highlightItem) {
                            highlightedRow_ = static_cast<int>(i);
                            break;
                        }
                    }
                }
                list_.updateContent();
                if (highlightedRow_ >= 0) {
                    list_.scrollToEnsureRowIsOnscreen(highlightedRow_);
                }
                repaint();
            }

            void moveHighlight(int delta) {
                if (items_.empty()) return;
                if (highlightedRow_ < 0) highlightedRow_ = 0;
                else highlightedRow_ = std::clamp(highlightedRow_ + delta, 0, static_cast<int>(items_.size()) - 1);
                list_.scrollToEnsureRowIsOnscreen(highlightedRow_);
                list_.repaint();
            }

            std::string getHighlightedItem() const {
                if (highlightedRow_ >= 0 && highlightedRow_ < static_cast<int>(items_.size())) {
                    return items_[static_cast<size_t>(highlightedRow_)];
                }
                return {};
            }

            int getDesiredHeight() const {
                int rows = std::max(1, std::min(7, static_cast<int>(items_.size())));
                return rows * 24 + 4;
            }

            int getNumRows() override {
                return items_.empty() ? 1 : static_cast<int>(items_.size());
            }

            void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool) override {
                if (items_.empty()) {
                    g.setFont(juce::Font(11.5f, juce::Font::italic));
                    g.setColour(AppTheme::textMuted);
                    g.drawText("No matching sections", 8, 0, width - 16, height, juce::Justification::centredLeft, true);
                    return;
                }
                if (rowNumber < 0 || rowNumber >= static_cast<int>(items_.size())) return;

                const bool isHighlighted = (rowNumber == highlightedRow_);
                if (isHighlighted) {
                    g.setColour(AppTheme::primaryBlue.withAlpha(0.35f));
                    g.fillRect(0, 0, width, height);
                }

                g.setFont(juce::Font(12.0f, isHighlighted ? juce::Font::bold : juce::Font::plain));
                g.setColour(isHighlighted ? juce::Colours::white : AppTheme::textLight);
                g.drawText(items_[static_cast<size_t>(rowNumber)], 8, 0, width - 16, height,
                           juce::Justification::centredLeft, true);
            }

            void listBoxItemClicked(int row, const juce::MouseEvent&) override {
                if (row >= 0 && row < static_cast<int>(items_.size())) {
                    owner_.commitDropdownSelection(items_[static_cast<size_t>(row)]);
                }
            }

            void paint(juce::Graphics& g) override {
                auto r = getLocalBounds().toFloat().reduced(0.5f);
                g.setColour(AppTheme::bgPanel.darker(0.15f));
                g.fillRoundedRectangle(r, 4.0f);
                g.setColour(AppTheme::primaryBlue.withAlpha(0.75f));
                g.drawRoundedRectangle(r, 4.0f, 1.2f);
            }

            void resized() override {
                list_.setBounds(getLocalBounds().reduced(2));
            }

        private:
            AddSectionsDialogContent& owner_;
            juce::ListBox list_;
            std::vector<std::string> items_;
            int highlightedRow_ = -1;
        };

        class RowsContainer : public juce::Component {
        public:
            explicit RowsContainer(AddSectionsDialogContent& owner) : owner_(owner) {
                filterEditor_.setJustification(juce::Justification::centredLeft);
                filterEditor_.setTextToShowWhenEmpty("Type or select section...", AppTheme::textMuted.withAlpha(0.6f));
                filterEditor_.onTextChange = [this]() {
                    owner_.onFilterTextChanged();
                };
                filterEditor_.onFocusLost = [this]() {
                    owner_.hideDropdown();
                };
                filterEditor_.onMouseClick = [this]() {
                    owner_.openDropdownForCurrentText(false);
                };
                filterEditor_.onKeyInterceptor = [this](const juce::KeyPress& key) -> bool {
                    if (key == juce::KeyPress::downKey) {
                        if (!owner_.isDropdownVisible()) {
                            owner_.openDropdownForCurrentText(false);
                        } else {
                            owner_.moveDropdownHighlight(1);
                        }
                        return true;
                    }
                    if (key == juce::KeyPress::upKey) {
                        if (owner_.isDropdownVisible()) {
                            owner_.moveDropdownHighlight(-1);
                            return true;
                        }
                    }
                    if (key == juce::KeyPress::escapeKey) {
                        if (owner_.isDropdownVisible()) {
                            owner_.hideDropdown();
                            return true;
                        }
                    }
                    if (key == juce::KeyPress::returnKey) {
                        if (owner_.isDropdownVisible()) {
                            std::string picked = owner_.getDropdownHighlightedItem();
                            if (!picked.empty()) {
                                owner_.commitDropdownSelection(picked);
                                return true;
                            }
                        }
                        owner_.addSectionFromInputs();
                        return true;
                    }
                    return false;
                };
                addAndMakeVisible(filterEditor_);

                dropdownBtn_.onClick = [this]() {
                    if (owner_.isDropdownVisible()) {
                        owner_.hideDropdown();
                    } else {
                        filterEditor_.grabKeyboardFocus();
                        owner_.openDropdownForCurrentText(true);
                    }
                };
                addAndMakeVisible(dropdownBtn_);

                barsEditor_.setJustification(juce::Justification::centred);
                barsEditor_.setInputRestrictions(4, "0123456789");
                barsEditor_.setText("4", false);
                barsEditor_.onReturnKey = [this]() {
                    owner_.addSectionFromInputs();
                };
                addAndMakeVisible(barsEditor_);

                barsUnitLabel_.setFont(juce::Font(11.0f, juce::Font::plain));
                barsUnitLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
                barsUnitLabel_.setJustificationType(juce::Justification::centredLeft);
                addAndMakeVisible(barsUnitLabel_);

                addBtn_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
                addBtn_.onClick = [this]() {
                    owner_.addSectionFromInputs();
                };
                addAndMakeVisible(addBtn_);
            }

            std::string getFilterText() const { return filterEditor_.getText().trim().toStdString(); }
            void setFilterText(const std::string& text) { filterEditor_.setText(text, false); }
            int getBarsValue() const { return barsEditor_.getText().trim().getIntValue(); }

            void focusFilterEditor() {
                filterEditor_.grabKeyboardFocus();
            }

            void focusBarsEditorAndSelectAll() {
                barsEditor_.grabKeyboardFocus();
                barsEditor_.selectAll();
            }

            juce::Rectangle<int> getDropdownAnchorBounds() const {
                return filterEditor_.getBounds().getUnion(dropdownBtn_.getBounds());
            }

            void layoutInlineRow() {
                const int n = static_cast<int>(owner_.sections_.size());
                const int y = n * kRowHeight;
                auto row = juce::Rectangle<int>(0, y, getWidth(), kRowHeight).reduced(6, 4);

                row.removeFromLeft(28); // Room for row number "N."
                auto addBounds = row.removeFromRight(58);
                row.removeFromRight(6);
                auto unitBounds = row.removeFromRight(32);
                row.removeFromRight(4);
                auto barsBounds = row.removeFromRight(52);
                row.removeFromRight(10);

                auto dropBtnBounds = row.removeFromRight(24);
                row.removeFromRight(2);
                auto filterBounds = row;

                filterEditor_.setBounds(filterBounds);
                dropdownBtn_.setBounds(dropBtnBounds);
                barsEditor_.setBounds(barsBounds);
                barsUnitLabel_.setBounds(unitBounds);
                addBtn_.setBounds(addBounds);
            }

            void paint(juce::Graphics& g) override {
                const int w = getWidth();
                int startBar = 1;

                for (int i = 0; i < static_cast<int>(owner_.sections_.size()); ++i) {
                    const int y = i * kRowHeight;
                    const auto& sec = owner_.sections_[static_cast<size_t>(i)];
                    const int len = std::max(1, sec.lengthBars);
                    const int endBar = startBar + len - 1;

                    if (i == owner_.selectedRow_) {
                        g.setColour(AppTheme::primaryBlue.withAlpha(0.25f));
                        g.fillRect(0, y, w, kRowHeight);
                    } else if (i % 2 == 1) {
                        g.setColour(AppTheme::bgDark.withAlpha(0.28f));
                        g.fillRect(0, y, w, kRowHeight);
                    }

                    g.setFont(juce::Font(12.0f, juce::Font::bold));
                    g.setColour(AppTheme::primaryBlue);
                    g.drawText(juce::String(i + 1) + ".", 8, y, 26, kRowHeight, juce::Justification::centredLeft);

                    g.setFont(juce::Font(12.5f, juce::Font::bold));
                    g.setColour(sec.isCountSection() ? AppTheme::warningAmber : AppTheme::textLight);
                    g.drawText(sec.name, 34, y, w - 210, kRowHeight, juce::Justification::centredLeft, true);

                    juce::String barInfo = juce::String(len) + (len == 1 ? " bar" : " bars") +
                                           "  (Bars " + juce::String(startBar) + "-" + juce::String(endBar) + ")";
                    g.setFont(juce::Font(11.5f, juce::Font::plain));
                    g.setColour(AppTheme::textMuted);
                    g.drawText(barInfo, w - 180, y, 170, kRowHeight, juce::Justification::centredRight, true);

                    g.setColour(AppTheme::borderCol.withAlpha(0.55f));
                    g.drawHorizontalLine(y + kRowHeight - 1, 0.0f, static_cast<float>(w));

                    startBar += len;
                }

                // Inline "next available row" highlight & index number
                const int nextIdx = static_cast<int>(owner_.sections_.size());
                const int nextY = nextIdx * kRowHeight;
                g.setColour(AppTheme::primaryBlue.withAlpha(0.08f));
                g.fillRect(0, nextY, w, kRowHeight);
                g.setFont(juce::Font(12.0f, juce::Font::bold));
                g.setColour(AppTheme::primaryBlue.brighter(0.2f));
                g.drawText(juce::String(nextIdx + 1) + ".", 8, nextY, 26, kRowHeight, juce::Justification::centredLeft);
                g.setColour(AppTheme::borderCol.withAlpha(0.45f));
                g.drawHorizontalLine(nextY + kRowHeight - 1, 0.0f, static_cast<float>(w));
            }

            void mouseDown(const juce::MouseEvent& e) override {
                owner_.hideDropdown();
                int clickedRow = e.y / kRowHeight;
                if (clickedRow >= 0 && clickedRow < static_cast<int>(owner_.sections_.size())) {
                    owner_.selectedRow_ = clickedRow;
                    repaint();
                } else {
                    owner_.selectedRow_ = -1;
                    repaint();
                }
            }

            void resized() override {
                layoutInlineRow();
            }

        private:
            AddSectionsDialogContent& owner_;
            FilterTextEditor filterEditor_;
            DropdownToggleButton dropdownBtn_;
            juce::TextEditor barsEditor_;
            juce::Label barsUnitLabel_{{}, "bars"};
            juce::TextButton addBtn_{"Add"};
        };

        std::vector<SongSection> sections_;
        std::function<void(std::vector<SongSection>)> onConfirm_;
        int selectedRow_ = -1;
        juce::Rectangle<int> listFrameBounds_;

        juce::Label colNumLabel_{{}, "#"};
        juce::Label colNameLabel_{{}, "Section Name"};
        juce::Label colBarsLabel_{{}, "Length"};

        SectionListViewport viewport_;
        std::unique_ptr<RowsContainer> rowsContent_;
        std::unique_ptr<DropdownOverlay> dropdownOverlay_;

        juce::Label errorLabel_;
        juce::TextButton moveUpButton_{"Move Up"};
        juce::TextButton moveDownButton_{"Move Down"};
        juce::TextButton removeButton_{"Remove"};

        juce::Label summaryLabel_;
        juce::TextButton cancelButton_{"Cancel"};
        juce::TextButton doneButton_{"Done"};

        bool isDropdownVisible() const {
            return dropdownOverlay_ && dropdownOverlay_->isVisible();
        }

        void hideDropdown() {
            if (dropdownOverlay_) {
                dropdownOverlay_->setVisible(false);
            }
        }

        void moveDropdownHighlight(int delta) {
            if (dropdownOverlay_) dropdownOverlay_->moveHighlight(delta);
        }

        std::string getDropdownHighlightedItem() const {
            return dropdownOverlay_ ? dropdownOverlay_->getHighlightedItem() : std::string{};
        }

        void openDropdownForCurrentText(bool showAllIfExactMatch) {
            if (!rowsContent_ || !dropdownOverlay_) return;
            std::string raw = rowsContent_->getFilterText();
            std::string exact = findCanonicalSectionName(raw);
            if (showAllIfExactMatch && !exact.empty()) {
                dropdownOverlay_->setItems(getAvailableSections(), exact);
            } else {
                dropdownOverlay_->setItems(filterAvailableSections(raw), exact);
            }
            positionDropdown();
            dropdownOverlay_->setVisible(true);
            dropdownOverlay_->toFront(false);
        }

        void onFilterTextChanged() {
            if (!rowsContent_ || !dropdownOverlay_) return;
            errorLabel_.setText("", juce::dontSendNotification);
            std::string raw = rowsContent_->getFilterText();
            dropdownOverlay_->setItems(filterAvailableSections(raw), findCanonicalSectionName(raw));
            positionDropdown();
            dropdownOverlay_->setVisible(true);
            dropdownOverlay_->toFront(false);
        }

        void commitDropdownSelection(const std::string& canonicalName) {
            if (!rowsContent_) return;
            rowsContent_->setFilterText(canonicalName);
            errorLabel_.setText("", juce::dontSendNotification);
            hideDropdown();
            rowsContent_->focusBarsEditorAndSelectAll();
        }

        void positionDropdown() {
            if (!rowsContent_ || !dropdownOverlay_) return;
            auto anchorInRows = rowsContent_->getDropdownAnchorBounds();
            auto anchorInDialog = getLocalArea(rowsContent_.get(), anchorInRows);
            const int desiredH = dropdownOverlay_->getDesiredHeight();
            const int spaceBelow = getHeight() - anchorInDialog.getBottom() - 8;
            const int spaceAbove = anchorInDialog.getY() - 8;

            int y = anchorInDialog.getBottom() + 2;
            int h = desiredH;
            if (spaceBelow < desiredH && spaceAbove > spaceBelow) {
                h = std::min(desiredH, std::max(60, spaceAbove - 2));
                y = anchorInDialog.getY() - h - 2;
            } else {
                h = std::min(desiredH, std::max(60, spaceBelow));
            }
            dropdownOverlay_->setBounds(anchorInDialog.getX(), y, anchorInDialog.getWidth(), h);
        }

        void refreshRowsLayout(bool scrollToBottom) {
            if (!rowsContent_) return;
            const int totalRows = static_cast<int>(sections_.size()) + 1; // Committed sections + inline next row
            const int neededH = std::max(viewport_.getHeight(), totalRows * kRowHeight);
            const int scrollBarW = (totalRows * kRowHeight > viewport_.getHeight()) ? viewport_.getScrollBarThickness() : 0;
            const int contentW = std::max(100, viewport_.getWidth() - scrollBarW);
            rowsContent_->setSize(contentW, neededH);
            rowsContent_->layoutInlineRow();
            rowsContent_->repaint();

            if (scrollToBottom) {
                int maxScrollY = std::max(0, totalRows * kRowHeight - viewport_.getHeight());
                viewport_.setViewPosition(0, maxScrollY);
            }
        }

        void addSectionFromInputs() {
            if (!rowsContent_) return;
            std::string rawName = rowsContent_->getFilterText();
            std::string secName = findCanonicalSectionName(rawName);
            int bars = rowsContent_->getBarsValue();

            if (secName.empty()) {
                errorLabel_.setText("Select a valid section from the filtered list.", juce::dontSendNotification);
                openDropdownForCurrentText(false);
                rowsContent_->focusFilterEditor();
                return;
            }
            if (bars <= 0) {
                errorLabel_.setText("Enter a section length of at least 1 bar.", juce::dontSendNotification);
                rowsContent_->focusBarsEditorAndSelectAll();
                return;
            }

            errorLabel_.setText("", juce::dontSendNotification);
            hideDropdown();

            SongSection sec;
            sec.name = secName;
            sec.lengthBars = bars;
            sec.orderIndex = static_cast<int>(sections_.size());
            sections_.push_back(std::move(sec));

            selectedRow_ = static_cast<int>(sections_.size()) - 1;
            rowsContent_->setFilterText("");
            refreshRowsLayout(true);
            updateSummary();
            rowsContent_->focusFilterEditor();
        }

        void reindexSections() {
            for (size_t i = 0; i < sections_.size(); ++i) {
                sections_[i].orderIndex = static_cast<int>(i);
            }
        }

        void updateSummary() {
            int totalBars = 0;
            for (const auto& s : sections_) {
                totalBars += std::max(1, s.lengthBars);
            }
            summaryLabel_.setText(juce::String(sections_.size()) +
                                      (sections_.size() == 1 ? " section" : " sections") +
                                      "  -  Total: " + juce::String(totalBars) +
                                      (totalBars == 1 ? " bar" : " bars"),
                                  juce::dontSendNotification);
        }

        void closeDialog() {
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
                dw->exitModalState(0);
            }
        }
    };

    const int targetArrIndex = selectedArrangementIndex_;
    const auto& initialSecs = pendingArrangements_[static_cast<size_t>(targetArrIndex)].sections;
    const juce::String arrTitle = pendingArrangements_[static_cast<size_t>(targetArrIndex)].name;

    juce::Component::SafePointer<LibraryView> safeView(this);
    auto* content = new AddSectionsDialogContent(
        initialSecs,
        [safeView, targetArrIndex](std::vector<SongSection> updatedSections) {
            if (safeView == nullptr) return;
            if (targetArrIndex < 0 || targetArrIndex >= static_cast<int>(safeView->pendingArrangements_.size())) return;

            auto& arr = safeView->pendingArrangements_[static_cast<size_t>(targetArrIndex)];
            arr.sections = std::move(updatedSections);

            if (safeView->currentSong_ && safeView->currentSong_->id.has_value() && arr.id.has_value()) {
                int64_t songId = *safeView->currentSong_->id;
                int64_t arrId = *arr.id;
                safeView->db_.saveArrangementSections(songId, arrId, arr.sections);
                for (auto& curArr : safeView->currentSong_->arrangements) {
                    if (curArr.id.has_value() && *curArr.id == arrId) {
                        curArr.sections = arr.sections;
                        break;
                    }
                }
                if (!safeView->currentSong_->arrangements.empty()) {
                    safeView->currentSong_->sections = safeView->currentSong_->arrangements.front().sections;
                }
                for (auto& s : safeView->songs_) {
                    if (s.id.has_value() && *s.id == songId) {
                        for (auto& sArr : s.arrangements) {
                            if (sArr.id.has_value() && *sArr.id == arrId) {
                                sArr.sections = arr.sections;
                                break;
                            }
                        }
                        if (!s.arrangements.empty()) s.sections = s.arrangements.front().sections;
                        break;
                    }
                }
                safeView->updateAttachmentStatusAndButtons();
            } else {
                safeView->updateAttachmentStatusAndButtons();
                safeView->markDirty(true);
            }
        });

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = arrTitle.isNotEmpty() ? ("Track Sections — " + arrTitle) : "Track Sections";
    options.content.setOwned(content);
    options.componentToCentreAround = getTopLevelComponent();
    options.dialogBackgroundColour = AppTheme::bgDark;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void LibraryView::rebuildTrackWidgets() {
    struct FileRowInfo {
        juce::String fileName;
        int numChannels = 0;
    };

    class TrackFileListComponent : public juce::Component {
    public:
        TrackFileListComponent(std::vector<FileRowInfo> rows, int rowHeight)
            : rows_(std::move(rows)), rowHeight_(rowHeight) {}

        void paint(juce::Graphics& g) override {
            g.fillAll(AppTheme::bgPanel);
            g.setColour(AppTheme::borderCol);
            g.drawRect(getLocalBounds(), 1);

            if (rows_.empty()) {
                g.setFont(juce::Font(12.0f, juce::Font::plain));
                g.setColour(AppTheme::textMuted);
                g.drawText("No audio files attached.", getLocalBounds().reduced(12, 8),
                           juce::Justification::topLeft, true);
                return;
            }

            const int badgeW = 64;
            const int badgeH = 18;
            for (size_t i = 0; i < rows_.size(); ++i) {
                int y = static_cast<int>(i) * rowHeight_;
                const auto& row = rows_[i];

                g.setFont(juce::Font(12.5f, juce::Font::plain));
                g.setColour(AppTheme::textLight);
                g.drawText(row.fileName, 10, y, std::max(0, getWidth() - badgeW - 24), rowHeight_,
                           juce::Justification::centredLeft, true);

                if (row.numChannels > 0) {
                    juce::String badgeText = (row.numChannels == 1)
                                                 ? "Mono"
                                                 : (row.numChannels == 2 ? "Stereo"
                                                                         : (juce::String(row.numChannels) + "-ch"));
                    juce::Colour badgeCol = (row.numChannels == 2) ? AppTheme::primaryBlue : AppTheme::textMuted;

                    juce::Rectangle<float> badgeRect(static_cast<float>(getWidth() - badgeW - 10),
                                                     static_cast<float>(y) + static_cast<float>(rowHeight_ - badgeH) * 0.5f,
                                                     static_cast<float>(badgeW),
                                                     static_cast<float>(badgeH));
                    g.setColour(badgeCol.withAlpha(0.18f));
                    g.fillRoundedRectangle(badgeRect, 4.0f);
                    g.setColour(badgeCol.withAlpha(0.65f));
                    g.drawRoundedRectangle(badgeRect, 4.0f, 1.0f);

                    g.setFont(juce::Font(10.5f, juce::Font::bold));
                    g.setColour(row.numChannels == 2 ? AppTheme::primaryBlue.brighter(0.35f) : AppTheme::textLight);
                    g.drawText(badgeText, badgeRect, juce::Justification::centred, false);
                }

                g.setColour(AppTheme::borderCol.withAlpha(0.6f));
                g.drawHorizontalLine(y + rowHeight_ - 1, 1.0f, static_cast<float>(getWidth() - 1));
            }
        }

    private:
        std::vector<FileRowInfo> rows_;
        int rowHeight_ = 28;
    };

    std::vector<FileRowInfo> rows;
    if (selectedArrangementIndex_ >= 0 &&
        selectedArrangementIndex_ < static_cast<int>(pendingArrangements_.size())) {
        auto& activeTracks = pendingArrangements_[static_cast<size_t>(selectedArrangementIndex_)].tracks;
        rows.reserve(activeTracks.size());
        for (auto& track : activeTracks) {
            FileRowInfo info;
            if (!track.filePath.empty()) {
                info.fileName = juce::File(track.filePath).getFileName();
                if (track.numChannels <= 0) {
                    track.numChannels = AudioAttachmentService::getAudioChannelCount(track.filePath);
                }
            } else {
                info.fileName = track.name;
            }
            info.numChannels = track.numChannels;
            rows.push_back(std::move(info));
        }
    }

    int rowH = 28;
    int totalH = static_cast<int>(rows.size()) * rowH;
    tracksContainer_ = std::make_unique<TrackFileListComponent>(std::move(rows), rowH);
    int w = std::max(100, tracksViewport_.getWidth() - 16);
    tracksContainer_->setSize(w, std::max(totalH, 80));
    tracksViewport_.setViewedComponent(tracksContainer_.get(), false);
}

void LibraryView::saveCurrentSong() {
    Song s;
    const bool isNewSong = !(currentSong_ && currentSong_->id.has_value());
    const std::string previousTitle = currentSong_ ? currentSong_->title : "";
    std::optional<Song> persistedSong;
    if (currentSong_ && currentSong_->id.has_value()) {
        s.id = currentSong_->id;
        persistedSong = db_.loadSong(*currentSong_->id);
        if (persistedSong.has_value()) {
            s.clickEighths = persistedSong->clickEighths;
            s.clickSixteenths = persistedSong->clickSixteenths;
            s.clickEighthsGain = persistedSong->clickEighthsGain;
            s.clickSixteenthsGain = persistedSong->clickSixteenthsGain;
        } else {
            s.clickEighths = currentSong_->clickEighths;
            s.clickSixteenths = currentSong_->clickSixteenths;
            s.clickEighthsGain = currentSong_->clickEighthsGain;
            s.clickSixteenthsGain = currentSong_->clickSixteenthsGain;
        }
    }

    s.title = titleEditor_.getText().trim().toStdString();
    if (s.title.empty()) s.title = "Untitled Song";

    s.artist = artistEditor_.getText().trim().toStdString();
    s.tempo = tempoEditor_.getText().getIntValue();
    if (s.tempo <= 0) s.tempo = 120;

    s.key = keyBox_.getText().toStdString();
    s.timeSignature = timeSigBox_.getText().toStdString();
    s.notes = notesEditor_.getText().toStdString();

    // Preserve per-arrangement click subdivisions remembered from sets
    s.arrangements = pendingArrangements_;
    if (persistedSong.has_value()) {
        for (auto& arr : s.arrangements) {
            if (!arr.id.has_value()) continue;
            for (const auto& pArr : persistedSong->arrangements) {
                if (pArr.id.has_value() && *pArr.id == *arr.id) {
                    arr.clickEighths = pArr.clickEighths;
                    arr.clickSixteenths = pArr.clickSixteenths;
                    arr.clickEighthsGain = pArr.clickEighthsGain;
                    arr.clickSixteenthsGain = pArr.clickSixteenthsGain;
                    break;
                }
            }
        }
    }

    const bool titleChanged = (!isNewSong && s.title != previousTitle);
    const bool audioFilesChanged = isNewSong || tracksModified_ || titleChanged;
    const int previouslySelectedArrIdx = selectedArrangementIndex_;

    // 1. Save to DB first to acquire song ID and any new arrangement IDs
    Song saved = db_.saveSong(s);

    // 2. Copy/migrate audio files per arrangement when tracks or song folder name changed
    if (saved.id.has_value() && audioFilesChanged) {
        for (int64_t removedId : removedArrangementIds_) {
            AudioAttachmentService::removeArrangementStorage(*saved.id, removedId);
        }
        bool updatedPaths = false;
        for (auto& arr : saved.arrangements) {
            if (arr.id.has_value() && !arr.tracks.empty()) {
                arr.tracks = AudioAttachmentService::copyTracksToArrangementDirectory(
                    *saved.id, saved.title, *arr.id, arr.name, arr.tracks);
                updatedPaths = true;
            }
        }
        if (saved.arrangements.empty()) {
            AudioAttachmentService::removeSongStorage(*saved.id);
        } else if (titleChanged) {
            AudioAttachmentService::cleanupStaleSongDirs(*saved.id, saved.title);
        }
        if (updatedPaths) {
            saved = db_.saveSong(saved);
        }
        audioEngine_.invalidateCachedSong(*saved.id);
    }

    currentSong_ = saved;
    removedArrangementIds_.clear();
    tracksModified_ = false;
    loadSongToEditor(saved);
    if (previouslySelectedArrIdx >= 0 && previouslySelectedArrIdx < static_cast<int>(pendingArrangements_.size())) {
        selectedArrangementIndex_ = previouslySelectedArrIdx;
        refreshArrangementPicker();
        updateAttachmentStatusAndButtons();
        rebuildTrackWidgets();
    }
    refreshSongList();
    markDirty(false);
}

void LibraryView::paint(juce::Graphics& g) {
    g.fillAll(AppTheme::bgDark);

    // Divider line between song list and editor
    g.setColour(AppTheme::borderCol);
    g.drawVerticalLine(320, 0.0f, static_cast<float>(getHeight()));
}

void LibraryView::resized() {
    auto area = getLocalBounds().reduced(16);

    // Left column: Songs list
    auto leftCol = area.removeFromLeft(290);
    auto leftButtons = leftCol.removeFromBottom(36);
    newSongButton_.setBounds(leftButtons.removeFromLeft(140));
    deleteSongButton_.setBounds(leftButtons.removeFromRight(120));

    leftCol.removeFromBottom(12);
    songListBox_.setBounds(leftCol);

    area.removeFromLeft(24);

    // Right column: Editor
    auto rightCol = area;
    headerLabel_.setBounds(rightCol.removeFromTop(28));
    rightCol.removeFromTop(12);

    int rowH = 26;
    int gap = 8;

    auto row1 = rightCol.removeFromTop(rowH);
    titleLabel_.setBounds(row1.removeFromLeft(100));
    titleEditor_.setBounds(row1);

    rightCol.removeFromTop(gap);
    auto row2 = rightCol.removeFromTop(rowH);
    artistLabel_.setBounds(row2.removeFromLeft(100));
    artistEditor_.setBounds(row2);

    rightCol.removeFromTop(gap);
    auto row3 = rightCol.removeFromTop(rowH);
    tempoLabel_.setBounds(row3.removeFromLeft(100));
    tempoEditor_.setBounds(row3.removeFromLeft(80));

    row3.removeFromLeft(16);
    keyLabel_.setBounds(row3.removeFromLeft(40));
    keyBox_.setBounds(row3.removeFromLeft(80));

    row3.removeFromLeft(16);
    timeSigLabel_.setBounds(row3.removeFromLeft(110));
    timeSigBox_.setBounds(row3.removeFromLeft(80));

    rightCol.removeFromTop(gap);
    auto row4 = rightCol.removeFromTop(54);
    notesLabel_.setBounds(row4.removeFromLeft(100));
    notesEditor_.setBounds(row4);

    rightCol.removeFromTop(14);
    auto attachRow1 = rightCol.removeFromTop(28);
    attachmentHeader_.setBounds(attachRow1.removeFromLeft(110));
    arrangementPicker_.setBounds(attachRow1);

    rightCol.removeFromTop(6);
    auto attachRow2 = rightCol.removeFromTop(28);
    attachFolderButton_.setBounds(attachRow2.removeFromLeft(132));
    attachRow2.removeFromLeft(6);
    editArrangementButton_.setBounds(attachRow2.removeFromLeft(132));
    attachRow2.removeFromLeft(6);
    addSectionsButton_.setBounds(attachRow2.removeFromLeft(118));
    attachRow2.removeFromLeft(6);
    removeArrangementButton_.setBounds(attachRow2.removeFromLeft(80));

    rightCol.removeFromTop(4);
    attachmentStatusLabel_.setBounds(rightCol.removeFromTop(20));

    rightCol.removeFromTop(8);

    auto bottomButtons = rightCol.removeFromBottom(36);
    saveButton_.setBounds(bottomButtons.removeFromLeft(100));
    bottomButtons.removeFromLeft(12);
    cancelButton_.setBounds(bottomButtons.removeFromLeft(100));

    rightCol.removeFromBottom(12);
    tracksViewport_.setBounds(rightCol);
    if (tracksContainer_) {
        tracksContainer_->setSize(tracksViewport_.getWidth() - 20, tracksContainer_->getHeight());
    }
}

} // namespace tracks
