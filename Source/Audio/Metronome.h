#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Models/Song.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>

namespace tracks {

class Metronome {
public:
    Metronome();
    ~Metronome() = default;

    void prepare(double sampleRate);
    void reset();

    void setTempo(int tempo);
    void setTimeSignature(const std::string& timeSignature);
    void setSections(const std::vector<SongSection>& sections);
    void setUseSectionCountIn(bool enabled);
    bool hasLeadingCountSection() const { return leadingCountBars_ > 0; }
    int getLeadingCountBars() const { return leadingCountBars_; }
    void setClickGain(float gain);
    void setEighthClickGain(float gain);
    void setSixteenthClickGain(float gain);
    void setClickSubdivisions(bool enableEighths, bool enableSixteenths);
    bool isEighthsEnabled() const { return enableEighths_; }
    bool isSixteenthsEnabled() const { return enableSixteenths_; }
    void setGuideGain(float gain);
    void setClickChannel(int channel);
    void setGuideChannel(int channel);
    int getClickChannel() const { return clickChannel_; }
    int getGuideChannel() const { return guideChannel_; }

    void loadClickSamples(const juce::AudioBuffer<float>& quarterClick,
                          const juce::AudioBuffer<float>& accentClick,
                          const juce::AudioBuffer<float>& eighthClick = {},
                          const juce::AudioBuffer<float>& sixteenthClick = {});
    void loadVoiceWord(const std::string& word, const juce::AudioBuffer<float>& buffer);
    void clearSectionCues();
    void loadSectionCue(const std::string& normalizedKey, const juce::AudioBuffer<float>& buffer);

    // Query the guide cue token scheduled for a given bar/beat (useful for testing & diagnostics).
    // Returns "section:<key>" for a section announcement, "one".."eight" for a count number, or "".
    std::string getScheduledGuideCueForBeat(int bar, int beatInBar) const;

    // Render metronome clicks and count-in voices for the specified block of frames
    void renderBlock(juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples);

    int getCurrentBeatInBar() const { return currentBeatInBar_; }
    int getCurrentBar() const { return currentBar_; }
    int getBeatsPerBar() const { return beatsPerBar_; }
    bool isCountIn() const { return metronomeFrame_ < totalCountInFrames_; }
    int64_t getMetronomeFrame() const { return metronomeFrame_; }
    int64_t getTotalCountInFrames() const { return totalCountInFrames_; }
    int64_t getFramesPerBar() const { return framesPerBarAtomic_.load(std::memory_order_relaxed); }
    double getTotalBarsElapsed() const { return totalBarsElapsed_.load(std::memory_order_relaxed); }
    bool isCompoundMeter() const { return isCompoundAtomic_.load(std::memory_order_relaxed); }

private:
    double sampleRate_ = 44100.0;
    int tempo_ = 120;
    std::string timeSignature_ = "4/4";
    float clickGain_ = 1.0f;
    float eighthClickGain_ = 1.0f;
    float sixteenthClickGain_ = 1.0f;
    bool enableEighths_ = false;
    bool enableSixteenths_ = false;
    float guideGain_ = 1.0f;
    int clickChannel_ = 0;
    int guideChannel_ = 0;

    int beatsPerBar_ = 4;
    int beatDivision_ = 4;
    bool isCompound_ = false;
    int pulseCountPerBar_ = 4;
    int64_t beatFrames_ = 22050;
    int64_t totalCountInFrames_ = 0;

    int64_t metronomeFrame_ = 0;
    int currentBeatInBar_ = 0;
    int currentBar_ = 0;
    struct CountWindow {
        int startBar = 0;
        int lengthBars = 2;
    };
    struct SectionAnnouncement {
        int cueBar = 0;
        std::string normalizedKey;
    };
    std::vector<CountWindow> sectionCountWindows_;
    std::vector<SectionAnnouncement> sectionAnnouncements_;
    bool useSectionCountIn_ = false;
    int leadingCountBars_ = 0;

    std::atomic<double> totalBarsElapsed_{0.0};
    std::atomic<int64_t> framesPerBarAtomic_{88200};
    std::atomic<bool> isCompoundAtomic_{false};

    juce::AudioBuffer<float> quarterClick_;
    juce::AudioBuffer<float> accentClick_;
    juce::AudioBuffer<float> eighthClick_;
    juce::AudioBuffer<float> sixteenthClick_;
    std::map<std::string, juce::AudioBuffer<float>> voiceWords_;
    std::map<std::string, juce::AudioBuffer<float>> sectionCues_;

    struct ActiveSample {
        const juce::AudioBuffer<float>* buffer = nullptr;
        int64_t startFrame = 0;
        float gain = 1.0f;
        int channel = 0;
    };
    std::vector<ActiveSample> activeSamples_;

    void updateTimingParameters();
    std::string getCountInVoiceWordForBeat(int bar, int beatInBar) const;
    const juce::AudioBuffer<float>* findSectionCueBuffer(const std::string& normalizedKey) const;
};

} // namespace tracks
