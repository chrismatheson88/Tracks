#include "AudioEngine.h"
#include "../Services/AudioAttachmentService.h"
#include <BinaryData.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <future>
#include <set>
#include <unordered_set>

namespace fs = std::filesystem;

namespace tracks {

AudioEngine::AudioEngine() {
    formatManager_.registerBasicFormats();

    deviceManager_.initialiseWithDefaultDevices(0, 32);
    deviceManager_.addAudioCallback(this);

    auto* curDev = deviceManager_.getCurrentAudioDevice();
    currentSampleRate_ = curDev ? curDev->getCurrentSampleRate() : 44100.0;
    int chCount = curDev ? curDev->getActiveOutputChannels().countNumberOfSetBits() : 2;
    chCount = std::max(1, chCount);
    currentOutputChannels_.store(chCount, std::memory_order_relaxed);

    outputPeaks_.resize(static_cast<size_t>(chCount), 0.0f);
    metronome_.prepare(currentSampleRate_);

    loadEmbeddedAssets();
    loaderThread_ = std::thread([this]() { loaderLoop(); });
}

AudioEngine::~AudioEngine() {
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        shuttingDown_ = true;
        preloadQueue_.clear();
    }
    loaderCv_.notify_all();
    if (loaderThread_.joinable()) {
        loaderThread_.join();
    }
    deviceManager_.removeAudioCallback(this);
}

void AudioEngine::loadEmbeddedClicksLocked() {
    auto quarterClick = loadWavFromMemory(BinaryData::quarter_wav, BinaryData::quarter_wavSize);
    auto accentClick = loadWavFromMemory(BinaryData::accent_wav, BinaryData::accent_wavSize);
    auto eighthClick = loadWavFromMemory(BinaryData::eighth_wav, BinaryData::eighth_wavSize);
    auto sixteenthClick = loadWavFromMemory(BinaryData::sixteenth_wav, BinaryData::sixteenth_wavSize);
    metronome_.loadClickSamples(quarterClick, accentClick, eighthClick, sixteenthClick);
}

void AudioEngine::loadEmbeddedVoicesLocked() {
    metronome_.loadVoiceWord("one", loadWavFromMemory(BinaryData::count1_wav, BinaryData::count1_wavSize));
    metronome_.loadVoiceWord("two", loadWavFromMemory(BinaryData::count2_wav, BinaryData::count2_wavSize));
    metronome_.loadVoiceWord("three", loadWavFromMemory(BinaryData::count3_wav, BinaryData::count3_wavSize));
    metronome_.loadVoiceWord("four", loadWavFromMemory(BinaryData::count4_wav, BinaryData::count4_wavSize));
    metronome_.loadVoiceWord("five", loadWavFromMemory(BinaryData::count5_wav, BinaryData::count5_wavSize));
    metronome_.loadVoiceWord("six", loadWavFromMemory(BinaryData::count6_wav, BinaryData::count6_wavSize));
    metronome_.loadVoiceWord("seven", loadWavFromMemory(BinaryData::count7_wav, BinaryData::count7_wavSize));
    metronome_.loadVoiceWord("eight", loadWavFromMemory(BinaryData::count8_wav, BinaryData::count8_wavSize));
}

void AudioEngine::loadEmbeddedAssets() {
    std::lock_guard<std::mutex> lock(audioMutex_);
    loadEmbeddedClicksLocked();
    loadEmbeddedVoicesLocked();
}

juce::AudioBuffer<float> AudioEngine::loadMonoAssetFile(const std::string& filePath, double targetSampleRate) const {
    auto decoded = loadAudioFileStatic(filePath);
    if (decoded.getNumSamples() <= 0 || decoded.getNumChannels() <= 0) return {};

    // Read source sample rate to resample if needed
    double srcSampleRate = targetSampleRate;
    {
        juce::File file(filePath);
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        if (std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file)); reader && reader->sampleRate > 0.0) {
            srcSampleRate = reader->sampleRate;
        }
    }

    const int srcSamples = decoded.getNumSamples();
    juce::AudioBuffer<float> mono(1, srcSamples);
    if (decoded.getNumChannels() >= 2) {
        const float* L = decoded.getReadPointer(0);
        const float* R = decoded.getReadPointer(1);
        float* dst = mono.getWritePointer(0);
        for (int i = 0; i < srcSamples; ++i) {
            dst[i] = 0.5f * (L[i] + R[i]);
        }
    } else {
        mono.copyFrom(0, 0, decoded, 0, 0, srcSamples);
    }

    if (targetSampleRate > 0.0 && srcSampleRate > 0.0 && std::abs(srcSampleRate - targetSampleRate) > 1.0) {
        const double ratio = srcSampleRate / targetSampleRate;
        const int dstSamples = std::max(1, static_cast<int>(std::round(static_cast<double>(srcSamples) / ratio)));
        juce::AudioBuffer<float> resampled(1, dstSamples);
        juce::LagrangeInterpolator interp;
        interp.reset();
        interp.process(ratio, mono.getReadPointer(0), resampled.getWritePointer(0), dstSamples);
        return resampled;
    }

    return mono;
}

