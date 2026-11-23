#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "../Models/Song.h"
#include "Metronome.h"
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>

namespace tracks {

struct AudioDeviceInfo {
    int index = 0;
    std::string name;
    int maxOutputChannels = 0;
    int maxInputChannels = 0;
    bool isDefault = false;
};

class AudioEngine : public juce::AudioIODeviceCallback {
public:
    AudioEngine();
    ~AudioEngine() override;

    // Device Management
    std::vector<AudioDeviceInfo> getAvailableOutputDevices();
    bool selectOutputDevice(const std::string& deviceName, int preferredChannels = 8);
    int getCurrentOutputChannels() const { return currentOutputChannels_; }
    double getCurrentSampleRate() const { return currentSampleRate_; }
    juce::AudioDeviceManager& getDeviceManager() { return deviceManager_; }

    // Playback Controls
    void loadSong(const Song& song);
    void play();
    void pause();
    void resume();
    void stop();

    bool isPlaying() const { return isPlaying_.load(); }
    bool isPaused() const { return isPaused_.load(); }

    // Dynamic parameter adjustments
    void setMasterGain(float gain);
    void setClickGain(float gain);
    void setGuideGain(float gain);
    void setClickChannel(int channel);
    void setGuideChannel(int channel);

    void setTrackMuted(int trackIndex, bool muted);
    void setTrackGain(int trackIndex, float gain);
    void setTrackOutputChannel(int trackIndex, int channel);

    void setTempo(int tempo);
    void setTimeSignature(const std::string& timeSignature);

    // Metering
    std::vector<float> getOutputPeaks();

    // Metronome visual query
    int getCurrentBeatInBar() const { return metronome_.getCurrentBeatInBar(); }
    int getCurrentBar() const { return metronome_.getCurrentBar(); }
    int getBeatsPerBar() const { return metronome_.getBeatsPerBar(); }

    // AudioIODeviceCallback overrides
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                         int numInputChannels,
                                         float* const* outputChannelData,
                                         int numOutputChannels,
                                         int numSamples,
                                         const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    struct LoadedTrack {
        SongTrack track;
        juce::AudioBuffer<float> buffer;
        int64_t position = 0;
    };

    juce::AudioDeviceManager deviceManager_;
    juce::AudioFormatManager formatManager_;
    Metronome metronome_;

    std::vector<LoadedTrack> loadedTracks_;
    int64_t maxSongLengthFrames_ = 0;

    std::atomic<bool> isPlaying_{false};
    std::atomic<bool> isPaused_{false};
    std::atomic<float> masterGain_{1.0f};

    int currentOutputChannels_ = 2;
    double currentSampleRate_ = 44100.0;

    std::mutex audioMutex_;
    std::mutex peakMutex_;
    std::vector<float> outputPeaks_;

    juce::AudioBuffer<float> renderScratchBuffer_;

    void loadEmbeddedAssets();
    juce::AudioBuffer<float> loadAudioFile(const std::string& filePath);
    juce::AudioBuffer<float> loadWavFromMemory(const void* data, size_t size);
};

} // namespace tracks
