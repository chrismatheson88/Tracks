#include "OpenSetView.h"
#include "../../Common/AppTheme.h"
#include <algorithm>

namespace tracks {

OpenSetView::OpenSetView(DatabaseService& db, AudioEngine& audioEngine)
    : db_(db), audioEngine_(audioEngine) {
    setOpaque(true);

    // Top Set Picker
    setSelectLabel_.setFont(juce::Font(12.0f, juce::Font::bold));
    setSelectLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
    addAndMakeVisible(setSelectLabel_);

    setPicker_.onChange = [this]() {
        int idx = setPicker_.getSelectedItemIndex();
        if (idx >= 0 && idx < static_cast<int>(setlists_.size())) {
            currentSetlist_ = setlists_[static_cast<size_t>(idx)];
            loadSetlistToUI(*currentSetlist_);
        }
    };
    addAndMakeVisible(setPicker_);

    newSetButton_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
    newSetButton_.onClick = [this]() {
        Setlist s;
        s.name = "New Setlist";
        s.setDate = "";
        s.setTime = "";
        s.notes = "";
        Setlist saved = db_.saveSetlist(s);
        refreshSetlists();
        if (saved.id.has_value()) {
            selectSetlist(*saved.id);
        }
    };
    addAndMakeVisible(newSetButton_);

    deleteSetButton_.setColour(juce::TextButton::buttonColourId, AppTheme::dangerRed);
    deleteSetButton_.onClick = [this]() {
        if (currentSetlist_ && currentSetlist_->id.has_value()) {
            db_.deleteSetlist(*currentSetlist_->id);
            currentSetlist_ = std::nullopt;
            refreshSetlists();
        }
    };
    addAndMakeVisible(deleteSetButton_);

    // Metadata
    auto setupMeta = [this](juce::Label& lbl, juce::TextEditor& ed) {
        lbl.setFont(juce::Font(11.0f, juce::Font::bold));
        lbl.setColour(juce::Label::textColourId, AppTheme::textMuted);
        addAndMakeVisible(lbl);
        addAndMakeVisible(ed);
    };

    setupMeta(nameLabel_, nameEditor_);
    setupMeta(dateLabel_, dateEditor_);
    setupMeta(timeLabel_, timeEditor_);
    setupMeta(notesLabel_, notesEditor_);

    // Set entries listbox
    entryListBox_.setModel(this);
    entryListBox_.setRowHeight(36);
    addAndMakeVisible(entryListBox_);

    // Reorder & remove buttons
    moveUpButton_.onClick = [this]() {
        if (!currentSetlist_ || selectedEntryIndex_ <= 0) return;
        std::swap(currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_)],
                  currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_ - 1)]);
        selectedEntryIndex_--;
        for (size_t i = 0; i < currentSetlist_->entries.size(); ++i) {
            currentSetlist_->entries[i].orderIndex = static_cast<int>(i);
        }
        entryListBox_.updateContent();
        entryListBox_.selectRow(selectedEntryIndex_);
        saveCurrentSetlist();
    };
    addAndMakeVisible(moveUpButton_);

    moveDownButton_.onClick = [this]() {
        if (!currentSetlist_ || selectedEntryIndex_ < 0 ||
            selectedEntryIndex_ >= static_cast<int>(currentSetlist_->entries.size()) - 1) return;
        std::swap(currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_)],
                  currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_ + 1)]);
        selectedEntryIndex_++;
        for (size_t i = 0; i < currentSetlist_->entries.size(); ++i) {
            currentSetlist_->entries[i].orderIndex = static_cast<int>(i);
        }
        entryListBox_.updateContent();
        entryListBox_.selectRow(selectedEntryIndex_);
        saveCurrentSetlist();
    };
    addAndMakeVisible(moveDownButton_);

    removeSongButton_.setColour(juce::TextButton::buttonColourId, AppTheme::dangerRed);
    removeSongButton_.onClick = [this]() {
        if (!currentSetlist_ || selectedEntryIndex_ < 0 ||
            selectedEntryIndex_ >= static_cast<int>(currentSetlist_->entries.size())) return;
        currentSetlist_->entries.erase(currentSetlist_->entries.begin() + selectedEntryIndex_);
        for (size_t i = 0; i < currentSetlist_->entries.size(); ++i) {
            currentSetlist_->entries[i].orderIndex = static_cast<int>(i);
        }
        selectedEntryIndex_ = -1;
        entryListBox_.updateContent();
        saveCurrentSetlist();
        updateOverridesUI();
    };
    addAndMakeVisible(removeSongButton_);

    // Add Song dropdown
    addAndMakeVisible(addSongPicker_);
    addSongButton_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
    addSongButton_.onClick = [this]() {
        if (!currentSetlist_) return;
        int songId = addSongPicker_.getSelectedId();
        if (songId > 0) {
            currentSetlist_->addSong(songId);
            saveCurrentSetlist();
            entryListBox_.updateContent();
            entryListBox_.selectRow(static_cast<int>(currentSetlist_->entries.size()) - 1);
        }
    };
    addAndMakeVisible(addSongButton_);

    // Overrides section
    overridesHeader_.setFont(juce::Font(14.0f, juce::Font::bold));
    overridesHeader_.setColour(juce::Label::textColourId, AppTheme::textLight);
    addAndMakeVisible(overridesHeader_);

    auto setupOverride = [this](juce::Label& lbl) {
        lbl.setFont(juce::Font(11.0f, juce::Font::bold));
        lbl.setColour(juce::Label::textColourId, AppTheme::textMuted);
        addAndMakeVisible(lbl);
    };

    setupOverride(overrideTempoLabel_);
    addAndMakeVisible(overrideTempoEditor_);
    overrideTempoEditor_.onTextChange = [this]() { applyOverridesFromUI(); };

    setupOverride(overrideKeyLabel_);
    const char* keys[] = { "", "C", "C#", "Db", "D", "Eb", "E", "F", "F#", "Gb", "G", "Ab", "A", "Bb", "B",
                           "Am", "Bbm", "Bm", "Cm", "C#m", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "Abm" };
    int kid = 1;
    for (const char* k : keys) {
        overrideKeyBox_.addItem(k[0] == '\0' ? "(Default)" : k, kid++);
    }
    overrideKeyBox_.onChange = [this]() { applyOverridesFromUI(); };
    addAndMakeVisible(overrideKeyBox_);

    setupOverride(overrideTimeSigLabel_);
    const char* sigs[] = { "", "4/4", "3/4", "6/8", "12/8", "2/4", "5/4", "7/8" };
    int sid = 1;
    for (const char* s : sigs) {
        overrideTimeSigBox_.addItem(s[0] == '\0' ? "(Default)" : s, sid++);
    }
    overrideTimeSigBox_.onChange = [this]() { applyOverridesFromUI(); };
    addAndMakeVisible(overrideTimeSigBox_);

    saveOverrideToLibraryButton_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
    saveOverrideToLibraryButton_.onClick = [this]() { saveOverridesToMasterSong(); };
    addAndMakeVisible(saveOverrideToLibraryButton_);

    saveSetButton_.setColour(juce::TextButton::buttonColourId, AppTheme::accentGreen);
    saveSetButton_.onClick = [this]() { saveCurrentSetlist(); };
    addAndMakeVisible(saveSetButton_);

    refreshSetlists();
}

