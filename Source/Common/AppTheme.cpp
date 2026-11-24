#include "AppTheme.h"

namespace tracks {

const juce::Colour AppTheme::bgDark       = juce::Colour::fromString("#FF18181B");
const juce::Colour AppTheme::bgPanel      = juce::Colour::fromString("#FF27272A");
const juce::Colour AppTheme::bgInput      = juce::Colour::fromString("#FF202023");
const juce::Colour AppTheme::borderCol    = juce::Colour::fromString("#FF3F3F46");
const juce::Colour AppTheme::primaryBlue  = juce::Colour::fromString("#FF3B82F6");
const juce::Colour AppTheme::accentGreen  = juce::Colour::fromString("#FF22C55E");
const juce::Colour AppTheme::textLight    = juce::Colour::fromString("#FFF4F4F5");
const juce::Colour AppTheme::textMuted    = juce::Colour::fromString("#FFA1A1AA");
const juce::Colour AppTheme::dangerRed    = juce::Colour::fromString("#FFEF4444");
const juce::Colour AppTheme::warningAmber  = juce::Colour::fromString("#FFF59E0B");

AppTheme::AppTheme() {
    setColour(juce::ResizableWindow::backgroundColourId, bgDark);
    setColour(juce::TextButton::buttonColourId, bgPanel);
    setColour(juce::TextButton::textColourOffId, textLight);
    setColour(juce::TextButton::textColourOnId, textLight);
    setColour(juce::ComboBox::backgroundColourId, bgInput);
    setColour(juce::ComboBox::textColourId, textLight);
    setColour(juce::ComboBox::outlineColourId, borderCol);
    setColour(juce::ComboBox::arrowColourId, textMuted);
    setColour(juce::TextEditor::backgroundColourId, bgInput);
    setColour(juce::TextEditor::textColourId, textLight);
    setColour(juce::TextEditor::outlineColourId, borderCol);
    setColour(juce::TextEditor::focusedOutlineColourId, primaryBlue);
    setColour(juce::ListBox::backgroundColourId, bgPanel);
    setColour(juce::ListBox::outlineColourId, borderCol);
    setColour(juce::Slider::trackColourId, primaryBlue);
    setColour(juce::Slider::thumbColourId, textLight);
    setColour(juce::ToggleButton::textColourId, textLight);
    setColour(juce::ToggleButton::tickColourId, primaryBlue);
    setColour(juce::ToggleButton::tickDisabledColourId, borderCol);
}

void AppTheme::drawButtonBackground(juce::Graphics& g,
                                    juce::Button& button,
                                    const juce::Colour& backgroundColour,
                                    bool shouldDrawButtonAsHighlighted,
                                    bool shouldDrawButtonAsDown) {
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    auto baseCol = backgroundColour;

    if (shouldDrawButtonAsDown) {
        baseCol = baseCol.darker(0.2f);
    } else if (shouldDrawButtonAsHighlighted) {
        baseCol = baseCol.brighter(0.15f);
    }

    g.setColour(baseCol);
    g.fillRoundedRectangle(bounds, 4.0f);

    g.setColour(borderCol);
    g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
}

void AppTheme::drawLinearSlider(juce::Graphics& g,
                                int x, int y, int width, int height,
                                float sliderPos,
                                float /*minSliderPos*/,
                                float /*maxSliderPos*/,
                                const juce::Slider::SliderStyle /*style*/,
                                juce::Slider& slider) {
    if (slider.isVertical()) {
        float trackWidth = 4.0f;
        float trackX = static_cast<float>(x) + (static_cast<float>(width) - trackWidth) * 0.5f;

        g.setColour(bgInput);
        g.fillRoundedRectangle(trackX, static_cast<float>(y), trackWidth, static_cast<float>(height), 2.0f);

        g.setColour(primaryBlue);
        g.fillRoundedRectangle(trackX, sliderPos, trackWidth, static_cast<float>(y + height) - sliderPos, 2.0f);

        // Thumb
        g.setColour(textLight);
        g.fillEllipse(trackX - 4.0f, sliderPos - 6.0f, 12.0f, 12.0f);
    } else {
        float trackHeight = 4.0f;
        float trackY = static_cast<float>(y) + (static_cast<float>(height) - trackHeight) * 0.5f;

        g.setColour(bgInput);
        g.fillRoundedRectangle(static_cast<float>(x), trackY, static_cast<float>(width), trackHeight, 2.0f);

        g.setColour(primaryBlue);
        g.fillRoundedRectangle(static_cast<float>(x), trackY, sliderPos - static_cast<float>(x), trackHeight, 2.0f);

        // Thumb
        g.setColour(textLight);
        g.fillEllipse(sliderPos - 6.0f, trackY - 4.0f, 12.0f, 12.0f);
    }
}

void AppTheme::drawComboBox(juce::Graphics& g, int width, int height, bool /*isButtonDown*/,
                            int buttonX, int buttonY, int buttonW, int buttonH,
                            juce::ComboBox& box) {
    auto bounds = box.getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(bgInput);
    g.fillRoundedRectangle(bounds, 4.0f);

    g.setColour(box.hasKeyboardFocus(true) ? primaryBlue : borderCol);
    g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

    // Draw dropdown arrow
    juce::Path arrow;
    float arrowX = static_cast<float>(buttonX + buttonW / 2);
    float arrowY = static_cast<float>(buttonY + buttonH / 2);
    arrow.addTriangle(arrowX - 4.0f, arrowY - 2.0f,
                      arrowX + 4.0f, arrowY - 2.0f,
                      arrowX, arrowY + 3.0f);
    g.setColour(textMuted);
    g.fillPath(arrow);
}

void AppTheme::drawTableHeaderColumn(juce::Graphics& g, juce::TableHeaderComponent& /*header*/,
                                     const juce::String& columnName, int /*columnId*/,
                                     int width, int height, bool isMouseOver,
                                     bool /*isMouseDown*/, int /*columnFlags*/) {
    auto area = juce::Rectangle<int>(0, 0, width, height);
    g.setColour(isMouseOver ? bgPanel.brighter(0.05f) : bgPanel);
    g.fillRect(area);

    g.setColour(borderCol);
    g.drawHorizontalLine(height - 1, 0.0f, static_cast<float>(width));
    g.drawVerticalLine(width - 1, 0.0f, static_cast<float>(height));

    g.setColour(textMuted);
    g.setFont(juce::Font(12.0f, juce::Font::bold));
    g.drawText(columnName, area.reduced(6, 0), juce::Justification::centredLeft, true);
}

} // namespace tracks
