#include "OpenSetView.h"
#include "../../Common/AppTheme.h"
#include "../Widgets/CalendarPopupComponent.h"
#include "../Widgets/TimePopupComponent.h"
#include <algorithm>

namespace tracks {

OpenSetView::OpenSetView(DatabaseService& db, AudioEngine& audioEngine)
    : db_(db), audioEngine_(audioEngine) {
    setOpaque(true);

    // Top Set Picker
    setSelectLabel_.setFont(juce::Font(12.0f, juce::Font::bold));
    setSelectLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
    addAndMakeVisible(setSelectLabel_);

    setPicker_.setEditableText(true);
    setPicker_.onChange = [this]() {
        int idx = setPicker_.getSelectedItemIndex();
        if (idx >= 0 && idx < static_cast<int>(setlists_.size())) {
            if (setlists_[static_cast<size_t>(idx)].id.has_value()) {
                selectSetlist(*setlists_[static_cast<size_t>(idx)].id);
            } else {
                currentSetlist_ = setlists_[static_cast<size_t>(idx)];
                loadSetlistToUI(*currentSetlist_);
            }
        } else if (currentSetlist_.has_value()) {
            saveCurrentSetlist();
        }
    };
    addAndMakeVisible(setPicker_);

    auto setupTopBarDateTime = [this](juce::Label& lbl, juce::TextEditor& ed, const juce::String& placeholder) {
        lbl.setFont(juce::Font(12.0f, juce::Font::bold));
        lbl.setColour(juce::Label::textColourId, AppTheme::textLight);
        lbl.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(lbl);

        ed.setJustification(juce::Justification::centredLeft);
        ed.setTextToShowWhenEmpty(placeholder, AppTheme::textMuted.withAlpha(0.5f));
        ed.onTextChange = [this]() { saveCurrentSetlist(); };
        addAndMakeVisible(ed);
    };

    setupTopBarDateTime(dateLabel_, dateEditor_, "YYYY-MM-DD");
    setupTopBarDateTime(timeLabel_, timeEditor_, "HH:MM AM");

    datePickerButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgInput);
    datePickerButton_.onClick = [this]() { showCalendarPopup(); };
    addAndMakeVisible(datePickerButton_);

    timePickerButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgInput);
    timePickerButton_.onClick = [this]() { showTimePopup(); };
    addAndMakeVisible(timePickerButton_);

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

    // Set entries listbox
    entryListBox_.setModel(this);
    entryListBox_.setRowHeight(40);
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
        notifySongNavigationState();
    };
    addAndMakeVisible(removeSongButton_);

    // Add Song dropdown (adds immediately on selection)
    addSongPicker_.setTextWhenNothingSelected("Add song to set...");
    addSongPicker_.setTextWhenNoChoicesAvailable("No songs in library");
    addSongPicker_.onChange = [this]() {
        int songId = addSongPicker_.getSelectedId();
        if (songId <= 0) return;

        addSongPicker_.setSelectedId(0, juce::dontSendNotification);

        if (!currentSetlist_) return;

        auto songOpt = db_.loadSong(songId);
        if (!songOpt.has_value()) return;

        SetlistEntry entry;
        entry.songId = songId;
        entry.orderIndex = static_cast<int>(currentSetlist_->entries.size());
        entry.song = *songOpt;
        currentSetlist_->entries.push_back(entry);

        saveCurrentSetlist();
        preloadCurrentSetlist();
        entryListBox_.updateContent();
        entryListBox_.selectRow(static_cast<int>(currentSetlist_->entries.size()) - 1);
    };
    addAndMakeVisible(addSongPicker_);

    refreshSetlists();
}

void OpenSetView::populateAddSongPicker() {
    addSongPicker_.clear(juce::dontSendNotification);
    auto allSongs = db_.listSongs();
    for (const auto& song : allSongs) {
        if (song.id.has_value()) {
            juce::String label = song.title;
            if (!song.artist.empty()) {
                label += " (" + juce::String(song.artist) + ")";
            }
            addSongPicker_.addItem(label, static_cast<int>(*song.id));
        }
    }
    addSongPicker_.setSelectedId(0, juce::dontSendNotification);
}

