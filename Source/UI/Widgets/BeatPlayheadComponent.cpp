#include "BeatPlayheadComponent.h"
#include "../../Common/AppTheme.h"
#include <algorithm>
#include <cmath>

namespace tracks {

BeatPlayheadComponent::BeatPlayheadComponent() {
    setOpaque(false);
}

void BeatPlayheadComponent::setWaveformData(std::vector<float> peaks) {
    waveformPeaks_ = std::move(peaks);
    repaint();
}

void BeatPlayheadComponent::setSections(std::vector<SongSection> sections) {
    sections_ = std::move(sections);
    repaint();
}

void BeatPlayheadComponent::updatePlayhead(double totalBarsElapsed,
                                           int currentBeatInBar,
                                           int currentBar,
                                           int beatsPerBar,
                                           bool isPlaying,
                                           bool isPaused,
                                           bool isCompound,
                                           bool showTracksWaveform,
                                           double trackDurationBars,
                                           int trackBarCount,
                                           bool isLoadingTracks,
                                           bool hasLeadingCountSection) {
    totalBarsElapsed_ = std::max(0.0, totalBarsElapsed);
    currentBeatInBar_ = currentBeatInBar;
    currentBar_ = currentBar;
    beatsPerBar_ = std::max(1, beatsPerBar);
    isPlaying_ = isPlaying;
    isPaused_ = isPaused;
    isCompound_ = isCompound;
    showTracksWaveform_ = showTracksWaveform;
    isLoadingTracks_ = isLoadingTracks;
    hasLeadingCountSection_ = hasLeadingCountSection;
    trackDurationBars_ = std::max(0.0, trackDurationBars);
    trackBarCount_ = std::max(0, trackBarCount);
    repaint();
}

void BeatPlayheadComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();

    // Background container
    g.setColour(AppTheme::bgPanel);
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(AppTheme::borderCol);
    g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

    const bool isExpanded = (bounds.getWidth() >= 300.0f && bounds.getHeight() >= 68.0f);
    const float padX = isExpanded ? 12.0f : 8.0f;
    const float padY = isExpanded ? 8.0f : 6.0f;
    const float contentX = bounds.getX() + padX;
    const float contentY = bounds.getY() + padY;
    const float contentW = std::max(1.0f, bounds.getWidth() - padX * 2.0f);
    const float contentH = std::max(1.0f, bounds.getHeight() - padY * 2.0f);

    const bool hasActivePosition = isPlaying_;
    const bool inCountIn = hasActivePosition && (totalBarsElapsed_ < 2.0);
    const int barIndex = static_cast<int>(std::floor(totalBarsElapsed_));
    const double barFraction = totalBarsElapsed_ - std::floor(totalBarsElapsed_);

    const bool waveformMode = showTracksWaveform_ && !waveformPeaks_.empty() && trackBarCount_ > 0;

    if (showTracksWaveform_ && isLoadingTracks_ && waveformPeaks_.empty()) {
        const float statusH = isExpanded ? 15.0f : 12.0f;
        g.setFont(juce::Font(isExpanded ? 11.5f : 9.5f, juce::Font::bold));
        g.setColour(AppTheme::warningAmber);
        g.drawText("LOADING TRACKS...", contentX, contentY, contentW, statusH,
                   isExpanded ? juce::Justification::centredLeft : juce::Justification::centred);

        const float trackY = contentY + statusH + (isExpanded ? 16.0f : 5.0f);
        const float trackH = std::max(14.0f, contentY + contentH - trackY);
        juce::Rectangle<float> trackRect(contentX, trackY, contentW, trackH);
        g.setColour(AppTheme::bgInput);
        g.fillRoundedRectangle(trackRect, 4.0f);
        g.setColour(AppTheme::borderCol);
        g.drawRoundedRectangle(trackRect, 4.0f, 1.0f);

        g.setFont(juce::Font(isExpanded ? 12.0f : 10.0f, juce::Font::plain));
        g.setColour(AppTheme::textMuted);
        g.drawText("Decoding audio stems in background...", trackRect, juce::Justification::centred, true);
        return;
    }

