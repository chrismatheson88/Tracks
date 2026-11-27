#include "SettingsView.h"
#include "../../Common/AppTheme.h"
#include "../../Services/AudioAttachmentService.h"
#include "../../Services/LibraryBackupService.h"
#include <cmath>

namespace tracks {

class SettingsView::MidiMappingRowComponent : public juce::Component {
public:
    MidiMappingRowComponent(const ControlActionMeta& meta,
                            std::function<void(ControlAction)> onKeyLearnClicked,
                            std::function<void(ControlAction)> onKeyClearClicked,
                            std::function<void(ControlAction)> onLearnClicked,
                            std::function<void(ControlAction)> onClearClicked)
        : action_(meta.action),
          onKeyLearnClicked_(std::move(onKeyLearnClicked)),
          onKeyClearClicked_(std::move(onKeyClearClicked)),
          onLearnClicked_(std::move(onLearnClicked)),
          onClearClicked_(std::move(onClearClicked)) {
        nameLabel_.setText(meta.displayName, juce::dontSendNotification);
        nameLabel_.setFont(juce::Font(11.5f, juce::Font::bold));
        nameLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
        nameLabel_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(nameLabel_);

        keyButton_.setWantsKeyboardFocus(false);
        keyButton_.setTooltip("Click to assign keyboard shortcut (right-click to clear)");
        keyButton_.onClick = [this]() {
            if (onKeyLearnClicked_) onKeyLearnClicked_(action_);
        };
        keyButton_.addMouseListener(this, false);
        addAndMakeVisible(keyButton_);

        midiButton_.setWantsKeyboardFocus(false);
        midiButton_.setTooltip("Click to learn MIDI trigger (right-click to clear)");
        midiButton_.onClick = [this]() {
            if (onLearnClicked_) onLearnClicked_(action_);
        };
        midiButton_.addMouseListener(this, false);
        addAndMakeVisible(midiButton_);

        clearButton_.setWantsKeyboardFocus(false);
        clearButton_.setTooltip("Clear MIDI mapping");
        clearButton_.onClick = [this]() {
            if (onClearClicked_) onClearClicked_(action_);
        };
        addAndMakeVisible(clearButton_);
    }

    ~MidiMappingRowComponent() override {
        keyButton_.removeMouseListener(this);
        midiButton_.removeMouseListener(this);
    }

    ControlAction getAction() const { return action_; }

    void mouseUp(const juce::MouseEvent& e) override {
        if (!e.mods.isPopupMenu()) return;
        if (e.eventComponent == &keyButton_ && !isKeyLearning_) {
            if (onKeyClearClicked_) onKeyClearClicked_(action_);
        } else if (e.eventComponent == &midiButton_ && !isMidiLearning_) {
            if (onClearClicked_) onClearClicked_(action_);
        }
    }

    void updateState(const std::optional<KeyBinding>& keyBinding,
                     bool isKeyLearning,
                     const std::optional<MidiBinding>& binding,
                     bool isMidiLearning) {
        isKeyLearning_ = isKeyLearning;
        isMidiLearning_ = isMidiLearning;

        if (isKeyLearning) {
            keyButton_.setButtonText("Cancel");
            keyButton_.setColour(juce::TextButton::buttonColourId, AppTheme::warningAmber.darker(0.25f));
            keyButton_.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
            keyButton_.setTooltip("Click to cancel key assignment");
        } else {
            const bool hasKey = keyBinding.has_value() && keyBinding->isValid();
            keyButton_.setButtonText(hasKey ? keyBinding->toDisplayString() : "No Key Set");
            keyButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgInput);
            keyButton_.setColour(juce::TextButton::textColourOffId, hasKey ? AppTheme::textLight : AppTheme::textMuted);
            keyButton_.setTooltip("Click to assign keyboard shortcut (right-click to clear)");
        }

        if (isMidiLearning) {
            midiButton_.setButtonText("Cancel");
            midiButton_.setColour(juce::TextButton::buttonColourId, AppTheme::warningAmber.darker(0.25f));
            midiButton_.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
            midiButton_.setTooltip("Waiting for MIDI input - click to cancel");
        } else {
            const bool hasBind = binding.has_value() && binding->isValid();
            midiButton_.setButtonText(hasBind ? binding->toDisplayString() : "No MIDI Set");
            midiButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgInput);
            midiButton_.setColour(juce::TextButton::textColourOffId, hasBind ? AppTheme::accentGreen : AppTheme::textMuted);
            midiButton_.setTooltip("Click to learn MIDI trigger (right-click to clear)");
        }
        clearButton_.setEnabled(!isMidiLearning && binding.has_value() && binding->isValid());
        repaint();
    }

    void paint(juce::Graphics& g) override {
        g.setColour(AppTheme::borderCol.withAlpha(0.32f));
        g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
    }

    void resized() override {
        auto area = getLocalBounds().reduced(2, 2);
        clearButton_.setBounds(area.removeFromRight(24));
        area.removeFromRight(6);

        nameLabel_.setBounds(area.removeFromLeft(134));
        area.removeFromLeft(8);
        const int keyW = std::clamp(area.getWidth() * 40 / 100, 76, 108);
        keyButton_.setBounds(area.removeFromLeft(keyW));
        area.removeFromLeft(8);
        midiButton_.setBounds(area);
    }