Song OpenSetView::buildEffectiveSongForEntry(const SetlistEntry& entry) const {
    Song effectiveSong = entry.song.value_or(Song{});
    const auto* arr = entry.getSelectedArrangement();
    if (arr != nullptr && !arr->tracks.empty()) {
        effectiveSong.activeArrangementId = arr->id;
        effectiveSong.tempo = arr->tempo;
        std::string baseKey = arr->key.empty() ? effectiveSong.key : arr->key;
        effectiveSong.key = (entry.overrideKey.has_value() && !entry.overrideKey->empty()) ? *entry.overrideKey : baseKey;
        effectiveSong.timeSignature = arr->timeSignature.empty() ? effectiveSong.timeSignature : arr->timeSignature;
        effectiveSong.clickEighths = arr->clickEighths;
        effectiveSong.clickSixteenths = arr->clickSixteenths;
        effectiveSong.clickEighthsGain = arr->clickEighthsGain;
        effectiveSong.clickSixteenthsGain = arr->clickSixteenthsGain;
        effectiveSong.tracks = arr->tracks;
        effectiveSong.sections = arr->sections;
    } else {
        effectiveSong.activeArrangementId = std::nullopt;
        effectiveSong.tempo = entry.overrideTempo.value_or(effectiveSong.tempo);
        effectiveSong.key = entry.overrideKey.value_or(effectiveSong.key);
        effectiveSong.timeSignature = entry.overrideTimeSignature.value_or(effectiveSong.timeSignature);
    }
    return effectiveSong;
}

std::optional<Song> OpenSetView::getClickOnlyBaselineForCurrentSong() const {
    if (!currentSetlist_ || selectedEntryIndex_ < 0 ||
        selectedEntryIndex_ >= static_cast<int>(currentSetlist_->entries.size()))
        return std::nullopt;
    const auto& entry = currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_)];
    if (!entry.song.has_value()) return std::nullopt;
    Song base = *entry.song;
    if (entry.overrideTempo.has_value() && *entry.overrideTempo > 0) base.tempo = *entry.overrideTempo;
    if (entry.overrideKey.has_value() && !entry.overrideKey->empty()) base.key = *entry.overrideKey;
    if (entry.overrideTimeSignature.has_value() && !entry.overrideTimeSignature->empty()) base.timeSignature = *entry.overrideTimeSignature;
    return base;
}

void OpenSetView::preloadCurrentSetlist() {
    if (!isVisible() || !currentSetlist_ || !currentSetlist_->id.has_value()) return;

    std::vector<Song> songsInSet;
    for (const auto& entry : currentSetlist_->entries) {
        if (!entry.song.has_value()) continue;
        if (!entry.song->arrangements.empty()) {
            for (const auto& arr : entry.song->arrangements) {
                if (arr.tracks.empty()) continue;
                Song s = *entry.song;
                s.activeArrangementId = arr.id;
                s.tempo = arr.tempo;
                s.key = arr.key;
                s.timeSignature = arr.timeSignature;
                s.tracks = arr.tracks;
                s.sections = arr.sections;
                songsInSet.push_back(std::move(s));
            }
        } else if (!entry.song->tracks.empty()) {
            songsInSet.push_back(*entry.song);
        }
    }
    audioEngine_.preloadSetlist(*currentSetlist_->id, songsInSet);
}