void OpenSetView::populateAddSongPicker() {
    addSongPicker_.clear();
    auto allSongs = db_.listSongs();
    for (const auto& song : allSongs) {
        if (song.id.has_value()) {
            addSongPicker_.addItem(song.title + " (" + song.artist + ")", static_cast<int>(*song.id));
        }
    }
    if (addSongPicker_.getNumItems() > 0) {
        addSongPicker_.setSelectedId(1, juce::dontSendNotification);
    }
}

void OpenSetView::refreshSetlists() {
    setlists_ = db_.listSetlists();
    setPicker_.clear();
    for (size_t i = 0; i < setlists_.size(); ++i) {
        setPicker_.addItem(setlists_[i].name, static_cast<int>(i + 1));
    }

    populateAddSongPicker();

    if (!setlists_.empty()) {
        if (!currentSetlist_ || !currentSetlist_->id.has_value()) {
            selectSetlist(*setlists_[0].id);
        } else {
            selectSetlist(*currentSetlist_->id);
        }
    } else {
        currentSetlist_ = std::nullopt;
        nameEditor_.setText("", false);
        dateEditor_.setText("", false);
        timeEditor_.setText("", false);
        notesEditor_.setText("", false);
        entryListBox_.updateContent();
        updateOverridesUI();
    }
}