private:
    ControlAction action_;
    bool isKeyLearning_ = false;
    bool isMidiLearning_ = false;
    juce::Label nameLabel_;
    juce::TextButton keyButton_{"No Key Set"};
    juce::TextButton midiButton_{"No MIDI Set"};
    juce::TextButton clearButton_{"x"};
    std::function<void(ControlAction)> onKeyLearnClicked_;
    std::function<void(ControlAction)> onKeyClearClicked_;
    std::function<void(ControlAction)> onLearnClicked_;
    std::function<void(ControlAction)> onClearClicked_;
};

SettingsView::SettingsView(DatabaseService& db, AudioEngine& audioEngine, ControlSurfaceService& controlSurface)
    : db_(db), audioEngine_(audioEngine), controlSurface_(controlSurface) {
    setOpaque(true);

    titleLabel_.setFont(juce::Font(18.0f, juce::Font::bold));
    titleLabel_.setColour(juce::Label::textColourId, AppTheme::textLight);
    addAndMakeVisible(titleLabel_);

    auto setupSection = [this](juce::Label& lbl) {
        lbl.setFont(juce::Font(12.0f, juce::Font::bold));
        lbl.setColour(juce::Label::textColourId, AppTheme::tabSettingsAccent.interpolatedWith(AppTheme::textLight, 0.28f));
        addAndMakeVisible(lbl);
    };

    auto setupField = [this](juce::Label& lbl) {
        lbl.setFont(juce::Font(11.0f, juce::Font::bold));
        lbl.setColour(juce::Label::textColourId, AppTheme::textMuted);
        addAndMakeVisible(lbl);
    };

    // Device section
    setupSection(deviceSectionLabel_);
    setupField(deviceLabel_);
    addAndMakeVisible(deviceBox_);
    deviceBox_.onChange = [this]() {
        auto name = deviceBox_.getText().toStdString();
        if (!name.empty()) {
            audioEngine_.selectOutputDevice(name);
            saveSetting("audio.device", name);
            channelCountLabel_.setText("Available Channels: " + juce::String(audioEngine_.getCurrentOutputChannels()),
                                       juce::dontSendNotification);
            applySavedChannelRouting();
        }
    };

    refreshDevicesButton_.onClick = [this]() {
        populateDevices();
        applySavedChannelRouting();
    };
    addAndMakeVisible(refreshDevicesButton_);

    channelCountLabel_.setFont(juce::Font(11.0f, juce::Font::plain));
    channelCountLabel_.setColour(juce::Label::textColourId, AppTheme::accentGreen);
    addAndMakeVisible(channelCountLabel_);

    // Assets section
    setupSection(assetsSectionLabel_);

    setupField(clickSoundLabel_);
    clickSoundBox_.onChange = [this]() {
        auto pack = clickSoundBox_.getText().toStdString();
        if (!pack.empty()) {
            audioEngine_.selectClickPack(pack);
            saveSetting("audio.click_pack", pack);
        }
    };
    addAndMakeVisible(clickSoundBox_);

    setupField(voiceLabel_);
    voiceBox_.onChange = [this]() {
        auto pack = voiceBox_.getText().toStdString();
        if (!pack.empty()) {
            audioEngine_.selectVoicePack(pack);
            saveSetting("audio.voice_pack", pack);
        }
    };
    addAndMakeVisible(voiceBox_);

    announceSongNameToggle_.setWantsKeyboardFocus(false);
    announceSongNameToggle_.setTooltip("Read out the song title via text-to-speech on the Guide channel when a song is selected");
    announceSongNameToggle_.onClick = [this]() {
        const bool enabled = announceSongNameToggle_.getToggleState();
        saveSetting("audio.announce_song_name", enabled ? "1" : "0");
        if (onAnnounceSongNameChanged) {
            onAnnounceSongNameChanged(enabled);
        }
    };
    addAndMakeVisible(announceSongNameToggle_);

    // Volumes section
    setupSection(volumeSectionLabel_);

    auto setupVolSlider = [this](juce::Slider& s, const std::string& key) {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
        s.setRange(-30.0, 6.0, 0.5);
        s.setTextValueSuffix(" dB");
        s.setScrollWheelEnabled(false);
        addAndMakeVisible(s);
    };

    setupField(masterVolLabel_);
    setupVolSlider(masterVolSlider_, "volume.master_db");
    masterVolSlider_.onValueChange = [this]() {
        float dbVal = static_cast<float>(masterVolSlider_.getValue());
        float lin = (dbVal <= -30.0f) ? 0.0f : std::pow(10.0f, dbVal / 20.0f);
        audioEngine_.setMasterGain(lin);
        saveSetting("volume.master_db", std::to_string(dbVal));
    };

    setupField(clickVolLabel_);
    setupVolSlider(clickVolSlider_, "volume.click_db");
    clickVolSlider_.onValueChange = [this]() {
        float dbVal = static_cast<float>(clickVolSlider_.getValue());
        float lin = (dbVal <= -30.0f) ? 0.0f : std::pow(10.0f, dbVal / 20.0f);
        audioEngine_.setClickGain(lin);
        saveSetting("volume.click_db", std::to_string(dbVal));
    };

    setupField(guideVolLabel_);
    setupVolSlider(guideVolSlider_, "volume.guide_db");
    guideVolSlider_.onValueChange = [this]() {
        float dbVal = static_cast<float>(guideVolSlider_.getValue());
        float lin = (dbVal <= -30.0f) ? 0.0f : std::pow(10.0f, dbVal / 20.0f);
        audioEngine_.setGuideGain(lin);
        saveSetting("volume.guide_db", std::to_string(dbVal));
    };

    // Library Backup & Restore section
    setupSection(backupSectionLabel_);
    backupInfoLabel_.setFont(juce::Font(11.5f, juce::Font::plain));
    backupInfoLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
    backupInfoLabel_.setJustificationType(juce::Justification::topLeft);
    backupInfoLabel_.setText("Create a compressed .zip backup of your library, songs, arrangements, audio tracks, and setlists, or restore from an existing backup file.",
                             juce::dontSendNotification);
    addAndMakeVisible(backupInfoLabel_);

    backupLibraryButton_.setColour(juce::TextButton::buttonColourId, AppTheme::primaryBlue);
    backupLibraryButton_.onClick = [this]() { handleBackupLibrary(); };
    addAndMakeVisible(backupLibraryButton_);

    importBackupButton_.setColour(juce::TextButton::buttonColourId, AppTheme::bgPanel.brighter(0.08f));
    importBackupButton_.onClick = [this]() { handleImportBackup(); };
    addAndMakeVisible(importBackupButton_);

    backupStatusLabel_.setFont(juce::Font(11.5f, juce::Font::plain));
    backupStatusLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
    backupStatusLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(backupStatusLabel_);

    // MIDI & Keyboard Controls section
    setupSection(midiSectionLabel_);
    keyboardShortcutsToggle_.setWantsKeyboardFocus(false);
    keyboardShortcutsToggle_.setTooltip("Enable global keyboard shortcuts (disabled automatically while typing)");
    keyboardShortcutsToggle_.onClick = [this]() {
        controlSurface_.setKeyboardShortcutsEnabled(keyboardShortcutsToggle_.getToggleState());
    };
    addAndMakeVisible(keyboardShortcutsToggle_);

    resetKeysButton_.setWantsKeyboardFocus(false);
    resetKeysButton_.setTooltip("Restore default keyboard shortcuts");
    resetKeysButton_.onClick = [this]() {
        controlSurface_.resetDefaultKeyBindings();
        refreshMidiMappingRows();
        updateMidiStatusLabel();
    };
    addAndMakeVisible(resetKeysButton_);

    setupField(midiInputLabel_);
    midiInputBox_.onChange = [this]() {
        const int selId = midiInputBox_.getSelectedId();
        if (selId == 1) {
            controlSurface_.selectMidiInput(ControlSurfaceService::kAllMidiInputsId);
        } else if (selId == 2) {
            controlSurface_.selectMidiInput("");
        } else if (selId >= 100) {
            const size_t idx = static_cast<size_t>(selId - 100);
            if (idx < cachedMidiDevices_.size()) {
                controlSurface_.selectMidiInput(cachedMidiDevices_[idx].identifier);
            }
        }
        updateMidiStatusLabel();
    };
    addAndMakeVisible(midiInputBox_);

    refreshMidiButton_.setWantsKeyboardFocus(false);
    refreshMidiButton_.onClick = [this]() {
        controlSurface_.refreshMidiInputs();
        populateMidiInputs();
        updateMidiStatusLabel();
    };
    addAndMakeVisible(refreshMidiButton_);

    midiStatusLabel_.setFont(juce::Font(11.0f, juce::Font::plain));
    midiStatusLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
    midiStatusLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(midiStatusLabel_);

    midiRowsViewport_.setScrollBarsShown(true, false);
    midiRowsViewport_.setScrollBarThickness(7);
    midiRowsContainer_ = std::make_unique<juce::Component>();
    for (const auto& meta : getAllControlActionMeta()) {
        auto row = std::make_unique<MidiMappingRowComponent>(
            meta,
            [this](ControlAction act) {
                auto cur = controlSurface_.getLearningKeyAction();
                if (cur.has_value() && *cur == act) {
                    controlSurface_.cancelKeyLearn();
                } else {
                    controlSurface_.startKeyLearn(act);
                    if (auto* parent = getParentComponent()) {
                        parent->grabKeyboardFocus();
                    }
                }
                refreshMidiMappingRows();
                updateMidiStatusLabel();
            },
            [this](ControlAction act) {
                controlSurface_.clearKeyBinding(act);
                refreshMidiMappingRows();
                updateMidiStatusLabel();
            },
            [this](ControlAction act) {
                auto cur = controlSurface_.getLearningAction();
                if (cur.has_value() && *cur == act) {
                    controlSurface_.cancelMidiLearn();
                } else {
                    controlSurface_.startMidiLearn(act);
                }
                refreshMidiMappingRows();
                updateMidiStatusLabel();
            },
            [this](ControlAction act) {
                controlSurface_.clearBinding(act);
                refreshMidiMappingRows();
                updateMidiStatusLabel();
            });
        midiRowsContainer_->addAndMakeVisible(row.get());
        midiRows_.push_back(std::move(row));
    }
    midiRowsViewport_.setViewedComponent(midiRowsContainer_.get(), false);
    addAndMakeVisible(midiRowsViewport_);

    juce::Component::SafePointer<SettingsView> safeThis(this);
    controlSurface_.onMidiLearned = [safeThis](ControlAction, MidiBinding) {
        if (safeThis == nullptr) return;
        safeThis->refreshMidiMappingRows();
        safeThis->updateMidiStatusLabel();
    };
    controlSurface_.onMidiActivity = [safeThis](const std::string& desc) {
        if (safeThis == nullptr) return;
        safeThis->updateMidiStatusLabel(desc);
    };

    refreshSettings();
}

