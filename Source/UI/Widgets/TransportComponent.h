#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BeatPlayheadComponent.h"
#include "OutputMeterComponent.h"
#include "../../Audio/AudioEngine.h"
#include "../../Models/SongTrack.h"
#include <optional>
#include <vector>
#include <memory>

namespace tracks {

class TransportComponent : public juce::Component, private juce::Timer {
public:
    enum class LayoutMode {
        DockedBar,
        ExpandedPanel
    };

    explicit TransportComponent(AudioEngine& audioEngine);
    ~TransportComponent() override;

    void setLayoutMode(LayoutMode mode);
    LayoutMode getLayoutMode() const { return layoutMode_; }

    void setSongInfo(const std::string& title,
                     const std::string& artist,
                     int tempo,
                     const std::string& key,
                     const std::string& timeSignature);
    void syncTracksAvailability(std::optional<int64_t> songId,
                                const std::vector<SongTrack>& tracks,
                                bool hasLoadedTracks,
                                const std::vector<SongSection>& sections = {});
    void syncArrangements(const std::vector<SongArrangement>& arrangements,
                          std::optional<int64_t> activeArrangementId,
                          int clickOnlyTempo,
                          const std::string& clickOnlyKey,
                          const std::string& clickOnlyTimeSig,
                          bool clickOnlyEighths,
                          bool clickOnlySixteenths,
                          float clickOnlyEighthsGain,
                          float clickOnlySixteenthsGain);
    void syncClickSubdivisions(std::optional<int64_t> songId,
                               bool enableEighths,
                               bool enableSixteenths,
                               float eighthsGain,
                               float sixteenthsGain);
    void syncClickAndGuideChannels(int clickChannel, int guideChannel);

    void setPlayingState(bool isPlaying, bool isPaused);
    void setSongNavigationState(bool canGoPrev, bool canGoNext);

    // Callbacks
    std::function<void()> onPlayClicked;
    std::function<void()> onPauseClicked;
    std::function<void()> onStopClicked;
    std::function<void()> onPreviousSongClicked;
    std::function<void()> onNextSongClicked;
    std::function<void(float)> onMasterGainChanged;
    std::function<void(float)> onClickGainChanged;
    std::function<void(float)> onGuideGainChanged;
    std::function<void(int)> onClickChannelChanged;
    std::function<void(int)> onGuideChannelChanged;
    std::function<void(int64_t, float, int, bool)> onTrackMixChanged;
    std::function<void(int64_t, std::optional<int64_t>, bool, bool, float, float)> onSongClickSubdivisionsChanged;
    std::function<void(std::optional<int64_t>, int, const std::string&, const std::string&)> onSongParamsChanged;
    std::function<void(int64_t)> onArrangementChanged;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    AudioEngine& audioEngine_;

    class TransportIconButton;
    std::unique_ptr<TransportIconButton> prevSongButton_;
    std::unique_ptr<TransportIconButton> playPauseButton_;
    std::unique_ptr<TransportIconButton> stopButton_;
    std::unique_ptr<TransportIconButton> nextSongButton_;

    juce::Label songTitleLabel_;
    juce::Label songArtistLabel_;

    juce::TextEditor tempoEditor_;
    juce::Label bpmSuffixLabel_{{}, "BPM"};
    juce::Label keyPrefixLabel_{{}, "Key:"};
    juce::ComboBox keyBox_;
    juce::ComboBox timeSigBox_;
    juce::ToggleButton useTracksToggle_{"Use tracks"};
    juce::ComboBox arrangementBox_;
    juce::ToggleButton eighthsToggle_{"8ths"};
    juce::ToggleButton sixteenthsToggle_{"16ths"};
    int currentTempo_ = 120;
    std::string currentKey_ = "C";
    std::string currentTimeSig_ = "4/4";
    std::vector<SongArrangement> currentArrangements_;
    std::optional<int64_t> activeArrangementId_;
    int clickOnlyTempo_ = 120;
    std::string clickOnlyKey_ = "C";
    std::string clickOnlyTimeSig_ = "4/4";
    bool clickOnlyEighths_ = false;
    bool clickOnlySixteenths_ = false;
    float clickOnlyEighthsGain_ = 1.0f;
    float clickOnlySixteenthsGain_ = 1.0f;

    juce::Slider masterSlider_;
    juce::Label masterLabel_{{}, "Master"};

    juce::Slider clickSlider_;
    juce::Label clickLabel_{{}, "Click"};
    juce::ComboBox clickChannelBox_;

    juce::Slider eighthClickSlider_;
    juce::Label eighthClickLabel_{{}, "8th Click"};

    juce::Slider sixteenthClickSlider_;
    juce::Label sixteenthClickLabel_{{}, "16th Click"};

    juce::Slider guideSlider_;
    juce::Label guideLabel_{{}, "Guide"};
    juce::ComboBox guideChannelBox_;

    class TrackStripComponent;
    juce::Viewport trackStripsViewport_;
    std::unique_ptr<juce::Component> trackStripsContainer_;
    std::vector<std::unique_ptr<TrackStripComponent>> trackStrips_;
    std::vector<SongTrack> currentTracks_;

    BeatPlayheadComponent beatPlayhead_;
    OutputMeterComponent outputMeter_;

    LayoutMode layoutMode_ = LayoutMode::DockedBar;
    std::vector<juce::Rectangle<int>> faderStripBounds_;
    uint64_t lastWaveformVersion_ = 0;
    std::optional<int64_t> lastSongId_;
    std::vector<std::string> lastTrackPaths_;
    bool lastHadTracks_ = false;
    int lastDeviceChannelCount_ = 0;

    void refreshAvailableChannels(int numChannels);
    void rebuildTrackStrips();
    void updateTrackStripsEnabled();
    void notifyClickSubdivisionsChanged();
    void notifySongParamsChanged();
    void applyActiveModeSettings();
    void updateSongParamEditorsEnabled();
    void commitTempoEditor(bool clampAndFormat);
    void updateSubdivisionFaderVisibility();
    void layoutTrackStrips(int viewportWidth, int viewportHeight);
    void timerCallback() override;
};

} // namespace tracks