namespace {

std::string matchCountInWordFromFilename(const std::string& fileStem) {
    std::string lower = fileStem;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Require "count" in the filename so non-count guide cues (e.g. "verse-1", "chorus-2") aren't matched
    if (lower.find("count") == std::string::npos) {
        return "";
    }

    static const std::pair<const char*, const char*> wordMap[] = {
        { "1", "one" }, { "2", "two" }, { "3", "three" }, { "4", "four" },
        { "5", "five" }, { "6", "six" }, { "7", "seven" }, { "8", "eight" },
        { "one", "one" }, { "two", "two" }, { "three", "three" }, { "four", "four" },
        { "five", "five" }, { "six", "six" }, { "seven", "seven" }, { "eight", "eight" }
    };

    // Tokenize on non-alphanumeric delimiters so "count-1", "guidecue-count-en-female-3", etc. match cleanly
    std::vector<std::string> tokens;
    std::string current;
    for (unsigned char ch : lower) {
        if (std::isalnum(ch)) {
            current.push_back(static_cast<char>(ch));
        } else if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(current);

    for (const auto& tok : tokens) {
        for (const auto& [key, canonical] : wordMap) {
            if (tok == key) return canonical;
        }
    }

    // Also handle compact forms like "count1".."count8"
    for (const auto& tok : tokens) {
        if (tok.rfind("count", 0) == 0 && tok.size() == 6 && tok[5] >= '1' && tok[5] <= '8') {
            return wordMap[tok[5] - '1'].second;
        }
    }

    return "";
}

std::string extractSectionKeyFromFilename(const std::string& fileStem) {
    std::string lower = fileStem;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Match Playback-style filenames like "guidecue-section-en-female-Pre Chorus 1"
    // by taking the segment after the last hyphen when "section-" is present.
    std::string candidate = fileStem;
    if (lower.find("section-") != std::string::npos || lower.find("section_") != std::string::npos) {
        auto lastDash = fileStem.rfind('-');
        if (lastDash != std::string::npos && lastDash + 1 < fileStem.size()) {
            candidate = fileStem.substr(lastDash + 1);
        }
    }
    return SongSection::normalizeSectionName(candidate);
}

} // namespace

std::vector<std::string> AudioEngine::getAvailableClickPacks() const {
    std::set<std::string> packs;
    auto clicksDir = AudioAttachmentService::getProjectRoot() / "assets" / "clicks";
    std::error_code ec;

    if (fs::exists(clicksDir, ec) && fs::is_directory(clicksDir, ec)) {
        for (const auto& entry : fs::directory_iterator(clicksDir, ec)) {
            if (!entry.is_directory()) continue;
            std::string dirName = entry.path().filename().string();
            if (dirName.empty() || dirName[0] == '.') continue;

            bool hasAudio = false;
            for (const auto& sub : fs::directory_iterator(entry.path(), ec)) {
                if (sub.is_regular_file() &&
                    AudioAttachmentService::isSupportedAudioExtension(sub.path().extension().string())) {
                    hasAudio = true;
                    break;
                }
            }
            if (hasAudio) {
                packs.insert(dirName);
            }
        }
    }

    std::vector<std::string> result(packs.begin(), packs.end());
    if (result.empty()) {
        result.push_back("tone");
    }
    return result;
}

std::vector<std::string> AudioEngine::getAvailableVoicePacks() const {
    std::set<std::string> packs;
    auto guideDir = AudioAttachmentService::getProjectRoot() / "assets" / "guide";
    std::error_code ec;

    if (fs::exists(guideDir, ec) && fs::is_directory(guideDir, ec)) {
        for (const auto& entry : fs::directory_iterator(guideDir, ec)) {
            if (!entry.is_directory()) continue;
            std::string dirName = entry.path().filename().string();
            if (!dirName.empty() && dirName[0] != '.') {
                packs.insert(dirName);
            }
        }
    }

    std::vector<std::string> result(packs.begin(), packs.end());
    if (result.empty()) {
        result.push_back("female");
    }
    return result;
}

bool AudioEngine::selectClickPack(const std::string& packName) {
    activeClickPack_ = packName;
    auto clicksDir = AudioAttachmentService::getProjectRoot() / "assets" / "clicks";
    fs::path quarterPath;
    fs::path accentPath;
    fs::path eighthPath;
    fs::path sixteenthPath;
    fs::path fallbackPath;
    std::error_code ec;

    auto subDir = clicksDir / packName;
    if (fs::exists(subDir, ec) && fs::is_directory(subDir, ec)) {
        for (const auto& entry : fs::directory_iterator(subDir, ec)) {
            if (!entry.is_regular_file() || !AudioAttachmentService::isSupportedAudioExtension(entry.path().extension().string()))
                continue;
            std::string lower = entry.path().stem().string();
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower == "accent" || lower.find("accent") != std::string::npos) {
                accentPath = entry.path();
            } else if (lower == "quarter" || lower.find("quarter") != std::string::npos) {
                quarterPath = entry.path();
            } else if (lower == "eighth" || lower.find("eighth") != std::string::npos) {
                eighthPath = entry.path();
            } else if (lower == "sixteenth" || lower.find("sixteenth") != std::string::npos) {
                sixteenthPath = entry.path();
            } else if (fallbackPath.empty()) {
                fallbackPath = entry.path();
            }
        }
    }

    if (quarterPath.empty()) {
        quarterPath = fallbackPath;
    }

    juce::AudioBuffer<float> quarterBuf = quarterPath.empty() ? juce::AudioBuffer<float>() : loadMonoAssetFile(quarterPath.string(), currentSampleRate_);
    juce::AudioBuffer<float> accentBuf = accentPath.empty() ? juce::AudioBuffer<float>() : loadMonoAssetFile(accentPath.string(), currentSampleRate_);
    juce::AudioBuffer<float> eighthBuf = eighthPath.empty() ? juce::AudioBuffer<float>() : loadMonoAssetFile(eighthPath.string(), currentSampleRate_);
    juce::AudioBuffer<float> sixteenthBuf = sixteenthPath.empty() ? juce::AudioBuffer<float>() : loadMonoAssetFile(sixteenthPath.string(), currentSampleRate_);