SettingsView::~SettingsView() {
    controlSurface_.cancelMidiLearn();
    controlSurface_.cancelKeyLearn();
    controlSurface_.onMidiLearned = nullptr;
    controlSurface_.onMidiActivity = nullptr;
    if (backupWorker_.joinable()) {
        backupWorker_.join();
    }
}

void SettingsView::populateDevices() {
    deviceBox_.clear();
    auto devices = audioEngine_.getAvailableOutputDevices();
    int selId = 1;
    for (size_t i = 0; i < devices.size(); ++i) {
        deviceBox_.addItem(devices[i].name, static_cast<int>(i + 1));
        if (devices[i].isDefault) {
            selId = static_cast<int>(i + 1);
        }
    }
    deviceBox_.setSelectedId(selId, juce::dontSendNotification);
    channelCountLabel_.setText("Available Channels: " + juce::String(audioEngine_.getCurrentOutputChannels()),
                               juce::dontSendNotification);
}

void SettingsView::applySavedChannelRouting() {
    const int numChannels = std::max(1, audioEngine_.getCurrentOutputChannels());

    int clickCh = 0;
    try { clickCh = std::stoi(db_.getAppSetting("volume.channel.click_db", "0")); } catch (...) {}
    clickCh = std::clamp(clickCh, 0, numChannels - 1);
    audioEngine_.setClickChannel(clickCh);

    int guideCh = 0;
    try { guideCh = std::stoi(db_.getAppSetting("volume.channel.guide_db", "0")); } catch (...) {}
    guideCh = std::clamp(guideCh, 0, numChannels - 1);
    audioEngine_.setGuideChannel(guideCh);
}

