#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../Database/DatabaseService.h"
#include "../../Audio/AudioEngine.h"
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace tracks {

class SettingsView : public juce::Component {
public:
    SettingsView(DatabaseService& db, AudioEngine& audioEngine);
    ~SettingsView() override;

    void refreshSettings();
    std::function<void()> onLibraryImported;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    DatabaseService& db_;
    AudioEngine& audioEngine_;

    juce::Label titleLabel_{{}, "Audio & System Settings"};

    // Device
    juce::Label deviceSectionLabel_{{}, "AUDIO OUTPUT DEVICE"};
    juce::Label deviceLabel_{{}, "Output Device:"};
    juce::ComboBox deviceBox_;
    juce::TextButton refreshDevicesButton_{"Refresh"};
    juce::Label channelCountLabel_;

    // Assets
    juce::Label assetsSectionLabel_{{}, "METRONOME ASSETS"};

    juce::Label clickSoundLabel_{{}, "Click Sound:"};
    juce::ComboBox clickSoundBox_;

    juce::Label voiceLabel_{{}, "Guide Voice:"};
    juce::ComboBox voiceBox_;

    // Volumes
    juce::Label volumeSectionLabel_{{}, "DEFAULT VOLUMES"};

    juce::Label masterVolLabel_{{}, "Master Volume:"};
    juce::Slider masterVolSlider_;

    juce::Label clickVolLabel_{{}, "Click Volume:"};
    juce::Slider clickVolSlider_;

    juce::Label guideVolLabel_{{}, "Guide Volume:"};
    juce::Slider guideVolSlider_;

    // Library Backup & Restore
    juce::Rectangle<int> backupCardBounds_;
    juce::Label backupSectionLabel_{{}, "LIBRARY BACKUP & RESTORE"};
    juce::Label backupInfoLabel_;
    juce::TextButton backupLibraryButton_{"Backup Library..."};
    juce::TextButton importBackupButton_{"Import Backup..."};
    juce::Label backupStatusLabel_;

    std::unique_ptr<juce::FileChooser> activeFileChooser_;
    std::thread backupWorker_;
    std::atomic<bool> backupBusy_{false};

    void populateDevices();
    void applySavedChannelRouting();
    void saveSetting(const std::string& key, const std::string& val);
    void setBackupBusy(bool busy);
    void handleBackupLibrary();
    void handleImportBackup();
    void runExportBackup(const std::string& destZipPath);
    void runImportBackup(const std::string& srcZipPath);
};

} // namespace tracks
