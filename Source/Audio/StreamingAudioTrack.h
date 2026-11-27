#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tracks {

// Chunked 16-bit PCM disk-streaming audio source for a single backing track stem.
// - Keeps Chunk 0 (~1.48s @ 44.1kHz = ~256 KB stereo) permanently pinned for instant 0ms Play start.
// - Keeps a 512-bin amplitude envelope (2 KB) for instant waveform recomputation on fader/mute changes.
// - Streams remaining 65,536-frame chunks on demand via a background streamer thread, keeping RAM at
//   ~1-2 MB per active stem instead of 40-100 MB for full-song uncompressed PCM.
class StreamingAudioTrack {
public:
    static constexpr int kChunkShift = 16;                  // 65,536 samples per chunk
    static constexpr int kChunkSamples = 1 << kChunkShift;  // ~1.48s @ 44.1kHz, ~1.36s @ 48kHz
    static constexpr int kMaxWindowSlots = 8;               // Sliding window + jump prefetch slots
    static constexpr int kWaveformBins = 512;

    struct PcmChunk {
        int64_t chunkIndex = -1;
        int numChannels = 0;
        int numSamples = 0; // <= kChunkSamples (final chunk may be shorter)
        std::vector<int16_t> data;

        const int16_t* getReadPointer(int ch) const {
            return data.data() + static_cast<size_t>(ch) * static_cast<size_t>(numSamples);
        }
    };

    // Opens the file, decodes ONLY Chunk 0 (~256 KB, resampled to targetSampleRate if needed)
    // + computes the 512-bin waveform envelope, then immediately closes the file handle.
    static std::shared_ptr<StreamingAudioTrack> createFromFile(const std::string& filePath,
                                                               double targetSampleRate = 0.0);

    uint64_t getUid() const { return uid_; }
    const std::string& getFilePath() const { return filePath_; }
    int getNumChannels() const { return numChannels_; }
    int64_t getNumSamples() const { return totalSamples_; }
    int64_t getTotalSamples() const { return totalSamples_; }
    double getSampleRate() const { return sampleRate_; }
    double getFileSampleRate() const { return fileSampleRate_; }
    int64_t getTotalChunks() const { return totalChunks_; }

    float getWaveformBin(int binIndex) const {
        if (binIndex < 0 || binIndex >= kWaveformBins) return 0.0f;
        return peakBins_[static_cast<size_t>(binIndex)];
    }

    // Audio-thread friendly: invokes fn(srcL, srcR, sliceOffsetInCount, sliceLen) for each
    // contiguous cached chunk slice covering [startFrame .. startFrame + count).
    // Never performs disk I/O or heap allocation; updates playheadChunk_ lock-free.
    template <typename Fn>
    void forEachSlice(int64_t startFrame, int count, Fn&& fn) const {
        if (count <= 0 || startFrame < 0 || startFrame >= totalSamples_ || numChannels_ <= 0) {
            return;
        }
        const int clampedCount = static_cast<int>(std::min<int64_t>(count, totalSamples_ - startFrame));
        if (clampedCount <= 0) return;

        const int64_t firstChunk = startFrame >> kChunkShift;
        if (playheadChunk_.load(std::memory_order_relaxed) != firstChunk) {
            playheadChunk_.store(firstChunk, std::memory_order_relaxed);
        }

        int processed = 0;
        while (processed < clampedCount) {
            const int64_t curFrame = startFrame + processed;
            const int64_t chunkIdx = curFrame >> kChunkShift;
            const int offsetInChunk = static_cast<int>(curFrame & (kChunkSamples - 1));
            const int remainingInReq = clampedCount - processed;

            std::shared_ptr<const PcmChunk> chunk = getChunkForAudioThread(chunkIdx);
            if (chunk && offsetInChunk < chunk->numSamples) {
                const int availInChunk = chunk->numSamples - offsetInChunk;
                const int sliceLen = std::min(remainingInReq, availInChunk);
                const int16_t* srcL = chunk->getReadPointer(0) + offsetInChunk;
                const int16_t* srcR = (chunk->numChannels >= 2)
                                          ? (chunk->getReadPointer(1) + offsetInChunk)
                                          : nullptr;
                fn(srcL, srcR, processed, sliceLen);
                processed += sliceLen;
            } else {
                // Chunk not resident yet (e.g. un-prefetched random jump) -> leave silence and advance
                needsService_.store(true, std::memory_order_release);
                const int skipLen = std::min(remainingInReq, kChunkSamples - offsetInChunk);
                processed += skipLen;
            }
        }
    }

    // Control/Streamer coordination
    int64_t getPlayheadChunk() const { return playheadChunk_.load(std::memory_order_relaxed); }
    void setPlayheadFrame(int64_t frame);
    int64_t getPrefetchChunk() const { return prefetchChunk_.load(std::memory_order_relaxed); }
    void setPrefetchFrame(int64_t frame);
    void clearPrefetch() { prefetchChunk_.store(-1, std::memory_order_relaxed); }

    bool consumeNeedsService() { return needsService_.exchange(false, std::memory_order_acq_rel); }

    // Streamer-thread methods (never called on audio thread)
    bool hasChunk(int64_t chunkIdx) const;
    // Returns the highest-priority missing chunk index in the desired window, or -1 if all resident.
    // Also evicts stale window chunks outside the active + prefetch windows.
    int64_t findNextChunkToLoadAndEvict(bool includePlayheadWindow = true);
    // Reads a single chunk from the supplied AudioFormatReader using readScratch and installs it.
    bool loadChunkFromReader(int64_t chunkIdx,
                             juce::AudioFormatReader& reader,
                             juce::AudioBuffer<float>& readScratch);
    // Releases all non-zero window chunks (keeping Chunk 0 pinned) to minimize idle RAM on Stop.
    void trimToIntroOnly();

    // Helper exposed for tests: converts a float AudioBuffer slice into a compact 16-bit PcmChunk
    static std::shared_ptr<PcmChunk> convertToPcmChunk(int64_t chunkIdx,
                                                       const juce::AudioBuffer<float>& src,
                                                       int numSamples);

private:
    StreamingAudioTrack();

    std::shared_ptr<const PcmChunk> getChunkForAudioThread(int64_t chunkIdx) const;
    void installChunk(std::shared_ptr<const PcmChunk> chunk);

    uint64_t uid_ = 0;
    std::string filePath_;
    int numChannels_ = 0;
    int64_t rawFileSamples_ = 0;
    int64_t totalSamples_ = 0;
    int64_t totalChunks_ = 0;
    double fileSampleRate_ = 44100.0;
    double sampleRate_ = 44100.0;
    double resampleRatio_ = 1.0; // fileSampleRate_ / sampleRate_
    bool needsResample_ = false;

    std::shared_ptr<const PcmChunk> introChunk_; // Chunk 0, always pinned
    std::array<float, kWaveformBins> peakBins_{};

    mutable std::mutex slotsMutex_;
    std::array<std::shared_ptr<const PcmChunk>, kMaxWindowSlots> slots_{};

    mutable std::atomic<int64_t> playheadChunk_{0};
    std::atomic<int64_t> prefetchChunk_{-1};
    mutable std::atomic<bool> needsService_{false};

    static std::atomic<uint64_t> nextUid_;
};

} // namespace tracks