void SettingsView::populateMidiInputs() {
    cachedMidiDevices_ = controlSurface_.getAvailableMidiInputs();
    const std::string selectedId = controlSurface_.getSelectedMidiInputIdentifier();

    midiInputBox_.clear(juce::dontSendNotification);
    midiInputBox_.addItem("All MIDI Inputs", 1);
    midiInputBox_.addItem("Disabled", 2);
    if (!cachedMidiDevices_.empty()) {
        midiInputBox_.addSeparator();
    }

    int chosenComboId = (selectedId.empty()) ? 2 : 1;
    for (size_t i = 0; i < cachedMidiDevices_.size(); ++i) {
        const int itemId = 100 + static_cast<int>(i);
        midiInputBox_.addItem(cachedMidiDevices_[i].name, itemId);
        if (!selectedId.empty() && selectedId != ControlSurfaceService::kAllMidiInputsId &&
            (cachedMidiDevices_[i].identifier == selectedId || cachedMidiDevices_[i].name == selectedId)) {
            chosenComboId = itemId;
        }
    }
    midiInputBox_.setSelectedId(chosenComboId, juce::dontSendNotification);
}

void SettingsView::refreshMidiMappingRows() {
    const auto learningKey = controlSurface_.getLearningKeyAction();
    const auto learningMidi = controlSurface_.getLearningAction();
    for (auto& row : midiRows_) {
        const ControlAction act = row->getAction();
        const bool isKeyLearning = learningKey.has_value() && *learningKey == act;
        const bool isMidiLearning = learningMidi.has_value() && *learningMidi == act;
        row->updateState(controlSurface_.getKeyBinding(act), isKeyLearning,
                         controlSurface_.getBinding(act), isMidiLearning);
    }
}

