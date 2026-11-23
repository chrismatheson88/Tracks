#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BeatPlayheadComponent.h"
#include "OutputMeterComponent.h"
#include "../../Audio/AudioEngine.h"

namespace tracks {

class TransportComponent : public juce::Component, private juce::Timer {
public:
    explicit TransportComponent(AudioEngine& audioEngine);
    ~TransportComponent() override;

    void setSongInfo(const std::string& title,
                     const std::string& artist,
                     int tempo,
                     const std::string& key,
                     const std::string& timeSignature);

    void setPlayingState(bool isPlaying, bool isPaused);

    // Callbacks
    std::function<void()> onPlayClicked;
    std::function<void()> onPauseClicked;
    std::function<void()> onStopClicked;
    std::function<void(float)> onMasterGainChanged;
    std::function<void(float)> onClickGainChanged;
    std::function<void(float)> onGuideGainChanged;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    AudioEngine& audioEngine_;

    juce::TextButton playPauseButton_{"▶ Play"};
    juce::TextButton stopButton_{"■ Stop"};

    juce::Label songTitleLabel_;
    juce::Label songArtistLabel_;

    juce::Label tempoBadge_;
    juce::Label keyBadge_;
    juce::Label timeSigBadge_;

    juce::Slider masterSlider_;
    juce::Label masterLabel_{{}, "Master"};

    juce::Slider clickSlider_;
    juce::Label clickLabel_{{}, "Click"};

    juce::Slider guideSlider_;
    juce::Label guideLabel_{{}, "Guide"};

    BeatPlayheadComponent beatPlayhead_;
    OutputMeterComponent outputMeter_;

    void timerCallback() override;
};

} // namespace tracks
