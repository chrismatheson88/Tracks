#include "LibraryView.h"
#include "../../Common/AppTheme.h"
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

    const char* keys[] = { "C", "C#", "Db", "D", "Eb", "E", "F", "F#", "Gb", "G", "Ab", "A", "Bb", "B",
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

    // Attachment header & button
    attachmentHeader_.setFont(juce::Font(14.0f, juce::Font::bold));
    attachmentHeader_.setColour(juce::Label::textColourId, AppTheme::textLight);
    addAndMakeVisible(attachmentHeader_);

    attachFolderButton_.onClick = [this]() { handleChooseFolder(); };
    addAndMakeVisible(attachFolderButton_);

    addSectionsButton_.setEnabled(false);
    addSectionsButton_.onClick = [this]() { handleAddSections(); };
    addAndMakeVisible(addSectionsButton_);

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

    pendingTracks_ = song.tracks;
    pendingSections_ = song.sections;
    tracksModified_ = false;
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
    pendingTracks_.clear();
    pendingSections_.clear();
    tracksModified_ = false;
    updateAttachmentStatusAndButtons();
    rebuildTrackWidgets();
    markDirty(false);
}

void LibraryView::updateAttachmentStatusAndButtons() {
    const bool hasTracks = !pendingTracks_.empty();
    addSectionsButton_.setEnabled(hasTracks);

    if (!hasTracks) {
        attachmentStatusLabel_.setText("", juce::dontSendNotification);
        return;
    }

    juce::String status = juce::String(pendingTracks_.size()) + " audio file(s) attached";
    if (!pendingTracks_[0].trackKey.empty()) {
        status += " (Tracks Key: " + juce::String(pendingTracks_[0].trackKey) + ")";
    }
    if (!pendingSections_.empty()) {
        int totalBars = 0;
        for (const auto& sec : pendingSections_) {
            totalBars += std::max(1, sec.lengthBars);
        }
        status += "  |  " + juce::String(pendingSections_.size()) +
                  (pendingSections_.size() == 1 ? " section (" : " sections (") +
                  juce::String(totalBars) + " bars)";
    }
    attachmentStatusLabel_.setText(status, juce::dontSendNotification);
}

void LibraryView::markDirty(bool dirty) {
    isDirty_ = dirty;
    saveButton_.setColour(juce::TextButton::buttonColourId,
                          isDirty_ ? AppTheme::primaryBlue : AppTheme::bgPanel);
}