void SettingsView::updateMidiStatusLabel(const std::string& activityOverride) {
    if (auto learningKey = controlSurface_.getLearningKeyAction(); learningKey.has_value()) {
        midiStatusLabel_.setColour(juce::Label::textColourId, AppTheme::warningAmber);
        midiStatusLabel_.setText("Press any key to assign to " +
                                     juce::String(getControlActionMeta(*learningKey).displayName) +
                                     " (click Cancel to abort)",
                                 juce::dontSendNotification);
        return;
    }

    if (auto learning = controlSurface_.getLearningAction(); learning.has_value()) {
        midiStatusLabel_.setColour(juce::Label::textColourId, AppTheme::warningAmber);
        midiStatusLabel_.setText("Press a MIDI pedal or key to assign to " +
                                     juce::String(getControlActionMeta(*learning).displayName) +
                                     " (click Cancel to abort)",
                                 juce::dontSendNotification);
        return;
    }

    if (!activityOverride.empty()) {
        midiStatusLabel_.setColour(juce::Label::textColourId, AppTheme::accentGreen);
        midiStatusLabel_.setText(activityOverride, juce::dontSendNotification);
        return;
    }

    const std::string sel = controlSurface_.getSelectedMidiInputIdentifier();
    if (sel.empty()) {
        midiStatusLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
        midiStatusLabel_.setText("MIDI input disabled", juce::dontSendNotification);
        return;
    }

    const int activeCount = controlSurface_.getActiveMidiInputCount();
    if (activeCount > 0) {
        midiStatusLabel_.setColour(juce::Label::textColourId, AppTheme::accentGreen);
        midiStatusLabel_.setText("MIDI ready (" + juce::String(activeCount) +
                                     (activeCount == 1 ? " device connected)" : " devices connected)"),
                                 juce::dontSendNotification);
    } else {
        midiStatusLabel_.setColour(juce::Label::textColourId, AppTheme::textMuted);
        midiStatusLabel_.setText("No MIDI controllers detected - plug in a device and click Refresh",
                                 juce::dontSendNotification);
    }
}

