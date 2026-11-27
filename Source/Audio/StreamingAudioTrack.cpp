#include "StreamingAudioTrack.h"
#include <cctype>
#include <filesystem>

namespace tracks {

std::atomic<uint64_t> StreamingAudioTrack::nextUid_{1};

StreamingAudioTrack::StreamingAudioTrack()
    : uid_(nextUid_.fetch_add(1, std::memory_order_relaxed)) {
    peakBins_.fill(0.0f);
}

std::shared_ptr<StreamingAudioTrack::PcmChunk> StreamingAudioTrack::convertToPcmChunk(
    int64_t chunkIdx,
    const juce::AudioBuffer<float>& src,
    int numSamples) {
    auto chunk = std::make_shared<PcmChunk>();
    chunk->chunkIndex = chunkIdx;
    chunk->numChannels = std::min(2, src.getNumChannels());
    chunk->numSamples = std::max(0, std::min(numSamples, src.getNumSamples()));
    if (chunk->numChannels <= 0 || chunk->numSamples <= 0) {
        return chunk;
    }

    chunk->data.resize(static_cast<size_t>(chunk->numChannels) * static_cast<size_t>(chunk->numSamples));
    for (int ch = 0; ch < chunk->numChannels; ++ch) {
        const float* s = src.getReadPointer(ch);
        int16_t* d = chunk->data.data() + static_cast<size_t>(ch) * static_cast<size_t>(chunk->numSamples);
        for (int i = 0; i < chunk->numSamples; ++i) {
            const float clamped = std::clamp(s[i], -1.0f, 1.0f);
            d[i] = static_cast<int16_t>(std::lrintf(clamped * 32767.0f));
        }
    }
    return chunk;
}

namespace {

bool isDirectSeekUncompressedFormat(const std::string& filePath) {
    std::string ext = std::filesystem::path(filePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return (ext == ".wav" || ext == ".aif" || ext == ".aiff");
}

float computeMonoPeakOfBuffer(const juce::AudioBuffer<float>& buf, int start, int count) {
    const int ch = buf.getNumChannels();
    if (ch <= 0 || count <= 0) return 0.0f;
    const int end = std::min(buf.getNumSamples(), start + count);
    if (end <= start) return 0.0f;

    float peak = 0.0f;
    if (ch >= 2) {
        const float* L = buf.getReadPointer(0);
        const float* R = buf.getReadPointer(1);
        for (int i = start; i < end; ++i) {
            peak = std::max(peak, std::abs(0.5f * (L[i] + R[i])));
        }
    } else {
        const float* M = buf.getReadPointer(0);
        for (int i = start; i < end; ++i) {
            peak = std::max(peak, std::abs(M[i]));
        }
    }
    return peak;
}

float cubicHermiteSample(const float* buf, int bufLen, double srcPos) {
    if (bufLen <= 0) return 0.0f;
    const int i1 = static_cast<int>(std::floor(srcPos));
    const float t = static_cast<float>(srcPos - static_cast<double>(i1));
    auto at = [&](int idx) -> float {
        return buf[std::clamp(idx, 0, bufLen - 1)];
    };
    const float y0 = at(i1 - 1);
    const float y1 = at(i1);
    const float y2 = at(i1 + 1);
    const float y3 = at(i1 + 2);
    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

std::shared_ptr<StreamingAudioTrack::PcmChunk> decodeChunkResampled(
    int64_t chunkIdx,
    int64_t outStart,
    int outLen,
    int numChannels,
    int64_t rawFileSamples,
    double resampleRatio,
    juce::AudioFormatReader& reader,
    juce::AudioBuffer<float>& readScratch) {
    auto chunk = std::make_shared<StreamingAudioTrack::PcmChunk>();
    chunk->chunkIndex = chunkIdx;
    chunk->numChannels = numChannels;
    chunk->numSamples = outLen;
    if (numChannels <= 0 || outLen <= 0) return chunk;

    const int64_t srcReadStart = std::max<int64_t>(0, static_cast<int64_t>(std::floor(static_cast<double>(outStart) * resampleRatio)) - 2);
    const int64_t srcReadEnd = std::min<int64_t>(rawFileSamples,
                                                 static_cast<int64_t>(std::ceil(static_cast<double>(outStart + outLen - 1) * resampleRatio)) + 4);
    const int srcLen = static_cast<int>(std::max<int64_t>(0, srcReadEnd - srcReadStart));
    if (srcLen <= 0) {
        chunk->data.assign(static_cast<size_t>(numChannels) * static_cast<size_t>(outLen), 0);
        return chunk;
    }

    if (readScratch.getNumChannels() < numChannels || readScratch.getNumSamples() < srcLen) {
        readScratch.setSize(numChannels, srcLen, false, false, true);
    }
    readScratch.clear(0, srcLen);
    if (!reader.read(&readScratch, 0, srcLen, srcReadStart, true, numChannels >= 2)) {
        return nullptr;
    }

    chunk->data.resize(static_cast<size_t>(numChannels) * static_cast<size_t>(outLen));
    for (int ch = 0; ch < numChannels; ++ch) {
        const float* s = readScratch.getReadPointer(ch);
        int16_t* d = chunk->data.data() + static_cast<size_t>(ch) * static_cast<size_t>(outLen);
        for (int i = 0; i < outLen; ++i) {
            const double localPos = static_cast<double>(outStart + i) * resampleRatio - static_cast<double>(srcReadStart);
            const float v = std::clamp(cubicHermiteSample(s, srcLen, localPos), -1.0f, 1.0f);
            d[i] = static_cast<int16_t>(std::lrintf(v * 32767.0f));
        }
    }
    return chunk;
}

} // namespace

std::shared_ptr<StreamingAudioTrack> StreamingAudioTrack::createFromFile(const std::string& filePath,
                                                                         double targetSampleRate) {
    juce::File file(filePath);
    if (!file.existsAsFile()) return nullptr;

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (!reader || reader->numChannels == 0 || reader->lengthInSamples <= 0) {
        return nullptr;
    }

    auto track = std::shared_ptr<StreamingAudioTrack>(new StreamingAudioTrack());
    track->filePath_ = filePath;
    track->numChannels_ = std::min(2, static_cast<int>(reader->numChannels));
    track->rawFileSamples_ = static_cast<int64_t>(reader->lengthInSamples);
    track->fileSampleRate_ = (reader->sampleRate > 0.0) ? reader->sampleRate : 44100.0;
    track->sampleRate_ = (targetSampleRate > 1.0) ? targetSampleRate : track->fileSampleRate_;
    track->needsResample_ = std::abs(track->fileSampleRate_ - track->sampleRate_) > 1.0;
    track->resampleRatio_ = track->needsResample_ ? (track->fileSampleRate_ / track->sampleRate_) : 1.0;
    track->totalSamples_ = track->needsResample_
                               ? std::max<int64_t>(1, static_cast<int64_t>(std::llround(static_cast<double>(track->rawFileSamples_) / track->resampleRatio_)))
                               : track->rawFileSamples_;
    track->totalChunks_ = (track->totalSamples_ + kChunkSamples - 1) >> kChunkShift;

    // 1. Decode ONLY Chunk 0 (~1.48s @ 44.1kHz, resampled to device sample rate if needed) into pinned introChunk_
    const int introOutLen = static_cast<int>(std::min<int64_t>(kChunkSamples, track->totalSamples_));
    juce::AudioBuffer<float> scratch(track->numChannels_, std::max(introOutLen, kChunkSamples));
    int introRawLen = 0;
    if (track->needsResample_) {
        track->introChunk_ = decodeChunkResampled(0, 0, introOutLen, track->numChannels_,
                                                  track->rawFileSamples_, track->resampleRatio_,
                                                  *reader, scratch);
        if (!track->introChunk_) return nullptr;
    } else {
        introRawLen = introOutLen;
        scratch.clear();
        reader->read(&scratch, 0, introRawLen, 0, true, track->numChannels_ >= 2);
        track->introChunk_ = convertToPcmChunk(0, scratch, introRawLen);
    }

    // 2. Compute 512-bin waveform envelope in raw file frame space (identical timeline fractions!)
    if (isDirectSeekUncompressedFormat(filePath) && track->rawFileSamples_ > kChunkSamples) {
        // Fast path for WAV/AIFF: sample a short window in each of the 512 bins (~256 KB total read, ~1-2 ms)
        constexpr int kProbeSamples = 128;
        juce::AudioBuffer<float> probe(track->numChannels_, kProbeSamples);
        for (int b = 0; b < kWaveformBins; ++b) {
            const int64_t startF = (static_cast<int64_t>(b) * track->rawFileSamples_) / kWaveformBins;
            const int64_t endF = std::max(startF + 1, (static_cast<int64_t>(b + 1) * track->rawFileSamples_) / kWaveformBins);
            const int64_t span = endF - startF;

            // Sample 2 probes per bin (start & middle) for smooth, accurate envelope representation
            float binPeak = 0.0f;
            const int64_t probes[2] = { startF, startF + span / 2 };
            const int probeCount = (span >= kProbeSamples * 2) ? 2 : 1;
            for (int p = 0; p < probeCount; ++p) {
                const int64_t pos = probes[p];
                const int nRead = static_cast<int>(std::min<int64_t>({ static_cast<int64_t>(kProbeSamples),
                                                                       endF - pos,
                                                                       track->rawFileSamples_ - pos }));
                if (nRead <= 0) continue;

                if (introRawLen > 0 && pos + nRead <= introRawLen) {
                    binPeak = std::max(binPeak, computeMonoPeakOfBuffer(scratch, static_cast<int>(pos), nRead));
                } else {
                    probe.clear();
                    reader->read(&probe, 0, nRead, pos, true, track->numChannels_ >= 2);
                    binPeak = std::max(binPeak, computeMonoPeakOfBuffer(probe, 0, nRead));
                }
            }
            track->peakBins_[static_cast<size_t>(b)] = binPeak;
        }
    } else {
        // Streaming single-pass scan (for compressed M4A/MP3 or short files): reuses a single 64K-sample buffer
        auto accumulateChunkIntoBins = [&](const juce::AudioBuffer<float>& buf, int64_t chunkStart, int count) {
            if (count <= 0 || track->rawFileSamples_ <= 0) return;
            const int ch = buf.getNumChannels();
            const float* L = buf.getReadPointer(0);
            const float* R = (ch >= 2) ? buf.getReadPointer(1) : nullptr;
            const int step = std::max(1, static_cast<int>(track->rawFileSamples_ / (kWaveformBins * 64)));
            for (int i = 0; i < count; i += step) {
                const int64_t f = chunkStart + i;
                const int b = std::clamp(static_cast<int>((f * kWaveformBins) / track->rawFileSamples_), 0, kWaveformBins - 1);
                const float sample = R ? (0.5f * (L[i] + R[i])) : L[i];
                const float a = std::abs(sample);
                if (a > track->peakBins_[static_cast<size_t>(b)]) {
                    track->peakBins_[static_cast<size_t>(b)] = a;
                }
            }
        };

        int64_t pos = 0;
        if (introRawLen > 0) {
            accumulateChunkIntoBins(scratch, 0, introRawLen);
            pos = introRawLen;
        }
        while (pos < track->rawFileSamples_) {
            const int nRead = static_cast<int>(std::min<int64_t>(kChunkSamples, track->rawFileSamples_ - pos));
            if (scratch.getNumSamples() < nRead) {
                scratch.setSize(track->numChannels_, nRead, false, false, true);
            }
            scratch.clear();
            reader->read(&scratch, 0, nRead, pos, true, track->numChannels_ >= 2);
            accumulateChunkIntoBins(scratch, pos, nRead);
            pos += nRead;
        }
    }

    // Reader closes immediately on scope exit — zero idle file descriptors!
    return track;
}

std::shared_ptr<const StreamingAudioTrack::PcmChunk> StreamingAudioTrack::getChunkForAudioThread(int64_t chunkIdx) const {
    if (chunkIdx == 0) {
        return introChunk_;
    }
    std::lock_guard<std::mutex> lock(slotsMutex_);
    for (const auto& slot : slots_) {
        if (slot && slot->chunkIndex == chunkIdx) {
            return slot;
        }
    }
    return nullptr;
}

void StreamingAudioTrack::setPlayheadFrame(int64_t frame) {
    const int64_t clamped = std::clamp<int64_t>(frame, 0, std::max<int64_t>(0, totalSamples_ - 1));
    playheadChunk_.store(clamped >> kChunkShift, std::memory_order_relaxed);
    needsService_.store(true, std::memory_order_release);
}

void StreamingAudioTrack::setPrefetchFrame(int64_t frame) {
    if (frame < 0 || totalSamples_ <= 0) {
        prefetchChunk_.store(-1, std::memory_order_relaxed);
        return;
    }
    const int64_t clamped = std::clamp<int64_t>(frame, 0, std::max<int64_t>(0, totalSamples_ - 1));
    prefetchChunk_.store(clamped >> kChunkShift, std::memory_order_relaxed);
    needsService_.store(true, std::memory_order_release);
}

bool StreamingAudioTrack::hasChunk(int64_t chunkIdx) const {
    if (chunkIdx < 0 || chunkIdx >= totalChunks_) return true;
    if (chunkIdx == 0) return introChunk_ != nullptr;
    std::lock_guard<std::mutex> lock(slotsMutex_);
    for (const auto& slot : slots_) {
        if (slot && slot->chunkIndex == chunkIdx) {
            return true;
        }
    }
    return false;
}

void StreamingAudioTrack::installChunk(std::shared_ptr<const PcmChunk> chunk) {
    if (!chunk || chunk->chunkIndex <= 0) return;

    std::shared_ptr<const PcmChunk> evicted;
    {
        std::lock_guard<std::mutex> lock(slotsMutex_);
        // Replace existing slot if same chunkIndex, or fill first empty slot
        int targetIdx = -1;
        for (int i = 0; i < kMaxWindowSlots; ++i) {
            if (slots_[static_cast<size_t>(i)] && slots_[static_cast<size_t>(i)]->chunkIndex == chunk->chunkIndex) {
                targetIdx = i;
                break;
            }
            if (!slots_[static_cast<size_t>(i)] && targetIdx < 0) {
                targetIdx = i;
            }
        }

        // If all slots full, evict the slot farthest from current playhead & prefetch target
        if (targetIdx < 0) {
            const int64_t p = playheadChunk_.load(std::memory_order_relaxed);
            const int64_t q = prefetchChunk_.load(std::memory_order_relaxed);
            int64_t worstDist = -1;
            targetIdx = 0;
            for (int i = 0; i < kMaxWindowSlots; ++i) {
                const int64_t ci = slots_[static_cast<size_t>(i)]->chunkIndex;
                int64_t dist = std::abs(ci - p);
                if (q >= 0) {
                    dist = std::min(dist, std::abs(ci - q));
                }
                if (dist > worstDist) {
                    worstDist = dist;
                    targetIdx = i;
                }
            }
        }

        evicted = std::move(slots_[static_cast<size_t>(targetIdx)]);
        slots_[static_cast<size_t>(targetIdx)] = std::move(chunk);
    }
    // Destroy evicted chunk outside slotsMutex_
    evicted.reset();
}

int64_t StreamingAudioTrack::findNextChunkToLoadAndEvict(bool includePlayheadWindow) {
    if (totalChunks_ <= 1) return -1;

    const int64_t p = std::clamp<int64_t>(playheadChunk_.load(std::memory_order_relaxed), 0, totalChunks_ - 1);
    const int64_t q = prefetchChunk_.load(std::memory_order_relaxed);

    auto isWanted = [&](int64_t ci) -> bool {
        if (ci <= 0 || ci >= totalChunks_) return false;
        if (includePlayheadWindow && ci >= p - 1 && ci <= p + 2) return true;
        if (q >= 0 && ci >= q && ci <= q + 1) return true;
        return false;
    };

    // Priority order of chunks to ensure resident
    std::array<int64_t, 5> candidates = includePlayheadWindow
        ? std::array<int64_t, 5>{ p, p + 1, q, p + 2, q + 1 }
        : std::array<int64_t, 5>{ q, q + 1, -1, -1, -1 };

    std::vector<std::shared_ptr<const PcmChunk>> toRelease;
    int64_t missingChunk = -1;
    {
        std::lock_guard<std::mutex> lock(slotsMutex_);

        // 1. Evict chunks outside the active + prefetch windows so RAM stays strictly bounded
        for (auto& slot : slots_) {
            if (slot && !isWanted(slot->chunkIndex)) {
                toRelease.push_back(std::move(slot));
            }
        }

        // 2. Find highest-priority missing chunk
        for (int64_t c : candidates) {
            if (c <= 0 || c >= totalChunks_) continue;
            bool present = false;
            for (const auto& slot : slots_) {
                if (slot && slot->chunkIndex == c) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                missingChunk = c;
                break;
            }
        }
    }
    // Destroy evicted chunks outside slotsMutex_
    toRelease.clear();
    return missingChunk;
}

bool StreamingAudioTrack::loadChunkFromReader(int64_t chunkIdx,
                                              juce::AudioFormatReader& reader,
                                              juce::AudioBuffer<float>& readScratch) {
    if (chunkIdx <= 0 || chunkIdx >= totalChunks_) return false;

    const int64_t outStart = chunkIdx << kChunkShift;
    const int outLen = static_cast<int>(std::min<int64_t>(kChunkSamples, totalSamples_ - outStart));
    if (outLen <= 0) return false;

    if (needsResample_) {
        auto chunk = decodeChunkResampled(chunkIdx, outStart, outLen, numChannels_,
                                          rawFileSamples_, resampleRatio_, reader, readScratch);
        if (!chunk) return false;
        installChunk(std::move(chunk));
        return true;
    }

    if (readScratch.getNumChannels() < numChannels_ || readScratch.getNumSamples() < outLen) {
        readScratch.setSize(numChannels_, kChunkSamples, false, false, true);
    }
    readScratch.clear(0, outLen);

    if (!reader.read(&readScratch, 0, outLen, outStart, true, numChannels_ >= 2)) {
        return false;
    }

    auto chunk = convertToPcmChunk(chunkIdx, readScratch, outLen);
    installChunk(std::move(chunk));
    return true;
}

void StreamingAudioTrack::trimToIntroOnly() {
    std::vector<std::shared_ptr<const PcmChunk>> toRelease;
    toRelease.reserve(kMaxWindowSlots);
    {
        std::lock_guard<std::mutex> lock(slotsMutex_);
        for (auto& slot : slots_) {
            if (slot) {
                toRelease.push_back(std::move(slot));
            }
        }
    }
    toRelease.clear();
    playheadChunk_.store(0, std::memory_order_relaxed);
    prefetchChunk_.store(-1, std::memory_order_relaxed);
    needsService_.store(false, std::memory_order_relaxed);
}

} // namespace tracks
