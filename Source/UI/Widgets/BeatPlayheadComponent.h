#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../Models/Song.h"
#include <functional>
#include <vector>

namespace tracks {

class BeatPlayheadComponent : public juce::Component,
                              public juce::SettableTooltipClient {
public:
    BeatPlayheadComponent();
    ~BeatPlayheadComponent() override = default;

    std::function<void(int)> onSectionClicked;

    void setWaveformData(std::vector<float> peaks);
    void setTimelineLayout(std::vector<double> barPositions, double waveformDurationFraction);
    void setSections(std::vector<SongSection> sections);

    void updatePlayhead(double totalBarsElapsed,
                        int currentBeatInBar,
                        int currentBar,
                        int beatsPerBar,
                        bool isPlaying,
                        bool isPaused,
                        bool isCompound = false,
                        bool showTracksWaveform = false,
                        double trackDurationBars = 0.0,
                        int trackBarCount = 0,
                        bool isLoadingTracks = false,
                        bool hasLeadingCountSection = false,
                        int queuedSectionIndex = -1,
                        int selectedStartSectionIndex = -1,
                        bool isSectionStartCountIn = false,
                        bool clickLoopEnabled = true,
                        bool isFadingOut = false);

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    struct SectionHitRect {
        int sectionIndex = -1;
        juce::Rectangle<float> bounds;
    };

    double totalBarsElapsed_ = 0.0;
    int currentBeatInBar_ = 0;
    int currentBar_ = 0;
    int beatsPerBar_ = 4;
    bool isPlaying_ = false;
    bool isPaused_ = false;
    bool isCompound_ = false;

    bool showTracksWaveform_ = false;
    bool isLoadingTracks_ = false;
    bool hasLeadingCountSection_ = false;
    int queuedSectionIndex_ = -1;
    int selectedStartSectionIndex_ = -1;
    bool isSectionStartCountIn_ = false;
    bool clickLoopEnabled_ = true;
    bool isFadingOut_ = false;
    double trackDurationBars_ = 0.0;
    int trackBarCount_ = 0;
    std::vector<float> waveformPeaks_;
    std::vector<double> barPositions_;
    double waveformDurationFraction_ = 1.0;
    std::vector<SongSection> sections_;
    std::vector<SectionHitRect> sectionHitRects_;
    int hoveredSectionIndex_ = -1;

    int findSectionAtPosition(juce::Point<float> pos) const;
    void updateHoverTooltip();

    // Offscreen Retina-aware cache for static waveform bars (avoids 512 fillRect calls every frame)
    juce::Image waveformUnplayedCache_;
    juce::Image waveformPlayedCache_;
    uint64_t waveformCacheRev_ = 0;
    uint64_t builtCacheRev_ = 0;
    int builtCacheW_ = 0;
    int builtCacheH_ = 0;
    float builtCacheScale_ = 1.0f;
    bool builtCacheIsExpanded_ = false;
    bool builtCacheActiveIsCount_ = false;
    int builtCacheBarCount_ = 0;
    double builtCacheDurationBars_ = 0.0;
    double builtCacheDurationFrac_ = 1.0;

    void rebuildWaveformCache(int logicalW, int logicalH, float scale, bool isExpanded, bool activeIsCount, float durationFrac);
};

} // namespace tracks