    const bool leadingCountInAudio = waveformMode && hasLeadingCountSection_ && !sections_.empty();
    const int prependedCountBars = leadingCountInAudio ? 0 : 2;
    const bool inPrependedCountIn = hasActivePosition && (prependedCountBars > 0) && (totalBarsElapsed_ < static_cast<double>(prependedCountBars));

    // Determine active section (if sections are defined and we are inside the audio timeline)
    int activeSectionIdx = -1;
    int activeSectionLocalBar = 0;
    int activeSectionLength = 0;
    bool inCountSection = false;
    if (waveformMode && !sections_.empty() && hasActivePosition && !inPrependedCountIn) {
        int audioBarZeroIdx = std::max(0, barIndex - prependedCountBars);
        int cursor = 0;
        for (size_t i = 0; i < sections_.size(); ++i) {
            int len = std::max(1, sections_[i].lengthBars);
            if (audioBarZeroIdx >= cursor && audioBarZeroIdx < cursor + len) {
                activeSectionIdx = static_cast<int>(i);
                activeSectionLocalBar = (audioBarZeroIdx - cursor) + 1;
                activeSectionLength = len;
                inCountSection = sections_[i].isCountSection();
                break;
            }
            cursor += len;
        }
    }

    const bool activeIsCount = inPrependedCountIn || inCountSection;

    // Status header
    juce::String statusText;
    juce::Colour statusCol = AppTheme::textMuted;
    if (isPlaying_) {
        if (inPrependedCountIn) {
            statusText = (isPaused_ ? "PAUSED - COUNT-IN " : "COUNT-IN - BAR ") +
                         juce::String(std::min(2, barIndex + 1)) + "/2";
            statusCol = AppTheme::warningAmber;
        } else if (waveformMode && activeSectionIdx >= 0) {
            const auto& sec = sections_[static_cast<size_t>(activeSectionIdx)];
            int audioBarNum = std::min(trackBarCount_, std::max(1, barIndex - prependedCountBars + 1));
            statusText = (isPaused_ ? "PAUSED - " : "") +
                         juce::String(sec.name).toUpperCase() +
                         " - BAR " + juce::String(activeSectionLocalBar) + "/" + juce::String(activeSectionLength) +
                         "  (Bar " + juce::String(audioBarNum) + "/" + juce::String(trackBarCount_) + ")";
            statusCol = (isPaused_ || inCountSection) ? AppTheme::warningAmber : AppTheme::accentGreen;
        } else {
            int songBarNum = std::max(1, barIndex - prependedCountBars + 1);
            if (waveformMode) {
                songBarNum = std::min(trackBarCount_, songBarNum);
                statusText = (isPaused_ ? "PAUSED - BAR " : "BAR ") +
                             juce::String(songBarNum) + " / " + juce::String(trackBarCount_);
            } else {
                statusText = (isPaused_ ? "PAUSED - BAR " : "BAR ") + juce::String(songBarNum);
            }
            statusCol = isPaused_ ? AppTheme::warningAmber : AppTheme::accentGreen;
        }
    } else {
        if (waveformMode && !sections_.empty()) {
            statusText = "STOPPED - " + juce::String(sections_.size()) +
                         (sections_.size() == 1 ? " SECTION, " : " SECTIONS, ") +
                         juce::String(trackBarCount_) + (trackBarCount_ == 1 ? " BAR" : " BARS");
        } else {
            statusText = waveformMode
                             ? ("STOPPED - " + juce::String(trackBarCount_) + (trackBarCount_ == 1 ? " BAR" : " BARS"))
                             : "STOPPED";
        }
    }

    const float statusH = isExpanded ? 15.0f : 12.0f;
    g.setFont(juce::Font(isExpanded ? 11.5f : 9.5f, juce::Font::bold));
    g.setColour(statusCol);
    g.drawText(statusText, contentX, contentY, contentW, statusH,
               isExpanded ? juce::Justification::centredLeft : juce::Justification::centred);