void SettingsView::refreshSettings() {
    populateDevices();
    applySavedChannelRouting();
    keyboardShortcutsToggle_.setToggleState(controlSurface_.isKeyboardShortcutsEnabled(), juce::dontSendNotification);
    populateMidiInputs();
    refreshMidiMappingRows();
    updateMidiStatusLabel();

    // Populate Click Sound packs (supports WAV, MP3, M4A)
    auto clickPacks = audioEngine_.getAvailableClickPacks();
    clickSoundBox_.clear(juce::dontSendNotification);
    std::string savedClickPack = db_.getAppSetting("audio.click_pack", "");
    int selectedClickId = 1;
    for (size_t i = 0; i < clickPacks.size(); ++i) {
        clickSoundBox_.addItem(clickPacks[i], static_cast<int>(i + 1));
        if (!savedClickPack.empty() && clickPacks[i] == savedClickPack) {
            selectedClickId = static_cast<int>(i + 1);
        }
    }
    if (!clickPacks.empty()) {
        clickSoundBox_.setSelectedId(selectedClickId, juce::dontSendNotification);
        audioEngine_.selectClickPack(clickPacks[static_cast<size_t>(selectedClickId - 1)]);
    }

    // Populate Guide Voice packs from assets/guide/<directory> (supports WAV, MP3, M4A)
    auto voicePacks = audioEngine_.getAvailableVoicePacks();
    voiceBox_.clear(juce::dontSendNotification);
    std::string savedVoicePack = db_.getAppSetting("audio.voice_pack", "");
    int selectedVoiceId = 1;
    for (size_t i = 0; i < voicePacks.size(); ++i) {
        voiceBox_.addItem(voicePacks[i], static_cast<int>(i + 1));
        if (!savedVoicePack.empty() && voicePacks[i] == savedVoicePack) {
            selectedVoiceId = static_cast<int>(i + 1);
        }
    }
    if (!voicePacks.empty()) {
        voiceBox_.setSelectedId(selectedVoiceId, juce::dontSendNotification);
        audioEngine_.selectVoicePack(voicePacks[static_cast<size_t>(selectedVoiceId - 1)]);
    }

    const std::string announceVal = db_.getAppSetting("audio.announce_song_name", "1");
    announceSongNameToggle_.setToggleState(announceVal != "0" && announceVal != "false", juce::dontSendNotification);

    float masterDb = 0.0f;
    try { masterDb = std::stof(db_.getAppSetting("volume.master_db", "0.0")); } catch (...) {}
    masterVolSlider_.setValue(masterDb, juce::dontSendNotification);

    float clickDb = 0.0f;
    try { clickDb = std::stof(db_.getAppSetting("volume.click_db", "0.0")); } catch (...) {}
    clickVolSlider_.setValue(clickDb, juce::dontSendNotification);

    float guideDb = 0.0f;
    try { guideDb = std::stof(db_.getAppSetting("volume.guide_db", "0.0")); } catch (...) {}
    guideVolSlider_.setValue(guideDb, juce::dontSendNotification);
}

void SettingsView::saveSetting(const std::string& key, const std::string& val) {
    db_.setAppSetting(key, val);
}

void SettingsView::setBackupBusy(bool busy) {
    backupBusy_.store(busy);
    backupLibraryButton_.setEnabled(!busy);
    importBackupButton_.setEnabled(!busy);
}

void SettingsView::handleBackupLibrary() {
    if (backupBusy_.load()) return;

    auto now = juce::Time::getCurrentTime();
    juce::String defaultName = "Tracks_Backup_" + now.formatted("%Y-%m-%d") + ".zip";
    auto defaultDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    auto initialFile = defaultDir.getChildFile(defaultName);

    activeFileChooser_ = std::make_unique<juce::FileChooser>(
        "Save Library Backup",
        initialFile,
        "*.zip");

    juce::Component::SafePointer<SettingsView> safeThis(this);
    activeFileChooser_->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis](const juce::FileChooser& chooser) {
            if (safeThis == nullptr) return;
            auto chosen = chooser.getResult();
            if (chosen == juce::File()) return;

            if (!chosen.hasFileExtension(".zip")) {
                chosen = chosen.withFileExtension(".zip");
            }
            safeThis->runExportBackup(chosen.getFullPathName().toStdString());
        });
}

void SettingsView::handleImportBackup() {
    if (backupBusy_.load()) return;

    auto defaultDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    activeFileChooser_ = std::make_unique<juce::FileChooser>(
        "Import Library Backup",
        defaultDir,
        "*.zip");

    juce::Component::SafePointer<SettingsView> safeThis(this);
    activeFileChooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis](const juce::FileChooser& chooser) {
            if (safeThis == nullptr) return;
            auto chosen = chooser.getResult();
            if (!chosen.existsAsFile()) return;

            juce::String zipPath = chosen.getFullPathName();
            juce::String fileName = chosen.getFileName();

            juce::AlertWindow::showOkCancelBox(
                juce::MessageBoxIconType::WarningIcon,
                "Import Library Backup",
                "Importing \"" + fileName + "\" will replace your current library, songs, tracks, and sets.\n\nDo you want to continue?",
                "Import",
                "Cancel",
                safeThis.getComponent(),
                juce::ModalCallbackFunction::create([safeThis, zipPath](int result) {
                    if (safeThis == nullptr || result == 0) return;
                    safeThis->runImportBackup(zipPath.toStdString());
                }));
        });
}