void OpenSetView::refreshSetlists() {
    setlists_ = db_.listSetlists();
    setPicker_.clear(juce::dontSendNotification);
    for (size_t i = 0; i < setlists_.size(); ++i) {
        setPicker_.addItem(setlists_[i].name, static_cast<int>(i + 1));
    }
    setPicker_.setEditableText(!setlists_.empty());
    addSongPicker_.setEnabled(!setlists_.empty());

    populateAddSongPicker();

    if (!setlists_.empty()) {
        if (currentSetlist_ && currentSetlist_->id.has_value()) {
            const int64_t currentId = *currentSetlist_->id;
            bool stillExists = false;
            for (size_t i = 0; i < setlists_.size(); ++i) {
                if (setlists_[i].id.has_value() && *setlists_[i].id == currentId) {
                    setPicker_.setSelectedId(static_cast<int>(i + 1), juce::dontSendNotification);
                    auto loaded = db_.loadSetlist(currentId);
                    if (loaded.has_value()) {
                        currentSetlist_ = loaded;
                        loadSetlistToUI(*currentSetlist_, true);
                        preloadCurrentSetlist();
                    }
                    stillExists = true;
                    break;
                }
            }
            if (!stillExists) {
                selectSetlist(*setlists_[0].id);
            }
        } else {
            selectSetlist(*setlists_[0].id);
        }
    } else {
        currentSetlist_ = std::nullopt;
        selectedEntryIndex_ = -1;
        setPicker_.setText("", juce::dontSendNotification);
        dateEditor_.setText("", false);
        timeEditor_.setText("", false);
        entryListBox_.updateContent();
        notifySongNavigationState();
    }
}

void OpenSetView::selectSetlist(int64_t setId) {
    for (size_t i = 0; i < setlists_.size(); ++i) {
        if (setlists_[i].id.has_value() && *setlists_[i].id == setId) {
            setPicker_.setSelectedId(static_cast<int>(i + 1), juce::dontSendNotification);
            auto loaded = db_.loadSetlist(setId);
            if (loaded.has_value()) {
                currentSetlist_ = loaded;
                loadSetlistToUI(*currentSetlist_, false);
                preloadCurrentSetlist();
            }
            break;
        }
    }
}

void OpenSetView::loadSetlistToUI(const Setlist& setlist, bool preserveSelection) {
    dateEditor_.setText(setlist.setDate, false);
    timeEditor_.setText(setlist.setTime, false);

    if (!preserveSelection || selectedEntryIndex_ >= static_cast<int>(setlist.entries.size())) {
        selectedEntryIndex_ = -1;
    }
    if (isVisible() && selectedEntryIndex_ < 0 && !setlist.entries.empty()) {
        selectedEntryIndex_ = 0;
    }

    entryListBox_.updateContent();
    if (selectedEntryIndex_ >= 0 && selectedEntryIndex_ < static_cast<int>(setlist.entries.size())) {
        entryListBox_.selectRow(selectedEntryIndex_, false, false);
        if (isVisible()) {
            const auto& entry = setlist.entries[static_cast<size_t>(selectedEntryIndex_)];
            if (entry.song.has_value() && onSongSelectedForPlayback) {
                onSongSelectedForPlayback(buildEffectiveSongForEntry(entry));
            }
        }
    } else {
        entryListBox_.deselectAllRows();
    }
    entryListBox_.repaint();
    notifySongNavigationState();
}

