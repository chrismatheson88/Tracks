#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "../Models/Song.h"
#include "Metronome.h"
#include "PitchShiftUtils.h"
#include "SectionJumpUtils.h"
#include <signalsmith-stretch.h>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>
#include <optional>

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
    bool selectOutputDevice(const std::string& deviceName, int preferredChannels = 32);
    int getCurrentOutputChannels() const { return currentOutputChannels_.load(std::memory_order_relaxed); }
    double getCurrentSampleRate() const { return currentSampleRate_; }
    juce::AudioDeviceManager& getDeviceManager() { return deviceManager_; }

    // Playback Controls
    void loadSong(const Song& song);
    void preloadSetlist(int64_t setlistId, const std::vector<Song>& songs);
    void invalidateCachedSong(int64_t songId);
    void play();
    void pause();
    void resume();
    void stop();

    bool isPlaying() const { return isPlaying_.load(); }
    bool isPaused() const { return isPaused_.load(); }
    bool isLoadingTracks() const { return isLoadingTracks_.load(std::memory_order_relaxed); }

    // Dynamic parameter adjustments
    void setMasterGain(float gain);
    void setClickGain(float gain);
    void setEighthClickGain(float gain);
    void setSixteenthClickGain(float gain);
    void setClickSubdivisions(bool enableEighths, bool enableSixteenths);
    void setGuideGain(float gain);
    void setClickChannel(int channel);
    void setGuideChannel(int channel);
    int getClickChannel() const { return metronome_.getClickChannel(); }
    int getGuideChannel() const { return metronome_.getGuideChannel(); }

    // Click & guide asset packs (supports WAV, MP3, M4A)
    std::vector<std::string> getAvailableClickPacks() const;
    std::vector<std::string> getAvailableVoicePacks() const;
    bool selectClickPack(const std::string& packName);
    bool selectVoicePack(const std::string& packName);

    void setTrackMuted(int trackIndex, bool muted);
    void setTrackGain(int trackIndex, float gain);
    void setTrackOutputChannel(int trackIndex, int channel, bool stereoOutput = true);

    void setTempo(int tempo);
    void setTimeSignature(const std::string& timeSignature);
    void setPitchSemitones(int semitones);
    int getPitchSemitones() const { return pitchSemitones_.load(std::memory_order_relaxed); }

    void setUseTracks(bool enabled);
    bool getUseTracks() const { return useTracks_.load(std::memory_order_relaxed); }
    bool hasLoadedTracks() const { return hasTracks_.load(std::memory_order_relaxed); }

    // Queued section navigation (executes seamlessly when the current section finishes)
    void skipSection(int delta);
    void toggleRepeatCurrentSection();
    void clearQueuedSection();
    int getQueuedSectionIndex() const { return queuedSectionIndexAtomic_.load(std::memory_order_relaxed); }
    int getSectionCount() const { return sectionCountAtomic_.load(std::memory_order_relaxed); }

    // Waveform & timeline query
    uint64_t getWaveformVersion() const { return waveformVersion_.load(std::memory_order_relaxed); }
    std::vector<float> getWaveformPeaks();
    double getTrackDurationInBars() const;
    int getTrackBarCount() const;
    bool hasLeadingCountSection() const { return hasLeadingCountSection_.load(std::memory_order_relaxed); }

    // Metering
    std::vector<float> getOutputPeaks();

    // Metronome visual query
    int getCurrentBeatInBar() const { return metronome_.getCurrentBeatInBar(); }
    int getCurrentBar() const { return metronome_.getCurrentBar(); }
    int getBeatsPerBar() const { return metronome_.getBeatsPerBar(); }
    double getTotalBarsElapsed() const { return metronome_.getTotalBarsElapsed(); }
    bool isCompoundMeter() const { return metronome_.isCompoundMeter(); }

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
        std::shared_ptr<const juce::AudioBuffer<float>> buffer;
        int64_t position = 0;
        float smoothedCorrelation = 0.0f;
        float monoGainL = 0.70710678f;
        float monoGainR = 0.70710678f;
        bool pitchShiftExcluded = false;
        std::shared_ptr<signalsmith::stretch::SignalsmithStretch<float>> pitchShifter;
        int64_t pitchShifterInputPos = 0;
        bool pitchShifterPrimed = false;
    };

    struct CachedSongAudio {
        int64_t songId = 0;
        std::vector<std::string> trackPaths;
        std::vector<LoadedTrack> tracks;
        int64_t maxLengthFrames = 0;
        std::vector<float> waveformPeaks;
    };

    juce::AudioDeviceManager deviceManager_;
    juce::AudioFormatManager formatManager_;
    Metronome metronome_;

    std::vector<LoadedTrack> loadedTracks_;
    int64_t maxSongLengthFrames_ = 0;
    std::string activeCacheKey_;
    std::vector<std::string> activeTrackPaths_;

    std::atomic<bool> isPlaying_{false};
    std::atomic<bool> isPaused_{false};
    std::atomic<bool> isLoadingTracks_{false};
    std::atomic<bool> useTracks_{true};
    std::atomic<bool> hasTracks_{false};
    std::atomic<bool> hasLeadingCountSection_{false};
    std::atomic<int> sectionTotalBars_{0};
    std::atomic<int> sectionCountAtomic_{0};
    std::atomic<int> queuedSectionIndexAtomic_{-1};
    std::atomic<int64_t> maxSongLengthFramesAtomic_{0};
    std::atomic<float> masterGain_{1.0f};
    std::atomic<int> pitchSemitones_{0};

    std::vector<SectionSpan> sectionSpans_;
    int queuedSectionIndex_ = -1;
    int queuedJumpBoundaryBar_ = -1;
    int trackJumpFadeSamplesRemaining_ = 0;
    static constexpr int kTrackJumpFadeSamples = 64;

    std::atomic<int> currentOutputChannels_{2};
    double currentSampleRate_ = 44100.0;

    std::mutex audioMutex_;
    std::mutex peakMutex_;
    std::vector<float> outputPeaks_;

    std::mutex waveformMutex_;
    std::vector<float> waveformPeaks_;
    std::atomic<uint64_t> waveformVersion_{0};

    // Setlist-scoped decoded audio cache & async loader
    std::mutex cacheMutex_;
    std::condition_variable loaderCv_;
    std::optional<int64_t> cachedSetlistId_;
    std::unordered_map<std::string, std::shared_ptr<CachedSongAudio>> songAudioCache_;
    std::deque<Song> preloadQueue_;
    uint64_t preloadGeneration_ = 0;
    bool shuttingDown_ = false;
    std::thread loaderThread_;

    juce::AudioBuffer<float> renderScratchBuffer_;
    juce::AudioBuffer<float> pitchInScratch_;
    juce::AudioBuffer<float> pitchOutScratch_;
    juce::AudioBuffer<float> pitchPrimeInScratch_;
    juce::AudioBuffer<float> pitchPrimeOutScratch_;

    void preparePitchShiftersLocked();
    void rebuildSectionSpansLocked(const std::vector<SongSection>& sections);
    void clearQueuedSectionLocked();
    void renderSegmentLocked(int scratchOffset,
                             int segmentSamples,
                             int numOutputChannels,
                             bool playTracks,
                             int64_t audioStartFrame);

    void loadEmbeddedAssets();
    void loadEmbeddedClicksLocked();
    void loadEmbeddedVoicesLocked();
    void syncSectionCuesForSections(const std::vector<SongSection>& sections);
    juce::AudioBuffer<float> loadMonoAssetFile(const std::string& filePath, double targetSampleRate) const;
    std::string activeClickPack_;
    std::string activeVoicePack_;
    std::map<std::string, std::string> sectionCueFilePaths_;
    std::map<std::string, juce::AudioBuffer<float>> sectionCueCache_;
    std::vector<SongSection> activeSections_;
    std::mutex guideAssetsMutex_;
    void loaderLoop();
    void enqueueSongLoadLocked(const Song& song, bool highPriority);
    void applyCachedAudio(const std::shared_ptr<CachedSongAudio>& cached, const Song* overrideSettings);
    void rebuildWaveformPeaksLocked();
    static std::string makeCacheKey(const Song& song);
    static std::vector<std::string> extractTrackPaths(const Song& song);
    static std::vector<float> computeWaveformPeaks(const std::vector<LoadedTrack>& tracks, int64_t maxLengthFrames);
    static std::shared_ptr<CachedSongAudio> decodeSongAudio(const Song& song);
    static juce::AudioBuffer<float> loadAudioFileStatic(const std::string& filePath);
    juce::AudioBuffer<float> loadAudioFile(const std::string& filePath);
    juce::AudioBuffer<float> loadWavFromMemory(const void* data, size_t size);
};

} // namespace tracks