    if (quarterBuf.getNumSamples() == 0 && accentBuf.getNumSamples() > 0) quarterBuf = accentBuf;
    if (accentBuf.getNumSamples() == 0 && quarterBuf.getNumSamples() > 0) accentBuf = quarterBuf;
    if (eighthBuf.getNumSamples() == 0 && quarterBuf.getNumSamples() > 0) eighthBuf = quarterBuf;
    if (sixteenthBuf.getNumSamples() == 0 && quarterBuf.getNumSamples() > 0) sixteenthBuf = quarterBuf;

    std::lock_guard<std::mutex> lock(audioMutex_);
    if (quarterBuf.getNumSamples() > 0 && accentBuf.getNumSamples() > 0) {
        metronome_.loadClickSamples(quarterBuf, accentBuf, eighthBuf, sixteenthBuf);
        return true;
    }

    loadEmbeddedClicksLocked();
    return false;
}

bool AudioEngine::selectVoicePack(const std::string& packName) {
    auto packDir = AudioAttachmentService::getProjectRoot() / "assets" / "guide" / packName;
    std::map<std::string, juce::AudioBuffer<float>> loadedWords;
    std::map<std::string, std::string> indexedSectionFiles;
    std::error_code ec;

    if (fs::exists(packDir, ec) && fs::is_directory(packDir, ec)) {
        for (const auto& entry : fs::directory_iterator(packDir, ec)) {
            if (!entry.is_regular_file() || !AudioAttachmentService::isSupportedAudioExtension(entry.path().extension().string()))
                continue;

            std::string stem = entry.path().stem().string();
            std::string word = matchCountInWordFromFilename(stem);
            if (!word.empty()) {
                auto buf = loadMonoAssetFile(entry.path().string(), currentSampleRate_);
                if (buf.getNumSamples() > 0) {
                    loadedWords[word] = std::move(buf);
                }
            } else {
                std::string secKey = extractSectionKeyFromFilename(stem);
                if (!secKey.empty()) {
                    indexedSectionFiles[secKey] = entry.path().string();
                }
            }
        }
    }

    std::vector<SongSection> currentSectionsCopy;
    {
        std::lock_guard<std::mutex> gLock(guideAssetsMutex_);
        activeVoicePack_ = packName;
        sectionCueFilePaths_ = std::move(indexedSectionFiles);
        sectionCueCache_.clear();
        currentSectionsCopy = activeSections_;
    }

    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        // Load embedded defaults first so any missing number (e.g. "eight") gracefully falls back
        loadEmbeddedVoicesLocked();
        metronome_.clearSectionCues();
        for (const auto& [word, buf] : loadedWords) {
            metronome_.loadVoiceWord(word, buf);
        }
    }

    if (!currentSectionsCopy.empty()) {
        syncSectionCuesForSections(currentSectionsCopy);
    }

    return !loadedWords.empty();
}