    const float trackY = contentY + statusH + (isExpanded ? 16.0f : 5.0f);
    const float trackH = std::max(14.0f, contentY + contentH - trackY);
    const juce::Rectangle<float> trackRect(contentX, trackY, contentW, trackH);

    // =========================================================================
    // MODE 1: Tracks Waveform Mode (with section splitting & count handling)
    // =========================================================================
    if (waveformMode) {
        float countInW = 0.0f;
        if (!leadingCountInAudio) {
            const float minCountInFrac = isExpanded ? 0.14f : 0.22f;
            const float naturalCountInFrac = 2.0f / static_cast<float>(2 + trackBarCount_);
            const float countInFrac = std::clamp(naturalCountInFrac, minCountInFrac, 0.40f);
            countInW = contentW * countInFrac;
        }

        const float countInBarW = countInW * 0.5f;
        const float tracksX = contentX + countInW;
        const float tracksW = std::max(1.0f, contentW - countInW);
        const float trackBarPx = tracksW / static_cast<float>(trackBarCount_);

        // Compute playhead X
        float playheadX = contentX;
        if (hasActivePosition) {
            if (!leadingCountInAudio && inPrependedCountIn) {
                playheadX = contentX + static_cast<float>(totalBarsElapsed_ / 2.0) * countInW;
            } else {
                double trackBarsElapsed = std::clamp(totalBarsElapsed_ - static_cast<double>(prependedCountBars),
                                                     0.0,
                                                     static_cast<double>(trackBarCount_));
                playheadX = tracksX + static_cast<float>(trackBarsElapsed / static_cast<double>(trackBarCount_)) * tracksW;
            }
        }

        // Section headers above track in expanded mode
        if (isExpanded) {
            const float secLabelY = contentY + statusH + 2.0f;
            const float secLabelH = 12.0f;
            g.setFont(juce::Font(9.5f, juce::Font::bold));

            if (!leadingCountInAudio) {
                g.setColour(inPrependedCountIn ? AppTheme::warningAmber : AppTheme::textMuted.withAlpha(0.75f));
                g.drawText(countInW >= 120.0f ? "COUNT-IN (2 BARS)" : "COUNT-IN",
                           contentX + 4.0f, secLabelY, countInW - 8.0f, secLabelH, juce::Justification::centredLeft, true);
            }

            juce::Colour tracksLblCol = (hasActivePosition && !activeIsCount)
                                            ? AppTheme::accentGreen
                                            : (activeIsCount && leadingCountInAudio ? AppTheme::warningAmber : AppTheme::primaryBlue.brighter(0.2f));
            g.setColour(tracksLblCol);
            juce::String tracksTitle = (!sections_.empty() ? "SECTIONS & TRACKS (" : "TRACKS (") +
                                       juce::String(trackBarCount_) +
                                       (trackBarCount_ == 1 ? " BAR)" : " BARS)");
            g.drawText(tracksTitle, tracksX + 4.0f, secLabelY, tracksW - 8.0f, secLabelH, juce::Justification::centredLeft, true);
        }

        // Track background
        g.setColour(AppTheme::bgInput);
        g.fillRoundedRectangle(trackRect, 4.0f);

        // Draw prepended 2-bar count-in region (only when song does NOT start with a "count" section)
        if (!leadingCountInAudio) {
            if (hasActivePosition && inPrependedCountIn) {
                float trailW = std::max(0.0f, playheadX - contentX);
                g.setColour(AppTheme::warningAmber.withAlpha(0.22f));
                g.fillRoundedRectangle(contentX, trackY, trailW, trackH, 3.0f);
            }

            const float countInBeatW = countInBarW / static_cast<float>(beatsPerBar_);
            const int activeCountInBar = inPrependedCountIn ? std::clamp(barIndex, 0, 1) : -1;
            const int activeBeatInBar = hasActivePosition
                                            ? std::clamp(static_cast<int>(std::floor(barFraction * beatsPerBar_)), 0, beatsPerBar_ - 1)
                                            : -1;

            for (int b = 0; b < 2; ++b) {
                const float bx = contentX + static_cast<float>(b) * countInBarW;
                for (int beat = 0; beat < beatsPerBar_; ++beat) {
                    const float beatX = bx + static_cast<float>(beat) * countInBeatW;
                    const bool isAccentBeat = isCompound_ ? (beat % 3 == 0) : (beat == 0);
                    if (b == activeCountInBar && beat == activeBeatInBar) {
                        g.setColour(AppTheme::warningAmber.withAlpha(0.24f));
                        g.fillRect(beatX + 0.5f, trackY + 1.0f, std::max(0.0f, countInBeatW - 1.0f), trackH - 2.0f);
                    }
                    if (beat > 0 && countInBeatW >= 6.0f) {
                        g.setColour(isAccentBeat ? AppTheme::borderCol.brighter(0.2f) : AppTheme::borderCol.withAlpha(0.45f));
                        g.drawVerticalLine(static_cast<int>(std::round(beatX)), trackY + 5.0f, trackY + trackH - 5.0f);
                    }
                    if (isExpanded && countInBeatW >= 18.0f) {
                        g.setFont(juce::Font(isAccentBeat ? 11.0f : 9.5f, isAccentBeat ? juce::Font::bold : juce::Font::plain));
                        g.setColour((b == activeCountInBar && beat == activeBeatInBar)
                                        ? AppTheme::textLight
                                        : (hasActivePosition && !inPrependedCountIn ? AppTheme::textMuted.withAlpha(0.30f) : AppTheme::textMuted.withAlpha(0.75f)));
                        g.drawText(juce::String(beat + 1), beatX, trackY, countInBeatW, trackH, juce::Justification::centred);
                    }
                }
            }
            g.setColour(AppTheme::borderCol.brighter(0.35f));
            g.fillRect(contentX + countInBarW - 0.5f, trackY, 1.0f, trackH);
        }

        // Section background fills (splits the audio timeline cleanly into sections in order)
        const float sectionHeaderH = (!sections_.empty() && isExpanded && trackH >= 38.0f) ? 16.0f : 0.0f;
        if (!sections_.empty()) {
            int startBarZeroIdx = 0;
            for (size_t i = 0; i < sections_.size(); ++i) {
                const auto& sec = sections_[i];
                int len = std::max(1, sec.lengthBars);
                if (startBarZeroIdx >= trackBarCount_) break;

                float secX = tracksX + static_cast<float>(startBarZeroIdx) * trackBarPx;
                float secW = static_cast<float>(std::min(len, trackBarCount_ - startBarZeroIdx)) * trackBarPx;
                const bool isCount = sec.isCountSection();
                const bool isActiveSec = (static_cast<int>(i) == activeSectionIdx);

                juce::Colour bgTint;
                if (isCount) {
                    bgTint = AppTheme::warningAmber.withAlpha(isActiveSec ? 0.22f : 0.10f);
                } else if (isActiveSec) {
                    bgTint = AppTheme::accentGreen.withAlpha(0.14f);
                } else {
                    bgTint = (i % 2 == 0) ? AppTheme::primaryBlue.withAlpha(0.08f)
                                          : AppTheme::bgPanel.withAlpha(0.35f);
                }
                g.setColour(bgTint);
                g.fillRect(secX, trackY, secW, trackH);

                if (sectionHeaderH > 0.0f) {
                    juce::Colour hdrBg = isCount
                                             ? AppTheme::warningAmber.withAlpha(isActiveSec ? 0.35f : 0.18f)
                                             : (isActiveSec ? AppTheme::accentGreen.withAlpha(0.28f)
                                                            : AppTheme::bgPanel.brighter(i % 2 == 0 ? 0.08f : 0.02f));
                    g.setColour(hdrBg);
                    g.fillRect(secX, trackY, secW, sectionHeaderH);
                }

                startBarZeroIdx += len;
            }
        } else {
            g.setColour(AppTheme::primaryBlue.withAlpha((hasActivePosition && !inPrependedCountIn) ? 0.10f : 0.05f));
            g.fillRect(tracksX, trackY, tracksW, trackH);
        }

        // Bar grid lines across the N track bars
        const float waveTopY = trackY + sectionHeaderH;
        const float waveH = std::max(10.0f, trackH - sectionHeaderH);
        int barGridStep = 1;
        if (trackBarPx < 5.0f) barGridStep = 4;
        if (trackBarPx < 2.0f) barGridStep = 8;

        int barLabelStep = 1;
        if (trackBarPx < 24.0f) barLabelStep = 4;
        if (trackBarPx < 10.0f) barLabelStep = 8;
        if (trackBarPx < 5.0f) barLabelStep = 16;

        for (int b = 0; b < trackBarCount_; ++b) {
            const float bx = tracksX + static_cast<float>(b) * trackBarPx;
            if (b > 0 && (b % barGridStep == 0)) {
                const bool isPhrase = (b % 4 == 0);
                g.setColour(AppTheme::borderCol.withAlpha(isPhrase ? 0.55f : 0.25f));
                g.drawVerticalLine(static_cast<int>(std::round(bx)), waveTopY, trackY + trackH - 1.0f);
            }
            if (trackBarPx >= 48.0f) {
                const float beatPx = trackBarPx / static_cast<float>(beatsPerBar_);
                for (int beat = 1; beat < beatsPerBar_; ++beat) {
                    const float beatX = bx + static_cast<float>(beat) * beatPx;
                    g.setColour(AppTheme::borderCol.withAlpha(0.22f));
                    g.drawVerticalLine(static_cast<int>(std::round(beatX)), waveTopY + 4.0f, trackY + trackH - 4.0f);
                }
            }
        }

        // Center zero-line for waveform
        const float midY = waveTopY + waveH * 0.5f;
        g.setColour(AppTheme::borderCol.withAlpha(0.35f));
        g.drawHorizontalLine(static_cast<int>(std::round(midY)), tracksX + 1.0f, tracksX + tracksW - 1.0f);

        // Draw summed waveform peaks across the actual audio duration inside the N bars
        const float durationFrac = std::clamp(static_cast<float>(trackDurationBars_ / static_cast<double>(trackBarCount_)), 0.0f, 1.0f);
        const float waveformW = std::max(1.0f, tracksW * durationFrac);
        const float maxHalfH = std::max(2.0f, (waveH - 6.0f) * 0.46f);
        const int numBins = static_cast<int>(waveformPeaks_.size());
        const int colStep = isExpanded ? 2 : 1;
        const float barDrawW = isExpanded ? 1.5f : 1.0f;

        for (float dx = 0.0f; dx < waveformW; dx += static_cast<float>(colStep)) {
            int binStart = std::clamp(static_cast<int>((dx / waveformW) * static_cast<float>(numBins)), 0, numBins - 1);
            int binEnd = std::clamp(static_cast<int>(((dx + static_cast<float>(colStep)) / waveformW) * static_cast<float>(numBins)), binStart + 1, numBins);

            float peak = 0.0f;
            for (int bi = binStart; bi < binEnd; ++bi) {
                peak = std::max(peak, waveformPeaks_[static_cast<size_t>(bi)]);
            }

            float halfH = std::max(1.0f, peak * maxHalfH);
            float drawX = tracksX + dx;
            bool isPlayed = hasActivePosition && !inPrependedCountIn && (drawX <= playheadX);
            juce::Colour playedCol = activeIsCount ? AppTheme::warningAmber.withAlpha(0.85f)
                                                   : AppTheme::accentGreen.withAlpha(0.90f);
            g.setColour(isPlayed ? playedCol : AppTheme::primaryBlue.withAlpha(0.65f));
            g.fillRect(drawX, midY - halfH, barDrawW, halfH * 2.0f);
        }

        // Section dividers & header text (drawn cleanly on top)
        if (!sections_.empty()) {
            if (sectionHeaderH > 0.0f) {
                g.setColour(AppTheme::borderCol.withAlpha(0.75f));
                g.drawHorizontalLine(static_cast<int>(std::round(waveTopY)), tracksX, tracksX + tracksW);
            }

            int startBarZeroIdx = 0;
            for (size_t i = 0; i < sections_.size(); ++i) {
                const auto& sec = sections_[i];
                int len = std::max(1, sec.lengthBars);
                if (startBarZeroIdx >= trackBarCount_) break;

                float secX = tracksX + static_cast<float>(startBarZeroIdx) * trackBarPx;
                float secW = static_cast<float>(std::min(len, trackBarCount_ - startBarZeroIdx)) * trackBarPx;
                const bool isCount = sec.isCountSection();
                const bool isActiveSec = (static_cast<int>(i) == activeSectionIdx);

                if (startBarZeroIdx > 0) {
                    g.setColour(isCount ? AppTheme::warningAmber.withAlpha(0.9f)
                                        : AppTheme::textMuted.withAlpha(0.85f));
                    g.fillRect(secX - 0.75f, trackY + 1.0f, 1.5f, trackH - 2.0f);
                }

                if (sectionHeaderH > 0.0f && secW >= 16.0f) {
                    g.setFont(juce::Font(9.5f, juce::Font::bold));
                    g.setColour(isActiveSec
                                    ? AppTheme::textLight
                                    : (isCount ? AppTheme::warningAmber : AppTheme::textLight.withAlpha(0.85f)));
                    g.drawText(sec.name, secX + 3.0f, trackY, secW - 6.0f, sectionHeaderH,
                               juce::Justification::centredLeft, true);
                }

                startBarZeroIdx += len;
            }
            // Trailing divider if sections end before the audio ends
            if (startBarZeroIdx > 0 && startBarZeroIdx < trackBarCount_) {
                float endX = tracksX + static_cast<float>(startBarZeroIdx) * trackBarPx;
                g.setColour(AppTheme::borderCol.brighter(0.25f));
                g.fillRect(endX - 0.5f, trackY + 1.0f, 1.0f, trackH - 2.0f);
            }
        } else if (isExpanded && trackH >= 38.0f) {
            g.setFont(juce::Font(8.5f, juce::Font::bold));
            g.setColour(AppTheme::textMuted.withAlpha(0.65f));
            for (int b = 0; b < trackBarCount_; b += barLabelStep) {
                const float bx = tracksX + static_cast<float>(b) * trackBarPx;
                if (bx + 18.0f <= tracksX + tracksW) {
                    g.drawText(juce::String(b + 1), bx + 2.0f, trackY + 1.0f, 22.0f, 10.0f, juce::Justification::centredLeft);
                }
            }
        }

        // Bold divider between prepended Count-In and Tracks Waveform (when present)
        if (!leadingCountInAudio) {
            g.setColour(AppTheme::primaryBlue.withAlpha(0.9f));
            g.fillRect(tracksX - 0.75f, trackY, 1.5f, trackH);
        }

        // Track outer border
        g.setColour(AppTheme::borderCol);
        g.drawRoundedRectangle(trackRect, 4.0f, 1.0f);

        // Moving playbar cursor
        const float clampedPlayheadX = std::clamp(playheadX, contentX + 1.5f, contentX + contentW - 1.5f);
        const juce::Colour cursorGlow = hasActivePosition
                                            ? (activeIsCount ? AppTheme::warningAmber : AppTheme::accentGreen)
                                            : AppTheme::textMuted;

        if (hasActivePosition) {
            g.setColour(cursorGlow.withAlpha(0.35f));
            g.fillRect(clampedPlayheadX - 3.5f, trackY, 7.0f, trackH);
        }

        g.setColour(hasActivePosition ? juce::Colours::white : AppTheme::textMuted.withAlpha(0.6f));
        g.fillRect(clampedPlayheadX - 1.25f, trackY - 1.0f, 2.5f, trackH + 2.0f);

        juce::Path cap;
        float capHalfW = isExpanded ? 5.0f : 3.5f;
        float capH = isExpanded ? 6.0f : 4.5f;
        cap.addTriangle(clampedPlayheadX - capHalfW, trackY - capH + 1.0f,
                        clampedPlayheadX + capHalfW, trackY - capH + 1.0f,
                        clampedPlayheadX, trackY + 2.0f);
        g.setColour(cursorGlow);
        g.fillPath(cap);
        return;
    }

