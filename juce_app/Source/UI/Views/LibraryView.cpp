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
        if (onSongLoadedForPlayback) {
            onSongLoadedForPlayback(*currentSong_);
        }
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
    attachmentStatusLabel_.setText("", juce::dontSendNotification);
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
    attachmentStatusLabel_.setText("", juce::dontSendNotification);
    rebuildTrackWidgets();
    markDirty(false);
}

void LibraryView::markDirty(bool dirty) {
    isDirty_ = dirty;
    saveButton_.setColour(juce::TextButton::buttonColourId,
                          isDirty_ ? AppTheme::primaryBlue : AppTheme::bgPanel);
}

void LibraryView::handleChooseFolder() {
    fileChooser_ = std::make_unique<juce::FileChooser>("Select Audio Folder with Stems",
                                                        juce::File::getSpecialLocation(juce::File::userHomeDirectory));
    auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;

    fileChooser_->launchAsync(flags, [this](const juce::FileChooser& fc) {
        auto result = fc.getResult();
        if (result.isDirectory()) {
            auto insp = AudioAttachmentService::inspectAudioDirectory(result.getFullPathName().toStdString());
            if (insp.error.has_value()) {
                attachmentStatusLabel_.setText(*insp.error, juce::dontSendNotification);
                return;
            }

            pendingTracks_ = insp.tracks;
            juce::String status = juce::String(pendingTracks_.size()) + " audio file(s) found.";
            if (!insp.mismatches.empty()) {
                status += " (" + juce::String(insp.mismatches[0]) + ")";
            }
            attachmentStatusLabel_.setText(status, juce::dontSendNotification);
            rebuildTrackWidgets();
            markDirty(true);
        }
    });
}

void LibraryView::rebuildTrackWidgets() {
    tracksContainer_ = std::make_unique<juce::Component>();
    int rowH = 32;
    int totalH = static_cast<int>(pendingTracks_.size()) * rowH;
    tracksContainer_->setSize(tracksViewport_.getWidth() - 20, std::max(totalH, 80));

    for (size_t i = 0; i < pendingTracks_.size(); ++i) {
        auto* row = new juce::Component();
        row->setBounds(0, static_cast<int>(i) * rowH, tracksContainer_->getWidth(), rowH);

        auto* nameLabel = new juce::Label({}, pendingTracks_[i].name);
        nameLabel->setFont(juce::Font(12.0f, juce::Font::bold));
        nameLabel->setColour(juce::Label::textColourId, AppTheme::textLight);
        nameLabel->setBounds(0, 4, 180, 24);
        row->addAndMakeVisible(nameLabel);

        // Gain slider
        auto* gainSlider = new juce::Slider(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
        gainSlider->setRange(0.0, 2.0, 0.05);
        gainSlider->setValue(pendingTracks_[i].gain);
        gainSlider->setBounds(190, 4, 100, 24);
        gainSlider->onValueChange = [this, i, gainSlider]() {
            pendingTracks_[i].gain = static_cast<float>(gainSlider->getValue());
            markDirty(true);
        };
        row->addAndMakeVisible(gainSlider);

        // Output channel selector (1-based for user, 0-based internal)
        auto* chBox = new juce::ComboBox();
        for (int ch = 1; ch <= 16; ++ch) {
            chBox->addItem("Ch " + juce::String(ch), ch);
        }
        chBox->setSelectedId(pendingTracks_[i].outputChannel + 1, juce::dontSendNotification);
        chBox->setBounds(300, 4, 80, 24);
        chBox->onChange = [this, i, chBox]() {
            pendingTracks_[i].outputChannel = chBox->getSelectedId() - 1;
            markDirty(true);
        };
        row->addAndMakeVisible(chBox);

        // Mute button
        auto* muteBtn = new juce::TextButton(pendingTracks_[i].muted ? "MUTED" : "Mute");
        muteBtn->setColour(juce::TextButton::buttonColourId,
                           pendingTracks_[i].muted ? AppTheme::dangerRed : AppTheme::bgInput);
        muteBtn->setBounds(390, 4, 60, 24);
        muteBtn->onClick = [this, i, muteBtn]() {
            pendingTracks_[i].muted = !pendingTracks_[i].muted;
            muteBtn->setButtonText(pendingTracks_[i].muted ? "MUTED" : "Mute");
            muteBtn->setColour(juce::TextButton::buttonColourId,
                               pendingTracks_[i].muted ? AppTheme::dangerRed : AppTheme::bgInput);
            markDirty(true);
        };
        row->addAndMakeVisible(muteBtn);

        tracksContainer_->addAndMakeVisible(row);
    }

    tracksViewport_.setViewedComponent(tracksContainer_.get(), false);
}

void LibraryView::saveCurrentSong() {
    Song s;
    if (currentSong_ && currentSong_->id.has_value()) {
        s.id = currentSong_->id;
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

    // 1. Save to DB first to acquire ID if new
    Song saved = db_.saveSong(s);

    // 2. Copy pending audio files to canonical uploaded_tracks/<id>/tracks/
    if (saved.id.has_value() && !pendingTracks_.empty()) {
        saved.tracks = AudioAttachmentService::copyTracksToSongDirectory(*saved.id, pendingTracks_);
        // Update database with canonical file paths
        saved = db_.saveSong(saved);
    }

    currentSong_ = saved;
    loadSongToEditor(saved);
    refreshSongList();
    markDirty(false);

    if (onSongLoadedForPlayback) {
        onSongLoadedForPlayback(saved);
    }
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
    attachFolderButton_.setBounds(attachRow.removeFromLeft(200));

    rightCol.removeFromTop(4);
    attachmentStatusLabel_.setBounds(rightCol.removeFromTop(20));

    rightCol.removeFromTop(8);

    auto bottomButtons = rightCol.removeFromBottom(36);
    saveButton_.setBounds(bottomButtons.removeFromLeft(180));
    bottomButtons.removeFromLeft(12);
    cancelButton_.setBounds(bottomButtons.removeFromLeft(100));

    rightCol.removeFromBottom(12);
    tracksViewport_.setBounds(rightCol);
    if (tracksContainer_) {
        tracksContainer_->setSize(tracksViewport_.getWidth() - 20, tracksContainer_->getHeight());
    }
}

} // namespace tracks
