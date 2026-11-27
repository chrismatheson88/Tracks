#include "OutputMeterComponent.h"
#include "../../Common/AppTheme.h"
#include <cmath>
#include <algorithm>

namespace tracks {

OutputMeterComponent::OutputMeterComponent() {
    setOpaque(false);
    peaks_.assign(2, 0.0f);
}

void OutputMeterComponent::updatePeaks(const std::vector<float>& peaks) {
    bool changed = (peaks.size() != peaks_.size());
    if (!changed) {
        for (size_t i = 0; i < peaks.size(); ++i) {
            if (std::abs(peaks[i] - peaks_[i]) > 0.0005f) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) return;
    peaks_ = peaks;
    repaint();
}

void OutputMeterComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();

    g.setColour(AppTheme::bgPanel);
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(AppTheme::borderCol);
    g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

    if (peaks_.empty()) return;

    int numChannels = static_cast<int>(peaks_.size());
    float pad = 6.0f;
    float contentX = bounds.getX() + pad;
    float contentY = bounds.getY() + pad;
    float contentW = bounds.getWidth() - pad * 2.0f;
    float contentH = bounds.getHeight() - pad * 2.0f;

    float labelH = 12.0f;
    float meterH = contentH - labelH - 4.0f;

    float barGap = 4.0f;
    float totalGaps = static_cast<float>(numChannels - 1) * barGap;
    float barW = std::max(4.0f, (contentW - totalGaps) / static_cast<float>(numChannels));

    for (int ch = 0; ch < numChannels; ++ch) {
        float x = contentX + static_cast<float>(ch) * (barW + barGap);
        float meterY = contentY;

        // Background groove for meter
        g.setColour(AppTheme::bgInput);
        g.fillRoundedRectangle(x, meterY, barW, meterH, 2.0f);

        // Convert linear peak to dB (-48 dB to 0 dB range)
        float linear = std::clamp(peaks_[static_cast<size_t>(ch)], 0.0f, 1.0f);
        float db = (linear > 0.00001f) ? 20.0f * std::log10(linear) : -60.0f;
        float norm = std::clamp((db + 48.0f) / 48.0f, 0.0f, 1.0f);

        if (norm > 0.001f) {
            float fillH = norm * meterH;
            float fillY = meterY + meterH - fillH;

            // Gradient: green at bottom, amber at mid, red at top
            juce::Colour barCol = AppTheme::accentGreen;
            if (db > -1.0f) {
                barCol = AppTheme::dangerRed;
            } else if (db > -6.0f) {
                barCol = AppTheme::warningAmber;
            }

            g.setColour(barCol);
            g.fillRoundedRectangle(x, fillY, barW, fillH, 2.0f);
        }

        // Channel number label
        g.setFont(juce::Font(9.0f, juce::Font::bold));
        g.setColour(AppTheme::textMuted);
        g.drawText(juce::String(ch + 1), x - 2.0f, contentY + meterH + 4.0f, barW + 4.0f, labelH, juce::Justification::centred);
    }
}

} // namespace tracks
