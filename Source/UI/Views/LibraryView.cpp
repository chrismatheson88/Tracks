#include "LibraryView.h"
#include "../../Common/AppTheme.h"
#include "../../Models/AvailableSections.h"
#include "../../Audio/MidiTempoImporter.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace tracks {

namespace {

std::string toLowerNorm(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prevSpace = true;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            if (!prevSpace) {
                out.push_back(' ');
                prevSpace = true;
            }
        } else {
            out.push_back(static_cast<char>(std::tolower(c)));
            prevSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::vector<std::string> splitSearchTokens(const std::string& norm) {
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < norm.size()) {
        size_t next = norm.find(' ', pos);
        if (next == std::string::npos) {
            tokens.push_back(norm.substr(pos));
            break;
        }
        if (next > pos) tokens.push_back(norm.substr(pos, next - pos));
        pos = next + 1;
    }
    return tokens;
}

} // namespace

LibraryView::LibraryView(DatabaseService& db, AudioEngine& audioEngine)
    : db_(db), audioEngine_(audioEngine) {
    setOpaque(true);

    // Search bar above song list
    searchEditor_.setJustification(juce::Justification::centredLeft);
    searchEditor_.setTextToShowWhenEmpty("Search songs...", AppTheme::textMuted.withAlpha(0.65f));
    searchEditor_.onTextChange = [this]() {
        const bool hasText = searchEditor_.getText().isNotEmpty();
        if (searchClearButton_.isVisible() != hasText) {
            searchClearButton_.setVisible(hasText);
            resized();
        }
        applySongFilter(false);
    };
    searchEditor_.onEscapeKey = [this]() {
        if (searchEditor_.getText().isNotEmpty()) {
            searchEditor_.setText("", true);
        }
    };
    searchEditor_.onReturnKey = [this]() {
        if (!songs_.empty()) {
            songListBox_.selectRow(0, false, true);
            selectedRowsChanged(0);
        }
    };
    addAndMakeVisible(searchEditor_);

    searchClearButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgInput);
    searchClearButton_.setTooltip("Clear search");
    searchClearButton_.setVisible(false);
    searchClearButton_.onClick = [this]() {
        searchEditor_.setText("", true);
        searchEditor_.grabKeyboardFocus();
    };
    addChildComponent(searchClearButton_);

    // Song List
    songListBox_.setModel(this);
    songListBox_.setRowHeight(36);
    addAndMakeVisible(songListBox_);

    newSongButton_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
    newSongButton_.onClick = [this]() {
        flushPendingAutoSave();
        if (searchEditor_.getText().isNotEmpty()) {
            searchEditor_.setText("", false);
            searchClearButton_.setVisible(false);
            resized();
        }
        Song s;
        s.title = "New Song";
        s.tempo = 120;
        s.key = "C";
        s.timeSignature = "4/4";
        Song saved = db_.saveSong(s);
        currentSong_ = saved;
        loadSongToEditor(saved);
        refreshSongList();
        setAutoSaveStatus("Saved", AppTheme::accentGreen.withAlpha(0.85f));
        titleEditor_.grabKeyboardFocus();
        titleEditor_.selectAll();
    };
    addAndMakeVisible(newSongButton_);

    deleteSongButton_.setColour(juce::TextButton::buttonColourId, AppTheme::dangerRed);
    deleteSongButton_.onClick = [this]() {
        if (!currentSong_ || !currentSong_->id.has_value()) return;

        const int64_t songId = *currentSong_->id;
        const juce::String songTitle = currentSong_->title.empty() ? "Untitled Song" : juce::String(currentSong_->title);
        juce::String details = "Are you sure you want to delete \"" + songTitle + "\" from your library?\n\n"
                               "This will permanently delete its arrangements, attached audio files, and remove it from any sets.";

        juce::Component::SafePointer<LibraryView> safeThis(this);
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::WarningIcon,
            "Delete Song",
            details,
            "Delete",
            "Cancel",
            this,
            juce::ModalCallbackFunction::create([safeThis, songId](int result) {
                if (safeThis == nullptr || result == 0) return;
                if (!safeThis->currentSong_ || !safeThis->currentSong_->id.has_value() ||
                    *safeThis->currentSong_->id != songId) {
                    return;
                }
                safeThis->stopTimer();
                safeThis->hasPendingAutoSave_ = false;
                safeThis->audioEngine_.invalidateCachedSong(songId);
                safeThis->db_.deleteSong(songId);
                AudioAttachmentService::removeSongStorage(songId);
                safeThis->clearEditor();
                safeThis->refreshSongList();
            }));
    };
    addAndMakeVisible(deleteSongButton_);

    // Editor fields
    headerLabel_.setFont(juce::Font(18.0f, juce::Font::bold));
    headerLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
    addAndMakeVisible(headerLabel_);

    autoSaveStatusLabel_.setFont(juce::Font(11.5f, juce::Font::plain));
    autoSaveStatusLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
    autoSaveStatusLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(autoSaveStatusLabel_);

    auto setupField = [this](juce::Label& lbl, juce::TextEditor& ed, bool flushOnReturn) {
        lbl.setFont(juce::Font(12.0f, juce::Font::bold));
        lbl.setColour(juce::Label::textColourId, AppTheme::textMuted);
        addAndMakeVisible(lbl);

        ed.onTextChange = [this]() {
            if (!isLoadingToEditor_) scheduleAutoSave();
        };
        ed.onFocusLost = [this]() {
            if (!isLoadingToEditor_ && hasPendingAutoSave_) {
                flushPendingAutoSave();
            }
        };
        if (flushOnReturn) {
            ed.onReturnKey = [this]() {
                if (!isLoadingToEditor_) {
                    flushPendingAutoSave();
                }
            };
        }
        addAndMakeVisible(ed);
    };

    setupField(titleLabel_, titleEditor_, true);
    setupField(artistLabel_, artistEditor_, true);
    setupField(tempoLabel_, tempoEditor_, true);
    setupField(notesLabel_, notesEditor_, false);
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
    keyBox_.onChange = [this]() {
        if (!isLoadingToEditor_) {
            stopTimer();
            saveCurrentSong(false, false);
        }
    };
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
    timeSigBox_.onChange = [this]() {
        if (!isLoadingToEditor_) {
            stopTimer();
            saveCurrentSong(false, false);
        }
    };
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
            setAttachmentWarning("");
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

    removeArrangementButton_.setColour(juce::TextButton::buttonColourId, AppTheme::dangerRed);
    removeArrangementButton_.setEnabled(false);
    removeArrangementButton_.onClick = [this]() { handleRemoveArrangement(); };
    addAndMakeVisible(removeArrangementButton_);

    attachmentStatusLabel_.setFont(juce::Font(11.0f, juce::Font::plain));
    attachmentStatusLabel_.setColour(juce::Label::textColourId, AppTheme::warningAmber);
    addAndMakeVisible(attachmentStatusLabel_);

    attachmentWarningBox_.setMultiLine(true, true);
    attachmentWarningBox_.setReadOnly(true);
    attachmentWarningBox_.setScrollbarsShown(true);
    attachmentWarningBox_.setCaretVisible(false);
    attachmentWarningBox_.setFont(juce::Font(11.5f, juce::Font::plain));
    attachmentWarningBox_.setColour(juce::TextEditor::textColourId, AppTheme::warningAmber);
    attachmentWarningBox_.setColour(juce::TextEditor::backgroundColourId, AppTheme::warningAmber.withAlpha(0.10f));
    attachmentWarningBox_.setColour(juce::TextEditor::outlineColourId, AppTheme::warningAmber.withAlpha(0.55f));
    attachmentWarningBox_.setColour(juce::TextEditor::focusedOutlineColourId, AppTheme::warningAmber.withAlpha(0.75f));
    attachmentWarningBox_.setVisible(false);
    addChildComponent(attachmentWarningBox_);

    // Tracks viewport
    tracksViewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(tracksViewport_);

    refreshSongList();
}

LibraryView::~LibraryView() {
    flushPendingAutoSave();
    stopTimer();
}

void LibraryView::refreshSongList() {
    allSongs_ = db_.listSongs();
    applySongFilter(true);
}

void LibraryView::applySongFilter(bool allowAutoSelectFirst) {
    const std::string query = toLowerNorm(searchEditor_.getText().toStdString());
    if (query.empty()) {
        songs_ = allSongs_;
    } else {
        auto tokens = splitSearchTokens(query);
        std::vector<Song> prefixMatches;
        std::vector<Song> titleMatches;
        std::vector<Song> otherMatches;

        for (const auto& s : allSongs_) {
            const std::string lowerTitle = toLowerNorm(s.title);
            const std::string lowerHaystack = toLowerNorm(s.title + " " + s.artist + " " + s.key + " " + s.notes);

            bool allTokensMatch = true;
            for (const auto& tok : tokens) {
                if (lowerHaystack.find(tok) == std::string::npos) {
                    allTokensMatch = false;
                    break;
                }
            }
            if (!allTokensMatch) continue;

            if (lowerTitle.rfind(query, 0) == 0) {
                prefixMatches.push_back(s);
            } else if (lowerTitle.find(query) != std::string::npos) {
                titleMatches.push_back(s);
            } else {
                otherMatches.push_back(s);
            }
        }

        songs_.clear();
        songs_.reserve(prefixMatches.size() + titleMatches.size() + otherMatches.size());
        songs_.insert(songs_.end(), prefixMatches.begin(), prefixMatches.end());
        songs_.insert(songs_.end(), titleMatches.begin(), titleMatches.end());
        songs_.insert(songs_.end(), otherMatches.begin(), otherMatches.end());
    }

    songListBox_.updateContent();

    if (allSongs_.empty()) {
        songListBox_.deselectAllRows();
        if (currentSong_.has_value()) {
            clearEditor();
        }
        songListBox_.repaint();
        return;
    }

    int visibleMatchIdx = -1;
    bool currentExistsInLibrary = false;
    if (currentSong_.has_value() && currentSong_->id.has_value()) {
        for (size_t i = 0; i < songs_.size(); ++i) {
            if (songs_[i].id.has_value() && *songs_[i].id == *currentSong_->id) {
                visibleMatchIdx = static_cast<int>(i);
                break;
            }
        }
        for (const auto& s : allSongs_) {
            if (s.id.has_value() && *s.id == *currentSong_->id) {
                currentExistsInLibrary = true;
                break;
            }
        }
    }

    if (visibleMatchIdx >= 0) {
        if (songListBox_.getSelectedRow() != visibleMatchIdx) {
            juce::SparseSet<int> sel;
            sel.addRange({ visibleMatchIdx, visibleMatchIdx + 1 });
            songListBox_.setSelectedRows(sel, juce::dontSendNotification);
        }
    } else if (currentExistsInLibrary) {
        // Current song still exists in library, just hidden by current search filter
        songListBox_.deselectAllRows();
    } else if (allowAutoSelectFirst) {
        if (!songs_.empty()) {
            currentSong_ = songs_[0];
            juce::SparseSet<int> sel;
            sel.addRange({ 0, 1 });
            songListBox_.setSelectedRows(sel, juce::dontSendNotification);
            songListBox_.scrollToEnsureRowIsOnscreen(0);
            loadSongToEditor(*currentSong_);
        } else {
            currentSong_ = allSongs_[0];
            songListBox_.deselectAllRows();
            loadSongToEditor(*currentSong_);
        }
    } else {
        songListBox_.deselectAllRows();
    }

    songListBox_.repaint();
}