void OpenSetView::selectSetlist(int64_t setId) {
    for (size_t i = 0; i < setlists_.size(); ++i) {
        if (setlists_[i].id.has_value() && *setlists_[i].id == setId) {
            setPicker_.setSelectedId(static_cast<int>(i + 1), juce::dontSendNotification);
            auto loaded = db_.loadSetlist(setId);
            if (loaded.has_value()) {
                currentSetlist_ = loaded;
                loadSetlistToUI(*currentSetlist_);
            }
            break;
        }
    }
}

void OpenSetView::loadSetlistToUI(const Setlist& setlist) {
    nameEditor_.setText(setlist.name, false);
    dateEditor_.setText(setlist.setDate, false);
    timeEditor_.setText(setlist.setTime, false);
    notesEditor_.setText(setlist.notes, false);

    selectedEntryIndex_ = -1;
    entryListBox_.updateContent();
    entryListBox_.repaint();
    updateOverridesUI();
}

void OpenSetView::saveCurrentSetlist() {
    if (!currentSetlist_) return;

    currentSetlist_->name = nameEditor_.getText().trim().toStdString();
    if (currentSetlist_->name.empty()) currentSetlist_->name = "Untitled Set";
    currentSetlist_->setDate = dateEditor_.getText().toStdString();
    currentSetlist_->setTime = timeEditor_.getText().toStdString();
    currentSetlist_->notes = notesEditor_.getText().toStdString();

    Setlist saved = db_.saveSetlist(*currentSetlist_);
    currentSetlist_ = saved;

    int curIdx = setPicker_.getSelectedItemIndex();
    setPicker_.changeItemText(curIdx + 1, saved.name);
}

int OpenSetView::getNumRows() {
    return currentSetlist_ ? static_cast<int>(currentSetlist_->entries.size()) : 0;
}

void OpenSetView::paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) {
    if (!currentSetlist_ || rowNumber < 0 || rowNumber >= static_cast<int>(currentSetlist_->entries.size()))
        return;

    if (rowIsSelected) {
        g.setColour(AppTheme::primaryBlue.withAlpha(0.25f));
        g.fillRect(0, 0, width, height);
        g.setColour(AppTheme::primaryBlue);
        g.drawVerticalLine(0, 0.0f, static_cast<float>(height));
    }

    const auto& entry = currentSetlist_->entries[static_cast<size_t>(rowNumber)];
    std::string songTitle = entry.song ? entry.song->title : "Unknown Song";
    std::string songArtist = entry.song ? entry.song->artist : "";
    int effTempo = entry.getEffectiveTempo();
    std::string effKey = entry.getEffectiveKey();
    std::string effTimeSig = entry.getEffectiveTimeSignature();

    // Order number
    g.setFont(juce::Font(12.0f, juce::Font::bold));
    g.setColour(AppTheme::primaryBlue);
    g.drawText(juce::String(rowNumber + 1) + ".", 8, 0, 24, height, juce::Justification::centredLeft);

    // Title & Artist
    g.setFont(juce::Font(13.0f, juce::Font::bold));
    g.setColour(AppTheme::textLight);
    g.drawText(songTitle, 36, 4, width - 200, 16, juce::Justification::centredLeft, true);

    g.setFont(juce::Font(11.0f, juce::Font::plain));
    g.setColour(AppTheme::textMuted);
    g.drawText(songArtist, 36, 20, width - 200, 14, juce::Justification::centredLeft, true);

    // Effective settings badge
    juce::String details = juce::String(effTempo) + " BPM  |  " + juce::String(effKey) + "  |  " + juce::String(effTimeSig);
    if (entry.overrideTempo.has_value() || entry.overrideKey.has_value() || entry.overrideTimeSignature.has_value()) {
        details += " (Custom)";
        g.setColour(AppTheme::warningAmber);
    } else {
        g.setColour(AppTheme::textMuted);
    }
    g.setFont(juce::Font(11.0f, juce::Font::bold));
    g.drawText(details, width - 200, 0, 190, height, juce::Justification::centredRight, true);

    g.setColour(AppTheme::borderCol);
    g.drawHorizontalLine(height - 1, 0.0f, static_cast<float>(width));
}

