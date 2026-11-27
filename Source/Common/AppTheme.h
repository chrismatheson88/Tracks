#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace tracks {

class AppTheme : public juce::LookAndFeel_V4 {
public:
    AppTheme();
    ~AppTheme() override = default;

    // Custom Color Palette
    static const juce::Colour bgDark;
    static const juce::Colour bgPanel;
    static const juce::Colour bgInput;
    static const juce::Colour borderCol;
    static const juce::Colour primaryBlue;
    static const juce::Colour accentGreen;
    static const juce::Colour textLight;
    static const juce::Colour textMuted;
    static const juce::Colour dangerRed;
    static const juce::Colour warningAmber;

    // Header & Attached Tab Palette (derived from Tracks logo: Blue / Purple / Pink)
    static const juce::Colour headerBg;
    static const juce::Colour tabLibraryAccent;
    static const juce::Colour tabLiveAccent;
    static const juce::Colour tabSettingsAccent;
    static const juce::Colour pageBgLibrary;
    static const juce::Colour pageBgLive;
    static const juce::Colour pageBgSettings;

    static void paintTintedPageBackground(juce::Graphics& g,
                                          juce::Rectangle<int> bounds,
                                          const juce::Colour& baseBg,
                                          const juce::Colour& accent);

    void drawButtonBackground(juce::Graphics& g,
                              juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;

    void drawLinearSlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPos,
                          float minSliderPos,
                          float maxSliderPos,
                          const juce::Slider::SliderStyle style,
                          juce::Slider& slider) override;

    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox& box) override;

    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override;

    void drawTableHeaderColumn(juce::Graphics& g, juce::TableHeaderComponent& header,
                               const juce::String& columnName, int columnId,
                               int width, int height, bool isMouseOver,
                               bool isMouseDown, int columnFlags) override;
};

} // namespace tracks
