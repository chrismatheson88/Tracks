#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "../Models/Song.h"
#include "../Models/SetlistEntry.h"
#include "Metronome.h"
#include "StreamingAudioTrack.h"
#include "PitchShiftUtils.h"
#include "SectionJumpUtils.h"
#include <signalsmith-stretch.h>
#include <array>
#include <cmath>
#include <cstdint>
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

class AudioEngine : public juce::AudioIODeviceCallback, private juce::AsyncUpdater {
public:
    enum class TransitionTrigger : int {
        None = 0,
        CueNext = 1,
        AutoplayNext = 2,
        CrossfadeNext = 3
    };

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
    void clearAllCachedAudio();
    void play();
    void pause();
    void resume();
    void stop();
    void fadeOutAndStop(double durationSeconds = 4.0);

    bool isPlaying() const { return isPlaying_.load(); }
    bool isPaused() const { return isPaused_.load(); }
    bool isFadingOut() const { return fadeOutActive_.load(std::memory_order_relaxed); }
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

    // One-shot text-to-speech announcement routed directly to the Guide channel
    void speakGuideText(const std::string& text);
    void stopGuideAnnouncement();

    // Click & guide asset packs (supports WAV, MP3, M4A)
    std::vector<std::string> getAvailableClickPacks() const;
    std::vector<std::string> getAvailableVoicePacks() const;
    bool selectClickPack(const std::string& packName);
    bool selectVoicePack(const std::string& packName);

    void setTrackMuted(int trackIndex, bool muted);
    void setTrackGain(int trackIndex, float gain);
    void setTrackOutputChannel(int trackIndex, int channel, bool stereoOutput = true);
    void flushPendingWaveformRebuild();

    void setTempo(int tempo);
    void setOriginalTrackTempo(int originalTempo);
    int getOriginalTrackTempo() const { return originalTrackTempo_.load(std::memory_order_relaxed); }
    void setTimeSignature(const std::string& timeSignature);
    void setPitchSemitones(int semitones);
    int getPitchSemitones() const { return pitchSemitones_.load(std::memory_order_relaxed); }

    void setUseTracks(bool enabled);
    bool getUseTracks() const { return useTracks_.load(std::memory_order_relaxed); }
    bool hasLoadedTracks() const { return hasTracks_.load(std::memory_order_relaxed); }
    // True when playing a structured arrangement timeline (backing tracks enabled, OR trackless arrangement with sections)
    bool isStructuredTimelineMode() const {
        const bool hasT = hasTracks_.load(std::memory_order_relaxed);
        return hasT ? useTracks_.load(std::memory_order_relaxed)
                    : (sectionTotalBars_.load(std::memory_order_relaxed) > 0);
    }
    bool shouldApplyTempoMap() const {
        return hasTracks_.load(std::memory_order_relaxed) ? useTracks_.load(std::memory_order_relaxed) : true;
    }

    // Queued section navigation (executes seamlessly when the current section finishes)
    // Also supports selecting a starting section before playback begins.
    void skipSection(int delta);
    void selectOrQueueSection(int sectionIndex);
    void toggleRepeatCurrentSection();
    void clearQueuedSection();
    int getQueuedSectionIndex() const { return queuedSectionIndexAtomic_.load(std::memory_order_relaxed); }
    int getSelectedStartSectionIndex() const { return selectedStartSectionIndexAtomic_.load(std::memory_order_relaxed); }
    bool isInSectionStartCountIn() const { return isSectionStartCountInAtomic_.load(std::memory_order_relaxed); }
    int getSectionCount() const { return sectionCountAtomic_.load(std::memory_order_relaxed); }

    // Click-only loop control (when not using tracks: active by default; deselecting stops cleanly at end of bar)
    void toggleClickLoop();
    void setClickLoopEnabled(bool enabled);
    bool isClickLoopEnabled() const { return clickLoopEnabled_.load(std::memory_order_relaxed); }

    // End-of-song / crossfade transitions between setlist songs
    void setTransitionConfig(SongTransitionMode mode, int crossfadeBars, bool hasNextSong);
    SongTransitionMode getTransitionMode() const {
        return clampTransitionMode(transitionModeAtomic_.load(std::memory_order_relaxed));
    }
    int getTransitionCrossfadeBars() const {
        return clampCrossfadeBars(transitionCrossfadeBarsAtomic_.load(std::memory_order_relaxed));
    }
    void setPreserveCrossfadeOnNextLoad(bool preserve) {
        preserveCrossfadeOnNextLoad_.store(preserve, std::memory_order_relaxed);
    }
    std::function<void(TransitionTrigger)> onSongTransitionTriggered;