    // =========================================================================
    // MODE 2: Click Loop Mode (2 count-in bars + 1 looping bar)
    // =========================================================================
    // Map continuous bar progress onto the 3 visual bars:
    // Bar 0 = Count-In 1 [0..1), Bar 1 = Count-In 2 [1..2), Bar 2 = Looping Song Bar [2..3)
    double visualBarPos = 0.0;
    int activeVisualBar = -1;
    if (hasActivePosition) {
        if (totalBarsElapsed_ < 2.0) {
            visualBarPos = totalBarsElapsed_;
            activeVisualBar = std::clamp(barIndex, 0, 1);
        } else {
            visualBarPos = 2.0 + barFraction;
            activeVisualBar = 2;
        }
    }

    const float barW = contentW / 3.0f;

    if (isExpanded) {
        const float secLabelY = contentY + statusH + 2.0f;
        const float secLabelH = 12.0f;
        g.setFont(juce::Font(9.5f, juce::Font::bold));

        const char* barTitles[3] = { "COUNT-IN 1", "COUNT-IN 2", "LOOP" };
        for (int b = 0; b < 3; ++b) {
            float bx = contentX + static_cast<float>(b) * barW;
            juce::Colour lblCol = AppTheme::textMuted.withAlpha(0.75f);
            if (activeVisualBar == b) {
                lblCol = (b < 2) ? AppTheme::warningAmber : AppTheme::accentGreen;
            } else if (b == 2) {
                lblCol = AppTheme::primaryBlue.brighter(0.2f);
            }
            g.setColour(lblCol);
            g.drawText(barTitles[b], bx + 4.0f, secLabelY, barW - 8.0f, secLabelH, juce::Justification::centredLeft);
        }
    }

