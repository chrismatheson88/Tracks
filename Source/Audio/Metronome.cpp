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
    overrideAnnouncementBar_ = -1;
    overrideAnnouncementKey_.clear();
    totalBarsElapsed_.store(0.0, std::memory_order_relaxed);
    activeSamples_.clear();
}

void Metronome::jumpToFrame(int64_t newFrame) {
    const int64_t clampedFrame = std::max<int64_t>(0, newFrame);
    const int64_t delta = clampedFrame - metronomeFrame_;

    // Shift ringing click / guide tails by delta so they finish smoothly across the jump
    // without cutting off abruptly or replaying later on the timeline.
    for (auto& sample : activeSamples_) {
        sample.startFrame += delta;
    }

    metronomeFrame_ = clampedFrame;
    const int64_t safeBeatFrames = std::max<int64_t>(1, beatFrames_);
    const int64_t beatIndex = metronomeFrame_ / safeBeatFrames;
    currentBar_ = static_cast<int>(beatIndex / pulseCountPerBar_);
    currentBeatInBar_ = static_cast<int>(beatIndex % pulseCountPerBar_);

    const int64_t framesPerBar = std::max<int64_t>(1, beatFrames_ * pulseCountPerBar_);
    totalBarsElapsed_.store(static_cast<double>(metronomeFrame_) / static_cast<double>(framesPerBar),
                            std::memory_order_relaxed);
}

void Metronome::setTempo(int tempo) {
    tempo_ = std::max(20, std::min(300, tempo));
    updateTimingParameters();
}

void Metronome::setTimeSignature(const std::string& timeSignature) {
    timeSignature_ = timeSignature.empty() ? "4/4" : timeSignature;
    updateTimingParameters();
}

void Metronome::setSections(const std::vector<SongSection>& sections) {
    sectionCountWindows_.clear();
    sectionAnnouncements_.clear();
    overrideAnnouncementBar_ = -1;
    overrideAnnouncementKey_.clear();
    leadingCountBars_ = 0;

    // Count consecutive leading "count" sections from the start of the audio files
    size_t idx = 0;
    while (idx < sections.size() && sections[idx].isCountSection()) {
        leadingCountBars_ += std::max(1, sections[idx].lengthBars);
        ++idx;
    }

    const int audioStartBar = (leadingCountBars_ > 0) ? 0 : 2;
    if (leadingCountBars_ == 0) {
        // No leading "count" section in the audio: keep standard 2-bar count-in before bar 0 of audio
        sectionCountWindows_.push_back({ 0, 2 });
    }

    // Add count windows for every "count" section and announcements at bar (start - 1) for every non-count section
    int cursorBar = audioStartBar;
    for (const auto& sec : sections) {
        int len = std::max(1, sec.lengthBars);
        if (sec.isCountSection()) {
            if (!sectionCountWindows_.empty() &&
                sectionCountWindows_.back().startBar + sectionCountWindows_.back().lengthBars == cursorBar) {
                sectionCountWindows_.back().lengthBars += len;
            } else {
                sectionCountWindows_.push_back({ cursorBar, len });
            }
        } else {
            std::string key = sec.normalizedKey();
            if (!key.empty() && cursorBar >= 1) {
                sectionAnnouncements_.push_back({ cursorBar - 1, key });
            }
        }
        cursorBar += len;
    }

    updateTimingParameters();
}

void Metronome::clearSectionCues() {
    activeSamples_.clear();
    sectionCues_.clear();
}

void Metronome::loadSectionCue(const std::string& normalizedKey, const juce::AudioBuffer<float>& buffer) {
    if (!normalizedKey.empty() && buffer.getNumSamples() > 0) {
        sectionCues_[normalizedKey] = buffer;
    }
}