void LibraryView::handleChooseFolder() {
    class AddTracksDialogContent : public juce::Component {
    public:
        AddTracksDialogContent(const juce::String& initialKey,
                               std::function<void(std::vector<SongTrack>, const std::string&, const juce::String&)> onConfirm)
            : onConfirm_(std::move(onConfirm)) {
            setSize(460, 196);

            dirLabel_.setFont(juce::Font(12.0f, juce::Font::bold));
            dirLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
            addAndMakeVisible(dirLabel_);

            dirEditor_.setJustification(juce::Justification::centredLeft);
            dirEditor_.setTextToShowWhenEmpty("Select a folder with WAV, MP3, or M4A files...", AppTheme::textMuted.withAlpha(0.6f));
            dirEditor_.onTextChange = [this]() {
                addButton_.setEnabled(dirEditor_.getText().trim().isNotEmpty());
            };
            addAndMakeVisible(dirEditor_);

            browseButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
            browseButton_.onClick = [this]() { openFolderChooser(); };
            addAndMakeVisible(browseButton_);

            keyLabel_.setFont(juce::Font(12.0f, juce::Font::bold));
            keyLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
            addAndMakeVisible(keyLabel_);

            const char* keys[] = { "C", "C#", "Db", "D", "Eb", "E", "F", "F#", "Gb", "G", "Ab", "A", "Bb", "B",
                                   "Am", "Bbm", "Bm", "Cm", "C#m", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "Abm" };
            int kid = 1;
            for (const char* k : keys) {
                keyBox_.addItem(k, kid++);
            }
            if (initialKey.isNotEmpty()) {
                keyBox_.setText(initialKey, juce::dontSendNotification);
            } else {
                keyBox_.setSelectedId(1, juce::dontSendNotification);
            }
            addAndMakeVisible(keyBox_);

            statusLabel_.setFont(juce::Font(11.5f, juce::Font::plain));
            statusLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
            addAndMakeVisible(statusLabel_);

            cancelButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
            cancelButton_.onClick = [this]() { closeDialog(); };
            addAndMakeVisible(cancelButton_);

            addButton_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
            addButton_.setEnabled(false);
            addButton_.onClick = [this]() { confirmSelection(); };
            addAndMakeVisible(addButton_);
        }

        void paint(juce::Graphics& g) override {
            g.fillAll(AppTheme::bgDark);
        }

        void resized() override {
            auto area = getLocalBounds().reduced(16);

            auto row1 = area.removeFromTop(28);
            dirLabel_.setBounds(row1.removeFromLeft(88));
            browseButton_.setBounds(row1.removeFromRight(84));
            row1.removeFromRight(8);
            dirEditor_.setBounds(row1);

            area.removeFromTop(14);

            auto row2 = area.removeFromTop(28);
            keyLabel_.setBounds(row2.removeFromLeft(88));
            keyBox_.setBounds(row2.removeFromLeft(110));

            area.removeFromTop(12);
            statusLabel_.setBounds(area.removeFromTop(32));

            auto bottomRow = area.removeFromBottom(32);
            addButton_.setBounds(bottomRow.removeFromRight(106));
            bottomRow.removeFromRight(8);
            cancelButton_.setBounds(bottomRow.removeFromRight(84));
        }

    private:
        juce::Label dirLabel_{{}, "Directory:"};
        juce::TextEditor dirEditor_;
        juce::TextButton browseButton_{"Browse..."};

        juce::Label keyLabel_{{}, "Tracks Key:"};
        juce::ComboBox keyBox_;

        juce::Label statusLabel_;
        juce::TextButton cancelButton_{"Cancel"};
        juce::TextButton addButton_{"Add Tracks"};

        std::unique_ptr<juce::FileChooser> chooser_;
        std::function<void(std::vector<SongTrack>, const std::string&, const juce::String&)> onConfirm_;

        void openFolderChooser() {
            auto startDir = dirEditor_.getText().trim().isNotEmpty()
                                ? juce::File(dirEditor_.getText().trim())
                                : juce::File::getSpecialLocation(juce::File::userHomeDirectory);
            if (!startDir.isDirectory()) {
                startDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
            }

            chooser_ = std::make_unique<juce::FileChooser>("Select Audio Folder with Stems", startDir);
            auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;

            juce::Component::SafePointer<AddTracksDialogContent> safeThis(this);
            chooser_->launchAsync(flags, [safeThis](const juce::FileChooser& fc) {
                if (safeThis == nullptr) return;
                auto result = fc.getResult();
                if (!result.isDirectory()) return;

                safeThis->dirEditor_.setText(result.getFullPathName(), true);
                auto insp = AudioAttachmentService::inspectAudioDirectory(result.getFullPathName().toStdString());
                if (insp.error.has_value()) {
                    safeThis->statusLabel_.setColour(juce::Label::textColourId, AppTheme::dangerRed);
                    safeThis->statusLabel_.setText(*insp.error, juce::dontSendNotification);
                    safeThis->addButton_.setEnabled(false);
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
                    safeThis->addButton_.setEnabled(true);
                }
            });
        }

        void confirmSelection() {
            auto dirPath = dirEditor_.getText().trim().toStdString();
            auto insp = AudioAttachmentService::inspectAudioDirectory(dirPath);
            if (insp.error.has_value()) {
                statusLabel_.setColour(juce::Label::textColourId, AppTheme::dangerRed);
                statusLabel_.setText(*insp.error, juce::dontSendNotification);
                return;
            }

            std::string selectedKey = keyBox_.getText().toStdString();
            for (auto& track : insp.tracks) {
                track.trackKey = selectedKey;
            }

            juce::String status = juce::String(insp.tracks.size()) + " audio file(s) found";
            if (!selectedKey.empty()) {
                status += " (Tracks Key: " + juce::String(selectedKey) + ")";
            }
            if (!insp.mismatches.empty()) {
                status += " — " + juce::String(insp.mismatches[0]);
            }

            if (onConfirm_) {
                onConfirm_(std::move(insp.tracks), selectedKey, status);
            }
            closeDialog();
        }

        void closeDialog() {
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
                dw->exitModalState(0);
            }
        }
    };

    juce::String initialKey = keyBox_.getText();
    if (!pendingTracks_.empty() && !pendingTracks_[0].trackKey.empty()) {
        initialKey = pendingTracks_[0].trackKey;
    }

    juce::Component::SafePointer<LibraryView> safeView(this);
    auto* content = new AddTracksDialogContent(
        initialKey,
        [safeView](std::vector<SongTrack> tracks, const std::string& /*trackKey*/, const juce::String& status) {
            if (safeView == nullptr) return;
            safeView->pendingTracks_ = std::move(tracks);
            safeView->tracksModified_ = true;
            safeView->addSectionsButton_.setEnabled(!safeView->pendingTracks_.empty());
            safeView->attachmentStatusLabel_.setText(status, juce::dontSendNotification);
            safeView->rebuildTrackWidgets();
            safeView->markDirty(true);
        });

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Add Tracks";
    options.content.setOwned(content);
    options.componentToCentreAround = getTopLevelComponent();
    options.dialogBackgroundColour = AppTheme::bgDark;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void LibraryView::handleAddSections() {
    if (pendingTracks_.empty()) return;

    class AddSectionsDialogContent : public juce::Component, public juce::ListBoxModel {
    public:
        AddSectionsDialogContent(std::vector<SongSection> initialSections,
                                 std::function<void(std::vector<SongSection>)> onConfirm)
            : sections_(std::move(initialSections)), onConfirm_(std::move(onConfirm)) {
            setSize(480, 400);

            nameLabel_.setFont(juce::Font(11.5f, juce::Font::bold));
            nameLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
            addAndMakeVisible(nameLabel_);

            nameEditor_.setJustification(juce::Justification::centredLeft);
            nameEditor_.setTextToShowWhenEmpty("e.g. Intro, Verse 1, Chorus...", AppTheme::textMuted.withAlpha(0.6f));
            nameEditor_.onReturnKey = [this]() { addSectionFromInputs(); };
            addAndMakeVisible(nameEditor_);

            barsLabel_.setFont(juce::Font(11.5f, juce::Font::bold));
            barsLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
            addAndMakeVisible(barsLabel_);

            barsEditor_.setJustification(juce::Justification::centred);
            barsEditor_.setInputRestrictions(4, "0123456789");
            barsEditor_.setText("4", false);
            barsEditor_.onReturnKey = [this]() { addSectionFromInputs(); };
            addAndMakeVisible(barsEditor_);

            addSectionButton_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
            addSectionButton_.onClick = [this]() { addSectionFromInputs(); };
            addAndMakeVisible(addSectionButton_);

            errorLabel_.setFont(juce::Font(11.0f, juce::Font::plain));
            errorLabel_.setColour(juce::Label::textColourId, AppTheme::dangerRed);
            addAndMakeVisible(errorLabel_);

            listBox_.setModel(this);
            listBox_.setRowHeight(30);
            addAndMakeVisible(listBox_);

            moveUpButton_.onClick = [this]() {
                int sel = listBox_.getSelectedRow();
                if (sel <= 0 || sel >= static_cast<int>(sections_.size())) return;
                std::swap(sections_[static_cast<size_t>(sel)], sections_[static_cast<size_t>(sel - 1)]);
                reindexSections();
                listBox_.updateContent();
                listBox_.selectRow(sel - 1);
                updateSummary();
            };
            addAndMakeVisible(moveUpButton_);

            moveDownButton_.onClick = [this]() {
                int sel = listBox_.getSelectedRow();
                if (sel < 0 || sel >= static_cast<int>(sections_.size()) - 1) return;
                std::swap(sections_[static_cast<size_t>(sel)], sections_[static_cast<size_t>(sel + 1)]);
                reindexSections();
                listBox_.updateContent();
                listBox_.selectRow(sel + 1);
                updateSummary();
            };
            addAndMakeVisible(moveDownButton_);

            removeButton_.setColour(juce::TextButton::buttonColourId, AppTheme::dangerRed);
            removeButton_.onClick = [this]() {
                int sel = listBox_.getSelectedRow();
                if (sel < 0 || sel >= static_cast<int>(sections_.size())) return;
                sections_.erase(sections_.begin() + sel);
                reindexSections();
                listBox_.updateContent();
                listBox_.deselectAllRows();
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
                // If user typed a section name but clicked Done before clicking Add, include it too
                auto pendingName = nameEditor_.getText().trim().toStdString();
                int pendingBars = barsEditor_.getText().trim().getIntValue();
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

            updateSummary();
        }

        int getNumRows() override {
            return static_cast<int>(sections_.size());
        }

        void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override {
            if (rowNumber < 0 || rowNumber >= static_cast<int>(sections_.size())) return;

            if (rowIsSelected) {
                g.setColour(AppTheme::primaryBlue.withAlpha(0.25f));
                g.fillRect(0, 0, width, height);
            }

            int startBar = 1;
            for (int i = 0; i < rowNumber; ++i) {
                startBar += std::max(1, sections_[static_cast<size_t>(i)].lengthBars);
            }
            const auto& sec = sections_[static_cast<size_t>(rowNumber)];
            int len = std::max(1, sec.lengthBars);
            int endBar = startBar + len - 1;

            g.setFont(juce::Font(12.0f, juce::Font::bold));
            g.setColour(AppTheme::primaryBlue);
            g.drawText(juce::String(rowNumber + 1) + ".", 8, 0, 24, height, juce::Justification::centredLeft);

            g.setFont(juce::Font(12.5f, juce::Font::bold));
            g.setColour(AppTheme::textLight);
            g.drawText(sec.name, 34, 0, width - 210, height, juce::Justification::centredLeft, true);

            juce::String barInfo = juce::String(len) + (len == 1 ? " bar" : " bars") +
                                   "  (Bars " + juce::String(startBar) + "-" + juce::String(endBar) + ")";
            g.setFont(juce::Font(11.5f, juce::Font::plain));
            g.setColour(AppTheme::textMuted);
            g.drawText(barInfo, width - 180, 0, 170, height, juce::Justification::centredRight, true);

            g.setColour(AppTheme::borderCol.withAlpha(0.6f));
            g.drawHorizontalLine(height - 1, 0.0f, static_cast<float>(width));
        }

        void paint(juce::Graphics& g) override {
            g.fillAll(AppTheme::bgDark);
        }

        void resized() override {
            auto area = getLocalBounds().reduced(16);

            auto labelRow = area.removeFromTop(18);
            nameLabel_.setBounds(labelRow.removeFromLeft(260));
            labelRow.removeFromLeft(8);
            barsLabel_.setBounds(labelRow.removeFromLeft(80));

            area.removeFromTop(4);

            auto inputRow = area.removeFromTop(28);
            addSectionButton_.setBounds(inputRow.removeFromRight(80));
            inputRow.removeFromRight(8);
            barsEditor_.setBounds(inputRow.removeFromRight(68));
            inputRow.removeFromRight(8);
            nameEditor_.setBounds(inputRow);

            area.removeFromTop(4);
            errorLabel_.setBounds(area.removeFromTop(16));
            area.removeFromTop(6);

            auto bottomRow = area.removeFromBottom(32);
            doneButton_.setBounds(bottomRow.removeFromRight(92));
            bottomRow.removeFromRight(8);
            cancelButton_.setBounds(bottomRow.removeFromRight(82));
            summaryLabel_.setBounds(bottomRow);

            area.removeFromBottom(10);

            auto editRow = area.removeFromBottom(26);
            moveUpButton_.setBounds(editRow.removeFromLeft(82));
            editRow.removeFromLeft(6);
            moveDownButton_.setBounds(editRow.removeFromLeft(90));
            editRow.removeFromLeft(6);
            removeButton_.setBounds(editRow.removeFromLeft(78));

            area.removeFromBottom(10);
            listBox_.setBounds(area);
        }

    private:
        std::vector<SongSection> sections_;
        std::function<void(std::vector<SongSection>)> onConfirm_;

        juce::Label nameLabel_{{}, "Section Name"};
        juce::TextEditor nameEditor_;
        juce::Label barsLabel_{{}, "Length (Bars)"};
        juce::TextEditor barsEditor_;
        juce::TextButton addSectionButton_{"Add"};
        juce::Label errorLabel_;

        juce::ListBox listBox_;
        juce::TextButton moveUpButton_{"Move Up"};
        juce::TextButton moveDownButton_{"Move Down"};
        juce::TextButton removeButton_{"Remove"};

        juce::Label summaryLabel_;
        juce::TextButton cancelButton_{"Cancel"};
        juce::TextButton doneButton_{"Done"};

        void addSectionFromInputs() {
            auto secName = nameEditor_.getText().trim().toStdString();
            int bars = barsEditor_.getText().trim().getIntValue();

            if (secName.empty()) {
                errorLabel_.setText("Enter a section name (e.g. Intro, Verse 1, Chorus).", juce::dontSendNotification);
                nameEditor_.grabKeyboardFocus();
                return;
            }
            if (bars <= 0) {
                errorLabel_.setText("Enter a section length of at least 1 bar.", juce::dontSendNotification);
                barsEditor_.grabKeyboardFocus();
                return;
            }

            errorLabel_.setText("", juce::dontSendNotification);

            SongSection sec;
            sec.name = secName;
            sec.lengthBars = bars;
            sec.orderIndex = static_cast<int>(sections_.size());
            sections_.push_back(std::move(sec));

            listBox_.updateContent();
            listBox_.selectRow(static_cast<int>(sections_.size()) - 1);
            updateSummary();

            nameEditor_.setText("", false);
            nameEditor_.grabKeyboardFocus();
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

    juce::Component::SafePointer<LibraryView> safeView(this);
    auto* content = new AddSectionsDialogContent(
        pendingSections_,
        [safeView](std::vector<SongSection> updatedSections) {
            if (safeView == nullptr) return;
            safeView->pendingSections_ = std::move(updatedSections);

            if (safeView->currentSong_ && safeView->currentSong_->id.has_value()) {
                int64_t songId = *safeView->currentSong_->id;
                safeView->db_.saveSongSections(songId, safeView->pendingSections_);
                safeView->currentSong_->sections = safeView->pendingSections_;
                for (auto& s : safeView->songs_) {
                    if (s.id.has_value() && *s.id == songId) {
                        s.sections = safeView->pendingSections_;
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
    options.dialogTitle = "Add Sections";
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
    rows.reserve(pendingTracks_.size());
    for (auto& track : pendingTracks_) {
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
    if (currentSong_ && currentSong_->id.has_value()) {
        s.id = currentSong_->id;
        // Preserve per-song click subdivision settings managed from the transport/mixer
        if (auto persisted = db_.loadSong(*currentSong_->id); persisted.has_value()) {
            s.clickEighths = persisted->clickEighths;
            s.clickSixteenths = persisted->clickSixteenths;
            s.clickEighthsGain = persisted->clickEighthsGain;
            s.clickSixteenthsGain = persisted->clickSixteenthsGain;
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
    s.tracks = pendingTracks_;
    s.sections = pendingSections_;

    const bool titleChanged = (!isNewSong && s.title != previousTitle);
    const bool audioFilesChanged = isNewSong || tracksModified_ || titleChanged;

    // 1. Save to DB first to acquire ID if new
    Song saved = db_.saveSong(s);

    // 2. Copy/migrate pending audio files only when tracks or song folder name changed
    if (saved.id.has_value() && !pendingTracks_.empty() && audioFilesChanged) {
        saved.tracks = AudioAttachmentService::copyTracksToSongDirectory(*saved.id, saved.title, pendingTracks_);
        // Update database with canonical file paths
        saved = db_.saveSong(saved);
    }

    if (saved.id.has_value() && audioFilesChanged) {
        audioEngine_.invalidateCachedSong(*saved.id);
    }

    currentSong_ = saved;
    tracksModified_ = false;
    loadSongToEditor(saved);
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

    rightCol.removeFromTop(16);
    auto attachRow = rightCol.removeFromTop(32);
    attachmentHeader_.setBounds(attachRow.removeFromLeft(230));
    attachFolderButton_.setBounds(attachRow.removeFromLeft(110));
    attachRow.removeFromLeft(8);
    addSectionsButton_.setBounds(attachRow.removeFromLeft(118));

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