    // Track background
    g.setColour(AppTheme::bgInput);
    g.fillRoundedRectangle(trackRect, 4.0f);

    // Subtle distinct tint for the looping 3rd bar
    {
        juce::Rectangle<float> loopRect(contentX + barW * 2.0f, trackY, barW, trackH);
        g.setColour(AppTheme::primaryBlue.withAlpha(activeVisualBar == 2 ? 0.14f : 0.07f));
        g.fillRoundedRectangle(loopRect, 4.0f);
    }

    // Sweep trail behind the moving playbar
    if (hasActivePosition) {
        const float playheadX = contentX + static_cast<float>(visualBarPos / 3.0) * contentW;
        if (inCountIn) {
            float trailW = std::max(0.0f, playheadX - contentX);
            g.setColour(AppTheme::warningAmber.withAlpha(0.22f));
            g.fillRoundedRectangle(contentX, trackY, trailW, trackH, 3.0f);
        } else {
            // Dim completed count-in bars slightly, illuminate current progress inside Bar 3
            float loopStartX = contentX + barW * 2.0f;
            float trailW = std::max(0.0f, playheadX - loopStartX);
            g.setColour(AppTheme::accentGreen.withAlpha(0.24f));
            g.fillRoundedRectangle(loopStartX, trackY, trailW, trackH, 3.0f);
        }
    }

