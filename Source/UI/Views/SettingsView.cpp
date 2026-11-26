#include "SettingsView.h"
#include "../../Common/AppTheme.h"
#include "../../Services/AudioAttachmentService.h"
#include "../../Services/LibraryBackupService.h"
#include <cmath>

namespace tracks {

SettingsView::SettingsView(DatabaseService& db, AudioEngine& audioEngine)
    : db_(db), audioEngine_(audioEngine) {
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

    refreshSettings();
}

SettingsView::~SettingsView() {
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

void SettingsView::refreshSettings() {
    populateDevices();
    applySavedChannelRouting();

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
}

void SettingsView::resized() {
    auto fullArea = getLocalBounds().reduced(24);

    titleLabel_.setBounds(fullArea.removeFromTop(32));
    fullArea.removeFromTop(16);

    // Split into left column (audio/routing/assets/volumes) and right column (Library Backup & Restore card)
    auto leftCol = fullArea.removeFromLeft(std::min(510, fullArea.getWidth() - 320));
    fullArea.removeFromLeft(28);
    auto rightCol = fullArea;

    auto area = leftCol;
    int rowH = 26;
    int gap = 8;
    int sectionGap = 18;

    // Device
    deviceSectionLabel_.setBounds(area.removeFromTop(20));
    area.removeFromTop(gap);
    auto dRow = area.removeFromTop(rowH);
    deviceLabel_.setBounds(dRow.removeFromLeft(120));
    deviceBox_.setBounds(dRow.removeFromLeft(260));
    dRow.removeFromLeft(8);
    refreshDevicesButton_.setBounds(dRow.removeFromLeft(84));
    area.removeFromTop(4);
    channelCountLabel_.setBounds(area.removeFromTop(18));

    area.removeFromTop(sectionGap);

    // Assets
    assetsSectionLabel_.setBounds(area.removeFromTop(20));
    area.removeFromTop(gap);
    auto asRow1 = area.removeFromTop(rowH);
    clickSoundLabel_.setBounds(asRow1.removeFromLeft(140));
    clickSoundBox_.setBounds(asRow1.removeFromLeft(240));

    area.removeFromTop(gap);
    auto asRow2 = area.removeFromTop(rowH);
    voiceLabel_.setBounds(asRow2.removeFromLeft(140));
    voiceBox_.setBounds(asRow2.removeFromLeft(240));

    area.removeFromTop(sectionGap);

    // Volumes
    volumeSectionLabel_.setBounds(area.removeFromTop(20));
    area.removeFromTop(gap);
    auto vRow1 = area.removeFromTop(rowH);
    masterVolLabel_.setBounds(vRow1.removeFromLeft(140));
    masterVolSlider_.setBounds(vRow1.removeFromLeft(280));

    area.removeFromTop(gap);
    auto vRow2 = area.removeFromTop(rowH);
    clickVolLabel_.setBounds(vRow2.removeFromLeft(140));
    clickVolSlider_.setBounds(vRow2.removeFromLeft(280));

    area.removeFromTop(gap);
    auto vRow3 = area.removeFromTop(rowH);
    guideVolLabel_.setBounds(vRow3.removeFromLeft(140));
    guideVolSlider_.setBounds(vRow3.removeFromLeft(280));

    // Right column: Library Backup & Restore card
    backupCardBounds_ = rightCol.removeFromTop(std::min(230, rightCol.getHeight()));
    auto card = backupCardBounds_.reduced(16);
    backupSectionLabel_.setBounds(card.removeFromTop(20));
    card.removeFromTop(8);
    backupInfoLabel_.setBounds(card.removeFromTop(52));
    card.removeFromTop(12);

    auto btnRow = card.removeFromTop(32);
    const int btnW = std::min(160, (btnRow.getWidth() - 10) / 2);
    backupLibraryButton_.setBounds(btnRow.removeFromLeft(btnW));
    btnRow.removeFromLeft(10);
    importBackupButton_.setBounds(btnRow.removeFromLeft(btnW));

    card.removeFromTop(14);
    backupStatusLabel_.setBounds(card);
}

} // namespace tracks