void OpenSetView::saveCurrentSetlist() {
    if (!currentSetlist_) return;

    currentSetlist_->name = setPicker_.getText().trim().toStdString();
    if (currentSetlist_->name.empty()) currentSetlist_->name = "Untitled Set";
    currentSetlist_->setDate = dateEditor_.getText().toStdString();
    currentSetlist_->setTime = timeEditor_.getText().toStdString();

    Setlist saved = db_.saveSetlist(*currentSetlist_);
    currentSetlist_ = saved;

    if (saved.id.has_value()) {
        for (size_t i = 0; i < setlists_.size(); ++i) {
            if (setlists_[i].id.has_value() && *setlists_[i].id == *saved.id) {
                setlists_[i] = saved;
                int itemId = static_cast<int>(i + 1);
                if (setPicker_.getItemText(static_cast<int>(i)).toStdString() != saved.name ||
                    setPicker_.getSelectedId() != itemId) {
                    setPicker_.changeItemText(itemId, saved.name);
                    setPicker_.setSelectedId(itemId, juce::dontSendNotification);
                }
                break;
            }
        }
    }
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
    g.drawText(juce::String(rowNumber + 1) + ".", 6, 0, 22, height, juce::Justification::centredLeft);

    const int contentX = 28;
    const int contentW = std::max(0, width - contentX - 8);

    // Title (full top row; show arrangement name when song has multiple arrangements)
    juce::String titleDisplay = songTitle;
    if (entry.song.has_value() && entry.song->arrangements.size() > 1) {
        if (const auto* arr = entry.getSelectedArrangement(); arr != nullptr && !arr->name.empty()) {
            titleDisplay += "  [" + juce::String(arr->name) + "]";
        }
    }
    g.setFont(juce::Font(13.0f, juce::Font::bold));
    g.setColour(AppTheme::textLight);
    g.drawText(titleDisplay, contentX, 4, contentW, 16, juce::Justification::centredLeft, true);

    // Effective settings badge (bottom right)
    juce::String details = juce::String(effTempo) + " BPM | " + juce::String(effKey) + " | " + juce::String(effTimeSig);
    if (entry.overrideTempo.has_value() || entry.overrideKey.has_value() || entry.overrideTimeSignature.has_value()) {
        details += " *";
        g.setColour(AppTheme::warningAmber);
    } else {
        g.setColour(AppTheme::textMuted);
    }
    juce::Font detailsFont(10.5f, juce::Font::bold);
    g.setFont(detailsFont);
    const int detailsW = std::min(contentW, detailsFont.getStringWidth(details) + 4);
    g.drawText(details, contentX + contentW - detailsW, 21, detailsW, 15, juce::Justification::centredRight, true);

    // Artist (bottom left, remaining space)
    const int artistW = contentW - detailsW - 6;
    if (artistW > 24 && !songArtist.empty()) {
        g.setFont(juce::Font(11.0f, juce::Font::plain));
        g.setColour(AppTheme::textMuted);
        g.drawText(songArtist, contentX, 21, artistW, 15, juce::Justification::centredLeft, true);
    }

    g.setColour(AppTheme::borderCol);
    g.drawHorizontalLine(height - 1, 0.0f, static_cast<float>(width));
}

void OpenSetView::selectedRowsChanged(int lastRowSelected) {
    selectedEntryIndex_ = lastRowSelected;
    notifySongNavigationState();

    if (currentSetlist_ && selectedEntryIndex_ >= 0 &&
        selectedEntryIndex_ < static_cast<int>(currentSetlist_->entries.size())) {
        const auto& entry = currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_)];
        if (entry.song.has_value() && onSongSelectedForPlayback) {
            onSongSelectedForPlayback(buildEffectiveSongForEntry(entry));
        }
    }
}

void OpenSetView::selectArrangementForCurrentSong(int64_t arrangementId) {
    if (!currentSetlist_ || selectedEntryIndex_ < 0 ||
        selectedEntryIndex_ >= static_cast<int>(currentSetlist_->entries.size()))
        return;

    auto& entry = currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_)];
    if (entry.arrangementId.has_value() && *entry.arrangementId == arrangementId) return;

    const bool wasPlaying = audioEngine_.isPlaying() && !audioEngine_.isPaused();
    if (audioEngine_.isPlaying()) {
        audioEngine_.stop();
    }

    entry.arrangementId = arrangementId;
    entry.overrideKey = std::nullopt; // Start cleanly in the newly selected arrangement's recorded key
    saveCurrentSetlist();
    entryListBox_.repaintRow(selectedEntryIndex_);

    if (entry.song.has_value() && onSongSelectedForPlayback) {
        onSongSelectedForPlayback(buildEffectiveSongForEntry(entry));
        if (wasPlaying) {
            audioEngine_.play();
        }
    }
}