void SettingsView::runExportBackup(const std::string& destZipPath) {
    if (backupWorker_.joinable()) {
        backupWorker_.join();
    }
    setBackupBusy(true);
    backupStatusLabel_.setColour(juce::Label::textColourId, AppTheme::warningAmber);
    backupStatusLabel_.setText("Starting library backup...", juce::dontSendNotification);

    const auto tracksRoot = AudioAttachmentService::getProjectRoot() / "uploaded_tracks";
    juce::Component::SafePointer<SettingsView> safeThis(this);

    backupWorker_ = std::thread([this, safeThis, destZipPath, tracksRoot]() {
        auto progressCb = [safeThis](const std::string& msg) {
            juce::MessageManager::callAsync([safeThis, msg]() {
                if (safeThis == nullptr) return;
                safeThis->backupStatusLabel_.setColour(juce::Label::textColourId, AppTheme::warningAmber);
                safeThis->backupStatusLabel_.setText(msg, juce::dontSendNotification);
            });
        };

        auto res = LibraryBackupService::exportBackup(db_, destZipPath, tracksRoot, progressCb);

        juce::MessageManager::callAsync([safeThis, res]() {
            if (safeThis == nullptr) return;
            safeThis->setBackupBusy(false);
            safeThis->backupStatusLabel_.setColour(juce::Label::textColourId,
                                                   res.success ? AppTheme::accentGreen : AppTheme::dangerRed);
            safeThis->backupStatusLabel_.setText(res.message, juce::dontSendNotification);
        });
    });
}

void SettingsView::runImportBackup(const std::string& srcZipPath) {
    if (backupWorker_.joinable()) {
        backupWorker_.join();
    }
    setBackupBusy(true);
    backupStatusLabel_.setColour(juce::Label::textColourId, AppTheme::warningAmber);
    backupStatusLabel_.setText("Starting backup import...", juce::dontSendNotification);

    const auto tracksRoot = AudioAttachmentService::getProjectRoot() / "uploaded_tracks";
    juce::Component::SafePointer<SettingsView> safeThis(this);

    backupWorker_ = std::thread([this, safeThis, srcZipPath, tracksRoot]() {
        auto progressCb = [safeThis](const std::string& msg) {
            juce::MessageManager::callAsync([safeThis, msg]() {
                if (safeThis == nullptr) return;
                safeThis->backupStatusLabel_.setColour(juce::Label::textColourId, AppTheme::warningAmber);
                safeThis->backupStatusLabel_.setText(msg, juce::dontSendNotification);
            });
        };

        auto beforeReplaceCb = [this]() {
            audioEngine_.clearAllCachedAudio();
        };

        auto res = LibraryBackupService::importBackup(db_, srcZipPath, tracksRoot, beforeReplaceCb, progressCb);

        juce::MessageManager::callAsync([safeThis, res]() {
            if (safeThis == nullptr) return;
            safeThis->setBackupBusy(false);
            safeThis->backupStatusLabel_.setColour(juce::Label::textColourId,
                                                   res.success ? AppTheme::accentGreen : AppTheme::dangerRed);
            safeThis->backupStatusLabel_.setText(res.message, juce::dontSendNotification);
            if (res.success && safeThis->onLibraryImported) {
                safeThis->onLibraryImported();
            }
        });
    });
}

void SettingsView::paint(juce::Graphics& g) {
    AppTheme::paintTintedPageBackground(g, getLocalBounds(), AppTheme::pageBgSettings, AppTheme::tabSettingsAccent);

    if (!backupCardBounds_.isEmpty()) {
        auto r = backupCardBounds_.toFloat().reduced(0.5f);
        g.setColour(AppTheme::bgPanel.interpolatedWith(AppTheme::pageBgSettings, 0.35f));
        g.fillRoundedRectangle(r, 6.0f);
        g.setColour(AppTheme::borderCol.interpolatedWith(AppTheme::tabSettingsAccent, 0.25f));
        g.drawRoundedRectangle(r, 6.0f, 1.0f);
    }

    if (!midiCardBounds_.isEmpty()) {
        auto r = midiCardBounds_.toFloat().reduced(0.5f);
        g.setColour(AppTheme::bgPanel.interpolatedWith(AppTheme::pageBgSettings, 0.35f));
        g.fillRoundedRectangle(r, 6.0f);
        g.setColour(AppTheme::borderCol.interpolatedWith(AppTheme::tabSettingsAccent, 0.25f));
        g.drawRoundedRectangle(r, 6.0f, 1.0f);
    }
}

