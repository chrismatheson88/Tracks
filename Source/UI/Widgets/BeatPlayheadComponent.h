#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../Models/Song.h"
#include <vector>

namespace tracks {

class BeatPlayheadComponent : public juce::Component {
public:
    BeatPlayheadComponent();
    ~BeatPlayheadComponent() override = default;

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
                        bool clickLoopEnabled = true);

    void paint(juce::Graphics& g) override;

private:
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
    double trackDurationBars_ = 0.0;
    int trackBarCount_ = 0;
    std::vector<float> waveformPeaks_;
    std::vector<double> barPositions_;
    double waveformDurationFraction_ = 1.0;
    std::vector<SongSection> sections_;
};

} // namespace tracks
