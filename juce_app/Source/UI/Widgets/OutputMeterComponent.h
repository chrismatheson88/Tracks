#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace tracks {

class OutputMeterComponent : public juce::Component {
public:
    OutputMeterComponent();
    ~OutputMeterComponent() override = default;

    void updatePeaks(const std::vector<float>& peaks);

    void paint(juce::Graphics& g) override;

private:
    std::vector<float> peaks_;
};

} // namespace tracks