void AudioEngine::syncSectionCuesForSections(const std::vector<SongSection>& sections) {
    std::vector<std::string> keysToDecode;
    std::map<std::string, std::string> pathsToDecode;
    std::map<std::string, juce::AudioBuffer<float>> readyBuffers;

    {
        std::lock_guard<std::mutex> gLock(guideAssetsMutex_);
        activeSections_ = sections;

        auto findKeyInMap = [&](const std::string& rawKey) -> std::string {
            if (sectionCueFilePaths_.count(rawKey)) return rawKey;
            auto lastSpace = rawKey.rfind(' ');
            if (lastSpace != std::string::npos) {
                std::string tail = rawKey.substr(lastSpace + 1);
                bool allDigits = !tail.empty() && std::all_of(tail.begin(), tail.end(),
                                                              [](unsigned char c) { return std::isdigit(c) != 0; });
                if (allDigits) {
                    std::string baseKey = rawKey.substr(0, lastSpace);
                    if (sectionCueFilePaths_.count(baseKey)) return baseKey;
                }
            }
            return "";
        };

        for (const auto& sec : sections) {
            if (sec.isCountSection()) continue;
            std::string matchedKey = findKeyInMap(sec.normalizedKey());
            if (matchedKey.empty()) continue;

            auto cacheIt = sectionCueCache_.find(matchedKey);
            if (cacheIt != sectionCueCache_.end()) {
                readyBuffers[matchedKey] = cacheIt->second;
            } else if (!pathsToDecode.count(matchedKey)) {
                pathsToDecode[matchedKey] = sectionCueFilePaths_[matchedKey];
                keysToDecode.push_back(matchedKey);
            }
        }
    }

    // Decode any uncached section cue files outside audioMutex_
    std::map<std::string, juce::AudioBuffer<float>> newlyDecoded;
    for (const auto& key : keysToDecode) {
        auto buf = loadMonoAssetFile(pathsToDecode[key], currentSampleRate_);
        if (buf.getNumSamples() > 0) {
            newlyDecoded[key] = std::move(buf);
        }
    }

    if (!newlyDecoded.empty()) {
        std::lock_guard<std::mutex> gLock(guideAssetsMutex_);
        for (const auto& [key, buf] : newlyDecoded) {
            sectionCueCache_[key] = buf;
            readyBuffers[key] = buf;
        }
    }

    std::lock_guard<std::mutex> lock(audioMutex_);
    for (const auto& [key, buf] : readyBuffers) {
        metronome_.loadSectionCue(key, buf);
    }
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

juce::AudioBuffer<float> AudioEngine::loadAudioFileStatic(const std::string& filePath) {
    juce::File file(filePath);
    if (!file.existsAsFile()) return {};

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (!reader) return {};

    juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    reader->read(&buffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
    return buffer;
}

juce::AudioBuffer<float> AudioEngine::loadAudioFile(const std::string& filePath) {
    return loadAudioFileStatic(filePath);
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
    if (auto* dev = deviceManager_.getCurrentAudioDevice()) {
        int activeCh = dev->getActiveOutputChannels().countNumberOfSetBits();
        if (activeCh <= 0) {
            activeCh = dev->getOutputChannelNames().size();
        }
        currentOutputChannels_.store(std::max(1, activeCh), std::memory_order_relaxed);
    }
    return error.isEmpty();
}

std::vector<std::string> AudioEngine::extractTrackPaths(const Song& song) {
    std::vector<std::string> paths;
    paths.reserve(song.tracks.size());
    for (const auto& t : song.tracks) {
        if (!t.filePath.empty()) {
            paths.push_back(t.filePath);
        }
    }
    return paths;
}

std::vector<float> AudioEngine::computeWaveformPeaks(const std::vector<LoadedTrack>& tracks, int64_t maxLengthFrames) {
    constexpr int kNumBins = 512;
    std::vector<float> peaks;

    if (!tracks.empty() && maxLengthFrames > 0) {
        peaks.assign(kNumBins, 0.0f);
        float globalMax = 0.0f;

        for (int b = 0; b < kNumBins; ++b) {
            int64_t startF = (static_cast<int64_t>(b) * maxLengthFrames) / kNumBins;
            int64_t endF = (static_cast<int64_t>(b + 1) * maxLengthFrames) / kNumBins;
            if (endF <= startF) endF = std::min(maxLengthFrames, startF + 1);

            int64_t span = endF - startF;
            int64_t step = std::max<int64_t>(1, span / 128);

            float binPeak = 0.0f;
            for (int64_t f = startF; f < endF; f += step) {
                float sum = 0.0f;
                for (const auto& lt : tracks) {
                    if (lt.track.muted || !lt.buffer) continue;
                    int numSamples = lt.buffer->getNumSamples();
                    if (f >= numSamples) continue;

                    int ch = lt.buffer->getNumChannels();
                    float sample = 0.0f;
                    if (ch >= 2) {
                        sample = 0.5f * (lt.buffer->getSample(0, static_cast<int>(f)) +
                                         lt.buffer->getSample(1, static_cast<int>(f)));
                    } else if (ch == 1) {
                        sample = lt.buffer->getSample(0, static_cast<int>(f));
                    }
                    sum += sample * lt.track.gain;
                }
                binPeak = std::max(binPeak, std::abs(sum));
            }
            peaks[static_cast<size_t>(b)] = binPeak;
            globalMax = std::max(globalMax, binPeak);
        }

        if (globalMax > 1e-5f) {
            float invScale = 1.0f / std::max(0.05f, globalMax);
            for (float& p : peaks) {
                p = std::clamp(p * invScale, 0.0f, 1.0f);
            }
        }
    }

    return peaks;
}

std::shared_ptr<AudioEngine::CachedSongAudio> AudioEngine::decodeSongAudio(const Song& song) {
    auto cached = std::make_shared<CachedSongAudio>();
    cached->songId = song.id.value_or(-1);
    cached->trackPaths = extractTrackPaths(song);

    // Decode stems in parallel (bounded batching to avoid oversubscription)
    std::vector<SongTrack> validTracks;
    for (const auto& t : song.tracks) {
        if (!t.filePath.empty()) {
            validTracks.push_back(t);
        }
    }

    const size_t maxConcurrent = std::max<size_t>(1, std::min<size_t>(4, std::thread::hardware_concurrency()));
    std::vector<juce::AudioBuffer<float>> decodedBuffers(validTracks.size());

    for (size_t offset = 0; offset < validTracks.size(); offset += maxConcurrent) {
        size_t batchEnd = std::min(validTracks.size(), offset + maxConcurrent);
        std::vector<std::future<juce::AudioBuffer<float>>> futures;
        futures.reserve(batchEnd - offset);

        for (size_t i = offset; i < batchEnd; ++i) {
            std::string path = validTracks[i].filePath;
            futures.push_back(std::async(std::launch::async, [path]() {
                return loadAudioFileStatic(path);
            }));
        }

        for (size_t i = offset; i < batchEnd; ++i) {
            decodedBuffers[i] = futures[i - offset].get();
        }
    }

    for (size_t i = 0; i < validTracks.size(); ++i) {
        if (decodedBuffers[i].getNumSamples() > 0) {
            LoadedTrack lt;
            lt.track = validTracks[i];
            lt.buffer = std::make_shared<const juce::AudioBuffer<float>>(std::move(decodedBuffers[i]));
            lt.position = 0;
            cached->maxLengthFrames = std::max(cached->maxLengthFrames, static_cast<int64_t>(lt.buffer->getNumSamples()));
            cached->tracks.push_back(std::move(lt));
        }
    }

    cached->waveformPeaks = computeWaveformPeaks(cached->tracks, cached->maxLengthFrames);
    return cached;
}

void AudioEngine::applyCachedAudio(const std::shared_ptr<CachedSongAudio>& cached, const Song* overrideSettings) {
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        loadedTracks_.clear();
        maxSongLengthFrames_ = 0;

        if (cached) {
            loadedTracks_.reserve(cached->tracks.size());
            for (const auto& srcTrack : cached->tracks) {
                LoadedTrack lt;
                lt.track = srcTrack.track;
                lt.buffer = srcTrack.buffer;
                lt.position = 0;
                loadedTracks_.push_back(std::move(lt));
            }
            maxSongLengthFrames_ = cached->maxLengthFrames;
        }

        if (overrideSettings) {
            metronome_.setTempo(overrideSettings->tempo);
            metronome_.setTimeSignature(overrideSettings->timeSignature);
            metronome_.setSections(overrideSettings->sections);
            metronome_.setClickSubdivisions(overrideSettings->clickEighths, overrideSettings->clickSixteenths);
            metronome_.setEighthClickGain(overrideSettings->clickEighthsGain);
            metronome_.setSixteenthClickGain(overrideSettings->clickSixteenthsGain);
            metronome_.setUseSectionCountIn(useTracks_.load(std::memory_order_relaxed) && !loadedTracks_.empty());
            metronome_.reset();
            hasLeadingCountSection_.store(metronome_.hasLeadingCountSection(), std::memory_order_relaxed);
            int totalSecBars = 0;
            for (const auto& sec : overrideSettings->sections) {
                totalSecBars += std::max(1, sec.lengthBars);
            }
            sectionTotalBars_.store(totalSecBars, std::memory_order_relaxed);
        }

        hasTracks_.store(!loadedTracks_.empty(), std::memory_order_relaxed);
        maxSongLengthFramesAtomic_.store(maxSongLengthFrames_, std::memory_order_relaxed);
        if (!overrideSettings) {
            metronome_.setUseSectionCountIn(useTracks_.load(std::memory_order_relaxed) && !loadedTracks_.empty());
        }
    }

    {
        std::lock_guard<std::mutex> wfLock(waveformMutex_);
        waveformPeaks_ = cached ? cached->waveformPeaks : std::vector<float>{};
    }
    waveformVersion_.fetch_add(1, std::memory_order_relaxed);
    isLoadingTracks_.store(false, std::memory_order_relaxed);
}

std::string AudioEngine::makeCacheKey(const Song& song) {
    if (!song.id.has_value()) return "";
    return std::to_string(*song.id) + ":" + std::to_string(song.activeArrangementId.value_or(0));
}

void AudioEngine::enqueueSongLoadLocked(const Song& song, bool highPriority) {
    std::string key = makeCacheKey(song);
    if (key.empty() || extractTrackPaths(song).empty()) return;

    auto targetPaths = extractTrackPaths(song);

    auto cacheIt = songAudioCache_.find(key);
    if (cacheIt != songAudioCache_.end() && cacheIt->second && cacheIt->second->trackPaths == targetPaths) {
        return; // Already cached
    }

    // Remove duplicate queued entry for this (songId, arrangementId) key
    preloadQueue_.erase(
        std::remove_if(preloadQueue_.begin(), preloadQueue_.end(),
                       [&key](const Song& queued) { return makeCacheKey(queued) == key; }),
        preloadQueue_.end());

    if (highPriority) {
        preloadQueue_.push_front(song);
    } else {
        preloadQueue_.push_back(song);
    }
}

void AudioEngine::loaderLoop() {
    while (true) {
        Song nextSong;
        uint64_t gen = 0;

        {
            std::unique_lock<std::mutex> lock(cacheMutex_);
            loaderCv_.wait(lock, [this]() { return shuttingDown_ || !preloadQueue_.empty(); });
            if (shuttingDown_) return;

            nextSong = std::move(preloadQueue_.front());
            preloadQueue_.pop_front();
            gen = preloadGeneration_;
        }

        auto decoded = decodeSongAudio(nextSong);
        std::string nextKey = makeCacheKey(nextSong);

        bool shouldApplyToActive = false;
        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            if (shuttingDown_) return;

            // Keep result if it belongs to the current setlist generation OR is the active song/arrangement
            bool isActiveSong = !nextKey.empty() && !activeCacheKey_.empty() &&
                                (nextKey == activeCacheKey_) &&
                                (activeTrackPaths_.empty() || decoded->trackPaths == activeTrackPaths_);

            if (gen == preloadGeneration_ || isActiveSong) {
                if (!nextKey.empty()) {
                    songAudioCache_[nextKey] = decoded;
                }
                if (isActiveSong) {
                    activeTrackPaths_ = decoded->trackPaths;
                    shouldApplyToActive = true;
                }
            }
        }

        if (shouldApplyToActive) {
            applyCachedAudio(decoded, nullptr);
        }
    }
}

