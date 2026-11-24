#include "BeatPlayheadComponent.h"
#include "../../Common/AppTheme.h"

namespace tracks {

BeatPlayheadComponent::BeatPlayheadComponent() {
    setOpaque(false);
}

void BeatPlayheadComponent::updatePlayhead(int currentBeatInBar, int currentBar, int beatsPerBar, bool isPlaying, bool isCountIn) {
    currentBeatInBar_ = currentBeatInBar;
    currentBar_ = currentBar;
    beatsPerBar_ = std::max(1, beatsPerBar);
    isPlaying_ = isPlaying;
    isCountIn_ = isCountIn;
    repaint();
}

void BeatPlayheadComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();

    // Background container
    g.setColour(AppTheme::bgPanel);
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(AppTheme::borderCol);
    g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

    float padding = 8.0f;
    float contentX = bounds.getX() + padding;
    float contentY = bounds.getY() + padding;
    float contentW = bounds.getWidth() - padding * 2.0f;
    float contentH = bounds.getHeight() - padding * 2.0f;

    // Count-in / Bar status text
    juce::String statusText;
    if (isPlaying_) {
        if (isCountIn_) {
            statusText = "COUNT-IN (Bar " + juce::String(currentBar_ + 1) + "/2)";
        } else {
            statusText = "BAR " + juce::String(currentBar_ + 1);
        }
    } else {
        statusText = "STOPPED";
    }

    g.setFont(juce::Font(10.0f, juce::Font::bold));
    g.setColour(isCountIn_ ? AppTheme::warningAmber : AppTheme::textMuted);
    g.drawText(statusText, contentX, contentY, contentW, 12.0f, juce::Justification::centred);

    // Draw beat dots / indicators
    float dotsY = contentY + 16.0f;
    float dotsH = contentH - 16.0f;
    float dotSize = std::min(14.0f, std::min(dotsH, (contentW / static_cast<float>(beatsPerBar_)) - 6.0f));
    float totalDotsW = beatsPerBar_ * dotSize + (beatsPerBar_ - 1) * 6.0f;
    float startX = contentX + (contentW - totalDotsW) * 0.5f;

    for (int i = 0; i < beatsPerBar_; ++i) {
        float x = startX + static_cast<float>(i) * (dotSize + 6.0f);
        float y = dotsY + (dotsH - dotSize) * 0.5f;

        bool isCurrent = isPlaying_ && (i == currentBeatInBar_);
        bool isAccent = (i == 0);

        if (isCurrent) {
            g.setColour(isAccent ? AppTheme::accentGreen : AppTheme::primaryBlue);
            g.fillEllipse(x, y, dotSize, dotSize);
            // Glow border
            g.setColour(juce::Colours::white);
            g.drawEllipse(x, y, dotSize, dotSize, 1.5f);
        } else {
            g.setColour(isAccent ? AppTheme::borderCol.brighter(0.2f) : AppTheme::bgInput);
            g.fillEllipse(x, y, dotSize, dotSize);
            g.setColour(AppTheme::borderCol);
            g.drawEllipse(x, y, dotSize, dotSize, 1.0f);
        }
    }
}

} // namespace tracks
