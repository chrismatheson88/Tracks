#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace tracks {

class TimePopupComponent : public juce::Component {
public:
    explicit TimePopupComponent(const juce::String& initialTimeStr = {});
    ~TimePopupComponent() override = default;

    std::function<void(const juce::String&)> onTimeSelected;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    int selectedHour_ = 10;
    int selectedMinute_ = 0; // 0, 15, 30, 45
    bool isPm_ = false;

    // AM/PM buttons
    juce::TextButton amButton_{"AM"};
    juce::TextButton pmButton_{"PM"};
    juce::TextButton nowButton_{"Now"};

    // Hour buttons 1..12
    juce::TextButton hourButtons_[12];

    // Minute buttons :00, :15, :30, :45
    juce::TextButton minuteButtons_[4];

    void parseInitialTime(const juce::String& str);
    void updateButtonHighlights();
    void selectTime(int hour, int minute, bool pm);
    void finishSelection();
};

} // namespace tracks