void SettingsView::resized() {
    auto fullArea = getLocalBounds().reduced(24);

    titleLabel_.setBounds(fullArea.removeFromTop(32));
    fullArea.removeFromTop(14);

    // Split into left column (audio/routing/assets/volumes) and right column (Backup + MIDI & Keyboard Controls)
    auto leftCol = fullArea.removeFromLeft(std::min(460, std::max(340, fullArea.getWidth() - 450)));
    fullArea.removeFromLeft(24);
    auto rightCol = fullArea;

    auto area = leftCol;
    int rowH = 26;
    int gap = 8;
    int sectionGap = 18;

    // Device
    deviceSectionLabel_.setBounds(area.removeFromTop(20));
    area.removeFromTop(gap);
    auto dRow = area.removeFromTop(rowH);
    deviceLabel_.setBounds(dRow.removeFromLeft(110));
    refreshDevicesButton_.setBounds(dRow.removeFromRight(78));
    dRow.removeFromRight(8);
    deviceBox_.setBounds(dRow);
    area.removeFromTop(4);
    channelCountLabel_.setBounds(area.removeFromTop(18));

    area.removeFromTop(sectionGap);

    // Assets
    assetsSectionLabel_.setBounds(area.removeFromTop(20));
    area.removeFromTop(gap);
    auto asRow1 = area.removeFromTop(rowH);
    clickSoundLabel_.setBounds(asRow1.removeFromLeft(120));
    clickSoundBox_.setBounds(asRow1.removeFromLeft(240));

    area.removeFromTop(gap);
    auto asRow2 = area.removeFromTop(rowH);
    voiceLabel_.setBounds(asRow2.removeFromLeft(120));
    voiceBox_.setBounds(asRow2.removeFromLeft(240));

    area.removeFromTop(gap);
    announceSongNameToggle_.setBounds(area.removeFromTop(24));

    area.removeFromTop(sectionGap);

    // Volumes
    volumeSectionLabel_.setBounds(area.removeFromTop(20));
    area.removeFromTop(gap);
    auto vRow1 = area.removeFromTop(rowH);
    masterVolLabel_.setBounds(vRow1.removeFromLeft(120));
    masterVolSlider_.setBounds(vRow1);

    area.removeFromTop(gap);
    auto vRow2 = area.removeFromTop(rowH);
    clickVolLabel_.setBounds(vRow2.removeFromLeft(120));
    clickVolSlider_.setBounds(vRow2);

    area.removeFromTop(gap);
    auto vRow3 = area.removeFromTop(rowH);
    guideVolLabel_.setBounds(vRow3.removeFromLeft(120));
    guideVolSlider_.setBounds(vRow3);

    // Right column top: Library Backup & Restore card
    backupCardBounds_ = rightCol.removeFromTop(std::min(168, rightCol.getHeight()));
    {
        auto card = backupCardBounds_.reduced(14);
        backupSectionLabel_.setBounds(card.removeFromTop(20));
        card.removeFromTop(6);
        backupInfoLabel_.setBounds(card.removeFromTop(38));
        card.removeFromTop(10);

        auto btnRow = card.removeFromTop(30);
        const int btnW = std::min(160, (btnRow.getWidth() - 10) / 2);
        backupLibraryButton_.setBounds(btnRow.removeFromLeft(btnW));
        btnRow.removeFromLeft(10);
        importBackupButton_.setBounds(btnRow.removeFromLeft(btnW));

        card.removeFromTop(8);
        backupStatusLabel_.setBounds(card);
    }

    rightCol.removeFromTop(14);

    // Right column bottom: MIDI & Keyboard Controls card
    midiCardBounds_ = rightCol;
    if (midiCardBounds_.getHeight() > 80) {
        auto card = midiCardBounds_.reduced(14);
        auto hdrRow = card.removeFromTop(22);
        resetKeysButton_.setBounds(hdrRow.removeFromRight(76));
        hdrRow.removeFromRight(8);
        keyboardShortcutsToggle_.setBounds(hdrRow.removeFromRight(146));
        midiSectionLabel_.setBounds(hdrRow);

        card.removeFromTop(8);
        auto devRow = card.removeFromTop(26);
        midiInputLabel_.setBounds(devRow.removeFromLeft(78));
        refreshMidiButton_.setBounds(devRow.removeFromRight(74));
        devRow.removeFromRight(8);
        midiInputBox_.setBounds(devRow);

        card.removeFromTop(4);
        midiStatusLabel_.setBounds(card.removeFromTop(16));

        card.removeFromTop(8);
        midiRowsViewport_.setBounds(card);

        const int rowHeight = 26;
        const int totalRowsH = static_cast<int>(midiRows_.size()) * rowHeight;
        const bool needsScroll = totalRowsH > card.getHeight();
        const int contentW = std::max(100, card.getWidth() - (needsScroll ? midiRowsViewport_.getScrollBarThickness() + 2 : 0));
        if (midiRowsContainer_) {
            midiRowsContainer_->setSize(contentW, totalRowsH);
            int y = 0;
            for (auto& row : midiRows_) {
                row->setBounds(0, y, contentW, rowHeight);
                y += rowHeight;
            }
        }
    }
}

} // namespace tracks
