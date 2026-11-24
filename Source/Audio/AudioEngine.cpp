#include "AudioEngine.h"
#include <BinaryData.h>
#include <algorithm>
#include <cmath>

namespace tracks {

AudioEngine::AudioEngine() {
    formatManager_.registerBasicFormats();

    deviceManager_.initialiseWithDefaultDevices(0, 8);
    deviceManager_.addAudioCallback(this);

    currentSampleRate_ = deviceManager_.getCurrentAudioDevice()
                             ? deviceManager_.getCurrentAudioDevice()->getCurrentSampleRate()
                             : 44100.0;
    currentOutputChannels_ = deviceManager_.getCurrentAudioDevice()
                                 ? deviceManager_.getCurrentAudioDevice()->getActiveOutputChannels().countNumberOfSetBits()
                                 : 2;
    currentOutputChannels_ = std::max(2, currentOutputChannels_);

    outputPeaks_.resize(static_cast<size_t>(currentOutputChannels_), 0.0f);
    metronome_.prepare(currentSampleRate_);

    loadEmbeddedAssets();
}

AudioEngine::~AudioEngine() {
    deviceManager_.removeAudioCallback(this);
}

void AudioEngine::loadEmbeddedAssets() {
    // Load embedded clicks from BinaryData
    auto normalClick = loadWavFromMemory(BinaryData::tone_normal_wav, BinaryData::tone_normal_wavSize);
    auto accentClick = loadWavFromMemory(BinaryData::tone_accent_wav, BinaryData::tone_accent_wavSize);
    metronome_.loadClickSamples(normalClick, accentClick);

    // Load embedded female count-in words
    metronome_.loadVoiceWord("one", loadWavFromMemory(BinaryData::one_wav, BinaryData::one_wavSize));
    metronome_.loadVoiceWord("two", loadWavFromMemory(BinaryData::two_wav, BinaryData::two_wavSize));
    metronome_.loadVoiceWord("three", loadWavFromMemory(BinaryData::three_wav, BinaryData::three_wavSize));
    metronome_.loadVoiceWord("four", loadWavFromMemory(BinaryData::four_wav, BinaryData::four_wavSize));
    metronome_.loadVoiceWord("five", loadWavFromMemory(BinaryData::five_wav, BinaryData::five_wavSize));
    metronome_.loadVoiceWord("six", loadWavFromMemory(BinaryData::six_wav, BinaryData::six_wavSize));
    metronome_.loadVoiceWord("seven", loadWavFromMemory(BinaryData::seven_wav, BinaryData::seven_wavSize));
    metronome_.loadVoiceWord("eight", loadWavFromMemory(BinaryData::eight_wav, BinaryData::eight_wavSize));
}

juce::AudioBuffer<float> AudioEngine::loadWavFromMemory(const void* data, size_t size) {
    if (!data || size == 0) return {};
    auto stream = std::make_unique<juce::MemoryInputStream>(data, size, false);
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(std::move(stream)));
    if (!reader) return {};

    juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    reader->read(&buffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
    return buffer;
}

juce::AudioBuffer<float> AudioEngine::loadAudioFile(const std::string& filePath) {
    juce::File file(filePath);
    if (!file.existsAsFile()) return {};

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
    if (!reader) return {};

    juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    reader->read(&buffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
    return buffer;
}

std::vector<AudioDeviceInfo> AudioEngine::getAvailableOutputDevices() {
    std::vector<AudioDeviceInfo> result;
    auto* currentDeviceType = deviceManager_.getCurrentDeviceTypeObject();
    if (!currentDeviceType) return result;

    currentDeviceType->scanForDevices();
    auto deviceNames = currentDeviceType->getDeviceNames(false); // output devices

    int currentIndex = 0;
    auto* curDev = deviceManager_.getCurrentAudioDevice();
    std::string activeName = curDev ? curDev->getName().toStdString() : "";

    for (int i = 0; i < deviceNames.size(); ++i) {
        AudioDeviceInfo info;
        info.index = currentIndex++;
        info.name = deviceNames[i].toStdString();
        info.isDefault = (info.name == activeName);

        // Query max channels if possible
        info.maxOutputChannels = 8;
        info.maxInputChannels = 0;
        result.push_back(info);
    }
    return result;
}

bool AudioEngine::selectOutputDevice(const std::string& deviceName, int preferredChannels) {
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager_.getAudioDeviceSetup(setup);
    setup.outputDeviceName = deviceName;
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.clear();
    for (int ch = 0; ch < preferredChannels; ++ch) {
        setup.outputChannels.setBit(ch);
    }

    auto error = deviceManager_.setAudioDeviceSetup(setup, true);
    return error.isEmpty();
}

void AudioEngine::loadSong(const Song& song) {
    std::lock_guard<std::mutex> lock(audioMutex_);
    loadedTracks_.clear();
    maxSongLengthFrames_ = 0;

    for (const auto& track : song.tracks) {
        if (track.filePath.empty()) continue;
        auto buf = loadAudioFile(track.filePath);
        if (buf.getNumSamples() > 0) {
            LoadedTrack lt;
            lt.track = track;
            lt.buffer = std::move(buf);
            lt.position = 0;
            maxSongLengthFrames_ = std::max(maxSongLengthFrames_, static_cast<int64_t>(lt.buffer.getNumSamples()));
            loadedTracks_.push_back(std::move(lt));
        }
    }

    metronome_.setTempo(song.tempo);
    metronome_.setTimeSignature(song.timeSignature);
    metronome_.reset();
}

void AudioEngine::play() {
    metronome_.reset();
    for (auto& t : loadedTracks_) {
        t.position = 0;
    }
    isPaused_.store(false);
    isPlaying_.store(true);
}

void AudioEngine::pause() {
    isPaused_.store(true);
}

void AudioEngine::resume() {
    isPaused_.store(false);
}

void AudioEngine::stop() {
    isPlaying_.store(false);
    isPaused_.store(false);
    metronome_.reset();
    for (auto& t : loadedTracks_) {
        t.position = 0;
    }
    std::lock_guard<std::mutex> lock(peakMutex_);
    std::fill(outputPeaks_.begin(), outputPeaks_.end(), 0.0f);
}

void AudioEngine::setMasterGain(float gain) {
    masterGain_.store(std::max(0.0f, std::min(4.0f, gain)));
}

void AudioEngine::setClickGain(float gain) {
    metronome_.setClickGain(gain);
}

void AudioEngine::setGuideGain(float gain) {
    metronome_.setGuideGain(gain);
}

void AudioEngine::setClickChannel(int channel) {
    metronome_.setClickChannel(channel);
}

void AudioEngine::setGuideChannel(int channel) {
    metronome_.setGuideChannel(channel);
}

void AudioEngine::setTempo(int tempo) {
    metronome_.setTempo(tempo);
}

void AudioEngine::setTimeSignature(const std::string& timeSignature) {
    metronome_.setTimeSignature(timeSignature);
}

void AudioEngine::setTrackMuted(int trackIndex, bool muted) {
    std::lock_guard<std::mutex> lock(audioMutex_);
    if (trackIndex >= 0 && trackIndex < static_cast<int>(loadedTracks_.size())) {
        loadedTracks_[trackIndex].track.muted = muted;
    }
}

void AudioEngine::setTrackGain(int trackIndex, float gain) {
    std::lock_guard<std::mutex> lock(audioMutex_);
    if (trackIndex >= 0 && trackIndex < static_cast<int>(loadedTracks_.size())) {
        loadedTracks_[trackIndex].track.gain = gain;
    }
}

void AudioEngine::setTrackOutputChannel(int trackIndex, int channel) {
    std::lock_guard<std::mutex> lock(audioMutex_);
    if (trackIndex >= 0 && trackIndex < static_cast<int>(loadedTracks_.size())) {
        loadedTracks_[trackIndex].track.outputChannel = channel;
    }
}

std::vector<float> AudioEngine::getOutputPeaks() {
    std::lock_guard<std::mutex> lock(peakMutex_);
    return outputPeaks_;
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    if (device) {
        currentSampleRate_ = device->getCurrentSampleRate();
        currentOutputChannels_ = std::max(2, device->getActiveOutputChannels().countNumberOfSetBits());
    }
    metronome_.prepare(currentSampleRate_);
    renderScratchBuffer_.setSize(currentOutputChannels_, 2048);

    std::lock_guard<std::mutex> lock(peakMutex_);
    outputPeaks_.assign(static_cast<size_t>(currentOutputChannels_), 0.0f);
}

void AudioEngine::audioDeviceStopped() {
    std::lock_guard<std::mutex> lock(peakMutex_);
    std::fill(outputPeaks_.begin(), outputPeaks_.end(), 0.0f);
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const*,
                                                   int,
                                                   float* const* outputChannelData,
                                                   int numOutputChannels,
                                                   int numSamples,
                                                   const juce::AudioIODeviceCallbackContext&) {
    // 1. Zero all hardware output channels to avoid stale buffer artifacts
    for (int ch = 0; ch < numOutputChannels; ++ch) {
        if (outputChannelData[ch]) {
            juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
        }
    }

    if (!isPlaying_.load() || isPaused_.load()) {
        std::lock_guard<std::mutex> lock(peakMutex_);
        for (float& p : outputPeaks_) {
            p *= 0.82f;
        }
        return;
    }

    // Ensure scratch buffer matches output channel count and sample size
    if (renderScratchBuffer_.getNumChannels() != numOutputChannels ||
        renderScratchBuffer_.getNumSamples() < numSamples) {
        renderScratchBuffer_.setSize(numOutputChannels, numSamples, false, false, true);
    }
    renderScratchBuffer_.clear();

    // 2. Mix backing tracks
    {
        std::unique_lock<std::mutex> lock(audioMutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            for (auto& lt : loadedTracks_) {
                if (lt.track.muted || lt.buffer.getNumSamples() == 0)
                    continue;

                int channelA = lt.track.outputChannel;
                int trackSamples = lt.buffer.getNumSamples();
                int trackChannels = lt.buffer.getNumChannels();
                float gain = lt.track.gain;

                int samplesRemaining = numSamples;
                int outOffset = 0;

                while (samplesRemaining > 0) {
                    int available = std::min(samplesRemaining, static_cast<int>(trackSamples - lt.position));
                    if (available <= 0) {
                        lt.position = 0;
                        available = std::min(samplesRemaining, trackSamples);
                    }

                    if (trackChannels >= 2) {
                        // Stereo track: route across [channelA, channelA + 1] if available,
                        // otherwise downmix to mono on channelA so right channel is preserved
                        if (channelA + 1 < numOutputChannels) {
                            renderScratchBuffer_.addFrom(channelA, outOffset, lt.buffer, 0, static_cast<int>(lt.position), available, gain);
                            renderScratchBuffer_.addFrom(channelA + 1, outOffset, lt.buffer, 1, static_cast<int>(lt.position), available, gain);
                        } else if (channelA < numOutputChannels) {
                            float halfGain = gain * 0.5f;
                            renderScratchBuffer_.addFrom(channelA, outOffset, lt.buffer, 0, static_cast<int>(lt.position), available, halfGain);
                            renderScratchBuffer_.addFrom(channelA, outOffset, lt.buffer, 1, static_cast<int>(lt.position), available, halfGain);
                        }
                    } else {
                        // Mono track: route directly to channelA
                        if (channelA < numOutputChannels) {
                            renderScratchBuffer_.addFrom(channelA, outOffset, lt.buffer, 0, static_cast<int>(lt.position), available, gain);
                        }
                    }

                    lt.position = (lt.position + available) % trackSamples;
                    outOffset += available;
                    samplesRemaining -= available;
                }
            }
        }
    }

    // 3. Render metronome and count-in
    metronome_.renderBlock(renderScratchBuffer_, 0, numSamples);

    // 4. Apply master gain and soft clip/clamp to [-0.95, 0.95]
    float mg = masterGain_.load();
    for (int ch = 0; ch < numOutputChannels; ++ch) {
        float* dst = outputChannelData[ch];
        const float* src = renderScratchBuffer_.getReadPointer(ch);
        if (!dst) continue;

        for (int i = 0; i < numSamples; ++i) {
            float s = src[i] * mg;
            dst[i] = std::max(-0.95f, std::min(0.95f, s));
        }
    }

    // 5. Compute channel peaks for metering
    {
        std::lock_guard<std::mutex> lock(peakMutex_);
        if (outputPeaks_.size() != static_cast<size_t>(numOutputChannels)) {
            outputPeaks_.assign(static_cast<size_t>(numOutputChannels), 0.0f);
        }
        for (int ch = 0; ch < numOutputChannels; ++ch) {
            float peak = 0.0f;
            if (outputChannelData[ch]) {
                auto range = juce::FloatVectorOperations::findMinAndMax(outputChannelData[ch], numSamples);
                peak = std::max(std::abs(range.getStart()), std::abs(range.getEnd()));
            }
            outputPeaks_[ch] = std::max(outputPeaks_[ch] * 0.82f, peak);
        }
    }
}

} // namespace tracks