const juce::AudioBuffer<float>* Metronome::findSectionCueBuffer(const std::string& normalizedKey) const {
    auto it = sectionCues_.find(normalizedKey);
    if (it != sectionCues_.end() && it->second.getNumSamples() > 0) {
        return &it->second;
    }

    // Fallback: strip trailing number (e.g. "intro 1" -> "intro", "chorus 9" -> "chorus")
    auto lastSpace = normalizedKey.rfind(' ');
    if (lastSpace != std::string::npos) {
        std::string tail = normalizedKey.substr(lastSpace + 1);
        bool allDigits = !tail.empty() && std::all_of(tail.begin(), tail.end(),
                                                      [](unsigned char c) { return std::isdigit(c) != 0; });
        if (allDigits) {
            std::string baseKey = normalizedKey.substr(0, lastSpace);
            auto baseIt = sectionCues_.find(baseKey);
            if (baseIt != sectionCues_.end() && baseIt->second.getNumSamples() > 0) {
                return &baseIt->second;
            }
        }
    }

    return nullptr;
}

void Metronome::setQueuedAnnouncementOverride(int cueBar, const std::string& normalizedKey) {
    overrideAnnouncementBar_ = cueBar;
    overrideAnnouncementKey_ = normalizedKey;
}

void Metronome::clearQueuedAnnouncementOverride() {
    overrideAnnouncementBar_ = -1;
    overrideAnnouncementKey_.clear();
}

std::string Metronome::getScheduledGuideCueForBeat(int bar, int beatInBar) const {
    static const char* countWords[8] = { "one", "two", "three", "four", "five", "six", "seven", "eight" };

    if (useSectionCountIn_ && overrideAnnouncementBar_ >= 0 && bar == overrideAnnouncementBar_) {
        if (overrideAnnouncementKey_.empty()) {
            // Target section is a "count" section: don't announce the skipped section
            return getCountInVoiceWordForBeat(bar, beatInBar);
        }
        if (beatInBar == 0) {
            return "section:" + overrideAnnouncementKey_;
        }
        if (beatInBar >= 1 && beatInBar < 8) {
            return countWords[beatInBar];
        }
        return "";
    }

    if (useSectionCountIn_ && !sectionAnnouncements_.empty()) {
        for (const auto& ann : sectionAnnouncements_) {
            if (ann.cueBar == bar) {
                if (beatInBar == 0) {
                    return "section:" + ann.normalizedKey;
                }
                if (beatInBar >= 1 && beatInBar < 8) {
                    return countWords[beatInBar];
                }
                return "";
            }
        }
    }

    return getCountInVoiceWordForBeat(bar, beatInBar);
}

void Metronome::setUseSectionCountIn(bool enabled) {
    useSectionCountIn_ = enabled;
    updateTimingParameters();
}

std::string Metronome::getCountInVoiceWordForBeat(int bar, int beatInBar) const {
    auto wordForWindow = [this](const CountWindow& w, int curBar, int curBeatInBar) -> std::string {
        int localBar = curBar - w.startBar;
        if (localBar < 0 || localBar >= w.lengthBars) return "";

        if (pulseCountPerBar_ == 4) {
            if (w.lengthBars == 1) {
                const char* oneBar[4] = { "one", "two", "three", "four" };
                return (curBeatInBar >= 0 && curBeatInBar < 4) ? oneBar[curBeatInBar] : "";
            }
            // For 2-bar (or final 2 bars of longer) count sections:
            // Bar 1: one, [rest], two, [rest]; Bar 2: one, two, three, four
            int offsetFromEnd = w.lengthBars - 2;
            if (localBar >= offsetFromEnd) {
                int twoBarIdx = (localBar - offsetFromEnd) * 4 + curBeatInBar;
                const char* pattern[8] = { "one", "", "two", "", "one", "two", "three", "four" };
                return (twoBarIdx >= 0 && twoBarIdx < 8) ? pattern[twoBarIdx] : "";
            }
            return "";
        }

        const char* words[8] = { "one", "two", "three", "four", "five", "six", "seven", "eight" };
        return (curBeatInBar >= 0 && curBeatInBar < 8) ? words[curBeatInBar] : "";
    };

    if (useSectionCountIn_ && !sectionCountWindows_.empty()) {
        for (const auto& w : sectionCountWindows_) {
            if (bar >= w.startBar && bar < w.startBar + w.lengthBars) {
                return wordForWindow(w, bar, beatInBar);
            }
        }
        return "";
    }

    // Default 2-bar count-in
    CountWindow defaultWin{ 0, 2 };
    return wordForWindow(defaultWin, bar, beatInBar);
}

void Metronome::setClickGain(float gain) {
    clickGain_ = std::max(0.0f, std::min(4.0f, gain));
}

