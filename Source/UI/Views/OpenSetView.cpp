#include "OpenSetView.h"
#include "../../Common/AppTheme.h"
#include "../Widgets/CalendarPopupComponent.h"
#include "../Widgets/TimePopupComponent.h"
#include <algorithm>
#include <cctype>

namespace tracks {

namespace {

std::string toLowerTrimmed(const std::string& s) {
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

std::vector<std::string> splitTokens(const std::string& norm) {
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

class OpenSetView::SongFilterEditor : public juce::TextEditor {
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

class OpenSetView::SongDropdownToggle : public juce::Button {
public:
    SongDropdownToggle() : juce::Button("SongDropdown") {
        setWantsKeyboardFocus(false);
    }

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override {
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        const bool enabled = isEnabled();
        juce::Colour bg = AppTheme::bgInput;
        if (!enabled) bg = bg.withAlpha(0.45f);
        else if (down) bg = bg.darker(0.15f);
        else if (highlighted) bg = bg.brighter(0.12f);

        g.setColour(bg);
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(enabled ? AppTheme::borderCol : AppTheme::borderCol.withAlpha(0.45f));
        g.drawRoundedRectangle(r, 3.0f, 1.0f);

        const float cx = r.getCentreX();
        const float cy = r.getCentreY();
        juce::Path tri;
        tri.addTriangle(cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.0f);
        g.setColour(enabled ? AppTheme::textLight : AppTheme::textMuted.withAlpha(0.45f));
        g.fillPath(tri);
    }
};

class OpenSetView::SongDropdownOverlay : public juce::Component, public juce::ListBoxModel {
public:
    explicit SongDropdownOverlay(OpenSetView& owner) : owner_(owner) {
        setWantsKeyboardFocus(false);
        list_.setModel(this);
        list_.setRowHeight(26);
        list_.setWantsKeyboardFocus(false);
        list_.setColour(juce::ListBox::backgroundColourId, AppTheme::bgPanel.darker(0.15f));
        addAndMakeVisible(list_);
    }

    void setItems(std::vector<LibrarySongChoice> items) {
        items_ = std::move(items);
        highlightedRow_ = items_.empty() ? -1 : 0;
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

    std::optional<int64_t> getHighlightedSongId() const {
        if (highlightedRow_ >= 0 && highlightedRow_ < static_cast<int>(items_.size())) {
            return items_[static_cast<size_t>(highlightedRow_)].songId;
        }
        return std::nullopt;
    }

    int getDesiredHeight() const {
        int rows = std::max(1, std::min(8, static_cast<int>(items_.size())));
        return rows * 26 + 4;
    }

    int getNumRows() override {
        return items_.empty() ? 1 : static_cast<int>(items_.size());
    }

    void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool) override {
        if (items_.empty()) {
            g.setFont(juce::Font(11.5f, juce::Font::italic));
            g.setColour(AppTheme::textMuted);
            g.drawText("No matching songs", 8, 0, width - 16, height, juce::Justification::centredLeft, true);
            return;
        }
        if (rowNumber < 0 || rowNumber >= static_cast<int>(items_.size())) return;

        const bool isHighlighted = (rowNumber == highlightedRow_);
        if (isHighlighted) {
            g.setColour(AppTheme::primaryBlue.withAlpha(0.35f));
            g.fillRect(0, 0, width, height);
        }

        const auto& item = items_[static_cast<size_t>(rowNumber)];
        g.setFont(juce::Font(12.0f, isHighlighted ? juce::Font::bold : juce::Font::plain));
        g.setColour(isHighlighted ? juce::Colours::white : AppTheme::textLight);
        g.drawText(item.displayLabel, 8, 0, width - 16, height, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked(int row, const juce::MouseEvent&) override {
        if (row >= 0 && row < static_cast<int>(items_.size())) {
            owner_.addSongToCurrentSet(items_[static_cast<size_t>(row)].songId);
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
    OpenSetView& owner_;
    juce::ListBox list_;
    std::vector<LibrarySongChoice> items_;
    int highlightedRow_ = -1;
};

class OpenSetView::SetlistPickerLookAndFeel : public AppTheme {
public:
    void setSubtitles(std::vector<juce::String> subtitles) {
        subtitles_ = std::move(subtitles);
    }

    void getIdealPopupMenuItemSize(const juce::String& text,
                                   bool isSeparator,
                                   int standardMenuItemHeight,
                                   int& idealWidth,
                                   int& idealHeight) override {
        AppTheme::getIdealPopupMenuItemSize(text, isSeparator, standardMenuItemHeight, idealWidth, idealHeight);
        if (!isSeparator) {
            idealHeight = 36;
            idealWidth = std::max(idealWidth + 16, 230);
        }
    }

    void drawPopupMenuItemWithOptions(juce::Graphics& g,
                                      const juce::Rectangle<int>& area,
                                      bool isHighlighted,
                                      const juce::PopupMenu::Item& item,
                                      const juce::PopupMenu::Options& options) override {
        if (item.isSeparator || item.isSectionHeader) {
            AppTheme::drawPopupMenuItemWithOptions(g, area, isHighlighted, item, options);
            return;
        }

        auto r = area.reduced(2, 1);
        if (isHighlighted && item.isEnabled) {
            g.setColour(AppTheme::primaryBlue.withAlpha(0.32f));
            g.fillRoundedRectangle(r.toFloat(), 3.0f);
        }

        auto content = r.reduced(6, 2);
        auto tickArea = content.removeFromLeft(16);
        if (item.isTicked) {
            auto tick = getTickShape(0.75f);
            g.setColour(AppTheme::primaryBlue);
            g.fillPath(tick, tick.getTransformToScaleToFit(tickArea.reduced(2, 9).toFloat(), true));
        }

        juce::String subtitle;
        const int idx = item.itemID - 1;
        if (idx >= 0 && idx < static_cast<int>(subtitles_.size())) {
            subtitle = subtitles_[static_cast<size_t>(idx)];
        }

        if (subtitle.isNotEmpty()) {
            auto titleArea = content.removeFromTop(17);
            g.setFont(juce::Font(12.5f, item.isTicked ? juce::Font::bold : juce::Font::plain));
            g.setColour(isHighlighted ? juce::Colours::white : AppTheme::textLight);
            g.drawText(item.text, titleArea, juce::Justification::centredLeft, true);

            g.setFont(juce::Font(10.0f, juce::Font::plain));
            g.setColour(isHighlighted ? AppTheme::textLight.withAlpha(0.72f) : AppTheme::textMuted);
            g.drawText(subtitle, content, juce::Justification::centredLeft, true);
        } else {
            g.setFont(juce::Font(12.5f, item.isTicked ? juce::Font::bold : juce::Font::plain));
            g.setColour(isHighlighted ? juce::Colours::white : AppTheme::textLight);
            g.drawText(item.text, content, juce::Justification::centredLeft, true);
        }

        g.setColour(AppTheme::borderCol.withAlpha(0.35f));
        g.drawHorizontalLine(area.getBottom() - 1, static_cast<float>(area.getX() + 6), static_cast<float>(area.getRight() - 6));
    }

private:
    std::vector<juce::String> subtitles_;
};

OpenSetView::OpenSetView(DatabaseService& db, AudioEngine& audioEngine)
    : db_(db), audioEngine_(audioEngine) {
    setOpaque(true);

    // Left sidebar set controls (Setlist picker, Date, Time, New/Delete)
    setSelectLabel_.setFont(juce::Font(12.0f, juce::Font::bold));
    setSelectLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
    setSelectLabel_.setJustificationType(juce::Justification::centredLeft);
    setSelectLabel_.setBorderSize(juce::BorderSize<int>(0, 0, 0, 4));
    addAndMakeVisible(setSelectLabel_);

    setPickerLookAndFeel_ = std::make_unique<SetlistPickerLookAndFeel>();
    setPicker_.setLookAndFeel(setPickerLookAndFeel_.get());
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

    auto setupSetDateTime = [this](juce::Label& lbl, juce::TextEditor& ed, const juce::String& placeholder) {
        lbl.setFont(juce::Font(12.0f, juce::Font::bold));
        lbl.setColour(juce::Label::textColourId, AppTheme::textLight);
        lbl.setJustificationType(juce::Justification::centredLeft);
        lbl.setBorderSize(juce::BorderSize<int>(0, 0, 0, 4));
        addAndMakeVisible(lbl);

        ed.setJustification(juce::Justification::centredLeft);
        ed.setTextToShowWhenEmpty(placeholder, AppTheme::textMuted.withAlpha(0.5f));
        ed.onTextChange = [this]() { saveCurrentSetlist(); };
        addAndMakeVisible(ed);
    };

    setupSetDateTime(dateLabel_, dateEditor_, "YYYY-MM-DD");
    setupSetDateTime(timeLabel_, timeEditor_, "HH:MM AM");

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
        if (!currentSetlist_ || !currentSetlist_->id.has_value()) return;

        const int64_t setId = *currentSetlist_->id;
        const juce::String setName = currentSetlist_->name.empty() ? "Untitled Set" : juce::String(currentSetlist_->name);
        juce::String details = "Are you sure you want to delete the set \"" + setName + "\"?";
        if (!currentSetlist_->entries.empty()) {
            details += "\n\nThis set contains " + juce::String(currentSetlist_->entries.size()) +
                       (currentSetlist_->entries.size() == 1 ? " song" : " songs") +
                       ". Songs in your library will not be deleted.";
        }

        juce::Component::SafePointer<OpenSetView> safeThis(this);
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::WarningIcon,
            "Delete Set",
            details,
            "Delete",
            "Cancel",
            this,
            juce::ModalCallbackFunction::create([safeThis, setId](int result) {
                if (safeThis == nullptr || result == 0) return;
                if (!safeThis->currentSetlist_ || !safeThis->currentSetlist_->id.has_value() ||
                    *safeThis->currentSetlist_->id != setId) {
                    return;
                }
                safeThis->db_.deleteSetlist(setId);
                safeThis->currentSetlist_ = std::nullopt;
                safeThis->refreshSetlists();
            }));
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

    // Filterable Add Song dropdown (type to filter, click or press Enter to add immediately)
    addSongFilterEditor_ = std::make_unique<SongFilterEditor>();
    addSongFilterEditor_->setJustification(juce::Justification::centredLeft);
    addSongFilterEditor_->setTextToShowWhenEmpty("Add song to set...", AppTheme::textMuted.withAlpha(0.65f));
    addSongFilterEditor_->onTextChange = [this]() {
        openAddSongDropdown();
    };
    addSongFilterEditor_->onFocusLost = [this]() {
        hideAddSongDropdown();
    };
    addSongFilterEditor_->onMouseClick = [this]() {
        if (addSongFilterEditor_->isEnabled()) {
            openAddSongDropdown();
        }
    };
    addSongFilterEditor_->onKeyInterceptor = [this](const juce::KeyPress& key) -> bool {
        if (key == juce::KeyPress::downKey) {
            if (!addSongDropdownOverlay_ || !addSongDropdownOverlay_->isVisible()) {
                openAddSongDropdown();
            } else {
                addSongDropdownOverlay_->moveHighlight(1);
            }
            return true;
        }
        if (key == juce::KeyPress::upKey) {
            if (addSongDropdownOverlay_ && addSongDropdownOverlay_->isVisible()) {
                addSongDropdownOverlay_->moveHighlight(-1);
                return true;
            }
        }
        if (key == juce::KeyPress::escapeKey) {
            if (addSongDropdownOverlay_ && addSongDropdownOverlay_->isVisible()) {
                hideAddSongDropdown();
                return true;
            }
        }
        if (key == juce::KeyPress::returnKey) {
            if (addSongDropdownOverlay_ && addSongDropdownOverlay_->isVisible()) {
                if (auto songId = addSongDropdownOverlay_->getHighlightedSongId(); songId.has_value()) {
                    addSongToCurrentSet(*songId);
                    return true;
                }
            }
            return true;
        }
        return false;
    };
    addAndMakeVisible(addSongFilterEditor_.get());

    addSongDropdownButton_ = std::make_unique<SongDropdownToggle>();
    addSongDropdownButton_->onClick = [this]() {
        if (!addSongDropdownOverlay_) return;
        if (addSongDropdownOverlay_->isVisible()) {
            hideAddSongDropdown();
        } else {
            if (addSongFilterEditor_) addSongFilterEditor_->grabKeyboardFocus();
            openAddSongDropdown();
        }
    };
    addAndMakeVisible(addSongDropdownButton_.get());

    addSongDropdownOverlay_ = std::make_unique<SongDropdownOverlay>(*this);
    addSongDropdownOverlay_->setAlwaysOnTop(true);
    addChildComponent(addSongDropdownOverlay_.get());

    refreshSetlists();
}

OpenSetView::~OpenSetView() {
    setPicker_.setLookAndFeel(nullptr);
}

void OpenSetView::populateAddSongPicker() {
    librarySongChoices_.clear();
    auto allSongs = db_.listSongs();
    librarySongChoices_.reserve(allSongs.size());
    for (const auto& song : allSongs) {
        if (song.id.has_value()) {
            LibrarySongChoice choice;
            choice.songId = *song.id;
            choice.title = song.title;
            choice.artist = song.artist;
            choice.displayLabel = song.title;
            if (!song.artist.empty()) {
                choice.displayLabel += " (" + song.artist + ")";
            }
            librarySongChoices_.push_back(std::move(choice));
        }
    }
    if (addSongFilterEditor_) {
        addSongFilterEditor_->setTextToShowWhenEmpty(
            librarySongChoices_.empty() ? "No songs in library" : "Add song to set...",
            AppTheme::textMuted.withAlpha(0.65f));
    }
    if (addSongDropdownOverlay_ && addSongDropdownOverlay_->isVisible()) {
        updateFilteredAddSongList();
        positionAddSongDropdown();
    }
}

void OpenSetView::updateFilteredAddSongList() {
    if (!addSongDropdownOverlay_) return;
    std::string query = addSongFilterEditor_ ? toLowerTrimmed(addSongFilterEditor_->getText().toStdString()) : "";
    if (query.empty()) {
        addSongDropdownOverlay_->setItems(librarySongChoices_);
        return;
    }

    auto tokens = splitTokens(query);
    std::vector<LibrarySongChoice> titlePrefixMatches;
    std::vector<LibrarySongChoice> titleContainsMatches;
    std::vector<LibrarySongChoice> otherMatches;

    for (const auto& choice : librarySongChoices_) {
        std::string lowerTitle = toLowerTrimmed(choice.title);
        std::string lowerFull = toLowerTrimmed(choice.displayLabel);

        bool allTokensMatch = true;
        for (const auto& tok : tokens) {
            if (lowerFull.find(tok) == std::string::npos) {
                allTokensMatch = false;
                break;
            }
        }
        if (!allTokensMatch) continue;

        if (lowerTitle.rfind(query, 0) == 0) {
            titlePrefixMatches.push_back(choice);
        } else if (lowerTitle.find(query) != std::string::npos) {
            titleContainsMatches.push_back(choice);
        } else {
            otherMatches.push_back(choice);
        }
    }

    std::vector<LibrarySongChoice> filtered;
    filtered.reserve(titlePrefixMatches.size() + titleContainsMatches.size() + otherMatches.size());
    filtered.insert(filtered.end(), titlePrefixMatches.begin(), titlePrefixMatches.end());
    filtered.insert(filtered.end(), titleContainsMatches.begin(), titleContainsMatches.end());
    filtered.insert(filtered.end(), otherMatches.begin(), otherMatches.end());
    addSongDropdownOverlay_->setItems(std::move(filtered));
}

void OpenSetView::openAddSongDropdown() {
    if (!addSongDropdownOverlay_ || !currentSetlist_) return;
    updateFilteredAddSongList();
    positionAddSongDropdown();
    addSongDropdownOverlay_->setVisible(true);
    addSongDropdownOverlay_->toFront(false);
}

void OpenSetView::hideAddSongDropdown() {
    if (addSongDropdownOverlay_) {
        addSongDropdownOverlay_->setVisible(false);
    }
}

void OpenSetView::positionAddSongDropdown() {
    if (!addSongDropdownOverlay_ || addSongBarBounds_.isEmpty()) return;
    const int desiredH = addSongDropdownOverlay_->getDesiredHeight();
    const int minTop = 16;
    const int spaceAbove = addSongBarBounds_.getY() - minTop - 2;
    const int h = std::min(desiredH, std::max(72, spaceAbove));
    const int y = std::max(minTop, addSongBarBounds_.getY() - h - 2);
    addSongDropdownOverlay_->setBounds(addSongBarBounds_.getX(), y, addSongBarBounds_.getWidth(), h);
}

void OpenSetView::addSongToCurrentSet(int64_t songId) {
    hideAddSongDropdown();
    if (addSongFilterEditor_) {
        addSongFilterEditor_->setText("", false);
    }
    if (songId <= 0 || !currentSetlist_) return;

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
}

Song OpenSetView::buildEffectiveSongForEntry(const SetlistEntry& entry) const {
    Song effectiveSong = entry.song.value_or(Song{});
    const auto* arr = entry.getSelectedArrangement();
    if (arr != nullptr) {
        effectiveSong.activeArrangementId = arr->id;
        const bool hasArrTracks = !arr->tracks.empty();
        int baseTempo = (arr->tempo > 0) ? arr->tempo : effectiveSong.tempo;
        if (entry.overrideTempo.has_value() && *entry.overrideTempo > 0) {
            effectiveSong.tempo = hasArrTracks
                                      ? clampTrackTempoForUi(*entry.overrideTempo, baseTempo)
                                      : std::clamp(*entry.overrideTempo, 20, 300);
        } else {
            effectiveSong.tempo = baseTempo;
        }
        std::string baseKey = arr->key.empty() ? effectiveSong.key : arr->key;
        effectiveSong.key = (entry.overrideKey.has_value() && !entry.overrideKey->empty()) ? *entry.overrideKey : baseKey;
        std::string baseSig = arr->timeSignature.empty() ? effectiveSong.timeSignature : arr->timeSignature;
        effectiveSong.timeSignature = (!hasArrTracks && entry.overrideTimeSignature.has_value() && !entry.overrideTimeSignature->empty())
                                          ? *entry.overrideTimeSignature
                                          : baseSig;
        effectiveSong.clickEighths = arr->clickEighths;
        effectiveSong.clickSixteenths = arr->clickSixteenths;
        effectiveSong.clickEighthsGain = arr->clickEighthsGain;
        effectiveSong.clickSixteenthsGain = arr->clickSixteenthsGain;
        effectiveSong.tracks = arr->tracks;
        effectiveSong.sections = arr->sections;
        effectiveSong.tempoPoints = arr->tempoPoints;
    } else {
        effectiveSong.activeArrangementId = std::nullopt;
        effectiveSong.tempo = entry.overrideTempo.value_or(effectiveSong.tempo);
        effectiveSong.key = entry.overrideKey.value_or(effectiveSong.key);
        effectiveSong.timeSignature = entry.overrideTimeSignature.value_or(effectiveSong.timeSignature);
        effectiveSong.tempoPoints.clear();
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
                s.tempoPoints = arr.tempoPoints;
                songsInSet.push_back(std::move(s));
            }
        } else if (!entry.song->tracks.empty()) {
            songsInSet.push_back(*entry.song);
        }
    }
    audioEngine_.preloadSetlist(*currentSetlist_->id, songsInSet);
}

void OpenSetView::populateSetPickerItems() {
    setPicker_.clear(juce::dontSendNotification);
    std::vector<juce::String> subtitles;
    subtitles.reserve(setlists_.size());
    for (size_t i = 0; i < setlists_.size(); ++i) {
        const auto& s = setlists_[i];
        juce::String dateStr = juce::String(s.setDate).trim();
        juce::String timeStr = juce::String(s.setTime).trim();
        juce::String subtitle;
        if (dateStr.isNotEmpty() && timeStr.isNotEmpty()) {
            subtitle = dateStr + "  " + timeStr;
        } else if (dateStr.isNotEmpty()) {
            subtitle = dateStr;
        } else if (timeStr.isNotEmpty()) {
            subtitle = timeStr;
        }
        subtitles.push_back(std::move(subtitle));
        setPicker_.addItem(s.name.empty() ? "Untitled Set" : s.name, static_cast<int>(i + 1));
    }
    if (setPickerLookAndFeel_) {
        setPickerLookAndFeel_->setSubtitles(std::move(subtitles));
    }
}

void OpenSetView::refreshSetlists() {
    setlists_ = db_.listSetlists();
    populateSetPickerItems();
    setPicker_.setEditableText(!setlists_.empty());
    const bool canAddSongs = !setlists_.empty();
    if (addSongFilterEditor_) addSongFilterEditor_->setEnabled(canAddSongs);
    if (addSongDropdownButton_) addSongDropdownButton_->setEnabled(canAddSongs);
    if (!canAddSongs) hideAddSongDropdown();

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
                populateSetPickerItems();
                setPicker_.setSelectedId(itemId, juce::dontSendNotification);
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

    // Title (full top row; show arrangement name when song has multiple arrangements + non-default transition tag)
    juce::String titleDisplay = songTitle;
    if (entry.song.has_value() && entry.song->arrangements.size() > 1) {
        if (const auto* arr = entry.getSelectedArrangement(); arr != nullptr && !arr->name.empty()) {
            titleDisplay += "  [" + juce::String(arr->name) + "]";
        }
    }
    juce::String transBadge;
    switch (entry.transitionMode) {
        case SongTransitionMode::CueNextSong:      transBadge = "Cue"; break;
        case SongTransitionMode::AutoplayNextSong: transBadge = "Auto"; break;
        case SongTransitionMode::Crossfade:
            transBadge = "XF " + juce::String(clampCrossfadeBars(entry.transitionCrossfadeBars)) + "b";
            break;
        case SongTransitionMode::StayInCurrentSong:
        default: break;
    }
    int titleW = contentW;
    if (transBadge.isNotEmpty()) {
        juce::Font tagFont(9.5f, juce::Font::bold);
        const int tagW = tagFont.getStringWidth(transBadge) + 10;
        juce::Rectangle<float> tagRect(static_cast<float>(contentX + contentW - tagW), 4.0f,
                                       static_cast<float>(tagW), 15.0f);
        const juce::Colour tagCol(0xff38bdf8);
        g.setColour(tagCol.withAlpha(0.16f));
        g.fillRoundedRectangle(tagRect, 3.0f);
        g.setColour(tagCol.withAlpha(0.75f));
        g.drawRoundedRectangle(tagRect, 3.0f, 1.0f);
        g.setFont(tagFont);
        g.setColour(tagCol.brighter(0.25f));
        g.drawText(transBadge, tagRect, juce::Justification::centred, false);
        titleW = std::max(0, contentW - tagW - 6);
    }
    g.setFont(juce::Font(13.0f, juce::Font::bold));
    g.setColour(AppTheme::textLight);
    g.drawText(titleDisplay, contentX, 4, titleW, 16, juce::Justification::centredLeft, true);

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
    entry.overrideTempo = std::nullopt; // Start cleanly at the newly selected arrangement's recorded tempo
    entry.overrideKey = std::nullopt;   // Start cleanly in the newly selected arrangement's recorded key
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
    audioEngine_.setTransitionConfig(getCurrentSongTransitionMode(), getCurrentSongCrossfadeBars(), canSelectNextSong());
    if (onSongNavigationStateChanged) {
        onSongNavigationStateChanged(canSelectPreviousSong(), canSelectNextSong());
    }
}

SongTransitionMode OpenSetView::getCurrentSongTransitionMode() const {
    if (!currentSetlist_ || selectedEntryIndex_ < 0 ||
        selectedEntryIndex_ >= static_cast<int>(currentSetlist_->entries.size())) {
        return SongTransitionMode::StayInCurrentSong;
    }
    return currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_)].transitionMode;
}

int OpenSetView::getCurrentSongCrossfadeBars() const {
    if (!currentSetlist_ || selectedEntryIndex_ < 0 ||
        selectedEntryIndex_ >= static_cast<int>(currentSetlist_->entries.size())) {
        return 1;
    }
    return clampCrossfadeBars(currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_)].transitionCrossfadeBars);
}

void OpenSetView::setCurrentSongTransition(SongTransitionMode mode, int crossfadeBars) {
    if (!currentSetlist_ || selectedEntryIndex_ < 0 ||
        selectedEntryIndex_ >= static_cast<int>(currentSetlist_->entries.size())) {
        return;
    }
    auto& entry = currentSetlist_->entries[static_cast<size_t>(selectedEntryIndex_)];
    entry.transitionMode = mode;
    entry.transitionCrossfadeBars = clampCrossfadeBars(crossfadeBars);
    audioEngine_.setTransitionConfig(entry.transitionMode, entry.transitionCrossfadeBars, canSelectNextSong());
    saveCurrentSetlist();
    entryListBox_.repaintRow(selectedEntryIndex_);
}

void OpenSetView::executeSongTransition(AudioEngine::TransitionTrigger trigger) {
    if (!canSelectNextSong()) {
        audioEngine_.stop();
        return;
    }

    const int nextRow = (selectedEntryIndex_ < 0) ? 0 : (selectedEntryIndex_ + 1);
    if (trigger == AudioEngine::TransitionTrigger::CrossfadeNext) {
        audioEngine_.setPreserveCrossfadeOnNextLoad(true);
        entryListBox_.selectRow(nextRow);
        audioEngine_.play();
    } else if (trigger == AudioEngine::TransitionTrigger::AutoplayNext) {
        entryListBox_.selectRow(nextRow);
        audioEngine_.play();
    } else if (trigger == AudioEngine::TransitionTrigger::CueNext) {
        entryListBox_.selectRow(nextRow);
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
        const bool hasArr = (arr != nullptr);
        const int baseTempo = (hasArr && arr->tempo > 0) ? arr->tempo : entry.song->tempo;
        const std::string baseKey = (hasArr && !arr->key.empty()) ? arr->key : entry.song->key;
        const std::string baseSig = (hasArr && !arr->timeSignature.empty()) ? arr->timeSignature : entry.song->timeSignature;

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
    AppTheme::paintTintedPageBackground(g, getLocalBounds(), AppTheme::pageBgLive, AppTheme::tabLiveAccent);
    g.setColour(AppTheme::borderCol.interpolatedWith(AppTheme::tabLiveAccent, 0.22f));
    const int setlistWidth = getWidth() / 4;
    g.drawVerticalLine(16 + setlistWidth + 10, 0.0f, static_cast<float>(getHeight()));
    if (setHeaderBottomY_ > 0) {
        g.setColour(AppTheme::borderCol.interpolatedWith(AppTheme::tabLiveAccent, 0.22f).withAlpha(0.75f));
        g.drawHorizontalLine(setHeaderBottomY_, 16.0f, static_cast<float>(16 + setlistWidth));
    }
}

void OpenSetView::mouseDown(const juce::MouseEvent&) {
    hideAddSongDropdown();
}

void OpenSetView::resized() {
    auto area = getLocalBounds().reduced(16);

    // Left Area: Set controls + Setlist songs (25% of window width)
    const int setlistWidth = getWidth() / 4;
    auto leftArea = area.removeFromLeft(setlistWidth);
    area.removeFromLeft(20);

    // Compact Set Controls block at top of left sidebar
    const int labelW = 52;
    const int pickBtnW = 44;
    const int rowGap = 6;

    auto setRow = leftArea.removeFromTop(28);
    setSelectLabel_.setBounds(setRow.removeFromLeft(labelW));
    setPicker_.setBounds(setRow);

    leftArea.removeFromTop(rowGap);

    auto dateRow = leftArea.removeFromTop(26);
    dateLabel_.setBounds(dateRow.removeFromLeft(labelW));
    datePickerButton_.setBounds(dateRow.removeFromRight(pickBtnW));
    dateRow.removeFromRight(4);
    dateEditor_.setBounds(dateRow);

    leftArea.removeFromTop(rowGap);

    auto timeRow = leftArea.removeFromTop(26);
    timeLabel_.setBounds(timeRow.removeFromLeft(labelW));
    timePickerButton_.setBounds(timeRow.removeFromRight(pickBtnW));
    timeRow.removeFromRight(4);
    timeEditor_.setBounds(timeRow);

    leftArea.removeFromTop(rowGap);

    auto setBtnRow = leftArea.removeFromTop(26);
    const int setBtnGap = 6;
    const int halfBtnW = std::max(0, (setBtnRow.getWidth() - setBtnGap) / 2);
    newSetButton_.setBounds(setBtnRow.removeFromLeft(halfBtnW));
    setBtnRow.removeFromLeft(setBtnGap);
    deleteSetButton_.setBounds(setBtnRow);

    leftArea.removeFromTop(14);
    setHeaderBottomY_ = leftArea.getY() - 7;

    // Controls below the setlist
    auto bottomControls = leftArea.removeFromBottom(68);

    auto addSongBar = bottomControls.removeFromTop(28);
    addSongBarBounds_ = addSongBar;
    auto toggleBounds = addSongBar.removeFromRight(26);
    addSongBar.removeFromRight(2);
    if (addSongFilterEditor_) addSongFilterEditor_->setBounds(addSongBar);
    if (addSongDropdownButton_) addSongDropdownButton_->setBounds(toggleBounds);
    if (addSongDropdownOverlay_ && addSongDropdownOverlay_->isVisible()) {
        positionAddSongDropdown();
    }

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