void LibraryView::reloadAfterImport() {
    stopTimer();
    hasPendingAutoSave_ = false;
    searchEditor_.setText("", false);
    searchClearButton_.setVisible(false);
    clearEditor();
    songListBox_.deselectAllRows();
    refreshSongList();
    resized();
}

void LibraryView::flushPendingAutoSave() {
    if (isTimerRunning()) {
        stopTimer();
    }
    if (hasPendingAutoSave_) {
        saveCurrentSong(false, true);
    }
}

void LibraryView::timerCallback() {
    stopTimer();
    if (hasPendingAutoSave_) {
        saveCurrentSong(false, false);
    }
}

void LibraryView::scheduleAutoSave() {
    hasPendingAutoSave_ = true;
    setAutoSaveStatus("Saving...", AppTheme::textMuted);
    startTimer(450);
}

void LibraryView::setAutoSaveStatus(const juce::String& text, juce::Colour colour) {
    autoSaveStatusLabel_.setText(text, juce::dontSendNotification);
    autoSaveStatusLabel_.setColour(juce::Label::textColourId, colour);
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
    if (lastRowSelected < 0 || lastRowSelected >= static_cast<int>(songs_.size())) return;

    const auto targetId = songs_[static_cast<size_t>(lastRowSelected)].id;
    if (currentSong_.has_value() && currentSong_->id.has_value() && targetId.has_value() &&
        *currentSong_->id == *targetId) {
        return;
    }

    if (hasPendingAutoSave_) {
        flushPendingAutoSave();
    }

    if (targetId.has_value()) {
        for (size_t i = 0; i < songs_.size(); ++i) {
            if (songs_[i].id.has_value() && *songs_[i].id == *targetId) {
                currentSong_ = songs_[i];
                juce::SparseSet<int> sel;
                sel.addRange({ static_cast<int>(i), static_cast<int>(i) + 1 });
                songListBox_.setSelectedRows(sel, juce::dontSendNotification);
                loadSongToEditor(*currentSong_);
                return;
            }
        }
    }

    if (lastRowSelected >= 0 && lastRowSelected < static_cast<int>(songs_.size())) {
        currentSong_ = songs_[static_cast<size_t>(lastRowSelected)];
        loadSongToEditor(*currentSong_);
    }
}

void LibraryView::loadSongToEditor(const Song& song) {
    isLoadingToEditor_ = true;
    stopTimer();
    hasPendingAutoSave_ = false;

    headerLabel_.setText("Edit Song: " + song.title, juce::dontSendNotification);
    titleEditor_.setText(song.title, false);
    artistEditor_.setText(song.artist, false);
    tempoEditor_.setText(juce::String(song.tempo), false);

    keyBox_.setText(song.key, juce::dontSendNotification);
    timeSigBox_.setText(song.timeSignature, juce::dontSendNotification);
    notesEditor_.setText(song.notes, false);

    pendingArrangements_ = song.arrangements;
    if (pendingArrangements_.empty() && (!song.tracks.empty() || !song.sections.empty() || !song.tempoPoints.empty())) {
        SongArrangement fallback;
        fallback.name = "Main";
        fallback.tempo = song.tempo;
        fallback.key = song.key;
        fallback.timeSignature = song.timeSignature;
        fallback.tracks = song.tracks;
        fallback.sections = song.sections;
        fallback.tempoPoints = song.tempoPoints;
        pendingArrangements_.push_back(std::move(fallback));
    }
    removedArrangementIds_.clear();
    selectedArrangementIndex_ = pendingArrangements_.empty() ? -1 : 0;
    tracksModified_ = false;
    setAttachmentWarning("");
    refreshArrangementPicker();
    updateAttachmentStatusAndButtons();
    rebuildTrackWidgets();
    setAutoSaveStatus("Saved", AppTheme::textMuted.withAlpha(0.75f));
    isLoadingToEditor_ = false;
}

void LibraryView::clearEditor() {
    isLoadingToEditor_ = true;
    stopTimer();
    hasPendingAutoSave_ = false;

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
    setAttachmentWarning("");
    refreshArrangementPicker();
    updateAttachmentStatusAndButtons();
    rebuildTrackWidgets();
    setAutoSaveStatus("", AppTheme::textMuted);
    isLoadingToEditor_ = false;
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
        attachmentStatusLabel_.setText("", juce::dontSendNotification);
        return;
    }

    const auto& arr = pendingArrangements_[static_cast<size_t>(selectedArrangementIndex_)];

    juce::String status = arr.tracks.empty()
                              ? "No audio tracks (click/guide only)"
                              : (juce::String(arr.tracks.size()) + " audio file(s)");
    status += "  |  " + juce::String(arr.tempo) + " BPM, Key: " + juce::String(arr.key.empty() ? "C" : arr.key) +
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
    if (!arr.tempoPoints.empty()) {
        status += "  |  " + juce::String(arr.tempoPoints.size()) +
                  (arr.tempoPoints.size() == 1 ? " tempo change" : " tempo changes");
    }
    attachmentStatusLabel_.setText(status, juce::dontSendNotification);
}

void LibraryView::setAttachmentWarning(const juce::String& warningText) {
    const bool show = warningText.trim().isNotEmpty();
    attachmentWarningBox_.setText(warningText, false);
    attachmentWarningBox_.setCaretPosition(0);
    if (attachmentWarningBox_.isVisible() != show) {
        attachmentWarningBox_.setVisible(show);
        resized();
    }
}

void LibraryView::handleRemoveArrangement() {
    if (selectedArrangementIndex_ < 0 ||
        selectedArrangementIndex_ >= static_cast<int>(pendingArrangements_.size())) {
        return;
    }

    const int targetIdx = selectedArrangementIndex_;
    const auto& targetArr = pendingArrangements_[static_cast<size_t>(targetIdx)];
    const std::optional<int64_t> targetId = targetArr.id;
    const juce::String arrName = targetArr.name.empty() ? "Main" : juce::String(targetArr.name);

    juce::String details = "Are you sure you want to remove the arrangement \"" + arrName + "\"?";
    if (!targetArr.tracks.empty()) {
        details += "\n\nThis will also permanently delete its " + juce::String(targetArr.tracks.size()) +
                   " attached audio file(s), along with any sections and tempo changes.";
    } else if (!targetArr.sections.empty() || !targetArr.tempoPoints.empty()) {
        details += "\n\nThis will also remove its sections and tempo map.";
    }

    juce::Component::SafePointer<LibraryView> safeThis(this);
    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::WarningIcon,
        "Remove Arrangement",
        details,
        "Remove",
        "Cancel",
        this,
        juce::ModalCallbackFunction::create([safeThis, targetIdx, targetId](int result) {
            if (safeThis == nullptr || result == 0) return;

            int removeIdx = -1;
            if (targetId.has_value()) {
                for (size_t i = 0; i < safeThis->pendingArrangements_.size(); ++i) {
                    if (safeThis->pendingArrangements_[i].id.has_value() &&
                        *safeThis->pendingArrangements_[i].id == *targetId) {
                        removeIdx = static_cast<int>(i);
                        break;
                    }
                }
            } else if (targetIdx >= 0 && targetIdx < static_cast<int>(safeThis->pendingArrangements_.size())) {
                removeIdx = targetIdx;
            }
            if (removeIdx < 0) return;

            safeThis->stopTimer();
            safeThis->setAttachmentWarning("");
            const auto& removed = safeThis->pendingArrangements_[static_cast<size_t>(removeIdx)];
            if (removed.id.has_value()) {
                safeThis->removedArrangementIds_.push_back(*removed.id);
            }
            safeThis->pendingArrangements_.erase(safeThis->pendingArrangements_.begin() + removeIdx);
            if (safeThis->selectedArrangementIndex_ >= static_cast<int>(safeThis->pendingArrangements_.size())) {
                safeThis->selectedArrangementIndex_ = static_cast<int>(safeThis->pendingArrangements_.size()) - 1;
            }
            safeThis->tracksModified_ = true;
            safeThis->saveCurrentSong(true, false);
        }));
}

void LibraryView::handleChooseFolder() {
    flushPendingAutoSave();
    openArrangementDialog(std::nullopt);
}