void AudioEngine::preloadSetlist(int64_t setlistId, const std::vector<Song>& songs) {
    std::unordered_set<std::string> neededKeys;
    for (const auto& s : songs) {
        std::string key = makeCacheKey(s);
        if (!key.empty() && !extractTrackPaths(s).empty()) {
            neededKeys.insert(key);
        }
    }

    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        bool setChanged = (!cachedSetlistId_.has_value() || *cachedSetlistId_ != setlistId);
        cachedSetlistId_ = setlistId;

        if (setChanged) {
            preloadGeneration_++;
            preloadQueue_.clear();
        }

        // Evict cached items that are not part of this setlist
        for (auto it = songAudioCache_.begin(); it != songAudioCache_.end(); ) {
            if (neededKeys.find(it->first) == neededKeys.end()) {
                it = songAudioCache_.erase(it);
            } else {
                ++it;
            }
        }

        // Queue any songs/arrangements in this setlist not yet cached (or whose files changed)
        for (const auto& s : songs) {
            enqueueSongLoadLocked(s, false);
        }
    }
    loaderCv_.notify_one();
}

void AudioEngine::invalidateCachedSong(int64_t songId) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    std::string prefix = std::to_string(songId) + ":";
    for (auto it = songAudioCache_.begin(); it != songAudioCache_.end(); ) {
        if (it->first.rfind(prefix, 0) == 0) {
            it = songAudioCache_.erase(it);
        } else {
            ++it;
        }
    }
    if (activeCacheKey_.rfind(prefix, 0) == 0) {
        activeTrackPaths_.clear();
    }
}