bool OpenSetView::canSelectPreviousSong() const {
    return currentSetlist_.has_value() && selectedEntryIndex_ > 0 &&
           selectedEntryIndex_ < static_cast<int>(currentSetlist_->entries.size());
}

bool OpenSetView::canSelectNextSong() const {
    if (!currentSetlist_.has_value() || currentSetlist_->entries.empty()) return false;
    return selectedEntryIndex_ < static_cast<int>(currentSetlist_->entries.size()) - 1;
}

void OpenSetView::selectPreviousSong() {
    if (!canSelectPreviousSong()) return;
    const bool wasPlaying = audioEngine_.isPlaying() && !audioEngine_.isPaused();
    audioEngine_.stop();
    entryListBox_.selectRow(selectedEntryIndex_ - 1);
    if (wasPlaying) {
        audioEngine_.play();
    }
}

void OpenSetView::selectNextSong() {
    if (!canSelectNextSong()) return;
    const bool wasPlaying = audioEngine_.isPlaying() && !audioEngine_.isPaused();
    audioEngine_.stop();
    int nextRow = (selectedEntryIndex_ < 0) ? 0 : (selectedEntryIndex_ + 1);
    entryListBox_.selectRow(nextRow);
    if (wasPlaying) {
        audioEngine_.play();
    }
}

void OpenSetView::notifySongNavigationState() {
    if (onSongNavigationStateChanged) {
        onSongNavigationStateChanged(canSelectPreviousSong(), canSelectNextSong());
    }
}

void OpenSetView::applySongParameters(int tempo, const std::string& key, const std::string& timeSignature) {
    if (!currentSetlist_ || selectedEntryIndex_ < 0 ||
        selectedEntryIndex_ >= static_cast<int>(currentSetlist_->entries.size()))
        return;

    auto& entry = currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_)];
    const int clampedTempo = std::clamp(tempo, 20, 300);
    const std::string cleanKey = key.empty() ? "C" : key;
    const std::string cleanSig = timeSignature.empty() ? "4/4" : timeSignature;

    if (entry.song.has_value()) {
        const auto* arr = entry.getSelectedArrangement();
        const bool hasArrTracks = (arr != nullptr && !arr->tracks.empty());
        const int baseTempo = hasArrTracks ? arr->tempo : entry.song->tempo;
        const std::string baseKey = (hasArrTracks && !arr->key.empty()) ? arr->key : entry.song->key;
        const std::string baseSig = (hasArrTracks && !arr->timeSignature.empty()) ? arr->timeSignature : entry.song->timeSignature;

        entry.overrideTempo = (clampedTempo == baseTempo) ? std::nullopt : std::optional<int>(clampedTempo);
        entry.overrideKey = (cleanKey == baseKey) ? std::nullopt : std::optional<std::string>(cleanKey);
        entry.overrideTimeSignature = (cleanSig == baseSig) ? std::nullopt : std::optional<std::string>(cleanSig);
    } else {
        entry.overrideTempo = clampedTempo;
        entry.overrideKey = cleanKey;
        entry.overrideTimeSignature = cleanSig;
    }

    saveCurrentSetlist();
    entryListBox_.repaintRow(selectedEntryIndex_);
}

void OpenSetView::syncSongClickSubdivisions(int64_t songId,
                                            std::optional<int64_t> arrangementId,
                                            bool clickEighths,
                                            bool clickSixteenths,
                                            float clickEighthsGain,
                                            float clickSixteenthsGain) {
    if (!currentSetlist_) return;
    for (auto& entry : currentSetlist_->entries) {
        if (entry.songId == songId && entry.song.has_value()) {
            if (arrangementId.has_value()) {
                for (auto& arr : entry.song->arrangements) {
                    if (arr.id.has_value() && *arr.id == *arrangementId) {
                        arr.clickEighths = clickEighths;
                        arr.clickSixteenths = clickSixteenths;
                        arr.clickEighthsGain = clickEighthsGain;
                        arr.clickSixteenthsGain = clickSixteenthsGain;
                    }
                }
            } else {
                entry.song->clickEighths = clickEighths;
                entry.song->clickSixteenths = clickSixteenths;
                entry.song->clickEighthsGain = clickEighthsGain;
                entry.song->clickSixteenthsGain = clickSixteenthsGain;
            }
        }
    }
}