    // Active beat cell highlight + beat subdivisions + barlines
    const float beatW = barW / static_cast<float>(beatsPerBar_);
    const int activeBeatInBar = hasActivePosition
                                    ? std::clamp(static_cast<int>(std::floor(barFraction * beatsPerBar_)), 0, beatsPerBar_ - 1)
                                    : -1;

    for (int b = 0; b < 3; ++b) {
        const float bx = contentX + static_cast<float>(b) * barW;

        for (int beat = 0; beat < beatsPerBar_; ++beat) {
            const float beatX = bx + static_cast<float>(beat) * beatW;
            const bool isCurrentCell = (b == activeVisualBar && beat == activeBeatInBar);
            const bool isAccentBeat = isCompound_ ? (beat % 3 == 0) : (beat == 0);

            if (isCurrentCell) {
                juce::Colour cellCol = (b < 2)
                                           ? AppTheme::warningAmber
                                           : (isAccentBeat ? AppTheme::accentGreen : AppTheme::primaryBlue);
                g.setColour(cellCol.withAlpha(0.22f));
                g.fillRect(beatX + 0.5f, trackY + 1.0f, std::max(0.0f, beatW - 1.0f), trackH - 2.0f);
            }

            // Beat subdivision tick (skip beat 0, which is a full barline)
            if (beat > 0) {
                g.setColour(isAccentBeat ? AppTheme::borderCol.brighter(0.25f) : AppTheme::borderCol.withAlpha(0.55f));
                float tickInset = isAccentBeat ? 3.0f : 6.0f;
                g.drawVerticalLine(static_cast<int>(std::round(beatX)), trackY + tickInset, trackY + trackH - tickInset);
            }

            // Beat numbers when wide enough
            if (isExpanded && beatW >= 18.0f) {
                g.setFont(juce::Font(isAccentBeat ? 11.5f : 10.0f, isAccentBeat ? juce::Font::bold : juce::Font::plain));
                if (isCurrentCell) {
                    g.setColour(AppTheme::textLight);
                } else if (hasActivePosition && !inCountIn && b < 2) {
                    g.setColour(AppTheme::textMuted.withAlpha(0.35f));
                } else {
                    g.setColour(isAccentBeat ? AppTheme::textLight.withAlpha(0.75f) : AppTheme::textMuted.withAlpha(0.7f));
                }
                g.drawText(juce::String(beat + 1), beatX, trackY, beatW, trackH, juce::Justification::centred);
            }
        }

        // Crisp barline separating Bar 1 | Bar 2 | Bar 3
        if (b > 0) {
            g.setColour(b == 2 ? AppTheme::primaryBlue.withAlpha(0.85f) : AppTheme::borderCol.brighter(0.35f));
            float lineX = bx;
            g.fillRect(lineX - 0.75f, trackY, 1.5f, trackH);
        }
    }