void LibraryView::handleEditArrangement() {
    if (selectedArrangementIndex_ < 0 ||
        selectedArrangementIndex_ >= static_cast<int>(pendingArrangements_.size())) {
        return;
    }
    flushPendingAutoSave();
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
              sections_(initialArr.sections),
              tempoPoints_(initialArr.tempoPoints),
              onConfirm_(std::move(onConfirm)) {
            sortAndReindexTempoPoints();
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
                (editMode && !initialArr.tracks.empty())
                    ? "Keep current audio files, browse to replace, or Clear for click-only..."
                    : "Optional - leave blank for click/guide-only arrangement...",
                AppTheme::textMuted.withAlpha(0.6f));
            dirEditor_.onTextChange = [this]() {
                const bool empty = dirEditor_.getText().trim().isEmpty();
                if (empty && !originalArr_.tracks.empty()) {
                    tracksCleared_ = true;
                    setStatusMessage("No audio tracks (click/guide-only arrangement)", AppTheme::textMuted, false);
                } else if (empty) {
                    setStatusMessage("No audio tracks (click/guide-only arrangement)", AppTheme::textMuted, false);
                }
                clearDirButton_.setEnabled(!empty || (!tracksCleared_ && !originalArr_.tracks.empty()));
                confirmButton_.setEnabled(true);
            };
            addAndMakeVisible(dirEditor_);

            browseButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
            browseButton_.onClick = [this]() { openFolderChooser(); };
            addAndMakeVisible(browseButton_);

            clearDirButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
            clearDirButton_.setTooltip("Remove audio tracks from this arrangement");
            clearDirButton_.setEnabled(initialDir.isNotEmpty() || !initialArr.tracks.empty());
            clearDirButton_.onClick = [this]() {
                tracksCleared_ = true;
                dirEditor_.setText("", false);
                clearDirButton_.setEnabled(false);
                confirmButton_.setEnabled(true);
                setStatusMessage("Audio tracks cleared (click/guide-only arrangement)", AppTheme::textMuted, false);
            };
            addAndMakeVisible(clearDirButton_);

            setupLbl(tempoLabel_);
            tempoEditor_.setJustification(juce::Justification::centred);
            tempoEditor_.setInputRestrictions(3, "0123456789");
            tempoEditor_.setText(juce::String(initialArr.tempo > 0 ? initialArr.tempo : 120), false);
            tempoEditor_.onTextChange = [this]() { updateSummary(); };
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

            statusBox_.setMultiLine(true, true);
            statusBox_.setReadOnly(true);
            statusBox_.setScrollbarsShown(true);
            statusBox_.setCaretVisible(false);
            statusBox_.setFont(juce::Font(11.5f, juce::Font::plain));
            addAndMakeVisible(statusBox_);
            if (isEditMode_ && !originalArr_.tracks.empty()) {
                setStatusMessage(juce::String(originalArr_.tracks.size()) + " existing audio file(s) attached (browse to replace, or Clear)",
                                 AppTheme::textMuted,
                                 false);
            } else {
                setStatusMessage("No audio tracks (click/guide-only arrangement - browse to add stems)",
                                 AppTheme::textMuted,
                                 false);
            }

            // Tab buttons: Track Sections | Tempo Map
            sectionsTabButton_.onClick = [this]() { setActiveTab(0); };
            addAndMakeVisible(sectionsTabButton_);

            tempoTabButton_.onClick = [this]() { setActiveTab(1); };
            addAndMakeVisible(tempoTabButton_);

            summaryLabel_.setFont(juce::Font(11.5f, juce::Font::bold));
            summaryLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
            summaryLabel_.setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(summaryLabel_);

            auto setupColHdr = [this](juce::Label& l, juce::Justification just) {
                l.setFont(juce::Font(11.0f, juce::Font::bold));
                l.setColour(juce::Label::textColourId, AppTheme::textMuted);
                l.setJustificationType(just);
                addAndMakeVisible(l);
            };
            setupColHdr(colNumLabel_, juce::Justification::centredLeft);
            setupColHdr(colNameLabel_, juce::Justification::centredLeft);
            setupColHdr(colBarsLabel_, juce::Justification::centredRight);

            setupColHdr(tempoColNumLabel_, juce::Justification::centredLeft);
            setupColHdr(tempoColPosLabel_, juce::Justification::centredLeft);
            setupColHdr(tempoColBpmLabel_, juce::Justification::centredLeft);
            setupColHdr(tempoColTransLabel_, juce::Justification::centredRight);

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

            tempoRowsContent_ = std::make_unique<TempoRowsContainer>(*this);
            tempoViewport_.setViewedComponent(tempoRowsContent_.get(), false);
            tempoViewport_.setScrollBarsShown(true, false);
            tempoViewport_.setScrollBarThickness(8);
            addAndMakeVisible(tempoViewport_);

            sectionErrorLabel_.setFont(juce::Font(11.0f, juce::Font::plain));
            sectionErrorLabel_.setColour(juce::Label::textColourId, AppTheme::dangerRed);
            addAndMakeVisible(sectionErrorLabel_);

            tempoStatusLabel_.setFont(juce::Font(11.0f, juce::Font::plain));
            tempoStatusLabel_.setColour(juce::Label::textColourId, AppTheme::dangerRed);
            addAndMakeVisible(tempoStatusLabel_);

            moveUpButton_.onClick = [this]() {
                hideDropdown();
                commitActiveEditIfValid();
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
                commitActiveEditIfValid();
                if (selectedRow_ < 0 || selectedRow_ >= static_cast<int>(sections_.size()) - 1) return;
                std::swap(sections_[static_cast<size_t>(selectedRow_)], sections_[static_cast<size_t>(selectedRow_ + 1)]);
                reindexSections();
                selectedRow_++;
                refreshRowsLayout(false);
                updateSummary();
            };
            addAndMakeVisible(moveDownButton_);

            removeSectionButton_.setColour(juce::TextButton::buttonColourId, AppTheme::dangerRed);
            removeSectionButton_.onClick = [this]() {
                hideDropdown();
                cancelInlineEdit(false);
                if (selectedRow_ < 0 || selectedRow_ >= static_cast<int>(sections_.size())) return;
                sections_.erase(sections_.begin() + selectedRow_);
                reindexSections();
                selectedRow_ = -1;
                refreshRowsLayout(false);
                updateSummary();
            };
            addAndMakeVisible(removeSectionButton_);

            sectionHintLabel_.setFont(juce::Font(11.0f, juce::Font::italic));
            sectionHintLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted.withAlpha(0.8f));
            sectionHintLabel_.setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(sectionHintLabel_);

            // Tempo Map tab controls
            importMidiButton_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue.withAlpha(0.85f));
            importMidiButton_.onClick = [this]() { openMidiTempoChooser(); };
            addAndMakeVisible(importMidiButton_);

            removeTempoButton_.setColour(juce::TextButton::buttonColourId, AppTheme::dangerRed);
            removeTempoButton_.onClick = [this]() {
                cancelTempoInlineEdit(false);
                if (selectedTempoRow_ < 0 || selectedTempoRow_ >= static_cast<int>(tempoPoints_.size())) return;
                tempoPoints_.erase(tempoPoints_.begin() + selectedTempoRow_);
                sortAndReindexTempoPoints();
                selectedTempoRow_ = -1;
                refreshTempoRowsLayout(false);
                updateSummary();
            };
            addAndMakeVisible(removeTempoButton_);

            clearTempoButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
            clearTempoButton_.onClick = [this]() {
                cancelTempoInlineEdit(false);
                tempoPoints_.clear();
                selectedTempoRow_ = -1;
                tempoStatusLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
                tempoStatusLabel_.setText("Cleared tempo map (using constant base tempo).", juce::dontSendNotification);
                refreshTempoRowsLayout(false);
                updateSummary();
            };
            addAndMakeVisible(clearTempoButton_);

            tempoHintLabel_.setFont(juce::Font(11.0f, juce::Font::italic));
            tempoHintLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted.withAlpha(0.8f));
            tempoHintLabel_.setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(tempoHintLabel_);

            cancelButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel);
            cancelButton_.onClick = [this]() { closeDialog(); };
            addAndMakeVisible(cancelButton_);

            confirmButton_.setButtonText(isEditMode_ ? "Save Changes" : "Add Arrangement");
            confirmButton_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
            confirmButton_.setEnabled(true);
            confirmButton_.onClick = [this]() { confirmSelection(); };
            addAndMakeVisible(confirmButton_);

            dropdownOverlay_ = std::make_unique<DropdownOverlay>(*this);
            dropdownOverlay_->setAlwaysOnTop(true);
            addChildComponent(dropdownOverlay_.get());

            setActiveTab(0);
            updateSummary();
            setSize(620, 630);
            refreshRowsLayout(true);
            refreshTempoRowsLayout(true);
        }

        void paint(juce::Graphics& g) override {
            g.fillAll(AppTheme::bgDark);

            if (dividerY_ > 0) {
                g.setColour(AppTheme::borderCol);
                g.drawHorizontalLine(dividerY_, 16.0f, static_cast<float>(getWidth() - 16));
            }

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

            auto row0 = area.removeFromTop(28);
            nameLabel_.setBounds(row0.removeFromLeft(96));
            nameEditor_.setBounds(row0);

            area.removeFromTop(10);

            auto row1 = area.removeFromTop(28);
            dirLabel_.setBounds(row1.removeFromLeft(96));
            clearDirButton_.setBounds(row1.removeFromRight(58));
            row1.removeFromRight(6);
            browseButton_.setBounds(row1.removeFromRight(84));
            row1.removeFromRight(8);
            dirEditor_.setBounds(row1);

            area.removeFromTop(10);

            auto row2 = area.removeFromTop(28);
            tempoLabel_.setBounds(row2.removeFromLeft(96));
            tempoEditor_.setBounds(row2.removeFromLeft(66));
            row2.removeFromLeft(16);
            keyLabel_.setBounds(row2.removeFromLeft(36));
            keyBox_.setBounds(row2.removeFromLeft(84));
            row2.removeFromLeft(16);
            timeSigLabel_.setBounds(row2.removeFromLeft(70));
            timeSigBox_.setBounds(row2);

            area.removeFromTop(6);
            statusBox_.setBounds(area.removeFromTop(statusExpanded_ ? 96 : 24));

            area.removeFromTop(10);
            dividerY_ = area.getY();
            area.removeFromTop(10);

            // Tabs row (Track Sections | Tempo Map) + right-aligned summary
            auto tabRow = area.removeFromTop(26);
            sectionsTabButton_.setBounds(tabRow.removeFromLeft(142));
            tabRow.removeFromLeft(6);
            tempoTabButton_.setBounds(tabRow.removeFromLeft(134));
            tabRow.removeFromLeft(8);
            summaryLabel_.setBounds(tabRow);

            area.removeFromTop(6);

            auto colRow = area.removeFromTop(18);
            {
                auto secCols = colRow;
                colNumLabel_.setBounds(secCols.removeFromLeft(34));
                auto barsHdrArea = secCols.removeFromRight(180);
                colBarsLabel_.setBounds(barsHdrArea.withTrimmedRight(10));
                colNameLabel_.setBounds(secCols);
            }
            {
                auto tpCols = colRow;
                tempoColNumLabel_.setBounds(tpCols.removeFromLeft(34));
                tempoColPosLabel_.setBounds(tpCols.removeFromLeft(152));
                tempoColBpmLabel_.setBounds(tpCols.removeFromLeft(116));
                tempoColTransLabel_.setBounds(tpCols.withTrimmedRight(10));
            }

            area.removeFromTop(4);

            // Bottom action buttons
            auto bottomRow = area.removeFromBottom(32);
            confirmButton_.setBounds(bottomRow.removeFromRight(136));
            bottomRow.removeFromRight(8);
            cancelButton_.setBounds(bottomRow.removeFromRight(84));

            area.removeFromBottom(12);

            // Per-tab toolbar row
            auto editRow = area.removeFromBottom(26);
            {
                auto secTools = editRow;
                moveUpButton_.setBounds(secTools.removeFromLeft(82));
                secTools.removeFromLeft(6);
                moveDownButton_.setBounds(secTools.removeFromLeft(90));
                secTools.removeFromLeft(6);
                removeSectionButton_.setBounds(secTools.removeFromLeft(78));
                sectionHintLabel_.setBounds(secTools);
            }
            {
                auto tpTools = editRow;
                importMidiButton_.setBounds(tpTools.removeFromLeft(118));
                tpTools.removeFromLeft(6);
                removeTempoButton_.setBounds(tpTools.removeFromLeft(76));
                tpTools.removeFromLeft(6);
                clearTempoButton_.setBounds(tpTools.removeFromLeft(76));
                tempoHintLabel_.setBounds(tpTools);
            }

            area.removeFromBottom(4);
            auto errRow = area.removeFromBottom(16);
            sectionErrorLabel_.setBounds(errRow);
            tempoStatusLabel_.setBounds(errRow);
            area.removeFromBottom(6);

            listFrameBounds_ = area;
            viewport_.setBounds(area.reduced(1));
            tempoViewport_.setBounds(area.reduced(1));
            refreshRowsLayout(false);
            refreshTempoRowsLayout(false);
            if (dropdownOverlay_ && dropdownOverlay_->isVisible()) {
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
            explicit DropdownOverlay(ArrangementDialogContent& owner) : owner_(owner) {
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
            ArrangementDialogContent& owner_;
            juce::ListBox list_;
            std::vector<std::string> items_;
            int highlightedRow_ = -1;
        };

        class RowsContainer : public juce::Component {
        public:
            explicit RowsContainer(ArrangementDialogContent& owner) : owner_(owner) {
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
                        if (owner_.editingRow_ >= 0) {
                            owner_.cancelInlineEdit(true);
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
                        owner_.submitInlineRow();
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
                    owner_.submitInlineRow();
                };
                barsEditor_.onEscapeKey = [this]() {
                    if (owner_.editingRow_ >= 0) {
                        owner_.cancelInlineEdit(true);
                    }
                };
                addAndMakeVisible(barsEditor_);

                barsUnitLabel_.setFont(juce::Font(11.0f, juce::Font::plain));
                barsUnitLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
                barsUnitLabel_.setJustificationType(juce::Justification::centredLeft);
                addAndMakeVisible(barsUnitLabel_);

                addBtn_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
                addBtn_.onClick = [this]() {
                    owner_.submitInlineRow();
                };
                addAndMakeVisible(addBtn_);
            }

            std::string getFilterText() const { return filterEditor_.getText().trim().toStdString(); }
            void setFilterText(const std::string& text) { filterEditor_.setText(text, false); }
            int getBarsValue() const { return barsEditor_.getText().trim().getIntValue(); }
            void setBarsValue(int bars) { barsEditor_.setText(juce::String(std::max(1, bars)), false); }

            void setEditModeVisual(bool isEditingExisting) {
                addBtn_.setButtonText(isEditingExisting ? "Save" : "Add");
                addBtn_.setColour(juce::TextButton::buttonColourId,
                                  isEditingExisting ? AppTheme::accentGreen : AppTheme::primaryBlue);
            }

            void focusFilterEditor(bool selectAll = false) {
                filterEditor_.grabKeyboardFocus();
                if (selectAll) {
                    filterEditor_.selectAll();
                }
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
                const int activeRow = (owner_.editingRow_ >= 0 && owner_.editingRow_ < n) ? owner_.editingRow_ : n;
                const int y = activeRow * kRowHeight;
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
                const int n = static_cast<int>(owner_.sections_.size());
                int startBar = 1;

                for (int i = 0; i < n; ++i) {
                    const int y = i * kRowHeight;
                    const auto& sec = owner_.sections_[static_cast<size_t>(i)];
                    const int len = std::max(1, sec.lengthBars);
                    const int endBar = startBar + len - 1;
                    const bool isEditingThis = (i == owner_.editingRow_);

                    if (isEditingThis) {
                        g.setColour(AppTheme::primaryBlue.withAlpha(0.18f));
                        g.fillRect(0, y, w, kRowHeight);
                    } else if (i == owner_.selectedRow_) {
                        g.setColour(AppTheme::primaryBlue.withAlpha(0.25f));
                        g.fillRect(0, y, w, kRowHeight);
                    } else if (i % 2 == 1) {
                        g.setColour(AppTheme::bgDark.withAlpha(0.28f));
                        g.fillRect(0, y, w, kRowHeight);
                    }

                    g.setFont(juce::Font(12.0f, juce::Font::bold));
                    g.setColour(isEditingThis ? AppTheme::accentGreen : AppTheme::primaryBlue);
                    g.drawText(juce::String(i + 1) + ".", 8, y, 26, kRowHeight, juce::Justification::centredLeft);

                    if (!isEditingThis) {
                        g.setFont(juce::Font(12.5f, juce::Font::bold));
                        g.setColour(sec.isCountSection() ? AppTheme::warningAmber : AppTheme::textLight);
                        g.drawText(sec.name, 34, y, w - 210, kRowHeight, juce::Justification::centredLeft, true);

                        juce::String barInfo = juce::String(len) + (len == 1 ? " bar" : " bars") +
                                               "  (Bars " + juce::String(startBar) + "-" + juce::String(endBar) + ")";
                        g.setFont(juce::Font(11.5f, juce::Font::plain));
                        g.setColour(AppTheme::textMuted);
                        g.drawText(barInfo, w - 180, y, 170, kRowHeight, juce::Justification::centredRight, true);
                    }

                    g.setColour(AppTheme::borderCol.withAlpha(0.55f));
                    g.drawHorizontalLine(y + kRowHeight - 1, 0.0f, static_cast<float>(w));

                    startBar += len;
                }

                // Bottom "next available row"
                const int nextY = n * kRowHeight;
                const bool editorAtBottom = (owner_.editingRow_ < 0 || owner_.editingRow_ >= n);
                g.setColour(AppTheme::primaryBlue.withAlpha(editorAtBottom ? 0.08f : 0.04f));
                g.fillRect(0, nextY, w, kRowHeight);
                g.setFont(juce::Font(12.0f, juce::Font::bold));
                g.setColour(AppTheme::primaryBlue.brighter(0.2f));
                g.drawText(juce::String(n + 1) + ".", 8, nextY, 26, kRowHeight, juce::Justification::centredLeft);
                if (!editorAtBottom) {
                    g.setFont(juce::Font(11.5f, juce::Font::italic));
                    g.setColour(AppTheme::textMuted.withAlpha(0.8f));
                    g.drawText("Click here to add a new section...", 34, nextY, w - 44, kRowHeight,
                               juce::Justification::centredLeft, true);
                }
                g.setColour(AppTheme::borderCol.withAlpha(0.45f));
                g.drawHorizontalLine(nextY + kRowHeight - 1, 0.0f, static_cast<float>(w));
            }

            void mouseDown(const juce::MouseEvent& e) override {
                owner_.hideDropdown();
                const int n = static_cast<int>(owner_.sections_.size());
                const int clickedRow = e.y / kRowHeight;
                if (clickedRow >= 0 && clickedRow < n) {
                    const bool clickedBarsCol = (e.x >= getWidth() - 185);
                    owner_.beginInlineEdit(clickedRow, clickedBarsCol);
                } else {
                    owner_.commitActiveEditIfValid();
                    owner_.selectedRow_ = -1;
                    owner_.refreshRowsLayout(false);
                    if (clickedRow == n) {
                        focusFilterEditor(false);
                    }
                }
            }

            void resized() override {
                layoutInlineRow();
            }

        private:
            ArrangementDialogContent& owner_;
            FilterTextEditor filterEditor_;
            DropdownToggleButton dropdownBtn_;
            juce::TextEditor barsEditor_;
            juce::Label barsUnitLabel_{{}, "bars"};
            juce::TextButton addBtn_{"Add"};
        };

        class TempoRowsContainer : public juce::Component {
        public:
            explicit TempoRowsContainer(ArrangementDialogContent& owner) : owner_(owner) {
                auto setupTinyLbl = [this](juce::Label& l) {
                    l.setFont(juce::Font(11.0f, juce::Font::plain));
                    l.setColour(juce::Label::textColourId, AppTheme::textMuted);
                    l.setJustificationType(juce::Justification::centredRight);
                    addAndMakeVisible(l);
                };
                setupTinyLbl(barLbl_);
                setupTinyLbl(beatLbl_);
                setupTinyLbl(bpmLbl_);

                barEd_.setJustification(juce::Justification::centred);
                barEd_.setInputRestrictions(4, "0123456789");
                barEd_.setTextToShowWhenEmpty("Bar", AppTheme::textMuted.withAlpha(0.55f));
                barEd_.onReturnKey = [this]() { owner_.submitTempoInlineRow(); };
                barEd_.onEscapeKey = [this]() {
                    if (owner_.editingTempoRow_ >= 0) owner_.cancelTempoInlineEdit(true);
                };
                addAndMakeVisible(barEd_);

                beatEd_.setJustification(juce::Justification::centred);
                beatEd_.setInputRestrictions(5, "0123456789.");
                beatEd_.setText("1", false);
                beatEd_.onReturnKey = [this]() { owner_.submitTempoInlineRow(); };
                beatEd_.onEscapeKey = [this]() {
                    if (owner_.editingTempoRow_ >= 0) owner_.cancelTempoInlineEdit(true);
                };
                addAndMakeVisible(beatEd_);

                bpmEd_.setJustification(juce::Justification::centred);
                bpmEd_.setInputRestrictions(6, "0123456789.");
                bpmEd_.setTextToShowWhenEmpty("BPM", AppTheme::textMuted.withAlpha(0.55f));
                bpmEd_.onReturnKey = [this]() { owner_.submitTempoInlineRow(); };
                bpmEd_.onEscapeKey = [this]() {
                    if (owner_.editingTempoRow_ >= 0) owner_.cancelTempoInlineEdit(true);
                };
                addAndMakeVisible(bpmEd_);

                curveBox_.addItem("Ramp", 1);
                curveBox_.addItem("Step", 2);
                curveBox_.setSelectedId(1, juce::dontSendNotification);
                curveBox_.onChange = [this]() { updateRampEditorState(); };
                addAndMakeVisible(curveBox_);

                rampBarsEd_.setJustification(juce::Justification::centred);
                rampBarsEd_.setInputRestrictions(5, "0123456789.");
                rampBarsEd_.setText("4", false);
                rampBarsEd_.onReturnKey = [this]() { owner_.submitTempoInlineRow(); };
                rampBarsEd_.onEscapeKey = [this]() {
                    if (owner_.editingTempoRow_ >= 0) owner_.cancelTempoInlineEdit(true);
                };
                addAndMakeVisible(rampBarsEd_);

                barsUnitLbl_.setFont(juce::Font(11.0f, juce::Font::plain));
                barsUnitLbl_.setColour(juce::Label::textColourId, AppTheme::textMuted);
                barsUnitLbl_.setJustificationType(juce::Justification::centredLeft);
                addAndMakeVisible(barsUnitLbl_);

                addBtn_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
                addBtn_.onClick = [this]() { owner_.submitTempoInlineRow(); };
                addAndMakeVisible(addBtn_);

                updateRampEditorState();
            }

            static juce::String formatCleanDecimal(double v) {
                double rounded = std::round(v * 100.0) / 100.0;
                if (std::abs(rounded - std::round(rounded)) < 1e-4) {
                    return juce::String(static_cast<int>(std::llround(rounded)));
                }
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(2) << rounded;
                std::string s = oss.str();
                while (!s.empty() && s.back() == '0') s.pop_back();
                if (!s.empty() && s.back() == '.') s.pop_back();
                return juce::String(s);
            }

            bool hasDraftEntry() const {
                return barEd_.getText().trim().isNotEmpty() && bpmEd_.getText().trim().isNotEmpty();
            }
            int getBarValue() const { return barEd_.getText().trim().getIntValue(); }
            double getBeatValue() const {
                auto t = beatEd_.getText().trim();
                return t.isEmpty() ? 1.0 : t.getDoubleValue();
            }
            double getBpmValue() const { return bpmEd_.getText().trim().getDoubleValue(); }
            bool isRampSelected() const { return curveBox_.getSelectedId() == 1; }
            double getRampBarsValue() const {
                if (!isRampSelected()) return 0.0;
                return rampBarsEd_.getText().trim().getDoubleValue();
            }

            void setPointValues(const TempoPoint& pt) {
                barEd_.setText(juce::String(std::max(1, pt.bar)), false);
                beatEd_.setText(formatCleanDecimal(std::max(1.0, pt.beat)), false);
                bpmEd_.setText(formatCleanDecimal(pt.bpm), false);
                if (pt.isRamp()) {
                    curveBox_.setSelectedId(1, juce::dontSendNotification);
                    rampBarsEd_.setText(formatCleanDecimal(pt.rampBars), false);
                } else {
                    curveBox_.setSelectedId(2, juce::dontSendNotification);
                    rampBarsEd_.setText("0", false);
                }
                updateRampEditorState();
            }

            void resetToNewEntryDefaults() {
                barEd_.setText("", false);
                beatEd_.setText("1", false);
                bpmEd_.setText("", false);
                curveBox_.setSelectedId(1, juce::dontSendNotification);
                rampBarsEd_.setText("4", false);
                updateRampEditorState();
                setEditModeVisual(false);
            }

            void setEditModeVisual(bool isEditingExisting) {
                addBtn_.setButtonText(isEditingExisting ? "Save" : "Add");
                addBtn_.setColour(juce::TextButton::buttonColourId,
                                  isEditingExisting ? AppTheme::accentGreen : AppTheme::primaryBlue);
            }

            void focusField(int col) {
                if (col == 1) {
                    bpmEd_.grabKeyboardFocus();
                    bpmEd_.selectAll();
                } else if (col == 2 && isRampSelected()) {
                    rampBarsEd_.grabKeyboardFocus();
                    rampBarsEd_.selectAll();
                } else {
                    barEd_.grabKeyboardFocus();
                    barEd_.selectAll();
                }
            }

            void updateRampEditorState() {
                const bool ramp = isRampSelected();
                rampBarsEd_.setEnabled(ramp);
                rampBarsEd_.setAlpha(ramp ? 1.0f : 0.38f);
                barsUnitLbl_.setAlpha(ramp ? 1.0f : 0.38f);
                if (ramp && rampBarsEd_.getText().trim().getDoubleValue() <= 0.0) {
                    rampBarsEd_.setText("4", false);
                }
            }

            void layoutInlineRow() {
                const int n = static_cast<int>(owner_.tempoPoints_.size());
                const int activeRow = (owner_.editingTempoRow_ >= 0 && owner_.editingTempoRow_ < n) ? owner_.editingTempoRow_ : n;
                const int y = activeRow * kRowHeight;
                auto row = juce::Rectangle<int>(0, y, getWidth(), kRowHeight).reduced(6, 4);

                row.removeFromLeft(26); // Room for row number "N."
                barLbl_.setBounds(row.removeFromLeft(24));
                row.removeFromLeft(2);
                barEd_.setBounds(row.removeFromLeft(44));
                row.removeFromLeft(6);

                beatLbl_.setBounds(row.removeFromLeft(30));
                row.removeFromLeft(2);
                beatEd_.setBounds(row.removeFromLeft(38));
                row.removeFromLeft(8);

                bpmLbl_.setBounds(row.removeFromLeft(30));
                row.removeFromLeft(2);
                bpmEd_.setBounds(row.removeFromLeft(52));
                row.removeFromLeft(8);

                auto addBounds = row.removeFromRight(58);
                row.removeFromRight(6);
                auto unitBounds = row.removeFromRight(32);
                row.removeFromRight(3);
                auto rampBounds = row.removeFromRight(42);
                row.removeFromRight(6);
                auto curveBounds = row;

                curveBox_.setBounds(curveBounds);
                rampBarsEd_.setBounds(rampBounds);
                barsUnitLbl_.setBounds(unitBounds);
                addBtn_.setBounds(addBounds);
            }

            void paint(juce::Graphics& g) override {
                const int w = getWidth();
                const int n = static_cast<int>(owner_.tempoPoints_.size());

                for (int i = 0; i < n; ++i) {
                    const int y = i * kRowHeight;
                    const auto& pt = owner_.tempoPoints_[static_cast<size_t>(i)];
                    const bool isEditingThis = (i == owner_.editingTempoRow_);

                    if (isEditingThis) {
                        g.setColour(AppTheme::primaryBlue.withAlpha(0.18f));
                        g.fillRect(0, y, w, kRowHeight);
                    } else if (i == owner_.selectedTempoRow_) {
                        g.setColour(AppTheme::primaryBlue.withAlpha(0.25f));
                        g.fillRect(0, y, w, kRowHeight);
                    } else if (i % 2 == 1) {
                        g.setColour(AppTheme::bgDark.withAlpha(0.28f));
                        g.fillRect(0, y, w, kRowHeight);
                    }

                    g.setFont(juce::Font(12.0f, juce::Font::bold));
                    g.setColour(isEditingThis ? AppTheme::accentGreen : AppTheme::primaryBlue);
                    g.drawText(juce::String(i + 1) + ".", 8, y, 26, kRowHeight, juce::Justification::centredLeft);

                    if (!isEditingThis) {
                        juce::String posText = "Bar " + juce::String(pt.bar) + ", Beat " + formatCleanDecimal(pt.beat);
                        g.setFont(juce::Font(12.5f, juce::Font::bold));
                        g.setColour(AppTheme::textLight);
                        g.drawText(posText, 34, y, 152, kRowHeight, juce::Justification::centredLeft, true);

                        juce::String bpmText = juce::String::fromUTF8("\xE2\x86\x92 ") + formatCleanDecimal(pt.bpm) + " BPM";
                        g.setFont(juce::Font(12.5f, juce::Font::bold));
                        g.setColour(AppTheme::warningAmber);
                        g.drawText(bpmText, 186, y, 116, kRowHeight, juce::Justification::centredLeft, true);

                        juce::String transText;
                        if (pt.isRamp()) {
                            transText = "Ramp over " + formatCleanDecimal(pt.rampBars) +
                                        (std::abs(pt.rampBars - 1.0) < 1e-4 ? " bar" : " bars");
                            g.setColour(AppTheme::accentGreen);
                        } else {
                            transText = "Instant (Step)";
                            g.setColour(AppTheme::textMuted);
                        }
                        g.setFont(juce::Font(11.5f, juce::Font::plain));
                        g.drawText(transText, 304, y, std::max(0, w - 314), kRowHeight, juce::Justification::centredRight, true);
                    }

                    g.setColour(AppTheme::borderCol.withAlpha(0.55f));
                    g.drawHorizontalLine(y + kRowHeight - 1, 0.0f, static_cast<float>(w));
                }

                // Bottom "next available row"
                const int nextY = n * kRowHeight;
                const bool editorAtBottom = (owner_.editingTempoRow_ < 0 || owner_.editingTempoRow_ >= n);
                g.setColour(AppTheme::primaryBlue.withAlpha(editorAtBottom ? 0.08f : 0.04f));
                g.fillRect(0, nextY, w, kRowHeight);
                g.setFont(juce::Font(12.0f, juce::Font::bold));
                g.setColour(AppTheme::primaryBlue.brighter(0.2f));
                g.drawText(juce::String(n + 1) + ".", 8, nextY, 26, kRowHeight, juce::Justification::centredLeft);
                if (!editorAtBottom) {
                    g.setFont(juce::Font(11.5f, juce::Font::italic));
                    g.setColour(AppTheme::textMuted.withAlpha(0.8f));
                    g.drawText("Click here to add a new tempo change...", 34, nextY, w - 44, kRowHeight,
                               juce::Justification::centredLeft, true);
                }
                g.setColour(AppTheme::borderCol.withAlpha(0.45f));
                g.drawHorizontalLine(nextY + kRowHeight - 1, 0.0f, static_cast<float>(w));
            }

            void mouseDown(const juce::MouseEvent& e) override {
                const int n = static_cast<int>(owner_.tempoPoints_.size());
                const int clickedRow = e.y / kRowHeight;
                if (clickedRow >= 0 && clickedRow < n) {
                    int focusCol = 0;
                    if (e.x >= 186 && e.x < 304) focusCol = 1;
                    else if (e.x >= 304) focusCol = 2;
                    owner_.beginTempoInlineEdit(clickedRow, focusCol);
                } else {
                    owner_.commitTempoEditIfValid();
                    owner_.selectedTempoRow_ = -1;
                    owner_.refreshTempoRowsLayout(false);
                    if (clickedRow == n) {
                        focusField(0);
                    }
                }
            }

            void resized() override {
                layoutInlineRow();
            }

        private:
            ArrangementDialogContent& owner_;
            juce::Label barLbl_{{}, "Bar"};
            juce::TextEditor barEd_;
            juce::Label beatLbl_{{}, "Beat"};
            juce::TextEditor beatEd_;
            juce::Label bpmLbl_{{}, "BPM"};
            juce::TextEditor bpmEd_;
            juce::ComboBox curveBox_;
            juce::TextEditor rampBarsEd_;
            juce::Label barsUnitLbl_{{}, "bars"};
            juce::TextButton addBtn_{"Add"};
        };

        bool isEditMode_ = false;
        SongArrangement originalArr_;
        juce::String initialDir_;
        std::vector<SongSection> sections_;
        std::vector<TempoPoint> tempoPoints_;
        int activeTab_ = 0; // 0 = Track Sections, 1 = Tempo Map
        int selectedRow_ = -1;
        int editingRow_ = -1;
        int selectedTempoRow_ = -1;
        int editingTempoRow_ = -1;
        int dividerY_ = 0;
        juce::Rectangle<int> listFrameBounds_;

        juce::Label nameLabel_{{}, "Arrangement:"};
        juce::TextEditor nameEditor_;

        juce::Label dirLabel_{{}, "Audio Folder:"};
        juce::TextEditor dirEditor_;
        juce::TextButton browseButton_{"Browse..."};
        juce::TextButton clearDirButton_{"Clear"};
        bool tracksCleared_ = false;

        juce::Label tempoLabel_{{}, "Tempo (BPM):"};
        juce::TextEditor tempoEditor_;

        juce::Label keyLabel_{{}, "Key:"};
        juce::ComboBox keyBox_;

        juce::Label timeSigLabel_{{}, "Time Sig:"};
        juce::ComboBox timeSigBox_;

        juce::TextEditor statusBox_;
        bool statusExpanded_ = false;

        juce::TextButton sectionsTabButton_{"Track Sections"};
        juce::TextButton tempoTabButton_{"Tempo Map"};
        juce::Label summaryLabel_;

        juce::Label colNumLabel_{{}, "#"};
        juce::Label colNameLabel_{{}, "Section Name"};
        juce::Label colBarsLabel_{{}, "Length"};

        juce::Label tempoColNumLabel_{{}, "#"};
        juce::Label tempoColPosLabel_{{}, "Start Position"};
        juce::Label tempoColBpmLabel_{{}, "Target BPM"};
        juce::Label tempoColTransLabel_{{}, "Transition"};

        SectionListViewport viewport_;
        std::unique_ptr<RowsContainer> rowsContent_;
        std::unique_ptr<DropdownOverlay> dropdownOverlay_;

        juce::Viewport tempoViewport_;
        std::unique_ptr<TempoRowsContainer> tempoRowsContent_;

        juce::Label sectionErrorLabel_;
        juce::TextButton moveUpButton_{"Move Up"};
        juce::TextButton moveDownButton_{"Move Down"};
        juce::TextButton removeSectionButton_{"Remove"};
        juce::Label sectionHintLabel_{{}, "Click any section to edit"};

        juce::Label tempoStatusLabel_;
        juce::TextButton importMidiButton_{"Import MIDI..."};
        juce::TextButton removeTempoButton_{"Remove"};
        juce::TextButton clearTempoButton_{"Clear All"};
        juce::Label tempoHintLabel_{{}, "Click any point to edit"};

        juce::TextButton cancelButton_{"Cancel"};
        juce::TextButton confirmButton_{"Add Arrangement"};

        std::unique_ptr<juce::FileChooser> chooser_;
        std::unique_ptr<juce::FileChooser> midiChooser_;
        std::function<void(SongArrangement, bool, const juce::String&)> onConfirm_;

        void setActiveTab(int tab) {
            if (tab != activeTab_) {
                hideDropdown();
                commitActiveEditIfValid();
                commitTempoEditIfValid();
            }
            activeTab_ = tab;
            const bool showSections = (activeTab_ == 0);

            sectionsTabButton_.setColour(juce::TextButton::buttonColourId,
                                         showSections ? AppTheme::primaryBlue : AppTheme::bgPanel);
            tempoTabButton_.setColour(juce::TextButton::buttonColourId,
                                      !showSections ? AppTheme::primaryBlue : AppTheme::bgPanel);

            colNumLabel_.setVisible(showSections);
            colNameLabel_.setVisible(showSections);
            colBarsLabel_.setVisible(showSections);
            viewport_.setVisible(showSections);
            sectionErrorLabel_.setVisible(showSections);
            moveUpButton_.setVisible(showSections);
            moveDownButton_.setVisible(showSections);
            removeSectionButton_.setVisible(showSections);
            sectionHintLabel_.setVisible(showSections);

            tempoColNumLabel_.setVisible(!showSections);
            tempoColPosLabel_.setVisible(!showSections);
            tempoColBpmLabel_.setVisible(!showSections);
            tempoColTransLabel_.setVisible(!showSections);
            tempoViewport_.setVisible(!showSections);
            tempoStatusLabel_.setVisible(!showSections);
            importMidiButton_.setVisible(!showSections);
            removeTempoButton_.setVisible(!showSections);
            clearTempoButton_.setVisible(!showSections);
            tempoHintLabel_.setVisible(!showSections);

            updateSummary();
            if (showSections) refreshRowsLayout(false);
            else refreshTempoRowsLayout(false);
        }

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
            sectionErrorLabel_.setText("", juce::dontSendNotification);
            std::string raw = rowsContent_->getFilterText();
            dropdownOverlay_->setItems(filterAvailableSections(raw), findCanonicalSectionName(raw));
            positionDropdown();
            dropdownOverlay_->setVisible(true);
            dropdownOverlay_->toFront(false);
        }

        void commitDropdownSelection(const std::string& canonicalName) {
            if (!rowsContent_) return;
            rowsContent_->setFilterText(canonicalName);
            sectionErrorLabel_.setText("", juce::dontSendNotification);
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
            const int totalRows = static_cast<int>(sections_.size()) + 1;
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

        void beginInlineEdit(int rowIdx, bool focusBars) {
            if (!rowsContent_ || rowIdx < 0 || rowIdx >= static_cast<int>(sections_.size())) return;
            if (editingRow_ >= 0 && editingRow_ != rowIdx) {
                commitActiveEditIfValid();
            }

            selectedRow_ = rowIdx;
            editingRow_ = rowIdx;
            const auto& sec = sections_[static_cast<size_t>(rowIdx)];
            rowsContent_->setFilterText(sec.name);
            rowsContent_->setBarsValue(sec.lengthBars);
            rowsContent_->setEditModeVisual(true);
            sectionErrorLabel_.setText("", juce::dontSendNotification);
            refreshRowsLayout(false);

            if (focusBars) {
                rowsContent_->focusBarsEditorAndSelectAll();
            } else {
                rowsContent_->focusFilterEditor(true);
            }
        }

        void cancelInlineEdit(bool refreshUi) {
            if (editingRow_ < 0) return;
            editingRow_ = -1;
            hideDropdown();
            sectionErrorLabel_.setText("", juce::dontSendNotification);
            if (rowsContent_) {
                rowsContent_->setFilterText("");
                rowsContent_->setBarsValue(4);
                rowsContent_->setEditModeVisual(false);
            }
            if (refreshUi) {
                refreshRowsLayout(false);
            }
        }

        void commitActiveEditIfValid() {
            if (editingRow_ < 0 || editingRow_ >= static_cast<int>(sections_.size()) || !rowsContent_) {
                editingRow_ = -1;
                return;
            }
            std::string secName = findCanonicalSectionName(rowsContent_->getFilterText());
            int bars = rowsContent_->getBarsValue();
            if (!secName.empty() && bars > 0) {
                sections_[static_cast<size_t>(editingRow_)].name = secName;
                sections_[static_cast<size_t>(editingRow_)].lengthBars = bars;
                updateSummary();
            }
            cancelInlineEdit(false);
        }

        void submitInlineRow() {
            if (!rowsContent_) return;
            std::string rawName = rowsContent_->getFilterText();
            std::string secName = findCanonicalSectionName(rawName);
            int bars = rowsContent_->getBarsValue();

            if (secName.empty()) {
                sectionErrorLabel_.setText("Select a valid section from the filtered list.", juce::dontSendNotification);
                openDropdownForCurrentText(false);
                rowsContent_->focusFilterEditor(false);
                return;
            }
            if (bars <= 0) {
                sectionErrorLabel_.setText("Enter a section length of at least 1 bar.", juce::dontSendNotification);
                rowsContent_->focusBarsEditorAndSelectAll();
                return;
            }

            sectionErrorLabel_.setText("", juce::dontSendNotification);
            hideDropdown();

            if (editingRow_ >= 0 && editingRow_ < static_cast<int>(sections_.size())) {
                sections_[static_cast<size_t>(editingRow_)].name = secName;
                sections_[static_cast<size_t>(editingRow_)].lengthBars = bars;
                selectedRow_ = editingRow_;
                editingRow_ = -1;
                rowsContent_->setFilterText("");
                rowsContent_->setBarsValue(4);
                rowsContent_->setEditModeVisual(false);
                refreshRowsLayout(false);
                updateSummary();
                return;
            }

            SongSection sec;
            sec.name = secName;
            sec.lengthBars = bars;
            sec.orderIndex = static_cast<int>(sections_.size());
            sections_.push_back(std::move(sec));

            selectedRow_ = static_cast<int>(sections_.size()) - 1;
            rowsContent_->setFilterText("");
            refreshRowsLayout(true);
            updateSummary();
            rowsContent_->focusFilterEditor(false);
        }

        void reindexSections() {
            for (size_t i = 0; i < sections_.size(); ++i) {
                sections_[i].orderIndex = static_cast<int>(i);
            }
        }

        void updateSummary() {
            sectionsTabButton_.setButtonText(
                sections_.empty()
                    ? "Track Sections"
                    : ("Track Sections (" + juce::String(sections_.size()) + ")"));
            tempoTabButton_.setButtonText(
                tempoPoints_.empty()
                    ? "Tempo Map"
                    : ("Tempo Map (" + juce::String(tempoPoints_.size()) + ")"));

            if (activeTab_ == 0) {
                int totalBars = 0;
                for (const auto& s : sections_) {
                    totalBars += std::max(1, s.lengthBars);
                }
                summaryLabel_.setText(juce::String(sections_.size()) +
                                          (sections_.size() == 1 ? " section" : " sections") +
                                          "  -  Total: " + juce::String(totalBars) +
                                          (totalBars == 1 ? " bar" : " bars"),
                                      juce::dontSendNotification);
            } else {
                int baseTempo = tempoEditor_.getText().trim().getIntValue();
                if (baseTempo <= 0) baseTempo = 120;
                juce::String txt = "Base: " + juce::String(baseTempo) + " BPM  -  ";
                if (tempoPoints_.empty()) {
                    txt += "Constant tempo";
                } else {
                    txt += juce::String(tempoPoints_.size()) +
                           (tempoPoints_.size() == 1 ? " change" : " changes");
                }
                summaryLabel_.setText(txt, juce::dontSendNotification);
            }
        }

        void sortAndReindexTempoPoints() {
            std::stable_sort(tempoPoints_.begin(), tempoPoints_.end(),
                             [](const TempoPoint& a, const TempoPoint& b) {
                                 if (a.bar != b.bar) return a.bar < b.bar;
                                 return a.beat < b.beat;
                             });
            for (size_t i = 0; i < tempoPoints_.size(); ++i) {
                tempoPoints_[i].orderIndex = static_cast<int>(i);
            }
        }

        void refreshTempoRowsLayout(bool scrollToBottom) {
            if (!tempoRowsContent_) return;
            const int totalRows = static_cast<int>(tempoPoints_.size()) + 1;
            const int neededH = std::max(tempoViewport_.getHeight(), totalRows * kRowHeight);
            const int scrollBarW = (totalRows * kRowHeight > tempoViewport_.getHeight()) ? tempoViewport_.getScrollBarThickness() : 0;
            const int contentW = std::max(100, tempoViewport_.getWidth() - scrollBarW);
            tempoRowsContent_->setSize(contentW, neededH);
            tempoRowsContent_->layoutInlineRow();
            tempoRowsContent_->repaint();

            if (scrollToBottom) {
                int maxScrollY = std::max(0, totalRows * kRowHeight - tempoViewport_.getHeight());
                tempoViewport_.setViewPosition(0, maxScrollY);
            }
        }

        void beginTempoInlineEdit(int rowIdx, int focusCol) {
            if (!tempoRowsContent_ || rowIdx < 0 || rowIdx >= static_cast<int>(tempoPoints_.size())) return;
            if (editingTempoRow_ >= 0 && editingTempoRow_ != rowIdx) {
                commitTempoEditIfValid();
            }
            selectedTempoRow_ = rowIdx;
            editingTempoRow_ = rowIdx;
            tempoRowsContent_->setPointValues(tempoPoints_[static_cast<size_t>(rowIdx)]);
            tempoRowsContent_->setEditModeVisual(true);
            tempoStatusLabel_.setText("", juce::dontSendNotification);
            refreshTempoRowsLayout(false);
            tempoRowsContent_->focusField(focusCol);
        }

        void cancelTempoInlineEdit(bool refreshUi) {
            if (editingTempoRow_ < 0) return;
            editingTempoRow_ = -1;
            tempoStatusLabel_.setText("", juce::dontSendNotification);
            if (tempoRowsContent_) {
                tempoRowsContent_->resetToNewEntryDefaults();
            }
            if (refreshUi) {
                refreshTempoRowsLayout(false);
            }
        }

        int getCurrentPulseCountPerBar() const {
            int bpb = 4, div = 4, pulses = 4;
            bool comp = false;
            TempoMap::parseTimeSignature(timeSigBox_.getText().toStdString(), bpb, div, comp, pulses);
            return std::max(1, pulses);
        }

        void commitTempoEditIfValid() {
            if (editingTempoRow_ < 0 || editingTempoRow_ >= static_cast<int>(tempoPoints_.size()) || !tempoRowsContent_) {
                editingTempoRow_ = -1;
                return;
            }
            const int bar = tempoRowsContent_->getBarValue();
            const double beat = tempoRowsContent_->getBeatValue();
            const double bpm = tempoRowsContent_->getBpmValue();
            const double rampBars = tempoRowsContent_->isRampSelected() ? tempoRowsContent_->getRampBarsValue() : 0.0;
            const int maxBeat = getCurrentPulseCountPerBar();

            if (bar >= 1 && beat >= 1.0 && beat < static_cast<double>(maxBeat) + 1.0 &&
                bpm >= 20.0 && bpm <= 300.0 &&
                (!tempoRowsContent_->isRampSelected() || rampBars > 0.0)) {
                auto& pt = tempoPoints_[static_cast<size_t>(editingTempoRow_)];
                pt.bar = bar;
                pt.beat = beat;
                pt.bpm = bpm;
                pt.rampBars = rampBars;
                sortAndReindexTempoPoints();
                updateSummary();
            }
            cancelTempoInlineEdit(false);
        }

        void submitTempoInlineRow() {
            if (!tempoRowsContent_) return;
            const int bar = tempoRowsContent_->getBarValue();
            const double beat = tempoRowsContent_->getBeatValue();
            const double bpm = tempoRowsContent_->getBpmValue();
            const bool isRamp = tempoRowsContent_->isRampSelected();
            const double rampBars = isRamp ? tempoRowsContent_->getRampBarsValue() : 0.0;
            const int maxBeat = getCurrentPulseCountPerBar();

            tempoStatusLabel_.setColour(juce::Label::textColourId, AppTheme::dangerRed);
            if (bar < 1) {
                tempoStatusLabel_.setText("Enter a start bar of 1 or higher.", juce::dontSendNotification);
                tempoRowsContent_->focusField(0);
                return;
            }
            if (beat < 1.0 || beat >= static_cast<double>(maxBeat) + 1.0) {
                tempoStatusLabel_.setText("Beat must be between 1 and " + juce::String(maxBeat) + ".", juce::dontSendNotification);
                return;
            }
            if (bpm < 20.0 || bpm > 300.0) {
                tempoStatusLabel_.setText("Target BPM must be between 20 and 300.", juce::dontSendNotification);
                tempoRowsContent_->focusField(1);
                return;
            }
            if (isRamp && rampBars <= 0.0) {
                tempoStatusLabel_.setText("Ramp length must be greater than 0 bars (or switch to Step).", juce::dontSendNotification);
                tempoRowsContent_->focusField(2);
                return;
            }

            tempoStatusLabel_.setText("", juce::dontSendNotification);

            if (editingTempoRow_ >= 0 && editingTempoRow_ < static_cast<int>(tempoPoints_.size())) {
                auto& pt = tempoPoints_[static_cast<size_t>(editingTempoRow_)];
                pt.bar = bar;
                pt.beat = beat;
                pt.bpm = bpm;
                pt.rampBars = rampBars;
                editingTempoRow_ = -1;
                sortAndReindexTempoPoints();
                tempoRowsContent_->resetToNewEntryDefaults();
                refreshTempoRowsLayout(false);
                updateSummary();
                return;
            }

            TempoPoint pt;
            pt.bar = bar;
            pt.beat = beat;
            pt.bpm = bpm;
            pt.rampBars = rampBars;
            tempoPoints_.push_back(pt);
            sortAndReindexTempoPoints();
            selectedTempoRow_ = static_cast<int>(tempoPoints_.size()) - 1;
            tempoRowsContent_->resetToNewEntryDefaults();
            refreshTempoRowsLayout(true);
            updateSummary();
            tempoRowsContent_->focusField(0);
        }

        void openMidiTempoChooser() {
            midiChooser_ = std::make_unique<juce::FileChooser>(
                "Import MIDI Tempo Map",
                juce::File::getSpecialLocation(juce::File::userHomeDirectory),
                "*.mid;*.midi");
            auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

            juce::Component::SafePointer<ArrangementDialogContent> safeThis(this);
            midiChooser_->launchAsync(flags, [safeThis](const juce::FileChooser& fc) {
                if (safeThis == nullptr) return;
                auto file = fc.getResult();
                if (!file.existsAsFile()) return;

                int fallbackBase = safeThis->tempoEditor_.getText().trim().getIntValue();
                if (fallbackBase <= 0) fallbackBase = 120;
                auto res = importMidiTempoMapFromFile(file.getFullPathName().toStdString(),
                                                      safeThis->timeSigBox_.getText().toStdString(),
                                                      fallbackBase);
                if (!res.success) {
                    safeThis->tempoStatusLabel_.setColour(juce::Label::textColourId, AppTheme::dangerRed);
                    safeThis->tempoStatusLabel_.setText(res.message, juce::dontSendNotification);
                    return;
                }

                safeThis->cancelTempoInlineEdit(false);
                if (res.detectedBaseTempo.has_value()) {
                    safeThis->tempoEditor_.setText(juce::String(*res.detectedBaseTempo), false);
                }
                safeThis->tempoPoints_ = std::move(res.points);
                safeThis->sortAndReindexTempoPoints();
                safeThis->selectedTempoRow_ = -1;
                safeThis->tempoStatusLabel_.setColour(juce::Label::textColourId, AppTheme::accentGreen);
                safeThis->tempoStatusLabel_.setText(res.message, juce::dontSendNotification);
                safeThis->refreshTempoRowsLayout(false);
                safeThis->updateSummary();
            });
        }

        void setStatusMessage(const juce::String& text, juce::Colour colour, bool expanded) {
            statusBox_.setColour(juce::TextEditor::textColourId, colour);
            if (expanded) {
                statusBox_.setColour(juce::TextEditor::backgroundColourId, colour.withAlpha(0.10f));
                statusBox_.setColour(juce::TextEditor::outlineColourId, colour.withAlpha(0.55f));
                statusBox_.setColour(juce::TextEditor::focusedOutlineColourId, colour.withAlpha(0.75f));
            } else {
                statusBox_.setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
                statusBox_.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
                statusBox_.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
            }
            statusBox_.setText(text, false);
            statusBox_.setCaretPosition(0);
            if (statusExpanded_ != expanded) {
                statusExpanded_ = expanded;
                resized();
                repaint();
            }
        }

        static juce::String formatAllMismatches(const std::vector<std::string>& mismatches) {
            juce::String out;
            for (size_t i = 0; i < mismatches.size(); ++i) {
                if (i > 0) out += "\n";
                out += juce::String(mismatches[i]);
            }
            return out;
        }

        void openFolderChooser() {
            hideDropdown();
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

                safeThis->tracksCleared_ = false;
                safeThis->dirEditor_.setText(result.getFullPathName(), false);
                safeThis->clearDirButton_.setEnabled(true);
                auto insp = AudioAttachmentService::inspectAudioDirectory(result.getFullPathName().toStdString());
                if (insp.error.has_value()) {
                    safeThis->setStatusMessage(*insp.error, AppTheme::dangerRed, true);
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
                        msg += "\n" + formatAllMismatches(insp.mismatches);
                        safeThis->setStatusMessage(msg, AppTheme::warningAmber, true);
                    } else {
                        safeThis->setStatusMessage(msg, AppTheme::accentGreen, false);
                    }
                    safeThis->confirmButton_.setEnabled(true);
                }
            });
        }

        void confirmSelection() {
            hideDropdown();
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

            juce::String warningStatus;
            bool filesReplacedOrRenamed = (isEditMode_ && arr.name != originalArr_.name);

            if (dirText.isEmpty()) {
                if (!isEditMode_ || tracksCleared_) {
                    if (!arr.tracks.empty()) {
                        filesReplacedOrRenamed = true;
                    }
                    arr.tracks.clear();
                } else {
                    for (auto& track : arr.tracks) {
                        track.trackKey = arr.key;
                    }
                }
            } else if (dirChanged) {
                auto insp = AudioAttachmentService::inspectAudioDirectory(dirText.toStdString());
                if (insp.error.has_value()) {
                    setStatusMessage(*insp.error, AppTheme::dangerRed, true);
                    return;
                }
                for (auto& track : insp.tracks) {
                    track.trackKey = arr.key;
                }
                arr.tracks = std::move(insp.tracks);
                filesReplacedOrRenamed = true;

                if (!insp.mismatches.empty()) {
                    warningStatus = juce::String(arr.tracks.size()) + " audio file(s) attached to \"" +
                                    juce::String(arr.name) + "\"\n" + formatAllMismatches(insp.mismatches);
                }
            } else {
                for (auto& track : arr.tracks) {
                    track.trackKey = arr.key;
                }
            }

            // Finalize any in-progress inline section edit or pending bottom row entry
            if (editingRow_ >= 0 && editingRow_ < static_cast<int>(sections_.size())) {
                commitActiveEditIfValid();
            } else if (rowsContent_) {
                std::string pendingName = findCanonicalSectionName(rowsContent_->getFilterText());
                int pendingBars = rowsContent_->getBarsValue();
                if (!pendingName.empty() && pendingBars > 0) {
                    SongSection sec;
                    sec.name = pendingName;
                    sec.lengthBars = pendingBars;
                    sec.orderIndex = static_cast<int>(sections_.size());
                    sections_.push_back(std::move(sec));
                }
            }
            reindexSections();
            arr.sections = sections_;

            // Finalize any in-progress tempo map inline edit or valid bottom row entry
            if (editingTempoRow_ >= 0 && editingTempoRow_ < static_cast<int>(tempoPoints_.size())) {
                commitTempoEditIfValid();
            } else if (tempoRowsContent_ && tempoRowsContent_->hasDraftEntry()) {
                const int bar = tempoRowsContent_->getBarValue();
                const double beat = tempoRowsContent_->getBeatValue();
                const double bpm = tempoRowsContent_->getBpmValue();
                const bool isRamp = tempoRowsContent_->isRampSelected();
                const double rampBars = isRamp ? tempoRowsContent_->getRampBarsValue() : 0.0;
                const int maxBeat = getCurrentPulseCountPerBar();
                if (bar >= 1 && beat >= 1.0 && beat < static_cast<double>(maxBeat) + 1.0 &&
                    bpm >= 20.0 && bpm <= 300.0 && (!isRamp || rampBars > 0.0)) {
                    TempoPoint pt;
                    pt.bar = bar;
                    pt.beat = beat;
                    pt.bpm = bpm;
                    pt.rampBars = rampBars;
                    tempoPoints_.push_back(pt);
                }
            }
            sortAndReindexTempoPoints();
            arr.tempoPoints = tempoPoints_;

            if (onConfirm_) {
                onConfirm_(std::move(arr), filesReplacedOrRenamed, warningStatus);
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
        [safeView, editIndex, isEditMode](SongArrangement updatedArr, bool filesChanged, const juce::String& warningStatus) {
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
            safeView->stopTimer();
            safeView->saveCurrentSong(true, false);
            safeView->setAttachmentWarning(warningStatus);
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

void LibraryView::saveCurrentSong(bool refreshTrackPanel, bool forceFallbackForEmptyFields) {
    Song s;
    const bool isNewSong = !(currentSong_ && currentSong_->id.has_value());
    const std::string previousTitle = currentSong_ ? currentSong_->title : "";

    std::string rawTitle = titleEditor_.getText().trim().toStdString();
    std::string rawArtist = artistEditor_.getText().trim().toStdString();
    std::string rawNotes = notesEditor_.getText().toStdString();
    int rawTempo = tempoEditor_.getText().trim().getIntValue();

    // When library is completely empty and user hasn't entered anything yet, don't create an empty phantom song
    if (isNewSong && !forceFallbackForEmptyFields && rawTitle.empty() && rawArtist.empty() &&
        rawNotes.empty() && pendingArrangements_.empty()) {
        hasPendingAutoSave_ = false;
        setAutoSaveStatus("", AppTheme::textMuted);
        return;
    }

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

    if (rawTitle.empty()) {
        if (!forceFallbackForEmptyFields && titleEditor_.hasKeyboardFocus(true) && !previousTitle.empty()) {
            s.title = previousTitle;
        } else {
            s.title = "Untitled Song";
            if (!titleEditor_.hasKeyboardFocus(true)) {
                isLoadingToEditor_ = true;
                titleEditor_.setText(s.title, false);
                isLoadingToEditor_ = false;
            }
        }
    } else {
        s.title = rawTitle;
    }

    s.artist = rawArtist;
    if (rawTempo <= 0) {
        if (!forceFallbackForEmptyFields && tempoEditor_.hasKeyboardFocus(true) && currentSong_.has_value() && currentSong_->tempo > 0) {
            s.tempo = currentSong_->tempo;
        } else {
            s.tempo = 120;
            if (!tempoEditor_.hasKeyboardFocus(true)) {
                isLoadingToEditor_ = true;
                tempoEditor_.setText("120", false);
                isLoadingToEditor_ = false;
            }
        }
    } else {
        s.tempo = std::clamp(rawTempo, 20, 300);
    }

    s.key = keyBox_.getText().toStdString();
    if (s.key.empty()) s.key = "C";
    s.timeSignature = timeSigBox_.getText().toStdString();
    if (s.timeSignature.empty()) s.timeSignature = "4/4";
    s.notes = rawNotes;

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
        bool anyTracksLeft = false;
        for (auto& arr : saved.arrangements) {
            if (arr.id.has_value()) {
                if (!arr.tracks.empty()) {
                    arr.tracks = AudioAttachmentService::copyTracksToArrangementDirectory(
                        *saved.id, saved.title, *arr.id, arr.name, arr.tracks);
                    updatedPaths = true;
                    anyTracksLeft = true;
                } else {
                    AudioAttachmentService::removeArrangementStorage(*saved.id, *arr.id);
                }
            }
        }
        if (saved.arrangements.empty() || !anyTracksLeft) {
            AudioAttachmentService::removeSongStorage(*saved.id);
        } else if (titleChanged) {
            AudioAttachmentService::cleanupStaleSongDirs(*saved.id, saved.title);
        }
        if (updatedPaths) {
            saved = db_.saveSong(saved);
        }
    }

    if (saved.id.has_value() && (audioFilesChanged || refreshTrackPanel)) {
        audioEngine_.invalidateCachedSong(*saved.id);
    }

    currentSong_ = saved;
    pendingArrangements_ = saved.arrangements;
    removedArrangementIds_.clear();
    tracksModified_ = false;
    hasPendingAutoSave_ = false;

    headerLabel_.setText("Edit Song: " + saved.title, juce::dontSendNotification);

    if (refreshTrackPanel || audioFilesChanged) {
        if (previouslySelectedArrIdx >= 0 && previouslySelectedArrIdx < static_cast<int>(pendingArrangements_.size())) {
            selectedArrangementIndex_ = previouslySelectedArrIdx;
        } else {
            selectedArrangementIndex_ = pendingArrangements_.empty() ? -1 : 0;
        }
        refreshArrangementPicker();
        updateAttachmentStatusAndButtons();
        rebuildTrackWidgets();
    }

    refreshSongList();
    setAutoSaveStatus("Saved", AppTheme::accentGreen.withAlpha(0.85f));
}

void LibraryView::paint(juce::Graphics& g) {
    AppTheme::paintTintedPageBackground(g, getLocalBounds(), AppTheme::pageBgLibrary, AppTheme::tabLibraryAccent);

    // Divider line between song list and editor
    g.setColour(AppTheme::borderCol.interpolatedWith(AppTheme::tabLibraryAccent, 0.22f));
    g.drawVerticalLine(320, 0.0f, static_cast<float>(getHeight()));
}

void LibraryView::resized() {
    auto area = getLocalBounds().reduced(16);

    // Left column: Search bar + Songs list
    auto leftCol = area.removeFromLeft(290);
    auto searchRow = leftCol.removeFromTop(28);
    if (searchClearButton_.isVisible()) {
        searchClearButton_.setBounds(searchRow.removeFromRight(26));
        searchRow.removeFromRight(4);
    }
    searchEditor_.setBounds(searchRow);
    leftCol.removeFromTop(8);

    auto leftButtons = leftCol.removeFromBottom(36);
    newSongButton_.setBounds(leftButtons.removeFromLeft(140));
    deleteSongButton_.setBounds(leftButtons.removeFromRight(120));

    leftCol.removeFromBottom(12);
    songListBox_.setBounds(leftCol);

    area.removeFromLeft(24);

    // Right column: Editor
    auto rightCol = area;
    auto headerRow = rightCol.removeFromTop(28);
    autoSaveStatusLabel_.setBounds(headerRow.removeFromRight(80));
    headerLabel_.setBounds(headerRow);
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
    attachFolderButton_.setBounds(attachRow2.removeFromLeft(136));
    attachRow2.removeFromLeft(6);
    editArrangementButton_.setBounds(attachRow2.removeFromLeft(136));
    attachRow2.removeFromLeft(6);
    removeArrangementButton_.setBounds(attachRow2.removeFromLeft(84));

    rightCol.removeFromTop(4);
    attachmentStatusLabel_.setBounds(rightCol.removeFromTop(20));

    if (attachmentWarningBox_.isVisible()) {
        rightCol.removeFromTop(4);
        attachmentWarningBox_.setBounds(rightCol.removeFromTop(84));
    }

    rightCol.removeFromTop(8);
    tracksViewport_.setBounds(rightCol);
    if (tracksContainer_) {
        tracksContainer_->setSize(tracksViewport_.getWidth() - 20, tracksContainer_->getHeight());
    }
}

} // namespace tracks
