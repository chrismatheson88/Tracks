#include "Metronome.h"
#include <cmath>
#include <algorithm>

namespace tracks {

Metronome::Metronome() {
    updateTimingParameters();
}

void Metronome::prepare(double sampleRate) {
    sampleRate_ = (sampleRate > 0.0) ? sampleRate : 44100.0;
    updateTimingParameters();
    reset();
}

void Metronome::reset() {
    metronomeFrame_ = 0;
    currentBeatInBar_ = 0;
    currentBar_ = 0;
    activeSamples_.clear();
}

void Metronome::setTempo(int tempo) {
    tempo_ = std::max(20, std::min(300, tempo));
    updateTimingParameters();
}

void Metronome::setTimeSignature(const std::string& timeSignature) {
    timeSignature_ = timeSignature.empty() ? "4/4" : timeSignature;
    updateTimingParameters();
}

void Metronome::setClickGain(float gain) {
    clickGain_ = std::max(0.0f, std::min(4.0f, gain));
}

void Metronome::setGuideGain(float gain) {
    guideGain_ = std::max(0.0f, std::min(4.0f, gain));
}

void Metronome::setClickChannel(int channel) {
    clickChannel_ = std::max(0, channel);
}

void Metronome::setGuideChannel(int channel) {
    guideChannel_ = std::max(0, channel);
}

void Metronome::loadClickSamples(const juce::AudioBuffer<float>& normalClick,
                                const juce::AudioBuffer<float>& accentClick) {
    normalClick_ = normalClick;
    accentClick_ = accentClick;
}

void Metronome::loadVoiceWord(const std::string& word, const juce::AudioBuffer<float>& buffer) {
    voiceWords_[word] = buffer;
}

void Metronome::updateTimingParameters() {
    int bpb = 4;
    int div = 4;
    auto slashPos = timeSignature_.find('/');
    if (slashPos != std::string::npos) {
        try {
            bpb = std::stoi(timeSignature_.substr(0, slashPos));
            div = std::stoi(timeSignature_.substr(slashPos + 1));
        } catch (...) {
            bpb = 4;
            div = 4;
        }
    }
    beatsPerBar_ = std::max(1, bpb);
    beatDivision_ = std::max(1, div);

    isCompound_ = (beatDivision_ == 8 && (beatsPerBar_ == 6 || beatsPerBar_ == 12));
    if (isCompound_) {
        // For compound time, tempo defines each set of 3 eighth notes
        double groupFrames = sampleRate_ * 60.0 / std::max(1, tempo_);
        beatFrames_ = std::max<int64_t>(1, static_cast<int64_t>(std::round(groupFrames / 3.0)));
        pulseCountPerBar_ = (beatsPerBar_ / 3) * 3;
    } else {
        beatFrames_ = std::max<int64_t>(1, static_cast<int64_t>(std::round(sampleRate_ * 60.0 / std::max(1, tempo_))));
        pulseCountPerBar_ = beatsPerBar_;
    }

    totalCountInFrames_ = beatFrames_ * pulseCountPerBar_ * 2; // 2 bars of count-in
}

void Metronome::renderBlock(juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples) {
    int numOutputChannels = outputBuffer.getNumChannels();
    int64_t blockStart = metronomeFrame_;
    int64_t blockEnd = blockStart + numSamples;

    // Trigger pulses that fall within this block
    int64_t firstBeatIndex = std::max<int64_t>(0, blockStart / beatFrames_);
    int64_t lastBeatIndex = (blockEnd + beatFrames_ - 1) / beatFrames_ + 1;

    for (int64_t beatIndex = firstBeatIndex; beatIndex <= lastBeatIndex; ++beatIndex) {
        int64_t beatStart = beatIndex * beatFrames_;
        if (beatStart < blockStart || beatStart >= blockEnd)
            continue;

        int beatInBar = static_cast<int>(beatIndex % pulseCountPerBar_);
        currentBeatInBar_ = beatInBar;
        currentBar_ = static_cast<int>(beatIndex / pulseCountPerBar_);

        bool isAccent = isCompound_ ? (beatInBar % 3 == 0) : (beatInBar == 0);
        const auto& clickBuf = isAccent ? accentClick_ : normalClick_;
        if (clickBuf.getNumSamples() > 0 && clickChannel_ < numOutputChannels) {
            activeSamples_.push_back({ &clickBuf, beatStart, clickGain_, clickChannel_ });
        }

        // Count-in speech check (first 2 bars)
        if (beatIndex < pulseCountPerBar_ * 2 && guideChannel_ < numOutputChannels) {
            std::string word;
            if (pulseCountPerBar_ == 4) {
                // Speech pattern: one, [none], two, [none], one, two, three, four
                const char* pattern[8] = { "one", "", "two", "", "one", "two", "three", "four" };
                if (beatIndex >= 0 && beatIndex < 8)
                    word = pattern[beatIndex];
            } else {
                const char* words[8] = { "one", "two", "three", "four", "five", "six", "seven", "eight" };
                if (beatInBar >= 0 && beatInBar < 8)
                    word = words[beatInBar];
            }

            if (!word.empty()) {
                auto it = voiceWords_.find(word);
                if (it != voiceWords_.end() && it->second.getNumSamples() > 0) {
                    activeSamples_.push_back({ &it->second, beatStart, guideGain_, guideChannel_ });
                }
            }
        }
    }

    // Mix active samples
    std::vector<ActiveSample> stillActive;
    for (const auto& sample : activeSamples_) {
        if (!sample.buffer || sample.channel >= numOutputChannels)
            continue;

        int sampleLength = sample.buffer->getNumSamples();
        int64_t sampleEnd = sample.startFrame + sampleLength;

        int64_t overlapStart = std::max(blockStart, sample.startFrame);
        int64_t overlapEnd = std::min(blockEnd, sampleEnd);

        if (overlapEnd > overlapStart) {
            int outOffset = startSample + static_cast<int>(overlapStart - blockStart);
            int bufOffset = static_cast<int>(overlapStart - sample.startFrame);
            int copyCount = static_cast<int>(overlapEnd - overlapStart);

            const float* src = sample.buffer->getReadPointer(0) + bufOffset;
            float* dst = outputBuffer.getWritePointer(sample.channel) + outOffset;

            for (int i = 0; i < copyCount; ++i) {
                dst[i] += src[i] * sample.gain;
            }
        }

        if (sampleEnd > blockEnd) {
            stillActive.push_back(sample);
        }
    }
    activeSamples_ = std::move(stillActive);

    metronomeFrame_ = blockEnd;
}

} // namespace tracks