    // Track outer border (and subtle highlight around Bar 3 loop boundary)
    g.setColour(AppTheme::borderCol);
    g.drawRoundedRectangle(trackRect, 4.0f, 1.0f);

    if (activeVisualBar == 2) {
        juce::Rectangle<float> loopRect(contentX + barW * 2.0f, trackY, barW, trackH);
        g.setColour(AppTheme::accentGreen.withAlpha(0.65f));
        g.drawRoundedRectangle(loopRect.reduced(0.5f), 3.0f, 1.25f);
    }

    // Moving playbar cursor
    const float playheadX = std::clamp(contentX + static_cast<float>(visualBarPos / 3.0) * contentW,
                                       contentX + 1.5f,
                                       contentX + contentW - 1.5f);
    const juce::Colour cursorGlow = hasActivePosition
                                        ? (inCountIn ? AppTheme::warningAmber : AppTheme::accentGreen)
                                        : AppTheme::textMuted;

    // Soft glow behind playbar
    if (hasActivePosition) {
        g.setColour(cursorGlow.withAlpha(0.35f));
        g.fillRect(playheadX - 3.5f, trackY, 7.0f, trackH);
    }

    // Solid playbar line
    g.setColour(hasActivePosition ? juce::Colours::white : AppTheme::textMuted.withAlpha(0.6f));
    g.fillRect(playheadX - 1.25f, trackY - 1.0f, 2.5f, trackH + 2.0f);

    // Top playhead triangle cap
    juce::Path cap;
    float capHalfW = isExpanded ? 5.0f : 3.5f;
    float capH = isExpanded ? 6.0f : 4.5f;
    cap.addTriangle(playheadX - capHalfW, trackY - capH + 1.0f,
                    playheadX + capHalfW, trackY - capH + 1.0f,
                    playheadX, trackY + 2.0f);
    g.setColour(cursorGlow);
    g.fillPath(cap);
}

} // namespace tracks