    // Waveform & timeline query
    struct TimelineLayoutSnapshot {
        std::vector<double> barPositions;       // Normalized [0..1] X positions for audio bars 0..trackBarCount (empty = uniform)
        double waveformDurationFraction = 1.0;  // Normalized [0..1] fraction of track area occupied by audio file
    };

    uint64_t getWaveformVersion() const { return waveformVersion_.load(std::memory_order_relaxed); }
    std::vector<float> getWaveformPeaks();
    TimelineLayoutSnapshot getTimelineLayout();
    double getTrackDurationInBars() const;
    int getTrackBarCount() const;
    bool hasLeadingCountSection() const { return hasLeadingCountSection_.load(std::memory_order_relaxed); }
    bool hasTempoAutomation() const { return hasTempoAutomation_.load(std::memory_order_relaxed); }

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
        std::shared_ptr<StreamingAudioTrack> stream;
        int64_t position = 0;
        float smoothedCorrelation = 0.0f;
        float monoGainL = 0.70710678f;
        float monoGainR = 0.70710678f;
        bool pitchShiftExcluded = false;
    };

    struct StretchBusKey {
        int channelA = 0;
        bool routeAsStereo = true;
        bool pitchShiftExcluded = false;
        bool operator==(const StretchBusKey& o) const {
            return channelA == o.channelA &&
                   routeAsStereo == o.routeAsStereo &&
                   pitchShiftExcluded == o.pitchShiftExcluded;
        }
    };

    struct StretchBus {
        StretchBusKey key;
        std::shared_ptr<signalsmith::stretch::SignalsmithStretch<float>> stretcher;
        int64_t inputPos = 0;
        bool primed = false;
    };

    struct CachedSongAudio {
        int64_t songId = 0;
        std::vector<std::string> trackPaths;
        std::vector<LoadedTrack> tracks;
        int64_t maxLengthFrames = 0;
        std::vector<float> waveformPeaks;
    };

    struct CrossfadeOutTail {
        bool active = false;
        std::vector<LoadedTrack> tracks;
        std::vector<StretchBus> stretchBuses;
        int64_t maxSongLengthFrames = 0;
        int64_t outTrackPos = 0;
        int64_t elapsedSamples = 0;
        int64_t totalFadeSamples = 0;
        double exactRate = 1.0;
        int semitones = 0;
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
    std::atomic<bool> fadeOutActive_{false};
    std::atomic<bool> pendingFadeStopCleanup_{false};
    std::atomic<bool> isLoadingTracks_{false};
    std::atomic<bool> useTracks_{true};
    std::atomic<bool> hasTracks_{false};
    std::atomic<bool> hasLeadingCountSection_{false};
    std::atomic<int> sectionTotalBars_{0};
    std::atomic<int> sectionCountAtomic_{0};
    std::atomic<int> queuedSectionIndexAtomic_{-1};
    std::atomic<int> selectedStartSectionIndexAtomic_{-1};
    std::atomic<bool> isSectionStartCountInAtomic_{false};
    std::atomic<bool> clickLoopEnabled_{true};
    std::atomic<int> transitionModeAtomic_{0};
    std::atomic<int> transitionCrossfadeBarsAtomic_{1};
    std::atomic<bool> hasNextSongAtomic_{false};
    std::atomic<int> pendingTransitionTrigger_{0};
    std::atomic<bool> preserveCrossfadeOnNextLoad_{false};
    std::atomic<int64_t> maxSongLengthFramesAtomic_{0};
    std::atomic<float> masterGain_{1.0f};
    std::atomic<float> guideGainAtomic_{1.0f};
    std::atomic<int> pitchSemitones_{0};
    std::atomic<int> originalTrackTempo_{120};
    std::atomic<double> cachedTrackDurationBars_{0.0};
    std::atomic<bool> hasTempoAutomation_{false};

    std::vector<TempoPoint> activeTempoPoints_;
    std::vector<SectionSpan> sectionSpans_;
    int queuedSectionIndex_ = -1;
    int queuedJumpBoundaryBar_ = -1;
    int selectedStartSectionIndex_ = -1;
    int clickLoopStopBar_ = -1;
    bool awaitingTransitionSwap_ = false;
    int64_t trackMuteUntilFrame_ = 0;
    int trackJumpFadeSamplesRemaining_ = 0;
    static constexpr int kTrackJumpFadeSamples = 64;
    int64_t fadeOutElapsedSamples_ = 0;
    int64_t fadeOutTotalSamples_ = 0;

    std::atomic<int> currentOutputChannels_{2};
    double currentSampleRate_ = 44100.0;

    static constexpr int kMaxMeterChannels = 32;
    std::mutex audioMutex_;
    std::array<std::atomic<float>, kMaxMeterChannels> outputPeaksAtomic_{};
    std::atomic<int> activePeakChannels_{2};

    std::mutex waveformMutex_;
    std::vector<float> waveformPeaks_;
    TimelineLayoutSnapshot timelineLayout_;
    std::atomic<uint64_t> waveformVersion_{0};
    std::atomic<bool> waveformRebuildPending_{false};
    std::atomic<int64_t> lastWaveformDirtyMs_{0};

    // Setlist-scoped decoded audio cache & async loader
    std::mutex cacheMutex_;
    std::condition_variable loaderCv_;
    std::optional<int64_t> cachedSetlistId_;
    std::unordered_map<std::string, std::shared_ptr<CachedSongAudio>> songAudioCache_;
    std::deque<Song> preloadQueue_;
    uint64_t preloadGeneration_ = 0;
    bool shuttingDown_ = false;
    std::thread loaderThread_;

    // Background disk streaming thread & active stream registry
    std::mutex streamControlMutex_;
    std::condition_variable streamerCv_;
    std::condition_variable streamerSyncCv_;
    std::vector<std::shared_ptr<StreamingAudioTrack>> activeStreamTracks_;
    bool streamerCloseReadersRequested_ = false;
    bool streamerShuttingDown_ = false;
    uint64_t streamerWakeSeq_ = 0;
    uint64_t streamerSyncRequest_ = 0;
    uint64_t streamerSyncCompleted_ = 0;
    std::thread streamerThread_;

    // Guide-channel text-to-speech playback & background synthesis worker
    std::atomic<bool> guideAnnouncementActive_{false};
    std::atomic<uint64_t> guideAnnouncementGen_{0};
    std::mutex guideAnnouncementMutex_;
    std::shared_ptr<const juce::AudioBuffer<float>> guideAnnouncementBuffer_;
    int guideAnnouncementPos_ = 0;

    struct PendingTtsJob {
        uint64_t generation = 0;
        std::string text;
        int sampleRate = 44100;
    };
    std::mutex ttsMutex_;
    std::condition_variable ttsCv_;
    bool ttsShuttingDown_ = false;
    std::optional<PendingTtsJob> pendingTtsJob_;
    std::unordered_map<std::string, std::shared_ptr<const juce::AudioBuffer<float>>> ttsCache_;
    std::deque<std::string> ttsCacheOrder_;
    std::thread ttsThread_;

    void ttsWorkerLoop();
    static std::shared_ptr<const juce::AudioBuffer<float>> synthesizeTtsMonoBuffer(const std::string& text, int sampleRate);

    juce::AudioBuffer<float> renderScratchBuffer_;
    juce::AudioBuffer<float> crossfadeScratchBuffer_;
    juce::AudioBuffer<float> pitchInScratch_;
    juce::AudioBuffer<float> pitchOutScratch_;
    juce::AudioBuffer<float> pitchPrimeInScratch_;
    juce::AudioBuffer<float> pitchPrimeOutScratch_;
    std::vector<StretchBus> stretchBuses_;
    CrossfadeOutTail crossfadeOut_;

    void handleAsyncUpdate() override;
    void clearCrossfadeOutLocked();
    void renderCrossfadeOutLocked(int scratchOffset, int numSamples, int numOutputChannels);

    void preparePitchShiftersLocked();
    void rebuildSectionSpansLocked(const std::vector<SongSection>& sections);
    void updateTimelineMetricsLocked();
    void clearQueuedSectionLocked();
    void clearSelectedStartSectionLocked();
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
    void streamerLoop();
    void syncActiveStreamsLocked(bool notifyNow);
    void notifyStreamer(bool closeReaders = false);
    void waitForStreamerFlush();
    void updateSectionPrefetchLocked(int targetSectionIndex);
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