void OpenSetView::paint(juce::Graphics& g) {
    g.fillAll(AppTheme::bgDark);
    g.setColour(AppTheme::borderCol);
    const int setlistWidth = getWidth() / 4;
    g.drawVerticalLine(16 + setlistWidth + 10, 56.0f, static_cast<float>(getHeight()));
}

void OpenSetView::resized() {
    auto area = getLocalBounds().reduced(16);

    // Top Bar: Set Picker + Date + Time + New/Delete
    auto topBar = area.removeFromTop(32);

    deleteSetButton_.setBounds(topBar.removeFromRight(96));
    topBar.removeFromRight(8);
    newSetButton_.setBounds(topBar.removeFromRight(90));
    topBar.removeFromRight(12);

    setSelectLabel_.setBounds(topBar.removeFromLeft(58));
    setPicker_.setBounds(topBar.removeFromLeft(200));
    topBar.removeFromLeft(12);

    dateLabel_.setBounds(topBar.removeFromLeft(44));
    dateEditor_.setBounds(topBar.removeFromLeft(104));
    topBar.removeFromLeft(4);
    datePickerButton_.setBounds(topBar.removeFromLeft(44));
    topBar.removeFromLeft(12);

    timeLabel_.setBounds(topBar.removeFromLeft(44));
    timeEditor_.setBounds(topBar.removeFromLeft(88));
    topBar.removeFromLeft(4);
    timePickerButton_.setBounds(topBar.removeFromLeft(44));

    area.removeFromTop(16);

    // Left Area: Setlist (25% of window width)
    const int setlistWidth = getWidth() / 4;
    auto leftArea = area.removeFromLeft(setlistWidth);
    area.removeFromLeft(20);

    // Controls below the setlist
    auto bottomControls = leftArea.removeFromBottom(68);

    auto addSongBar = bottomControls.removeFromTop(28);
    addSongPicker_.setBounds(addSongBar);

    bottomControls.removeFromTop(8);

    auto actionButtons = bottomControls.removeFromTop(28);
    const int btnGap = 6;
    const int availW = std::max(0, actionButtons.getWidth() - btnGap * 2);
    const int upW = static_cast<int>(availW * 0.31f);
    const int downW = static_cast<int>(availW * 0.36f);
    moveUpButton_.setBounds(actionButtons.removeFromLeft(upW));
    actionButtons.removeFromLeft(btnGap);
    moveDownButton_.setBounds(actionButtons.removeFromLeft(downW));
    actionButtons.removeFromLeft(btnGap);
    removeSongButton_.setBounds(actionButtons);

    leftArea.removeFromBottom(10);
    entryListBox_.setBounds(leftArea);

    // Right Area: Full-height playback & mixer panel
    transportArea_ = area;
}

void OpenSetView::showCalendarPopup() {
    auto cal = std::make_unique<CalendarPopupComponent>(dateEditor_.getText());
    cal->onDateSelected = [this](const juce::String& selectedDate) {
        dateEditor_.setText(selectedDate, false);
        saveCurrentSetlist();
    };
    juce::CallOutBox::launchAsynchronously(std::move(cal), dateEditor_.getScreenBounds(), nullptr);
}

void OpenSetView::showTimePopup() {
    auto timePop = std::make_unique<TimePopupComponent>(timeEditor_.getText());
    timePop->onTimeSelected = [this](const juce::String& selectedTime) {
        timeEditor_.setText(selectedTime, false);
        saveCurrentSetlist();
    };
    juce::CallOutBox::launchAsynchronously(std::move(timePop), timeEditor_.getScreenBounds(), nullptr);
}

} // namespace tracks