void Metronome::setEighthClickGain(float gain) {
    eighthClickGain_ = std::max(0.0f, std::min(4.0f, gain));
}

void Metronome::setSixteenthClickGain(float gain) {
    sixteenthClickGain_ = std::max(0.0f, std::min(4.0f, gain));
}

void Metronome::setClickSubdivisions(bool enableEighths, bool enableSixteenths) {
    enableEighths_ = enableEighths;
    enableSixteenths_ = enableSixteenths;
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

void Metronome::loadClickSamples(const juce::AudioBuffer<float>& quarterClick,
                                const juce::AudioBuffer<float>& accentClick,
                                const juce::AudioBuffer<float>& eighthClick,
                                const juce::AudioBuffer<float>& sixteenthClick) {
    activeSamples_.clear();
    quarterClick_ = quarterClick;
    accentClick_ = accentClick;
    eighthClick_ = eighthClick;
    sixteenthClick_ = sixteenthClick;
}

void Metronome::loadVoiceWord(const std::string& word, const juce::AudioBuffer<float>& buffer) {
    activeSamples_.clear();
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
    isCompoundAtomic_.store(isCompound_, std::memory_order_relaxed);
    if (isCompound_) {
        // For compound time, tempo defines each set of 3 eighth notes
        double groupFrames = sampleRate_ * 60.0 / std::max(1, tempo_);
        beatFrames_ = std::max<int64_t>(1, static_cast<int64_t>(std::round(groupFrames / 3.0)));
        pulseCountPerBar_ = (beatsPerBar_ / 3) * 3;
    } else {
        beatFrames_ = std::max<int64_t>(1, static_cast<int64_t>(std::round(sampleRate_ * 60.0 / std::max(1, tempo_))));
        pulseCountPerBar_ = beatsPerBar_;
    }

    int initialCountBars = (useSectionCountIn_ && leadingCountBars_ > 0) ? leadingCountBars_ : 2;
    totalCountInFrames_ = beatFrames_ * pulseCountPerBar_ * initialCountBars;
    int64_t framesPerBar = std::max<int64_t>(1, beatFrames_ * pulseCountPerBar_);
    framesPerBarAtomic_.store(framesPerBar, std::memory_order_relaxed);
    totalBarsElapsed_.store(static_cast<double>(metronomeFrame_) / static_cast<double>(framesPerBar), std::memory_order_relaxed);
}

void Metronome::renderBlock(juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples) {
    int numOutputChannels = outputBuffer.getNumChannels();
    int64_t blockStart = metronomeFrame_;
    int64_t blockEnd = blockStart + numSamples;

    // Trigger pulses and subdivisions that fall within this block
    int64_t firstBeatIndex = std::max<int64_t>(0, blockStart / beatFrames_);
    int64_t lastBeatIndex = (blockEnd + beatFrames_ - 1) / beatFrames_ + 1;
    const int effClickCh = (numOutputChannels > 0) ? std::clamp(clickChannel_, 0, numOutputChannels - 1) : 0;
    const int effGuideCh = (numOutputChannels > 0) ? std::clamp(guideChannel_, 0, numOutputChannels - 1) : 0;

    auto resolveEighthBuf = [this]() -> const juce::AudioBuffer<float>& {
        return (eighthClick_.getNumSamples() > 0) ? eighthClick_ : quarterClick_;
    };
    auto resolveSixteenthBuf = [this]() -> const juce::AudioBuffer<float>& {
        return (sixteenthClick_.getNumSamples() > 0) ? sixteenthClick_ : quarterClick_;
    };

    for (int64_t beatIndex = firstBeatIndex; beatIndex <= lastBeatIndex; ++beatIndex) {
        int64_t beatStart = beatIndex * beatFrames_;
        int beatInBar = static_cast<int>(beatIndex % pulseCountPerBar_);

        if (beatStart >= blockStart && beatStart < blockEnd) {
            currentBeatInBar_ = beatInBar;
            currentBar_ = static_cast<int>(beatIndex / pulseCountPerBar_);

            bool isAccent = isCompound_ ? (beatInBar % 3 == 0) : (beatInBar == 0);
            if (numOutputChannels > 0) {
                if (isAccent) {
                    if (accentClick_.getNumSamples() > 0) {
                        activeSamples_.push_back({ &accentClick_, beatStart, clickGain_, effClickCh });
                    }
                } else if (isCompound_ && enableEighths_) {
                    const auto& buf = resolveEighthBuf();
                    if (buf.getNumSamples() > 0) {
                        activeSamples_.push_back({ &buf, beatStart, eighthClickGain_, effClickCh });
                    }
                } else {
                    if (quarterClick_.getNumSamples() > 0) {
                        activeSamples_.push_back({ &quarterClick_, beatStart, clickGain_, effClickCh });
                    }
                }
            }

            // Guide speech check: section announcements (bar prior to section start) + count-in
            if (numOutputChannels > 0) {
                std::string cue = getScheduledGuideCueForBeat(currentBar_, beatInBar);
                const juce::AudioBuffer<float>* cueBuf = nullptr;

                const std::string sectionPrefix = "section:";
                if (cue.rfind(sectionPrefix, 0) == 0) {
                    std::string secKey = cue.substr(sectionPrefix.size());
                    cueBuf = findSectionCueBuffer(secKey);

                    // If the active guide pack has no audio clip for this section name and this
                    // bar is also a count-in bar, fall back to the count-in word (e.g. "one").
                    if (!cueBuf) {
                        std::string fallbackWord = getCountInVoiceWordForBeat(currentBar_, beatInBar);
                        if (!fallbackWord.empty()) {
                            auto it = voiceWords_.find(fallbackWord);
                            if (it != voiceWords_.end() && it->second.getNumSamples() > 0) {
                                cueBuf = &it->second;
                            }
                        }
                    }
                } else if (!cue.empty()) {
                    auto it = voiceWords_.find(cue);
                    if (it != voiceWords_.end() && it->second.getNumSamples() > 0) {
                        cueBuf = &it->second;
                    }
                }

                if (cueBuf != nullptr) {
                    activeSamples_.push_back({ cueBuf, beatStart, guideGain_, effGuideCh });
                }
            }
        }

        // Eighth & sixteenth click subdivisions inside this beat
        if (numOutputChannels > 0 && (enableEighths_ || enableSixteenths_)) {
            if (isCompound_) {
                // Compound meter (6/8, 12/8): beatFrames_ is 1 eighth note; split in half for sixteenths
                if (enableSixteenths_) {
                    int64_t subPos = beatStart + beatFrames_ / 2;
                    if (subPos >= blockStart && subPos < blockEnd) {
                        const auto& buf = resolveSixteenthBuf();
                        if (buf.getNumSamples() > 0) {
                            activeSamples_.push_back({ &buf, subPos, sixteenthClickGain_, effClickCh });
                        }
                    }
                }
            } else {
                // Simple meter: beatFrames_ is 1 quarter note -> 4 sixteenth steps (1 = e, 2 = &, 3 = a)
                for (int sub = 1; sub <= 3; ++sub) {
                    int64_t subPos = beatStart + (beatFrames_ * sub) / 4;
                    if (subPos < blockStart || subPos >= blockEnd)
                        continue;

                    if (sub == 2) {
                        if (enableEighths_) {
                            const auto& buf = resolveEighthBuf();
                            if (buf.getNumSamples() > 0) {
                                activeSamples_.push_back({ &buf, subPos, eighthClickGain_, effClickCh });
                            }
                        } else if (enableSixteenths_) {
                            const auto& buf = resolveSixteenthBuf();
                            if (buf.getNumSamples() > 0) {
                                activeSamples_.push_back({ &buf, subPos, sixteenthClickGain_, effClickCh });
                            }
                        }
                    } else if (enableSixteenths_) {
                        const auto& buf = resolveSixteenthBuf();
                        if (buf.getNumSamples() > 0) {
                            activeSamples_.push_back({ &buf, subPos, sixteenthClickGain_, effClickCh });
                        }
                    }
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
    int64_t framesPerBar = std::max<int64_t>(1, beatFrames_ * pulseCountPerBar_);
    totalBarsElapsed_.store(static_cast<double>(metronomeFrame_) / static_cast<double>(framesPerBar), std::memory_order_relaxed);
}

} // namespace tracks