void OpenSetView::selectedRowsChanged(int lastRowSelected) {
    selectedEntryIndex_ = lastRowSelected;
    updateOverridesUI();

    if (currentSetlist_ && selectedEntryIndex_ >= 0 &&
        selectedEntryIndex_ < static_cast<int>(currentSetlist_->entries.size())) {
        const auto& entry = currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_)];
        if (entry.song.has_value()) {
            Song effectiveSong = *entry.song;
            effectiveSong.tempo = entry.getEffectiveTempo();
            effectiveSong.key = entry.getEffectiveKey();
            effectiveSong.timeSignature = entry.getEffectiveTimeSignature();
            if (onSongSelectedForPlayback) {
                onSongSelectedForPlayback(effectiveSong);
            }
        }
    }
}

void OpenSetView::updateOverridesUI() {
    bool hasSelection = (currentSetlist_ && selectedEntryIndex_ >= 0 &&
                         selectedEntryIndex_ < static_cast<int>(currentSetlist_->entries.size()));

    overrideTempoEditor_.setEnabled(hasSelection);
    overrideKeyBox_.setEnabled(hasSelection);
    overrideTimeSigBox_.setEnabled(hasSelection);
    saveOverrideToLibraryButton_.setEnabled(hasSelection);

    if (hasSelection) {
        const auto& entry = currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_)];
        if (entry.overrideTempo.has_value()) {
            overrideTempoEditor_.setText(juce::String(*entry.overrideTempo), false);
        } else {
            overrideTempoEditor_.setText("", false);
        }

        if (entry.overrideKey.has_value()) {
            overrideKeyBox_.setText(*entry.overrideKey, juce::dontSendNotification);
        } else {
            overrideKeyBox_.setSelectedId(1, juce::dontSendNotification); // (Default)
        }

        if (entry.overrideTimeSignature.has_value()) {
            overrideTimeSigBox_.setText(*entry.overrideTimeSignature, juce::dontSendNotification);
        } else {
            overrideTimeSigBox_.setSelectedId(1, juce::dontSendNotification); // (Default)
        }
    } else {
        overrideTempoEditor_.setText("", false);
        overrideKeyBox_.setSelectedId(1, juce::dontSendNotification);
        overrideTimeSigBox_.setSelectedId(1, juce::dontSendNotification);
    }
}

void OpenSetView::applyOverridesFromUI() {
    if (!currentSetlist_ || selectedEntryIndex_ < 0 ||
        selectedEntryIndex_ >= static_cast<int>(currentSetlist_->entries.size()))
        return;

    auto& entry = currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_)];

    auto tempoText = overrideTempoEditor_.getText().trim();
    if (!tempoText.isEmpty() && tempoText.getIntValue() > 0) {
        entry.overrideTempo = tempoText.getIntValue();
    } else {
        entry.overrideTempo = std::nullopt;
    }

    auto keyText = overrideKeyBox_.getText();
    if (!keyText.isEmpty() && keyText != "(Default)") {
        entry.overrideKey = keyText.toStdString();
    } else {
        entry.overrideKey = std::nullopt;
    }

    auto sigText = overrideTimeSigBox_.getText();
    if (!sigText.isEmpty() && sigText != "(Default)") {
        entry.overrideTimeSignature = sigText.toStdString();
    } else {
        entry.overrideTimeSignature = std::nullopt;
    }

    saveCurrentSetlist();
    entryListBox_.repaintRow(selectedEntryIndex_);

    if (entry.song.has_value()) {
        Song effectiveSong = *entry.song;
        effectiveSong.tempo = entry.getEffectiveTempo();
        effectiveSong.key = entry.getEffectiveKey();
        effectiveSong.timeSignature = entry.getEffectiveTimeSignature();
        if (onSongSelectedForPlayback) {
            onSongSelectedForPlayback(effectiveSong);
        }
    }
}

void OpenSetView::saveOverridesToMasterSong() {
    if (!currentSetlist_ || selectedEntryIndex_ < 0 ||
        selectedEntryIndex_ >= static_cast<int>(currentSetlist_->entries.size()))
        return;

    auto& entry = currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_)];
    if (entry.song.has_value() && entry.song->id.has_value()) {
        Song master = *entry.song;
        master.tempo = entry.getEffectiveTempo();
        master.key = entry.getEffectiveKey();
        master.timeSignature = entry.getEffectiveTimeSignature();
        db_.saveSong(master);
        entry.song = master;
        entry.overrideTempo = std::nullopt;
        entry.overrideKey = std::nullopt;
        entry.overrideTimeSignature = std::nullopt;
        saveCurrentSetlist();
        updateOverridesUI();
        entryListBox_.repaintRow(selectedEntryIndex_);
    }
}

