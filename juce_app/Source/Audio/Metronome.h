#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace tracks {

class Metronome {
public:
    Metronome();
    ~Metronome() = default;

    void prepare(double sampleRate);
    void reset();

    void setTempo(int tempo);
    void setTimeSignature(const std::string& timeSignature);
    void setClickGain(float gain);
    void setGuideGain(float gain);
    void setClickChannel(int channel);
    void setGuideChannel(int channel);

    void loadClickSamples(const juce::AudioBuffer<float>& normalClick,
                          const juce::AudioBuffer<float>& accentClick);
    void loadVoiceWord(const std::string& word, const juce::AudioBuffer<float>& buffer);

    // Render metronome clicks and count-in voices for the specified block of frames
    void renderBlock(juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples);

    int getCurrentBeatInBar() const { return currentBeatInBar_; }
    int getCurrentBar() const { return currentBar_; }
    int getBeatsPerBar() const { return beatsPerBar_; }
    bool isCountIn() const { return metronomeFrame_ < totalCountInFrames_; }

private:
    double sampleRate_ = 44100.0;
    int tempo_ = 120;
    std::string timeSignature_ = "4/4";
    float clickGain_ = 1.0f;
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

    juce::AudioBuffer<float> normalClick_;
    juce::AudioBuffer<float> accentClick_;
    std::map<std::string, juce::AudioBuffer<float>> voiceWords_;

    struct ActiveSample {
        const juce::AudioBuffer<float>* buffer = nullptr;
        int64_t startFrame = 0;
        float gain = 1.0f;
        int channel = 0;
    };
    std::vector<ActiveSample> activeSamples_;

    void updateTimingParameters();
};

} // namespace tracks