void AudioEngine::loadSong(const Song& song) {
    syncSectionCuesForSections(song.sections);
    auto newPaths = extractTrackPaths(song);
    std::string cacheKey = makeCacheKey(song);

    // Fast path 1: Same song/arrangement and identical track files -> only update tempo/time signature/sections!
    {
        std::lock_guard<std::mutex> cacheLock(cacheMutex_);
        if (!cacheKey.empty() && !activeCacheKey_.empty() &&
            cacheKey == activeCacheKey_ && newPaths == activeTrackPaths_) {
            std::lock_guard<std::mutex> audioLock(audioMutex_);
            metronome_.setTempo(song.tempo);
            metronome_.setTimeSignature(song.timeSignature);
            metronome_.setSections(song.sections);
            metronome_.setClickSubdivisions(song.clickEighths, song.clickSixteenths);
            metronome_.setEighthClickGain(song.clickEighthsGain);
            metronome_.setSixteenthClickGain(song.clickSixteenthsGain);
            metronome_.setUseSectionCountIn(useTracks_.load(std::memory_order_relaxed) && hasTracks_.load(std::memory_order_relaxed));
            hasLeadingCountSection_.store(metronome_.hasLeadingCountSection(), std::memory_order_relaxed);
            int totalSecBars = 0;
            for (const auto& sec : song.sections) {
                totalSecBars += std::max(1, sec.lengthBars);
            }
            sectionTotalBars_.store(totalSecBars, std::memory_order_relaxed);
            return;
        }
    }

    // No tracks on this song -> instant reset
    if (newPaths.empty()) {
        {
            std::lock_guard<std::mutex> cacheLock(cacheMutex_);
            activeCacheKey_ = cacheKey;
            activeTrackPaths_.clear();
        }
        applyCachedAudio(nullptr, &song);
        return;
    }

    // Fast path 2: Already decoded in the setlist cache -> instant pointer swap!
    std::shared_ptr<CachedSongAudio> cachedHit;
    {
        std::lock_guard<std::mutex> cacheLock(cacheMutex_);
        activeCacheKey_ = cacheKey;
        activeTrackPaths_ = newPaths;

        if (!cacheKey.empty()) {
            auto it = songAudioCache_.find(cacheKey);
            if (it != songAudioCache_.end() && it->second && it->second->trackPaths == newPaths) {
                cachedHit = it->second;
            }
        }

        if (!cachedHit) {
            // Not yet decoded -> prioritize on background thread immediately (non-blocking UI!)
            enqueueSongLoadLocked(song, true);
        }
    }

    if (cachedHit) {
        applyCachedAudio(cachedHit, &song);
        return;
    }

    // Clear previous song's buffers immediately and mark loading while background thread decodes
    {
        std::lock_guard<std::mutex> audioLock(audioMutex_);
        loadedTracks_.clear();
        maxSongLengthFrames_ = 0;
        metronome_.setTempo(song.tempo);
        metronome_.setTimeSignature(song.timeSignature);
        metronome_.setSections(song.sections);
        metronome_.setClickSubdivisions(song.clickEighths, song.clickSixteenths);
        metronome_.setEighthClickGain(song.clickEighthsGain);
        metronome_.setSixteenthClickGain(song.clickSixteenthsGain);
        metronome_.setUseSectionCountIn(useTracks_.load(std::memory_order_relaxed));
        metronome_.reset();
        hasTracks_.store(true, std::memory_order_relaxed);
        hasLeadingCountSection_.store(metronome_.hasLeadingCountSection(), std::memory_order_relaxed);
        int totalSecBars = 0;
        for (const auto& sec : song.sections) {
            totalSecBars += std::max(1, sec.lengthBars);
        }
        sectionTotalBars_.store(totalSecBars, std::memory_order_relaxed);
        maxSongLengthFramesAtomic_.store(0, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> wfLock(waveformMutex_);
        waveformPeaks_.clear();
    }
    isLoadingTracks_.store(true, std::memory_order_relaxed);
    waveformVersion_.fetch_add(1, std::memory_order_relaxed);
    loaderCv_.notify_one();
}

void AudioEngine::rebuildWaveformPeaksLocked() {
    auto peaks = computeWaveformPeaks(loadedTracks_, maxSongLengthFrames_);
    {
        std::lock_guard<std::mutex> lock(waveformMutex_);
        waveformPeaks_ = std::move(peaks);
    }
    waveformVersion_.fetch_add(1, std::memory_order_relaxed);
}

void AudioEngine::setUseTracks(bool enabled) {
    useTracks_.store(enabled, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(audioMutex_);
    metronome_.setUseSectionCountIn(enabled && hasTracks_.load(std::memory_order_relaxed));
}

std::vector<float> AudioEngine::getWaveformPeaks() {
    std::lock_guard<std::mutex> lock(waveformMutex_);
    return waveformPeaks_;
}

double AudioEngine::getTrackDurationInBars() const {
    int64_t frames = maxSongLengthFramesAtomic_.load(std::memory_order_relaxed);
    int64_t framesPerBar = metronome_.getFramesPerBar();
    if (frames <= 0 || framesPerBar <= 0) return 0.0;
    return static_cast<double>(frames) / static_cast<double>(framesPerBar);
}

int AudioEngine::getTrackBarCount() const {
    double bars = getTrackDurationInBars();
    if (bars <= 0.0) return 0;
    int audioBars = std::max(1, static_cast<int>(std::ceil(bars)));
    int secBars = sectionTotalBars_.load(std::memory_order_relaxed);
    return std::max(audioBars, secBars);
}

void AudioEngine::play() {
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        metronome_.reset();
        for (auto& t : loadedTracks_) {
            t.position = 0;
        }
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
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        metronome_.reset();
        for (auto& t : loadedTracks_) {
            t.position = 0;
        }
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

void AudioEngine::setEighthClickGain(float gain) {
    metronome_.setEighthClickGain(gain);
}

void AudioEngine::setSixteenthClickGain(float gain) {
    metronome_.setSixteenthClickGain(gain);
}

void AudioEngine::setClickSubdivisions(bool enableEighths, bool enableSixteenths) {
    std::lock_guard<std::mutex> lock(audioMutex_);
    metronome_.setClickSubdivisions(enableEighths, enableSixteenths);
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
    std::vector<LoadedTrack> tracksSnapshot;
    int64_t maxFrames = 0;
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        if (trackIndex >= 0 && trackIndex < static_cast<int>(loadedTracks_.size())) {
            loadedTracks_[trackIndex].track.muted = muted;
            tracksSnapshot = loadedTracks_;
            maxFrames = maxSongLengthFrames_;
        } else {
            return;
        }
    }

    auto peaks = computeWaveformPeaks(tracksSnapshot, maxFrames);
    {
        std::lock_guard<std::mutex> wfLock(waveformMutex_);
        waveformPeaks_ = peaks;
    }
    waveformVersion_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> cacheLock(cacheMutex_);
    if (!activeCacheKey_.empty()) {
        auto it = songAudioCache_.find(activeCacheKey_);
        if (it != songAudioCache_.end() && it->second &&
            trackIndex < static_cast<int>(it->second->tracks.size())) {
            it->second->tracks[trackIndex].track.muted = muted;
            it->second->waveformPeaks = std::move(peaks);
        }
    }
}

void AudioEngine::setTrackGain(int trackIndex, float gain) {
    std::vector<LoadedTrack> tracksSnapshot;
    int64_t maxFrames = 0;
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        if (trackIndex >= 0 && trackIndex < static_cast<int>(loadedTracks_.size())) {
            loadedTracks_[trackIndex].track.gain = gain;
            tracksSnapshot = loadedTracks_;
            maxFrames = maxSongLengthFrames_;
        } else {
            return;
        }
    }

    auto peaks = computeWaveformPeaks(tracksSnapshot, maxFrames);
    {
        std::lock_guard<std::mutex> wfLock(waveformMutex_);
        waveformPeaks_ = peaks;
    }
    waveformVersion_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> cacheLock(cacheMutex_);
    if (!activeCacheKey_.empty()) {
        auto it = songAudioCache_.find(activeCacheKey_);
        if (it != songAudioCache_.end() && it->second &&
            trackIndex < static_cast<int>(it->second->tracks.size())) {
            it->second->tracks[trackIndex].track.gain = gain;
            it->second->waveformPeaks = std::move(peaks);
        }
    }
}

void AudioEngine::setTrackOutputChannel(int trackIndex, int channel, bool stereoOutput) {
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        if (trackIndex >= 0 && trackIndex < static_cast<int>(loadedTracks_.size())) {
            loadedTracks_[trackIndex].track.outputChannel = channel;
            loadedTracks_[trackIndex].track.stereoOutput = stereoOutput;
        } else {
            return;
        }
    }

    std::lock_guard<std::mutex> cacheLock(cacheMutex_);
    if (!activeCacheKey_.empty()) {
        auto it = songAudioCache_.find(activeCacheKey_);
        if (it != songAudioCache_.end() && it->second &&
            trackIndex < static_cast<int>(it->second->tracks.size())) {
            it->second->tracks[trackIndex].track.outputChannel = channel;
            it->second->tracks[trackIndex].track.stereoOutput = stereoOutput;
        }
    }
}

