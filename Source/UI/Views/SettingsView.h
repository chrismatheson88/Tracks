#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../Database/DatabaseService.h"
#include "../../Audio/AudioEngine.h"
#include "../../Services/ControlSurfaceService.h"
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace tracks {

class SettingsView : public juce::Component {
public:
    SettingsView(DatabaseService& db, AudioEngine& audioEngine, ControlSurfaceService& controlSurface);
    ~SettingsView() override;

    void refreshSettings();
    std::function<void()> onLibraryImported;
    std::function<void(bool)> onAnnounceSongNameChanged;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    class MidiMappingRowComponent;

    DatabaseService& db_;
    AudioEngine& audioEngine_;
    ControlSurfaceService& controlSurface_;

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
    juce::ToggleButton announceSongNameToggle_{"Announce song name on Guide channel"};

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

    // MIDI & Keyboard Controls
    juce::Rectangle<int> midiCardBounds_;
    juce::Label midiSectionLabel_{{}, "MIDI & KEYBOARD CONTROLS"};
    juce::ToggleButton keyboardShortcutsToggle_{"Keyboard Shortcuts"};
    juce::TextButton resetKeysButton_{"Reset Keys"};
    juce::Label midiInputLabel_{{}, "MIDI Input:"};
    juce::ComboBox midiInputBox_;
    juce::TextButton refreshMidiButton_{"Refresh"};
    juce::Label midiStatusLabel_;
    juce::Viewport midiRowsViewport_;
    std::unique_ptr<juce::Component> midiRowsContainer_;
    std::vector<std::unique_ptr<MidiMappingRowComponent>> midiRows_;
    std::vector<MidiInputDeviceInfo> cachedMidiDevices_;

    std::unique_ptr<juce::FileChooser> activeFileChooser_;
    std::thread backupWorker_;
    std::atomic<bool> backupBusy_{false};

    void populateDevices();
    void populateMidiInputs();
    void refreshMidiMappingRows();
    void updateMidiStatusLabel(const std::string& activityOverride = "");
    void applySavedChannelRouting();
    void saveSetting(const std::string& key, const std::string& val);
    void setBackupBusy(bool busy);
    void handleBackupLibrary();
    void handleImportBackup();
    void runExportBackup(const std::string& destZipPath);
    void runImportBackup(const std::string& srcZipPath);
};

} // namespace tracks
