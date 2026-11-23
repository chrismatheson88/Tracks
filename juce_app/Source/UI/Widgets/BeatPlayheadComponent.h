#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace tracks {

class BeatPlayheadComponent : public juce::Component {
public:
    BeatPlayheadComponent();
    ~BeatPlayheadComponent() override = default;

    void updatePlayhead(int currentBeatInBar, int currentBar, int beatsPerBar, bool isPlaying, bool isCountIn);

    void paint(juce::Graphics& g) override;

private:
    int currentBeatInBar_ = 0;
    int currentBar_ = 0;
    int beatsPerBar_ = 4;
    bool isPlaying_ = false;
    bool isCountIn_ = false;
};

} // namespace tracks