void OpenSetView::paint(juce::Graphics& g) {
    g.fillAll(AppTheme::bgDark);
    g.setColour(AppTheme::borderCol);
    g.drawVerticalLine(getWidth() - 320, 56.0f, static_cast<float>(getHeight()));
}

void OpenSetView::resized() {
    auto area = getLocalBounds().reduced(16);

    // Top Bar: Set Picker + New/Delete
    auto topBar = area.removeFromTop(32);
    setSelectLabel_.setBounds(topBar.removeFromLeft(60));
    setPicker_.setBounds(topBar.removeFromLeft(220));
    topBar.removeFromLeft(12);
    newSetButton_.setBounds(topBar.removeFromLeft(110));
    topBar.removeFromLeft(8);
    deleteSetButton_.setBounds(topBar.removeFromLeft(100));

    area.removeFromTop(16);

    // Right Column: Set Details & Overrides (300px)
    auto rightCol = area.removeFromRight(300);
    area.removeFromRight(20);

    // Right Column contents
    auto metaHeader = rightCol.removeFromTop(20);
    juce::Label lbl({}, "Setlist Details");
    lbl.setFont(juce::Font(14.0f, juce::Font::bold));
    lbl.setColour(juce::Label::textColourId, AppTheme::textLight);

    int rowH = 26;
    int gap = 6;

    auto r1 = rightCol.removeFromTop(rowH);
    nameLabel_.setBounds(r1.removeFromLeft(60));
    nameEditor_.setBounds(r1);

    rightCol.removeFromTop(gap);
    auto r2 = rightCol.removeFromTop(rowH);
    dateLabel_.setBounds(r2.removeFromLeft(60));
    dateEditor_.setBounds(r2.removeFromLeft(100));
    r2.removeFromLeft(8);
    timeLabel_.setBounds(r2.removeFromLeft(40));
    timeEditor_.setBounds(r2);

    rightCol.removeFromTop(gap);
    auto r3 = rightCol.removeFromTop(50);
    notesLabel_.setBounds(r3.removeFromLeft(60));
    notesEditor_.setBounds(r3);

    rightCol.removeFromTop(gap);
    saveSetButton_.setBounds(rightCol.removeFromTop(28));

    rightCol.removeFromTop(20);
    overridesHeader_.setBounds(rightCol.removeFromTop(24));
    rightCol.removeFromTop(gap);

    auto o1 = rightCol.removeFromTop(rowH);
    overrideTempoLabel_.setBounds(o1.removeFromLeft(110));
    overrideTempoEditor_.setBounds(o1);

    rightCol.removeFromTop(gap);
    auto o2 = rightCol.removeFromTop(rowH);
    overrideKeyLabel_.setBounds(o2.removeFromLeft(110));
    overrideKeyBox_.setBounds(o2);

    rightCol.removeFromTop(gap);
    auto o3 = rightCol.removeFromTop(rowH);
    overrideTimeSigLabel_.setBounds(o3.removeFromLeft(110));
    overrideTimeSigBox_.setBounds(o3);

    rightCol.removeFromTop(12);
    saveOverrideToLibraryButton_.setBounds(rightCol.removeFromTop(32));

    // Left Area: Song list in set
    auto leftArea = area;

    // Controls below the setlist
    auto bottomControls = leftArea.removeFromBottom(72);

    auto addSongBar = bottomControls.removeFromTop(30);
    addSongPicker_.setBounds(addSongBar.removeFromLeft(260));
    addSongBar.removeFromLeft(8);
    addSongButton_.setBounds(addSongBar.removeFromLeft(110));

    bottomControls.removeFromTop(10);

    auto actionButtons = bottomControls.removeFromTop(30);
    moveUpButton_.setBounds(actionButtons.removeFromLeft(90));
    actionButtons.removeFromLeft(8);
    moveDownButton_.setBounds(actionButtons.removeFromLeft(100));
    actionButtons.removeFromLeft(8);
    removeSongButton_.setBounds(actionButtons.removeFromLeft(110));

    leftArea.removeFromBottom(12);
    entryListBox_.setBounds(leftArea);
}

} // namespace tracks
