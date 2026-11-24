#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../Database/DatabaseService.h"
#include "../../Audio/AudioEngine.h"

namespace tracks {

class SettingsView : public juce::Component {
public:
    SettingsView(DatabaseService& db, AudioEngine& audioEngine);
    ~SettingsView() override = default;

    void refreshSettings();

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
    juce::TextButton refreshDevicesButton_{"🔄 Refresh"};
    juce::Label channelCountLabel_;

    // Routing
    juce::Label routingSectionLabel_{{}, "CHANNEL ROUTING"};

    juce::Label clickChannelLabel_{{}, "Click Channel:"};
    juce::ComboBox clickChannelBox_;

    juce::Label guideChannelLabel_{{}, "Guide / Voice Channel:"};
    juce::ComboBox guideChannelBox_;

    // Assets
    juce::Label assetsSectionLabel_{{}, "METRONOME ASSETS"};

    juce::Label clickSoundLabel_{{}, "Click Sound:"};
    juce::ComboBox clickSoundBox_;

    juce::Label voiceLabel_{{}, "Count-in Voice:"};
    juce::ComboBox voiceBox_;

    // Volumes
    juce::Label volumeSectionLabel_{{}, "DEFAULT VOLUMES"};

    juce::Label masterVolLabel_{{}, "Master Volume:"};
    juce::Slider masterVolSlider_;

    juce::Label clickVolLabel_{{}, "Click Volume:"};
    juce::Slider clickVolSlider_;

    juce::Label guideVolLabel_{{}, "Guide Volume:"};
    juce::Slider guideVolSlider_;

    void populateDevices();
    void saveSetting(const std::string& key, const std::string& val);
};

} // namespace tracks