std::vector<float> AudioEngine::getOutputPeaks() {
    std::lock_guard<std::mutex> lock(peakMutex_);
    return outputPeaks_;
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    int chCount = currentOutputChannels_.load(std::memory_order_relaxed);
    if (device) {
        currentSampleRate_ = device->getCurrentSampleRate();
        int activeCh = device->getActiveOutputChannels().countNumberOfSetBits();
        if (activeCh <= 0) {
            activeCh = device->getOutputChannelNames().size();
        }
        chCount = std::max(1, activeCh);
        currentOutputChannels_.store(chCount, std::memory_order_relaxed);
    }
    metronome_.prepare(currentSampleRate_);
    renderScratchBuffer_.setSize(chCount, 2048);

    std::lock_guard<std::mutex> lock(peakMutex_);
    outputPeaks_.assign(static_cast<size_t>(chCount), 0.0f);
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

    // 2. Mix backing tracks (only when "Use tracks" is enabled).
    // If the song's first section is called "count", the audio files start at frame 0
    // so that section plays during the count; otherwise audio starts after the 2-bar count-in.
    const bool playTracks = useTracks_.load(std::memory_order_relaxed) && hasTracks_.load(std::memory_order_relaxed);
    const int64_t blockStartFrame = metronome_.getMetronomeFrame();
    const int64_t blockEndFrame = blockStartFrame + numSamples;
    const int64_t audioStartFrame = (playTracks && hasLeadingCountSection_.load(std::memory_order_relaxed))
                                        ? 0
                                        : metronome_.getTotalCountInFrames();

    if (playTracks && blockEndFrame > audioStartFrame) {
        std::unique_lock<std::mutex> lock(audioMutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            const int outOffset = static_cast<int>(std::max<int64_t>(0, audioStartFrame - blockStartFrame));
            const int samplesForTrack = numSamples - outOffset;
            const int64_t trackStartPos = std::max<int64_t>(0, blockStartFrame - audioStartFrame);

            for (auto& lt : loadedTracks_) {
                if (lt.track.muted || !lt.buffer || lt.buffer->getNumSamples() == 0)
                    continue;

                const int64_t trackSamples = static_cast<int64_t>(lt.buffer->getNumSamples());
                if (trackStartPos >= trackSamples)
                    continue;

                const int available = static_cast<int>(std::min<int64_t>(samplesForTrack, trackSamples - trackStartPos));
                if (available <= 0)
                    continue;

                const int channelA = std::clamp(lt.track.outputChannel, 0, std::max(0, numOutputChannels - 1));
                const int trackChannels = lt.buffer->getNumChannels();
                const float gain = lt.track.gain;

                if (trackChannels >= 2) {
                    const bool routeAsStereo = lt.track.stereoOutput && (channelA + 1 < numOutputChannels);
                    if (routeAsStereo) {
                        // Explicit stereo pair output: [channelA (L), channelA + 1 (R)]
                        renderScratchBuffer_.addFrom(channelA, outOffset, *lt.buffer, 0, static_cast<int>(trackStartPos), available, gain);
                        renderScratchBuffer_.addFrom(channelA + 1, outOffset, *lt.buffer, 1, static_cast<int>(trackStartPos), available, gain);
                    } else if (channelA < numOutputChannels) {
                        // Single-channel mono sum with adaptive equal-power RMS compensation
                        // and anti-phase cancellation protection (eliminates -3 dB wide-stereo drop
                        // and comb-filtering hollowing on stereo FX/guitars/pads).
                        const float* srcL = lt.buffer->getReadPointer(0) + static_cast<int>(trackStartPos);
                        const float* srcR = lt.buffer->getReadPointer(1) + static_cast<int>(trackStartPos);
                        float* dst = renderScratchBuffer_.getWritePointer(channelA) + outOffset;

                        double sumEnergy = 0.0;
                        double diffEnergy = 0.0;
                        for (int i = 0; i < available; ++i) {
                            const float l = srcL[i];
                            const float r = srcR[i];
                            const float s = l + r;
                            const float d = l - r;
                            sumEnergy += static_cast<double>(s) * static_cast<double>(s);
                            diffEnergy += static_cast<double>(d) * static_cast<double>(d);
                        }

                        const double totalEnergy = sumEnergy + diffEnergy;
                        if (totalEnergy > 1e-10) {
                            const float blockCorr = static_cast<float>((sumEnergy - diffEnergy) / totalEnergy);
                            lt.smoothedCorrelation = lt.smoothedCorrelation * 0.85f + blockCorr * 0.15f;
                        }

                        const float posCorr = std::clamp(lt.smoothedCorrelation, 0.0f, 1.0f);
                        const float rmsSumGain = 1.0f / std::sqrt(2.0f * (1.0f + posCorr));
                        const float antiPhaseBlend = std::clamp(-lt.smoothedCorrelation, 0.0f, 1.0f);

                        const float targetGainL = (1.0f - antiPhaseBlend) * rmsSumGain + antiPhaseBlend;
                        const float targetGainR = (1.0f - antiPhaseBlend) * rmsSumGain;

                        const float startGainL = lt.monoGainL;
                        const float startGainR = lt.monoGainR;
                        const float invN = (available > 1) ? (1.0f / static_cast<float>(available - 1)) : 0.0f;

                        for (int i = 0; i < available; ++i) {
                            const float t = static_cast<float>(i) * invN;
                            const float gL = (startGainL + (targetGainL - startGainL) * t) * gain;
                            const float gR = (startGainR + (targetGainR - startGainR) * t) * gain;
                            dst[i] += srcL[i] * gL + srcR[i] * gR;
                        }

                        lt.monoGainL = targetGainL;
                        lt.monoGainR = targetGainR;
                    }
                } else {
                    // Mono track: route directly to channelA
                    if (channelA < numOutputChannels) {
                        renderScratchBuffer_.addFrom(channelA, outOffset, *lt.buffer, 0, static_cast<int>(trackStartPos), available, gain);
                    }
                }

                lt.position = trackStartPos + available;
            }
        }
    }

    // 3. Render metronome and count-in
    metronome_.renderBlock(renderScratchBuffer_, 0, numSamples);

    // If playing with tracks enabled, stop cleanly once all bars needed for the audio files/sections finish
    if (playTracks) {
        const int64_t framesPerBar = metronome_.getFramesPerBar();
        const int64_t maxFrames = maxSongLengthFramesAtomic_.load(std::memory_order_relaxed);
        if (framesPerBar > 0 && maxFrames > 0) {
            const int64_t audioBars = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(static_cast<double>(maxFrames) / static_cast<double>(framesPerBar))));
            const int64_t secBars = static_cast<int64_t>(sectionTotalBars_.load(std::memory_order_relaxed));
            const int64_t trackBars = std::max(audioBars, secBars);
            const int64_t totalPlaybackFrames = audioStartFrame + trackBars * framesPerBar;
            if (metronome_.getMetronomeFrame() >= totalPlaybackFrames) {
                isPlaying_.store(false);
                isPaused_.store(false);
                metronome_.reset();
            }
        }
    }

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
