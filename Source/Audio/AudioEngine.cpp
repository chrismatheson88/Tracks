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

    activePeakChannels_.store(std::min(chCount, kMaxMeterChannels), std::memory_order_relaxed);
    for (auto& p : outputPeaksAtomic_) p.store(0.0f, std::memory_order_relaxed);
    metronome_.prepare(currentSampleRate_);
    pitchInScratch_.setSize(2, 8192);
    pitchOutScratch_.setSize(2, 4096);
    pitchPrimeInScratch_.setSize(2, 32768);
    pitchPrimeOutScratch_.setSize(2, 32768);

    loadEmbeddedAssets();
    loaderThread_ = std::thread([this]() { loaderLoop(); });
    streamerThread_ = std::thread([this]() { streamerLoop(); });
}

AudioEngine::~AudioEngine() {
    cancelPendingUpdate();
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        shuttingDown_ = true;
        preloadQueue_.clear();
    }
    loaderCv_.notify_all();
    {
        std::lock_guard<std::mutex> sLock(streamControlMutex_);
        streamerShuttingDown_ = true;
        activeStreamTracks_.clear();
    }
    streamerCv_.notify_all();
    if (loaderThread_.joinable()) {
        loaderThread_.join();
    }
    if (streamerThread_.joinable()) {
        streamerThread_.join();
    }
    deviceManager_.removeAudioCallback(this);
}

void AudioEngine::setTransitionConfig(SongTransitionMode mode, int crossfadeBars, bool hasNextSong) {
    transitionModeAtomic_.store(static_cast<int>(mode), std::memory_order_relaxed);
    transitionCrossfadeBarsAtomic_.store(clampCrossfadeBars(crossfadeBars), std::memory_order_relaxed);
    hasNextSongAtomic_.store(hasNextSong, std::memory_order_relaxed);
}

void AudioEngine::handleAsyncUpdate() {
    const int raw = pendingTransitionTrigger_.exchange(0, std::memory_order_acq_rel);
    if (raw != 0 && onSongTransitionTriggered) {
        onSongTransitionTriggered(static_cast<TransitionTrigger>(raw));
    }
}

void AudioEngine::clearCrossfadeOutLocked() {
    crossfadeOut_.active = false;
    crossfadeOut_.tracks.clear();
    crossfadeOut_.stretchBuses.clear();
    crossfadeOut_.maxSongLengthFrames = 0;
    crossfadeOut_.outTrackPos = 0;
    crossfadeOut_.elapsedSamples = 0;
    crossfadeOut_.totalFadeSamples = 0;
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
    constexpr int kNumBins = StreamingAudioTrack::kWaveformBins;
    std::vector<float> peaks;

    if (!tracks.empty() && maxLengthFrames > 0) {
        peaks.assign(kNumBins, 0.0f);
        float globalMax = 0.0f;

        for (int b = 0; b < kNumBins; ++b) {
            const double binStartFrac = static_cast<double>(b) / static_cast<double>(kNumBins);
            const double binEndFrac = static_cast<double>(b + 1) / static_cast<double>(kNumBins);
            float sum = 0.0f;
            for (const auto& lt : tracks) {
                if (lt.track.muted || !lt.stream) continue;
                const int64_t trackLen = lt.stream->getTotalSamples();
                if (trackLen <= 0) continue;

                const double startF = binStartFrac * static_cast<double>(maxLengthFrames);
                if (startF >= static_cast<double>(trackLen)) continue;

                const double endF = std::min(static_cast<double>(trackLen), binEndFrac * static_cast<double>(maxLengthFrames));
                const int tb0 = std::clamp(static_cast<int>(std::floor((startF / static_cast<double>(trackLen)) * kNumBins)), 0, kNumBins - 1);
                const int tb1 = std::clamp(static_cast<int>(std::floor(((endF - 1.0) / static_cast<double>(trackLen)) * kNumBins)), tb0, kNumBins - 1);
                float trackPeak = 0.0f;
                for (int tb = tb0; tb <= tb1; ++tb) {
                    trackPeak = std::max(trackPeak, lt.stream->getWaveformBin(tb));
                }
                sum += trackPeak * lt.track.gain;
            }
            peaks[static_cast<size_t>(b)] = sum;
            globalMax = std::max(globalMax, sum);
        }

        if (globalMax > 1e-5f) {
            const float invScale = 1.0f / std::max(0.05f, globalMax);
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

    // Index stems & decode only Chunk 0 (~256 KB/stem) + 512-bin waveform peaks in parallel.
    // Remaining chunks stream on demand from disk via streamerThread_, keeping RAM minimal.
    std::vector<SongTrack> validTracks;
    for (const auto& t : song.tracks) {
        if (!t.filePath.empty()) {
            validTracks.push_back(t);
        }
    }

    const size_t maxConcurrent = std::max<size_t>(1, std::min<size_t>(4, std::thread::hardware_concurrency()));
    std::vector<std::shared_ptr<StreamingAudioTrack>> streams(validTracks.size());

    for (size_t offset = 0; offset < validTracks.size(); offset += maxConcurrent) {
        size_t batchEnd = std::min(validTracks.size(), offset + maxConcurrent);
        std::vector<std::future<std::shared_ptr<StreamingAudioTrack>>> futures;
        futures.reserve(batchEnd - offset);

        for (size_t i = offset; i < batchEnd; ++i) {
            std::string path = validTracks[i].filePath;
            futures.push_back(std::async(std::launch::async, [path]() {
                return StreamingAudioTrack::createFromFile(path);
            }));
        }

        for (size_t i = offset; i < batchEnd; ++i) {
            streams[i] = futures[i - offset].get();
        }
    }

    for (size_t i = 0; i < validTracks.size(); ++i) {
        if (streams[i] && streams[i]->getTotalSamples() > 0) {
            LoadedTrack lt;
            lt.track = validTracks[i];
            lt.stream = std::move(streams[i]);
            lt.position = 0;
            cached->maxLengthFrames = std::max(cached->maxLengthFrames, lt.stream->getTotalSamples());
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
                lt.stream = srcTrack.stream;
                lt.position = 0;
                if (lt.stream) {
                    lt.stream->setPlayheadFrame(0);
                    lt.stream->clearPrefetch();
                }
                loadedTracks_.push_back(std::move(lt));
            }
            maxSongLengthFrames_ = cached->maxLengthFrames;
            preparePitchShiftersLocked();
            for (auto& bus : stretchBuses_) {
                bus.primed = false;
                bus.inputPos = 0;
            }
        } else {
            stretchBuses_.clear();
        }

        if (overrideSettings) {
            const auto* activeArr = overrideSettings->findArrangement(overrideSettings->activeArrangementId);
            int origTempo = (activeArr && activeArr->tempo > 0) ? activeArr->tempo : overrideSettings->tempo;
            const int clampedOrigTempo = std::clamp(origTempo > 0 ? origTempo : 120, 20, 300);
            originalTrackTempo_.store(clampedOrigTempo, std::memory_order_relaxed);
            activeTempoPoints_ = (activeArr && !activeArr->tempoPoints.empty())
                                     ? activeArr->tempoPoints
                                     : overrideSettings->tempoPoints;
            const bool hasTracksNow = !loadedTracks_.empty();
            const bool hasSections = !overrideSettings->sections.empty();
            const bool applySections = hasTracksNow ? useTracks_.load(std::memory_order_relaxed) : hasSections;
            const bool applyTempoMap = hasTracksNow ? useTracks_.load(std::memory_order_relaxed) : true;
            metronome_.setTempo(overrideSettings->tempo);
            metronome_.setTimeSignature(overrideSettings->timeSignature);
            metronome_.setSections(overrideSettings->sections);
            metronome_.setClickSubdivisions(overrideSettings->clickEighths, overrideSettings->clickSixteenths);
            metronome_.setEighthClickGain(overrideSettings->clickEighthsGain);
            metronome_.setSixteenthClickGain(overrideSettings->clickSixteenthsGain);
            metronome_.setUseSectionCountIn(applySections);
            metronome_.setTempoMap(clampedOrigTempo, applyTempoMap ? activeTempoPoints_ : std::vector<TempoPoint>{});
            metronome_.reset();
            hasLeadingCountSection_.store(metronome_.hasLeadingCountSection(), std::memory_order_relaxed);
            rebuildSectionSpansLocked(overrideSettings->sections);
            int totalSecBars = 0;
            for (const auto& sec : overrideSettings->sections) {
                totalSecBars += std::max(1, sec.lengthBars);
            }
            sectionTotalBars_.store(totalSecBars, std::memory_order_relaxed);
        }

        hasTracks_.store(!loadedTracks_.empty(), std::memory_order_relaxed);
        maxSongLengthFramesAtomic_.store(maxSongLengthFrames_, std::memory_order_relaxed);
        if (!overrideSettings) {
            const bool applySections = isStructuredTimelineMode();
            const bool applyTempoMap = shouldApplyTempoMap();
            metronome_.setUseSectionCountIn(applySections);
            metronome_.setTempoMap(originalTrackTempo_.load(std::memory_order_relaxed),
                                   applyTempoMap ? activeTempoPoints_ : std::vector<TempoPoint>{});
        }
        updateTimelineMetricsLocked();
        syncActiveStreamsLocked(isPlaying_.load(std::memory_order_relaxed));
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

void AudioEngine::syncActiveStreamsLocked(bool notifyNow) {
    std::vector<std::shared_ptr<StreamingAudioTrack>> active;
    active.reserve(loadedTracks_.size() + crossfadeOut_.tracks.size());
    for (const auto& lt : loadedTracks_) {
        if (lt.stream) active.push_back(lt.stream);
    }
    if (crossfadeOut_.active) {
        for (const auto& lt : crossfadeOut_.tracks) {
            if (lt.stream) active.push_back(lt.stream);
        }
    }
    {
        std::lock_guard<std::mutex> sLock(streamControlMutex_);
        activeStreamTracks_ = std::move(active);
        if (notifyNow) {
            ++streamerWakeSeq_;
        }
    }
    if (notifyNow) {
        streamerCv_.notify_one();
    }
}

void AudioEngine::notifyStreamer(bool closeReaders) {
    {
        std::lock_guard<std::mutex> sLock(streamControlMutex_);
        ++streamerWakeSeq_;
        if (closeReaders) {
            streamerCloseReadersRequested_ = true;
        }
    }
    streamerCv_.notify_one();
}

void AudioEngine::waitForStreamerFlush() {
    uint64_t target = 0;
    {
        std::lock_guard<std::mutex> sLock(streamControlMutex_);
        streamerCloseReadersRequested_ = true;
        target = ++streamerSyncRequest_;
    }
    streamerCv_.notify_one();
    std::unique_lock<std::mutex> sLock(streamControlMutex_);
    streamerSyncCv_.wait(sLock, [&]() {
        return streamerShuttingDown_ || streamerSyncCompleted_ >= target;
    });
}

void AudioEngine::updateSectionPrefetchLocked(int targetSectionIndex) {
    if (targetSectionIndex < 0 || targetSectionIndex >= static_cast<int>(sectionSpans_.size()) || loadedTracks_.empty()) {
        for (auto& lt : loadedTracks_) {
            if (lt.stream) lt.stream->clearPrefetch();
        }
        return;
    }

    const auto& targetSpan = sectionSpans_[static_cast<size_t>(targetSectionIndex)];
    const bool structuredTimeline = isStructuredTimelineMode();
    const int64_t audioStartFrame = (structuredTimeline && hasLeadingCountSection_.load(std::memory_order_relaxed))
                                        ? 0
                                        : metronome_.getTotalCountInFrames();
    const int64_t targetTimelineFrame = metronome_.getFrameForBar(static_cast<double>(targetSpan.startBar));
    const int64_t targetOutTrackFrame = std::max<int64_t>(0, targetTimelineFrame - audioStartFrame);

    const int origTempo = originalTrackTempo_.load(std::memory_order_relaxed);
    const int playTempo = metronome_.getPlaybackTempo();
    const double rawRate = (origTempo > 0 && playTempo > 0)
                               ? (static_cast<double>(playTempo) / static_cast<double>(origTempo))
                               : 1.0;
    const double exactRate = std::clamp(rawRate, kEngineMinSpeedRatio, kEngineMaxSpeedRatio);
    const int64_t targetSrcFrame = static_cast<int64_t>(std::llround(static_cast<double>(targetOutTrackFrame) * exactRate));

    for (auto& lt : loadedTracks_) {
        if (lt.stream) {
            lt.stream->setPrefetchFrame(targetSrcFrame);
        }
    }
    notifyStreamer(false);
}

void AudioEngine::streamerLoop() {
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unordered_map<uint64_t, std::unique_ptr<juce::AudioFormatReader>> openReaders;
    juce::AudioBuffer<float> readScratch(2, StreamingAudioTrack::kChunkSamples);

    while (true) {
        std::vector<std::shared_ptr<StreamingAudioTrack>> activeList;
        bool closeReaders = false;
        uint64_t syncTarget = 0;
        uint64_t seenWakeSeq = 0;

        {
            std::unique_lock<std::mutex> lock(streamControlMutex_);
            if (streamerShuttingDown_) break;
            activeList = activeStreamTracks_;
            closeReaders = streamerCloseReadersRequested_;
            streamerCloseReadersRequested_ = false;
            syncTarget = streamerSyncRequest_;
            seenWakeSeq = streamerWakeSeq_;
        }

        if (closeReaders) {
            openReaders.clear();
        } else if (!openReaders.empty()) {
            // Prune open readers for tracks no longer in the active set
            std::unordered_set<uint64_t> activeUids;
            activeUids.reserve(activeList.size());
            for (const auto& t : activeList) {
                if (t) activeUids.insert(t->getUid());
            }
            for (auto it = openReaders.begin(); it != openReaders.end(); ) {
                if (activeUids.find(it->first) == activeUids.end()) {
                    it = openReaders.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (syncTarget > 0) {
            std::lock_guard<std::mutex> lock(streamControlMutex_);
            if (streamerSyncCompleted_ < syncTarget) {
                streamerSyncCompleted_ = syncTarget;
                streamerSyncCv_.notify_all();
            }
        }

        const bool playing = isPlaying_.load(std::memory_order_relaxed);
        bool didWork = false;

        for (const auto& track : activeList) {
            if (!track) continue;
            const bool hasPrefetch = (track->getPrefetchChunk() >= 0);
            const bool needsSvc = track->consumeNeedsService();
            if (!playing && !hasPrefetch && !needsSvc) {
                continue;
            }

            const int64_t nextChunk = track->findNextChunkToLoadAndEvict(playing);
            if (nextChunk <= 0) continue;

            auto rIt = openReaders.find(track->getUid());
            if (rIt == openReaders.end() || !rIt->second) {
                juce::File file(track->getFilePath());
                if (!file.existsAsFile()) continue;
                std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
                if (!reader) continue;
                rIt = openReaders.emplace(track->getUid(), std::move(reader)).first;
            }

            if (track->loadChunkFromReader(nextChunk, *rIt->second, readScratch)) {
                didWork = true;
            }
        }

        if (!didWork) {
            // If stopped and no prefetch is pending, close any idle file handles right away
            if (!playing && !openReaders.empty()) {
                bool anyPrefetch = false;
                for (const auto& t : activeList) {
                    if (t && t->getPrefetchChunk() >= 0) { anyPrefetch = true; break; }
                }
                if (!anyPrefetch) {
                    openReaders.clear();
                }
            }

            std::unique_lock<std::mutex> lock(streamControlMutex_);
            if (streamerShuttingDown_) break;
            if (streamerWakeSeq_ != seenWakeSeq || streamerCloseReadersRequested_ ||
                streamerSyncRequest_ > streamerSyncCompleted_) {
                continue;
            }
            if (isPlaying_.load(std::memory_order_relaxed) && !activeStreamTracks_.empty()) {
                streamerCv_.wait_for(lock, std::chrono::milliseconds(20), [&]() {
                    return streamerShuttingDown_ || streamerWakeSeq_ != seenWakeSeq ||
                           streamerCloseReadersRequested_ ||
                           streamerSyncRequest_ > streamerSyncCompleted_;
                });
            } else {
                streamerCv_.wait(lock, [&]() {
                    return streamerShuttingDown_ || streamerWakeSeq_ != seenWakeSeq ||
                           streamerCloseReadersRequested_ ||
                           streamerSyncRequest_ > streamerSyncCompleted_ ||
                           (isPlaying_.load(std::memory_order_relaxed) && !activeStreamTracks_.empty());
                });
            }
        }
    }

    openReaders.clear();
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

void AudioEngine::clearAllCachedAudio() {
    stop();
    {
        std::lock_guard<std::mutex> cacheLock(cacheMutex_);
        preloadGeneration_++;
        preloadQueue_.clear();
        songAudioCache_.clear();
        cachedSetlistId_.reset();
        activeCacheKey_.clear();
        activeTrackPaths_.clear();
    }
    {
        std::lock_guard<std::mutex> audioLock(audioMutex_);
        loadedTracks_.clear();
        stretchBuses_.clear();
        sectionSpans_.clear();
        activeTempoPoints_.clear();
        metronome_.clearTempoMap();
        clearQueuedSectionLocked();
        clearSelectedStartSectionLocked();
        clearCrossfadeOutLocked();
        awaitingTransitionSwap_ = false;
        pendingTransitionTrigger_.store(0, std::memory_order_relaxed);
        clickLoopStopBar_ = -1;
        clickLoopEnabled_.store(true, std::memory_order_relaxed);
        trackMuteUntilFrame_ = 0;
        isSectionStartCountInAtomic_.store(false, std::memory_order_relaxed);
        maxSongLengthFrames_ = 0;
        hasTracks_.store(false, std::memory_order_relaxed);
        hasLeadingCountSection_.store(false, std::memory_order_relaxed);
        hasTempoAutomation_.store(false, std::memory_order_relaxed);
        cachedTrackDurationBars_.store(0.0, std::memory_order_relaxed);
        sectionCountAtomic_.store(0, std::memory_order_relaxed);
        sectionTotalBars_.store(0, std::memory_order_relaxed);
        maxSongLengthFramesAtomic_.store(0, std::memory_order_relaxed);
        syncActiveStreamsLocked(false);
    }
    waitForStreamerFlush();
    {
        std::lock_guard<std::mutex> wfLock(waveformMutex_);
        waveformPeaks_.clear();
        timelineLayout_ = TimelineLayoutSnapshot{};
    }
    isLoadingTracks_.store(false, std::memory_order_relaxed);
    waveformVersion_.fetch_add(1, std::memory_order_relaxed);
}

void AudioEngine::loadSong(const Song& song) {
    const bool keepCrossfade = preserveCrossfadeOnNextLoad_.exchange(false, std::memory_order_acq_rel);
    if (!keepCrossfade) {
        std::lock_guard<std::mutex> audioLock(audioMutex_);
        clearCrossfadeOutLocked();
        awaitingTransitionSwap_ = false;
    }
    syncSectionCuesForSections(song.sections);
    auto newPaths = extractTrackPaths(song);
    std::string cacheKey = makeCacheKey(song);

    // Fast path 1: Same song/arrangement and identical track files -> only update tempo/time signature/sections!
    {
        std::lock_guard<std::mutex> cacheLock(cacheMutex_);
        if (!cacheKey.empty() && !activeCacheKey_.empty() &&
            cacheKey == activeCacheKey_ && newPaths == activeTrackPaths_) {
            std::lock_guard<std::mutex> audioLock(audioMutex_);
            const auto* activeArr = song.findArrangement(song.activeArrangementId);
            int origTempo = (activeArr && activeArr->tempo > 0) ? activeArr->tempo : song.tempo;
            const int clampedOrigTempo = std::clamp(origTempo > 0 ? origTempo : 120, 20, 300);
            originalTrackTempo_.store(clampedOrigTempo, std::memory_order_relaxed);
            activeTempoPoints_ = (activeArr && !activeArr->tempoPoints.empty())
                                     ? activeArr->tempoPoints
                                     : song.tempoPoints;
            const bool hasTracksNow = hasTracks_.load(std::memory_order_relaxed);
            const bool hasSections = !song.sections.empty();
            const bool applySections = hasTracksNow ? useTracks_.load(std::memory_order_relaxed) : hasSections;
            const bool applyTempoMap = hasTracksNow ? useTracks_.load(std::memory_order_relaxed) : true;
            metronome_.setTempo(song.tempo);
            metronome_.setTimeSignature(song.timeSignature);
            metronome_.setSections(song.sections);
            metronome_.setClickSubdivisions(song.clickEighths, song.clickSixteenths);
            metronome_.setEighthClickGain(song.clickEighthsGain);
            metronome_.setSixteenthClickGain(song.clickSixteenthsGain);
            metronome_.setUseSectionCountIn(applySections);
            metronome_.setTempoMap(clampedOrigTempo, applyTempoMap ? activeTempoPoints_ : std::vector<TempoPoint>{});
            hasLeadingCountSection_.store(metronome_.hasLeadingCountSection(), std::memory_order_relaxed);
            rebuildSectionSpansLocked(song.sections);
            int totalSecBars = 0;
            for (const auto& sec : song.sections) {
                totalSecBars += std::max(1, sec.lengthBars);
            }
            sectionTotalBars_.store(totalSecBars, std::memory_order_relaxed);
            updateTimelineMetricsLocked();
            waveformVersion_.fetch_add(1, std::memory_order_relaxed);
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
        const auto* activeArr = song.findArrangement(song.activeArrangementId);
        int origTempo = (activeArr && activeArr->tempo > 0) ? activeArr->tempo : song.tempo;
        const int clampedOrigTempo = std::clamp(origTempo > 0 ? origTempo : 120, 20, 300);
        originalTrackTempo_.store(clampedOrigTempo, std::memory_order_relaxed);
        activeTempoPoints_ = (activeArr && !activeArr->tempoPoints.empty())
                                 ? activeArr->tempoPoints
                                 : song.tempoPoints;
        const bool applyTrackMap = useTracks_.load(std::memory_order_relaxed);
        metronome_.setTempo(song.tempo);
        metronome_.setTimeSignature(song.timeSignature);
        metronome_.setSections(song.sections);
        metronome_.setClickSubdivisions(song.clickEighths, song.clickSixteenths);
        metronome_.setEighthClickGain(song.clickEighthsGain);
        metronome_.setSixteenthClickGain(song.clickSixteenthsGain);
        metronome_.setUseSectionCountIn(applyTrackMap);
        metronome_.setTempoMap(clampedOrigTempo, applyTrackMap ? activeTempoPoints_ : std::vector<TempoPoint>{});
        metronome_.reset();
        hasTracks_.store(true, std::memory_order_relaxed);
        hasLeadingCountSection_.store(metronome_.hasLeadingCountSection(), std::memory_order_relaxed);
        rebuildSectionSpansLocked(song.sections);
        int totalSecBars = 0;
        for (const auto& sec : song.sections) {
            totalSecBars += std::max(1, sec.lengthBars);
        }
        sectionTotalBars_.store(totalSecBars, std::memory_order_relaxed);
        maxSongLengthFramesAtomic_.store(0, std::memory_order_relaxed);
        updateTimelineMetricsLocked();
        syncActiveStreamsLocked(false);
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
    const bool applySections = isStructuredTimelineMode();
    const bool applyTempoMap = shouldApplyTempoMap();
    metronome_.setUseSectionCountIn(applySections);
    metronome_.setTempoMap(originalTrackTempo_.load(std::memory_order_relaxed),
                           applyTempoMap ? activeTempoPoints_ : std::vector<TempoPoint>{});
    updateTimelineMetricsLocked();
    waveformVersion_.fetch_add(1, std::memory_order_relaxed);
    if (!applySections) {
        clearQueuedSectionLocked();
        clearSelectedStartSectionLocked();
        clickLoopStopBar_ = -1;
        clickLoopEnabled_.store(true, std::memory_order_relaxed);
        trackMuteUntilFrame_ = 0;
        isSectionStartCountInAtomic_.store(false, std::memory_order_relaxed);
    }
}

void AudioEngine::rebuildSectionSpansLocked(const std::vector<SongSection>& sections) {
    sectionSpans_ = buildSectionSpans(sections, metronome_.hasLeadingCountSection());
    sectionCountAtomic_.store(static_cast<int>(sectionSpans_.size()), std::memory_order_relaxed);
    clearQueuedSectionLocked();
    clearSelectedStartSectionLocked();
}

void AudioEngine::clearQueuedSectionLocked() {
    queuedSectionIndex_ = -1;
    queuedJumpBoundaryBar_ = -1;
    queuedSectionIndexAtomic_.store(-1, std::memory_order_relaxed);
    metronome_.clearQueuedAnnouncementOverride();
    if (isPlaying_.load(std::memory_order_relaxed)) {
        for (auto& lt : loadedTracks_) {
            if (lt.stream) lt.stream->clearPrefetch();
        }
    }
}

void AudioEngine::clearSelectedStartSectionLocked() {
    selectedStartSectionIndex_ = -1;
    selectedStartSectionIndexAtomic_.store(-1, std::memory_order_relaxed);
    if (!isPlaying_.load(std::memory_order_relaxed)) {
        for (auto& lt : loadedTracks_) {
            if (lt.stream) lt.stream->clearPrefetch();
        }
    }
}

void AudioEngine::clearQueuedSection() {
    std::lock_guard<std::mutex> lock(audioMutex_);
    clearQueuedSectionLocked();
}

void AudioEngine::skipSection(int delta) {
    if (delta == 0) return;

    std::lock_guard<std::mutex> lock(audioMutex_);
    const int n = static_cast<int>(sectionSpans_.size());
    if (n == 0 || !isStructuredTimelineMode()) {
        return;
    }

    // Before playback starts: step the selected starting section
    if (!isPlaying_.load()) {
        const int nextStart = computeNextStartSection(sectionSpans_, selectedStartSectionIndex_, delta);
        selectedStartSectionIndex_ = nextStart;
        selectedStartSectionIndexAtomic_.store(nextStart, std::memory_order_relaxed);
        updateSectionPrefetchLocked(nextStart);
        return;
    }

    const int currentBar = static_cast<int>(std::floor(std::max(0.0, metronome_.getBarForFrame(metronome_.getMetronomeFrame()))));
    const SectionContext ctx = computeSectionContext(sectionSpans_, currentBar);
    const int nextQueued = computeNextQueuedSection(n, ctx, queuedSectionIndex_, delta);

    if (nextQueued < 0) {
        clearQueuedSectionLocked();
        return;
    }

    queuedSectionIndex_ = nextQueued;
    queuedJumpBoundaryBar_ = ctx.boundaryBar;
    queuedSectionIndexAtomic_.store(nextQueued, std::memory_order_relaxed);
    updateSectionPrefetchLocked(nextQueued);

    const auto& targetSpan = sectionSpans_[static_cast<size_t>(nextQueued)];
    const int cueBar = ctx.boundaryBar - 1;
    if (cueBar >= 0) {
        metronome_.setQueuedAnnouncementOverride(cueBar, targetSpan.isCount ? "" : targetSpan.normalizedKey);
    } else {
        metronome_.clearQueuedAnnouncementOverride();
    }
}

void AudioEngine::toggleClickLoop() {
    setClickLoopEnabled(!clickLoopEnabled_.load(std::memory_order_relaxed));
}

void AudioEngine::setClickLoopEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(audioMutex_);
    clickLoopEnabled_.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        clickLoopStopBar_ = -1;
        return;
    }

    if (isPlaying_.load()) {
        const double curBars = metronome_.getBarForFrame(metronome_.getMetronomeFrame());
        const int curBar = static_cast<int>(std::floor(std::max(0.0, curBars)));
        // If deselected during the 2-bar count-in, play the 1 main bar and end at bar 3;
        // if deselected during the looped bar (bar >= 2), stop cleanly at the end of the current bar.
        clickLoopStopBar_ = (curBar < 2) ? 3 : (curBar + 1);
    } else {
        clickLoopStopBar_ = 3;
    }
}

void AudioEngine::toggleRepeatCurrentSection() {
    std::lock_guard<std::mutex> lock(audioMutex_);
    const int n = static_cast<int>(sectionSpans_.size());
    if (n == 0 || !isPlaying_.load() || !isStructuredTimelineMode()) {
        return;
    }

    const int currentBar = static_cast<int>(std::floor(std::max(0.0, metronome_.getBarForFrame(metronome_.getMetronomeFrame()))));
    const SectionContext ctx = computeSectionContext(sectionSpans_, currentBar);
    if (!canRepeatCurrentSection(n, ctx)) {
        return;
    }

    const int nextQueued = computeToggleRepeatCurrentSection(n, ctx, queuedSectionIndex_);
    if (nextQueued < 0) {
        clearQueuedSectionLocked();
        return;
    }

    queuedSectionIndex_ = nextQueued;
    queuedJumpBoundaryBar_ = ctx.boundaryBar;
    queuedSectionIndexAtomic_.store(nextQueued, std::memory_order_relaxed);
    updateSectionPrefetchLocked(nextQueued);

    const auto& targetSpan = sectionSpans_[static_cast<size_t>(nextQueued)];
    const int cueBar = ctx.boundaryBar - 1;
    if (cueBar >= 0) {
        metronome_.setQueuedAnnouncementOverride(cueBar, targetSpan.isCount ? "" : targetSpan.normalizedKey);
    } else {
        metronome_.clearQueuedAnnouncementOverride();
    }
}

std::vector<float> AudioEngine::getWaveformPeaks() {
    std::lock_guard<std::mutex> lock(waveformMutex_);
    return waveformPeaks_;
}

AudioEngine::TimelineLayoutSnapshot AudioEngine::getTimelineLayout() {
    std::lock_guard<std::mutex> lock(waveformMutex_);
    return timelineLayout_;
}

void AudioEngine::updateTimelineMetricsLocked() {
    const bool hasMap = metronome_.hasTempoAutomation();
    hasTempoAutomation_.store(hasMap, std::memory_order_relaxed);

    const int64_t maxFrames = maxSongLengthFrames_;
    double durationBars = 0.0;
    if (maxFrames > 0) {
        if (hasMap) {
            durationBars = metronome_.getAudioDurationBarsForOriginalFrames(maxFrames);
        } else {
            const int origTempo = originalTrackTempo_.load(std::memory_order_relaxed);
            const int64_t fpb = (origTempo > 0) ? metronome_.getFramesPerBarForTempo(origTempo)
                                                : metronome_.getFramesPerBar();
            if (fpb > 0) {
                durationBars = static_cast<double>(maxFrames) / static_cast<double>(fpb);
            }
        }
    }
    cachedTrackDurationBars_.store(std::max(0.0, durationBars), std::memory_order_relaxed);

    TimelineLayoutSnapshot snap;
    const int secBars = sectionTotalBars_.load(std::memory_order_relaxed);
    if (hasMap && (maxFrames > 0 || secBars > 0)) {
        const int audioBars = (maxFrames > 0) ? std::max(1, static_cast<int>(std::ceil(durationBars))) : 0;
        const int trackBars = std::max(audioBars, secBars);
        const double totalBarEndFrames = std::max(static_cast<double>(maxFrames),
                                                  metronome_.getOriginalAudioBarFrameOffset(static_cast<double>(trackBars)));
        if (totalBarEndFrames > 0.0 && trackBars > 0) {
            snap.barPositions.resize(static_cast<size_t>(trackBars + 1), 0.0);
            for (int b = 0; b <= trackBars; ++b) {
                double f = metronome_.getOriginalAudioBarFrameOffset(static_cast<double>(b));
                snap.barPositions[static_cast<size_t>(b)] = std::clamp(f / totalBarEndFrames, 0.0, 1.0);
            }
            snap.waveformDurationFraction = (maxFrames > 0)
                                                ? std::clamp(static_cast<double>(maxFrames) / totalBarEndFrames, 0.0, 1.0)
                                                : 1.0;
        }
    }

    std::lock_guard<std::mutex> wfLock(waveformMutex_);
    timelineLayout_ = std::move(snap);
}

double AudioEngine::getTrackDurationInBars() const {
    return cachedTrackDurationBars_.load(std::memory_order_relaxed);
}

int AudioEngine::getTrackBarCount() const {
    const int secBars = sectionTotalBars_.load(std::memory_order_relaxed);
    double bars = getTrackDurationInBars();
    if (bars <= 0.0) {
        return (!hasTracks_.load(std::memory_order_relaxed) && secBars > 0) ? secBars : 0;
    }
    int audioBars = std::max(1, static_cast<int>(std::ceil(bars)));
    return std::max(audioBars, secBars);
}

void AudioEngine::preparePitchShiftersLocked() {
    const int semitones = pitchSemitones_.load(std::memory_order_relaxed);
    const int origTempo = originalTrackTempo_.load(std::memory_order_relaxed);
    const int playTempo = metronome_.getPlaybackTempo();
    const bool needsTimeStretch = (origTempo > 0 && playTempo > 0 && playTempo != origTempo);

    // Always tag pitch-excluded tracks (cheap string check)
    for (auto& lt : loadedTracks_) {
        lt.pitchShiftExcluded = shouldExcludeFromPitchShift(lt.track.name);
    }

    // Lazy allocation: skip allocating SignalsmithStretch FFT engines when pitch=0 and tempo=original
    if (semitones == 0 && !needsTimeStretch) {
        stretchBuses_.clear();
        return;
    }

    const float sr = static_cast<float>(currentSampleRate_ > 0.0 ? currentSampleRate_ : 44100.0);
    const int numOutCh = std::max(1, currentOutputChannels_.load(std::memory_order_relaxed));

    std::vector<StretchBusKey> neededKeys;
    for (auto& lt : loadedTracks_) {
        if (!lt.stream || lt.stream->getNumChannels() <= 0) continue;

        const int channelA = std::clamp(lt.track.outputChannel, 0, std::max(0, numOutCh - 1));
        const bool routeAsStereo = (lt.stream->getNumChannels() >= 2) && lt.track.stereoOutput && (channelA + 1 < numOutCh);

        StretchBusKey key{ channelA, routeAsStereo, lt.pitchShiftExcluded };
        if (std::find(neededKeys.begin(), neededKeys.end(), key) == neededKeys.end()) {
            neededKeys.push_back(key);
        }
    }

    std::vector<StretchBus> updatedBuses;
    updatedBuses.reserve(neededKeys.size());
    for (const auto& key : neededKeys) {
        auto existingIt = std::find_if(stretchBuses_.begin(), stretchBuses_.end(),
                                       [&key](const StretchBus& b) { return b.key == key; });
        if (existingIt != stretchBuses_.end() && existingIt->stretcher) {
            existingIt->stretcher->setTransposeSemitones(key.pitchShiftExcluded ? 0.0f : static_cast<float>(semitones));
            updatedBuses.push_back(std::move(*existingIt));
        } else {
            StretchBus bus;
            bus.key = key;
            bus.stretcher = std::make_shared<signalsmith::stretch::SignalsmithStretch<float>>();
            const int busCh = key.routeAsStereo ? 2 : 1;
            bus.stretcher->presetDefault(busCh, sr, true /* splitComputation */);
            bus.stretcher->setTransposeSemitones(key.pitchShiftExcluded ? 0.0f : static_cast<float>(semitones));
            updatedBuses.push_back(std::move(bus));
        }
    }
    stretchBuses_ = std::move(updatedBuses);
}

void AudioEngine::setPitchSemitones(int semitones) {
    int clamped = std::clamp(semitones, -12, 12);
    int prev = pitchSemitones_.exchange(clamped, std::memory_order_relaxed);
    if (prev == clamped) return;

    std::lock_guard<std::mutex> lock(audioMutex_);
    if (clamped != 0 && stretchBuses_.empty() && !loadedTracks_.empty()) {
        preparePitchShiftersLocked();
        return;
    }
    for (auto& bus : stretchBuses_) {
        if (bus.stretcher && !bus.key.pitchShiftExcluded) {
            bus.stretcher->setTransposeSemitones(static_cast<float>(clamped));
        }
    }
}

void AudioEngine::play() {
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        metronome_.reset();
        clearQueuedSectionLocked();
        awaitingTransitionSwap_ = false;
        pendingTransitionTrigger_.store(0, std::memory_order_relaxed);
        trackMuteUntilFrame_ = 0;
        isSectionStartCountInAtomic_.store(false, std::memory_order_relaxed);
        trackJumpFadeSamplesRemaining_ = 0;
        for (auto& t : loadedTracks_) {
            t.position = 0;
            if (t.stream) {
                t.stream->setPlayheadFrame(0);
            }
        }
        for (auto& bus : stretchBuses_) {
            bus.primed = false;
            bus.inputPos = 0;
        }

        // If a later starting section was pre-selected while stopped, begin 1 bar earlier
        // for that section's normal bar count-in ("<Section>, 2, 3, 4") with tracks muted until Beat 1.
        const bool structuredTimeline = isStructuredTimelineMode();
        clickLoopStopBar_ = (!structuredTimeline && !clickLoopEnabled_.load(std::memory_order_relaxed)) ? 3 : -1;
        if (structuredTimeline && selectedStartSectionIndex_ >= 0 &&
            selectedStartSectionIndex_ < static_cast<int>(sectionSpans_.size())) {
            const auto& targetSpan = sectionSpans_[static_cast<size_t>(selectedStartSectionIndex_)];
            if (!targetSpan.isCount && targetSpan.startBar >= 1) {
                const int64_t countInStartFrame = metronome_.getFrameForBar(static_cast<double>(targetSpan.startBar - 1));
                const int64_t sectionStartFrame = metronome_.getFrameForBar(static_cast<double>(targetSpan.startBar));
                metronome_.jumpToFrame(countInStartFrame);
                trackMuteUntilFrame_ = sectionStartFrame;
                isSectionStartCountInAtomic_.store(true, std::memory_order_relaxed);
            } else if (targetSpan.startBar > 0) {
                const int64_t sectionStartFrame = metronome_.getFrameForBar(static_cast<double>(targetSpan.startBar));
                metronome_.jumpToFrame(sectionStartFrame);
                trackJumpFadeSamplesRemaining_ = kTrackJumpFadeSamples;
            }
            const int64_t audioStartFrame = hasLeadingCountSection_.load(std::memory_order_relaxed)
                                                ? 0
                                                : metronome_.getTotalCountInFrames();
            const int64_t sectionFrame = metronome_.getFrameForBar(static_cast<double>(targetSpan.startBar));
            const int64_t startTrackFrame = std::max<int64_t>(0, sectionFrame - audioStartFrame);
            const int origTempo = originalTrackTempo_.load(std::memory_order_relaxed);
            const int playTempo = metronome_.getPlaybackTempo();
            const double rawRate = (origTempo > 0 && playTempo > 0)
                                       ? (static_cast<double>(playTempo) / static_cast<double>(origTempo))
                                       : 1.0;
            const double exactRate = std::clamp(rawRate, kEngineMinSpeedRatio, kEngineMaxSpeedRatio);
            const int64_t startSrcFrame = static_cast<int64_t>(std::llround(static_cast<double>(startTrackFrame) * exactRate));
            for (auto& t : loadedTracks_) {
                if (t.stream) {
                    t.stream->setPlayheadFrame(startSrcFrame);
                    t.stream->setPrefetchFrame(startSrcFrame);
                }
            }
        }
        syncActiveStreamsLocked(false);
    }
    isPaused_.store(false);
    isPlaying_.store(true);
    notifyStreamer(false);
}

void AudioEngine::pause() {
    isPaused_.store(true);
}

void AudioEngine::resume() {
    isPaused_.store(false);
    notifyStreamer(false);
}

void AudioEngine::stop() {
    const bool wasPlaying = isPlaying_.exchange(false);
    isPaused_.store(false);
    pendingTransitionTrigger_.store(0, std::memory_order_relaxed);
    preserveCrossfadeOnNextLoad_.store(false, std::memory_order_relaxed);
    bool keepStartPrefetch = false;
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        metronome_.reset();
        clearQueuedSectionLocked();
        clearCrossfadeOutLocked();
        awaitingTransitionSwap_ = false;
        clickLoopStopBar_ = -1;
        clickLoopEnabled_.store(true, std::memory_order_relaxed);
        trackMuteUntilFrame_ = 0;
        isSectionStartCountInAtomic_.store(false, std::memory_order_relaxed);
        if (!wasPlaying) {
            // Pressing Stop while already stopped clears the pre-selected start section back to bar 1
            clearSelectedStartSectionLocked();
        }
        trackJumpFadeSamplesRemaining_ = 0;
        for (auto& t : loadedTracks_) {
            t.position = 0;
            if (t.stream) {
                t.stream->trimToIntroOnly();
            }
        }
        if (selectedStartSectionIndex_ >= 0) {
            updateSectionPrefetchLocked(selectedStartSectionIndex_);
            keepStartPrefetch = true;
        }
        for (auto& bus : stretchBuses_) {
            bus.primed = false;
            bus.inputPos = 0;
        }
        syncActiveStreamsLocked(false);
    }
    notifyStreamer(!keepStartPrefetch);
    for (auto& p : outputPeaksAtomic_) {
        p.store(0.0f, std::memory_order_relaxed);
    }
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
    std::lock_guard<std::mutex> lock(audioMutex_);
    metronome_.setTempo(tempo);
    const int origTempo = originalTrackTempo_.load(std::memory_order_relaxed);
    if (origTempo > 0 && metronome_.getPlaybackTempo() != origTempo && stretchBuses_.empty() && !loadedTracks_.empty()) {
        preparePitchShiftersLocked();
    }
}

void AudioEngine::setOriginalTrackTempo(int originalTempo) {
    int clamped = std::clamp(originalTempo > 0 ? originalTempo : 120, 20, 300);
    originalTrackTempo_.store(clamped, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(audioMutex_);
    const bool applyTempoMap = shouldApplyTempoMap();
    metronome_.setTempoMap(clamped, applyTempoMap ? activeTempoPoints_ : std::vector<TempoPoint>{});
    if (metronome_.getPlaybackTempo() != clamped && stretchBuses_.empty() && !loadedTracks_.empty()) {
        preparePitchShiftersLocked();
    }
    updateTimelineMetricsLocked();
}

void AudioEngine::setTimeSignature(const std::string& timeSignature) {
    std::lock_guard<std::mutex> lock(audioMutex_);
    metronome_.setTimeSignature(timeSignature);
    updateTimelineMetricsLocked();
    waveformVersion_.fetch_add(1, std::memory_order_relaxed);
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
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        if (trackIndex >= 0 && trackIndex < static_cast<int>(loadedTracks_.size())) {
            loadedTracks_[trackIndex].track.gain = gain;
        } else {
            return;
        }
    }

    {
        std::lock_guard<std::mutex> cacheLock(cacheMutex_);
        if (!activeCacheKey_.empty()) {
            auto it = songAudioCache_.find(activeCacheKey_);
            if (it != songAudioCache_.end() && it->second &&
                trackIndex < static_cast<int>(it->second->tracks.size())) {
                it->second->tracks[trackIndex].track.gain = gain;
            }
        }
    }

    // Debounce waveform peak recomputation so dragging faders never blocks the GUI thread
    lastWaveformDirtyMs_.store(static_cast<int64_t>(juce::Time::getMillisecondCounterHiRes()), std::memory_order_relaxed);
    waveformRebuildPending_.store(true, std::memory_order_release);
}

void AudioEngine::flushPendingWaveformRebuild() {
    if (!waveformRebuildPending_.load(std::memory_order_acquire)) return;
    const int64_t nowMs = static_cast<int64_t>(juce::Time::getMillisecondCounterHiRes());
    if (nowMs - lastWaveformDirtyMs_.load(std::memory_order_relaxed) < 120) return;

    if (!waveformRebuildPending_.exchange(false, std::memory_order_acq_rel)) return;

    std::vector<LoadedTrack> tracksSnapshot;
    int64_t maxFrames = 0;
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        tracksSnapshot = loadedTracks_;
        maxFrames = maxSongLengthFrames_;
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
        if (it != songAudioCache_.end() && it->second) {
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
            preparePitchShiftersLocked();
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
    const int n = std::clamp(activePeakChannels_.load(std::memory_order_relaxed), 1, kMaxMeterChannels);
    std::vector<float> out(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        out[static_cast<size_t>(i)] = outputPeaksAtomic_[static_cast<size_t>(i)].load(std::memory_order_relaxed);
    }
    return out;
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
    crossfadeScratchBuffer_.setSize(chCount, 2048);
    {
        std::lock_guard<std::mutex> aLock(audioMutex_);
        stretchBuses_.clear(); // Recreate stretchers cleanly for the new sample rate / channel count
        preparePitchShiftersLocked();
        updateTimelineMetricsLocked();
    }

    activePeakChannels_.store(std::min(chCount, kMaxMeterChannels), std::memory_order_relaxed);
    for (auto& p : outputPeaksAtomic_) {
        p.store(0.0f, std::memory_order_relaxed);
    }
}

void AudioEngine::audioDeviceStopped() {
    for (auto& p : outputPeaksAtomic_) {
        p.store(0.0f, std::memory_order_relaxed);
    }
}

void AudioEngine::renderSegmentLocked(int scratchOffset,
                                      int segmentSamples,
                                      int numOutputChannels,
                                      bool playTracks,
                                      int64_t audioStartFrame) {
    const int64_t blockStartFrame = metronome_.getMetronomeFrame();
    const int64_t blockEndFrame = blockStartFrame + segmentSamples;

    if (playTracks && blockEndFrame > audioStartFrame && blockStartFrame >= trackMuteUntilFrame_) {
        const int relOutOffset = static_cast<int>(std::max<int64_t>(0, audioStartFrame - blockStartFrame));
        const int outOffset = scratchOffset + relOutOffset;
        const int samplesForTrack = segmentSamples - relOutOffset;
        const int64_t outTrackStartPos = std::max<int64_t>(0, blockStartFrame - audioStartFrame);
        const int semitones = pitchSemitones_.load(std::memory_order_relaxed);

        // Compute exact source-samples-per-output-sample ratio so backing tracks stay 100% locked
        // to the metronome grid across both constant tempo and tempo-mapped ritardandos/accelerandos.
        const int origTempo = originalTrackTempo_.load(std::memory_order_relaxed);
        const int playTempo = metronome_.getPlaybackTempo();
        const double rawRate = (origTempo > 0 && playTempo > 0)
                                   ? (static_cast<double>(playTempo) / static_cast<double>(origTempo))
                                   : 1.0;
        const double exactRate = std::clamp(rawRate, kEngineMinSpeedRatio, kEngineMaxSpeedRatio);
        const bool isTimeStretched = std::abs(exactRate - 1.0) > 1e-6;

        if ((isTimeStretched || semitones != 0) && pitchOutScratch_.getNumSamples() < samplesForTrack) {
            pitchOutScratch_.setSize(2, samplesForTrack, false, false, true);
        }

        // Helper to mix a single track directly (bypassing Signalsmith Stretch) into renderScratchBuffer_
        constexpr float kInt16Scale = 1.0f / 32768.0f;
        auto mixTrackDirect = [&](LoadedTrack& lt, int64_t trackSamples, int available, int channelA) {
            const int trackChannels = lt.stream->getNumChannels();
            const float gain = lt.track.gain;
            const float scaledGain = gain * kInt16Scale;

            if (trackChannels >= 2) {
                const bool routeAsStereo = lt.track.stereoOutput && (channelA + 1 < numOutputChannels);
                if (routeAsStereo) {
                    lt.stream->forEachSlice(outTrackStartPos, available, [&](const int16_t* srcL, const int16_t* srcR, int sliceOff, int sliceLen) {
                        float* dstL = renderScratchBuffer_.getWritePointer(channelA) + outOffset + sliceOff;
                        float* dstR = renderScratchBuffer_.getWritePointer(channelA + 1) + outOffset + sliceOff;
                        if (srcR != nullptr) {
                            for (int i = 0; i < sliceLen; ++i) {
                                dstL[i] += static_cast<float>(srcL[i]) * scaledGain;
                                dstR[i] += static_cast<float>(srcR[i]) * scaledGain;
                            }
                        } else {
                            for (int i = 0; i < sliceLen; ++i) {
                                const float v = static_cast<float>(srcL[i]) * scaledGain;
                                dstL[i] += v;
                                dstR[i] += v;
                            }
                        }
                    });
                } else if (channelA < numOutputChannels) {
                    float* dst = renderScratchBuffer_.getWritePointer(channelA) + outOffset;
                    double sumEnergy = 0.0;
                    double diffEnergy = 0.0;
                    lt.stream->forEachSlice(outTrackStartPos, available, [&](const int16_t* srcL, const int16_t* srcR, int, int sliceLen) {
                        if (!srcR) return;
                        for (int i = 0; i < sliceLen; ++i) {
                            const float l = static_cast<float>(srcL[i]) * kInt16Scale;
                            const float r = static_cast<float>(srcR[i]) * kInt16Scale;
                            const float s = l + r;
                            const float d = l - r;
                            sumEnergy += static_cast<double>(s) * static_cast<double>(s);
                            diffEnergy += static_cast<double>(d) * static_cast<double>(d);
                        }
                    });
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
                    lt.stream->forEachSlice(outTrackStartPos, available, [&](const int16_t* srcL, const int16_t* srcR, int sliceOff, int sliceLen) {
                        if (srcR != nullptr) {
                            for (int i = 0; i < sliceLen; ++i) {
                                const float t = static_cast<float>(sliceOff + i) * invN;
                                const float gL = (startGainL + (targetGainL - startGainL) * t) * scaledGain;
                                const float gR = (startGainR + (targetGainR - startGainR) * t) * scaledGain;
                                dst[sliceOff + i] += static_cast<float>(srcL[i]) * gL + static_cast<float>(srcR[i]) * gR;
                            }
                        } else {
                            for (int i = 0; i < sliceLen; ++i) {
                                dst[sliceOff + i] += static_cast<float>(srcL[i]) * scaledGain;
                            }
                        }
                    });
                    lt.monoGainL = targetGainL;
                    lt.monoGainR = targetGainR;
                }
            } else if (channelA < numOutputChannels) {
                lt.stream->forEachSlice(outTrackStartPos, available, [&](const int16_t* srcL, const int16_t*, int sliceOff, int sliceLen) {
                    float* dst = renderScratchBuffer_.getWritePointer(channelA) + outOffset + sliceOff;
                    for (int i = 0; i < sliceLen; ++i) {
                        dst[i] += static_cast<float>(srcL[i]) * scaledGain;
                    }
                });
            }
            (void)trackSamples;
        };

        // Fast bypass when neither time-stretching nor pitch-shifting is active (0% DSP overhead)
        if (!isTimeStretched && semitones == 0) {
            for (auto& bus : stretchBuses_) {
                bus.primed = false;
            }
            for (auto& lt : loadedTracks_) {
                if (lt.track.muted || !lt.stream || lt.stream->getTotalSamples() == 0)
                    continue;
                const int64_t trackSamples = lt.stream->getTotalSamples();
                if (outTrackStartPos >= trackSamples)
                    continue;
                const int available = static_cast<int>(std::min<int64_t>(samplesForTrack, trackSamples - outTrackStartPos));
                if (available <= 0)
                    continue;
                const int channelA = std::clamp(lt.track.outputChannel, 0, std::max(0, numOutputChannels - 1));
                mixTrackDirect(lt, trackSamples, available, channelA);
                lt.position = outTrackStartPos + available;
            }
        } else {
            // 1. Mix any unstretched tracks directly (e.g. Drums/FX when pitch-shifting only at original tempo)
            if (!isTimeStretched) {
                for (auto& lt : loadedTracks_) {
                    if (!lt.pitchShiftExcluded || lt.track.muted || !lt.stream || lt.stream->getTotalSamples() == 0)
                        continue;
                    const int64_t trackSamples = lt.stream->getTotalSamples();
                    if (outTrackStartPos >= trackSamples)
                        continue;
                    const int available = static_cast<int>(std::min<int64_t>(samplesForTrack, trackSamples - outTrackStartPos));
                    if (available <= 0)
                        continue;
                    const int channelA = std::clamp(lt.track.outputChannel, 0, std::max(0, numOutputChannels - 1));
                    mixTrackDirect(lt, trackSamples, available, channelA);
                    lt.position = outTrackStartPos + available;
                }
            }

            // 2. Process each active StretchBus (sums all tracks sharing the same routing + pitch group
            //    BEFORE running Signalsmith Stretch -> ~8x-16x lower CPU than per-stem stretching!).
            const int64_t maxOutSamplesSong = isTimeStretched
                                                  ? static_cast<int64_t>(std::ceil(static_cast<double>(maxSongLengthFrames_) / exactRate))
                                                  : maxSongLengthFrames_;
            const int busAvailable = (outTrackStartPos < maxOutSamplesSong)
                                         ? static_cast<int>(std::min<int64_t>(samplesForTrack, maxOutSamplesSong - outTrackStartPos))
                                         : 0;

            for (auto& bus : stretchBuses_) {
                const bool busNeedsStretch = isTimeStretched || (!bus.key.pitchShiftExcluded && semitones != 0);
                if (!busNeedsStretch || !bus.stretcher || busAvailable <= 0) {
                    bus.primed = false;
                    continue;
                }

                // Helper that sums all unmuted tracks belonging to this bus across [srcStart .. srcStart + count)
                const int busCh = bus.key.routeAsStereo ? 2 : 1;
                auto fillBusFromTracks = [&](juce::AudioBuffer<float>& dst, int64_t srcStart, int count, bool& hasAnyTrack) {
                    hasAnyTrack = false;
                    for (int c = 0; c < busCh; ++c) {
                        juce::FloatVectorOperations::clear(dst.getWritePointer(c), count);
                    }
                    for (auto& lt : loadedTracks_) {
                        if (lt.track.muted || !lt.stream || lt.stream->getTotalSamples() == 0)
                            continue;
                        const int channelA = std::clamp(lt.track.outputChannel, 0, std::max(0, numOutputChannels - 1));
                        const int trkCh = lt.stream->getNumChannels();
                        const bool routeStereo = (trkCh >= 2) && lt.track.stereoOutput && (channelA + 1 < numOutputChannels);
                        if (channelA != bus.key.channelA ||
                            routeStereo != bus.key.routeAsStereo ||
                            lt.pitchShiftExcluded != bus.key.pitchShiftExcluded) {
                            continue;
                        }

                        hasAnyTrack = true;
                        const int64_t trackSamples = lt.stream->getTotalSamples();
                        const int64_t copyStart = std::max<int64_t>(0, srcStart);
                        const int64_t copyEnd = std::min<int64_t>(trackSamples, srcStart + count);
                        if (copyEnd <= copyStart) continue;

                        const int dstOff = static_cast<int>(copyStart - srcStart);
                        const int nCopy = static_cast<int>(copyEnd - copyStart);
                        const float scaledGain = lt.track.gain * kInt16Scale;

                        lt.stream->forEachSlice(copyStart, nCopy, [&](const int16_t* sL, const int16_t* sR, int sliceOff, int sliceLen) {
                            if (bus.key.routeAsStereo) {
                                float* dL = dst.getWritePointer(0) + dstOff + sliceOff;
                                float* dR = dst.getWritePointer(1) + dstOff + sliceOff;
                                if (sR != nullptr) {
                                    for (int i = 0; i < sliceLen; ++i) {
                                        dL[i] += static_cast<float>(sL[i]) * scaledGain;
                                        dR[i] += static_cast<float>(sR[i]) * scaledGain;
                                    }
                                } else {
                                    for (int i = 0; i < sliceLen; ++i) {
                                        const float v = static_cast<float>(sL[i]) * scaledGain;
                                        dL[i] += v;
                                        dR[i] += v;
                                    }
                                }
                            } else {
                                float* dM = dst.getWritePointer(0) + dstOff + sliceOff;
                                if (sR != nullptr) {
                                    const float g = scaledGain * 0.70710678f;
                                    for (int i = 0; i < sliceLen; ++i) {
                                        dM[i] += static_cast<float>(static_cast<int>(sL[i]) + static_cast<int>(sR[i])) * g;
                                    }
                                } else {
                                    for (int i = 0; i < sliceLen; ++i) {
                                        dM[i] += static_cast<float>(sL[i]) * scaledGain;
                                    }
                                }
                            }
                        });
                    }
                };

                auto& shifter = *bus.stretcher;
                const int inLat = shifter.inputLatency();
                const int outLat = shifter.outputLatency();
                const int outLatSrc = static_cast<int>(std::round(exactRate * static_cast<double>(outLat)));
                const int totalLatSrc = inLat + outLatSrc;

                const double desiredSrcStart = static_cast<double>(outTrackStartPos) * exactRate + static_cast<double>(totalLatSrc);
                const double desiredSrcEnd = static_cast<double>(outTrackStartPos + busAvailable) * exactRate + static_cast<double>(totalLatSrc);

                if (!bus.primed || std::abs(static_cast<double>(bus.inputPos) - desiredSrcStart) > 512.0) {
                    shifter.reset();
                    shifter.setTransposeSemitones(bus.key.pitchShiftExcluded ? 0.0f : static_cast<float>(semitones));
                    const int64_t primeSrcStart = static_cast<int64_t>(std::llround(static_cast<double>(outTrackStartPos) * exactRate));
                    const int primeIn = std::max(0, inLat + outLatSrc);
                    const int primeOut = std::max(0, static_cast<int>(std::round(static_cast<double>(inLat) / exactRate)) + outLat);
                    if (primeIn > 0 && primeOut > 0) {
                        if (pitchPrimeInScratch_.getNumSamples() < primeIn) {
                            pitchPrimeInScratch_.setSize(2, primeIn, false, false, true);
                        }
                        if (pitchPrimeOutScratch_.getNumSamples() < primeOut) {
                            pitchPrimeOutScratch_.setSize(2, primeOut, false, false, true);
                        }
                        bool anyPrime = false;
                        fillBusFromTracks(pitchPrimeInScratch_, primeSrcStart, primeIn, anyPrime);
                        if (!anyPrime) {
                            bus.primed = false;
                            continue;
                        }
                        float* pInPtrs[2] = { pitchPrimeInScratch_.getWritePointer(0), pitchPrimeInScratch_.getWritePointer(1) };
                        float* pOutPtrs[2] = { pitchPrimeOutScratch_.getWritePointer(0), pitchPrimeOutScratch_.getWritePointer(1) };
                        shifter.process(pInPtrs, primeIn, pOutPtrs, primeOut);
                    }
                    bus.primed = true;
                    bus.inputPos = primeSrcStart + primeIn;
                }

                const int64_t blockInStart = bus.inputPos;
                const int64_t blockInEnd = static_cast<int64_t>(std::llround(desiredSrcEnd));
                const int inputCount = std::max(0, static_cast<int>(blockInEnd - blockInStart));

                if (pitchInScratch_.getNumSamples() < std::max(1, inputCount)) {
                    pitchInScratch_.setSize(2, std::max(1, inputCount), false, false, true);
                }
                bool anyActive = false;
                if (inputCount > 0) {
                    fillBusFromTracks(pitchInScratch_, blockInStart, inputCount, anyActive);
                } else {
                    anyActive = true;
                }
                if (!anyActive) {
                    bus.primed = false;
                    continue;
                }

                float* inPtrs[2] = { pitchInScratch_.getWritePointer(0), pitchInScratch_.getWritePointer(1) };
                float* outPtrs[2] = { pitchOutScratch_.getWritePointer(0), pitchOutScratch_.getWritePointer(1) };
                shifter.process(inPtrs, inputCount, outPtrs, busAvailable);
                bus.inputPos = blockInStart + inputCount;

                const int chA = bus.key.channelA;
                if (bus.key.routeAsStereo && chA + 1 < numOutputChannels) {
                    juce::FloatVectorOperations::add(renderScratchBuffer_.getWritePointer(chA) + outOffset,
                                                     pitchOutScratch_.getReadPointer(0), busAvailable);
                    juce::FloatVectorOperations::add(renderScratchBuffer_.getWritePointer(chA + 1) + outOffset,
                                                     pitchOutScratch_.getReadPointer(1), busAvailable);
                } else if (chA < numOutputChannels) {
                    juce::FloatVectorOperations::add(renderScratchBuffer_.getWritePointer(chA) + outOffset,
                                                     pitchOutScratch_.getReadPointer(0), busAvailable);
                }
            }

            const int64_t srcPosNow = static_cast<int64_t>(std::llround(static_cast<double>(outTrackStartPos + busAvailable) * exactRate));
            for (auto& lt : loadedTracks_) {
                if (isTimeStretched || !lt.pitchShiftExcluded) {
                    lt.position = srcPosNow;
                }
            }
        }

        // Apply a brief de-click fade-in to backing tracks right after a section jump
        if (trackJumpFadeSamplesRemaining_ > 0 && samplesForTrack > 0) {
            const int fadeCount = std::min(samplesForTrack, trackJumpFadeSamplesRemaining_);
            const int alreadyFaded = kTrackJumpFadeSamples - trackJumpFadeSamplesRemaining_;
            for (int ch = 0; ch < numOutputChannels; ++ch) {
                float* dst = renderScratchBuffer_.getWritePointer(ch) + outOffset;
                for (int i = 0; i < fadeCount; ++i) {
                    const float t = static_cast<float>(alreadyFaded + i) / static_cast<float>(kTrackJumpFadeSamples);
                    dst[i] *= t;
                }
            }
            trackJumpFadeSamplesRemaining_ -= fadeCount;
        }
    }

    // Render metronome and count-in on top (never faded by track jump fade-in)
    metronome_.renderBlock(renderScratchBuffer_, scratchOffset, segmentSamples);
}

void AudioEngine::renderCrossfadeOutLocked(int scratchOffset, int numSamples, int numOutputChannels) {
    if (!crossfadeOut_.active || numSamples <= 0 || numOutputChannels <= 0) {
        return;
    }

    const int64_t remainingFade = crossfadeOut_.totalFadeSamples - crossfadeOut_.elapsedSamples;
    if (remainingFade <= 0) {
        clearCrossfadeOutLocked();
        return;
    }

    const int samplesToRender = static_cast<int>(std::min<int64_t>(numSamples, remainingFade));
    if (samplesToRender <= 0) {
        clearCrossfadeOutLocked();
        return;
    }

    if (crossfadeScratchBuffer_.getNumChannels() != numOutputChannels ||
        crossfadeScratchBuffer_.getNumSamples() < samplesToRender) {
        crossfadeScratchBuffer_.setSize(numOutputChannels, samplesToRender, false, false, true);
    }
    crossfadeScratchBuffer_.clear();

    const int64_t outTrackStartPos = crossfadeOut_.outTrackPos;
    const double exactRate = std::clamp(crossfadeOut_.exactRate, kEngineMinSpeedRatio, kEngineMaxSpeedRatio);
    const int semitones = crossfadeOut_.semitones;
    const bool isTimeStretched = std::abs(exactRate - 1.0) > 1e-6;

    if ((isTimeStretched || semitones != 0) && pitchOutScratch_.getNumSamples() < samplesToRender) {
        pitchOutScratch_.setSize(2, samplesToRender, false, false, true);
    }

    constexpr float kInt16Scale = 1.0f / 32768.0f;
    auto mixTrackDirect = [&](LoadedTrack& lt, int available, int channelA) {
        const int trackChannels = lt.stream->getNumChannels();
        const float scaledGain = lt.track.gain * kInt16Scale;

        lt.stream->forEachSlice(outTrackStartPos, available, [&](const int16_t* srcL, const int16_t* srcR, int sliceOff, int sliceLen) {
            if (trackChannels >= 2 && srcR != nullptr) {
                const bool routeAsStereo = lt.track.stereoOutput && (channelA + 1 < numOutputChannels);
                if (routeAsStereo) {
                    float* dstL = crossfadeScratchBuffer_.getWritePointer(channelA) + sliceOff;
                    float* dstR = crossfadeScratchBuffer_.getWritePointer(channelA + 1) + sliceOff;
                    for (int i = 0; i < sliceLen; ++i) {
                        dstL[i] += static_cast<float>(srcL[i]) * scaledGain;
                        dstR[i] += static_cast<float>(srcR[i]) * scaledGain;
                    }
                } else if (channelA < numOutputChannels) {
                    float* dst = crossfadeScratchBuffer_.getWritePointer(channelA) + sliceOff;
                    const float g = scaledGain * 0.70710678f;
                    for (int i = 0; i < sliceLen; ++i) {
                        dst[i] += static_cast<float>(static_cast<int>(srcL[i]) + static_cast<int>(srcR[i])) * g;
                    }
                }
            } else if (channelA < numOutputChannels) {
                float* dst = crossfadeScratchBuffer_.getWritePointer(channelA) + sliceOff;
                for (int i = 0; i < sliceLen; ++i) {
                    dst[i] += static_cast<float>(srcL[i]) * scaledGain;
                }
            }
        });
    };

    if (!isTimeStretched && semitones == 0) {
        for (auto& lt : crossfadeOut_.tracks) {
            if (lt.track.muted || !lt.stream || lt.stream->getTotalSamples() == 0) continue;
            const int64_t trackSamples = lt.stream->getTotalSamples();
            if (outTrackStartPos >= trackSamples) continue;
            const int available = static_cast<int>(std::min<int64_t>(samplesToRender, trackSamples - outTrackStartPos));
            if (available <= 0) continue;
            const int channelA = std::clamp(lt.track.outputChannel, 0, std::max(0, numOutputChannels - 1));
            mixTrackDirect(lt, available, channelA);
        }
    } else {
        if (!isTimeStretched) {
            for (auto& lt : crossfadeOut_.tracks) {
                if (!lt.pitchShiftExcluded || lt.track.muted || !lt.stream || lt.stream->getTotalSamples() == 0) continue;
                const int64_t trackSamples = lt.stream->getTotalSamples();
                if (outTrackStartPos >= trackSamples) continue;
                const int available = static_cast<int>(std::min<int64_t>(samplesToRender, trackSamples - outTrackStartPos));
                if (available <= 0) continue;
                const int channelA = std::clamp(lt.track.outputChannel, 0, std::max(0, numOutputChannels - 1));
                mixTrackDirect(lt, available, channelA);
            }
        }

        const int64_t maxOutSamplesSong = isTimeStretched
                                              ? static_cast<int64_t>(std::ceil(static_cast<double>(crossfadeOut_.maxSongLengthFrames) / exactRate))
                                              : crossfadeOut_.maxSongLengthFrames;
        const int busAvailable = (outTrackStartPos < maxOutSamplesSong)
                                     ? static_cast<int>(std::min<int64_t>(samplesToRender, maxOutSamplesSong - outTrackStartPos))
                                     : 0;

        for (auto& bus : crossfadeOut_.stretchBuses) {
            const bool busNeedsStretch = isTimeStretched || (!bus.key.pitchShiftExcluded && semitones != 0);
            if (!busNeedsStretch || !bus.stretcher || busAvailable <= 0) continue;

            const int busCh = bus.key.routeAsStereo ? 2 : 1;
            auto fillBus = [&](juce::AudioBuffer<float>& dst, int64_t srcStart, int count, bool& hasAny) {
                hasAny = false;
                for (int c = 0; c < busCh; ++c) {
                    juce::FloatVectorOperations::clear(dst.getWritePointer(c), count);
                }
                for (auto& lt : crossfadeOut_.tracks) {
                    if (lt.track.muted || !lt.stream || lt.stream->getTotalSamples() == 0) continue;
                    const int channelA = std::clamp(lt.track.outputChannel, 0, std::max(0, numOutputChannels - 1));
                    const int trkCh = lt.stream->getNumChannels();
                    const bool routeStereo = (trkCh >= 2) && lt.track.stereoOutput && (channelA + 1 < numOutputChannels);
                    if (channelA != bus.key.channelA || routeStereo != bus.key.routeAsStereo ||
                        lt.pitchShiftExcluded != bus.key.pitchShiftExcluded) {
                        continue;
                    }
                    hasAny = true;
                    const int64_t trackSamples = lt.stream->getTotalSamples();
                    const int64_t copyStart = std::max<int64_t>(0, srcStart);
                    const int64_t copyEnd = std::min<int64_t>(trackSamples, srcStart + count);
                    if (copyEnd <= copyStart) continue;
                    const int dstOff = static_cast<int>(copyStart - srcStart);
                    const int nCopy = static_cast<int>(copyEnd - copyStart);
                    const float scaledGain = lt.track.gain * kInt16Scale;
                    lt.stream->forEachSlice(copyStart, nCopy, [&](const int16_t* sL, const int16_t* sR, int sliceOff, int sliceLen) {
                        if (bus.key.routeAsStereo) {
                            float* dL = dst.getWritePointer(0) + dstOff + sliceOff;
                            float* dR = dst.getWritePointer(1) + dstOff + sliceOff;
                            if (sR) {
                                for (int i = 0; i < sliceLen; ++i) { dL[i] += static_cast<float>(sL[i]) * scaledGain; dR[i] += static_cast<float>(sR[i]) * scaledGain; }
                            } else {
                                for (int i = 0; i < sliceLen; ++i) { const float v = static_cast<float>(sL[i]) * scaledGain; dL[i] += v; dR[i] += v; }
                            }
                        } else {
                            float* dM = dst.getWritePointer(0) + dstOff + sliceOff;
                            if (sR) {
                                const float g = scaledGain * 0.70710678f;
                                for (int i = 0; i < sliceLen; ++i) dM[i] += static_cast<float>(static_cast<int>(sL[i]) + static_cast<int>(sR[i])) * g;
                            } else {
                                for (int i = 0; i < sliceLen; ++i) dM[i] += static_cast<float>(sL[i]) * scaledGain;
                            }
                        }
                    });
                }
            };

            auto& shifter = *bus.stretcher;
            const int inLat = shifter.inputLatency();
            const int outLat = shifter.outputLatency();
            const int totalLatSrc = inLat + static_cast<int>(std::round(exactRate * static_cast<double>(outLat)));
            const double desiredSrcEnd = static_cast<double>(outTrackStartPos + busAvailable) * exactRate + static_cast<double>(totalLatSrc);
            const int64_t blockInStart = bus.inputPos;
            const int64_t blockInEnd = static_cast<int64_t>(std::llround(desiredSrcEnd));
            const int inputCount = std::max(0, static_cast<int>(blockInEnd - blockInStart));

            if (pitchInScratch_.getNumSamples() < std::max(1, inputCount)) {
                pitchInScratch_.setSize(2, std::max(1, inputCount), false, false, true);
            }
            bool anyActive = false;
            if (inputCount > 0) fillBus(pitchInScratch_, blockInStart, inputCount, anyActive);
            else anyActive = true;
            if (!anyActive) continue;

            float* inPtrs[2] = { pitchInScratch_.getWritePointer(0), pitchInScratch_.getWritePointer(1) };
            float* outPtrs[2] = { pitchOutScratch_.getWritePointer(0), pitchOutScratch_.getWritePointer(1) };
            shifter.process(inPtrs, inputCount, outPtrs, busAvailable);
            bus.inputPos = blockInStart + inputCount;

            const int chA = bus.key.channelA;
            if (bus.key.routeAsStereo && chA + 1 < numOutputChannels) {
                float* dstL = crossfadeScratchBuffer_.getWritePointer(chA);
                float* dstR = crossfadeScratchBuffer_.getWritePointer(chA + 1);
                const float* sL = pitchOutScratch_.getReadPointer(0);
                const float* sR = pitchOutScratch_.getReadPointer(1);
                for (int i = 0; i < busAvailable; ++i) { dstL[i] += sL[i]; dstR[i] += sR[i]; }
            } else if (chA < numOutputChannels) {
                float* dst = crossfadeScratchBuffer_.getWritePointer(chA);
                const float* sM = pitchOutScratch_.getReadPointer(0);
                for (int i = 0; i < busAvailable; ++i) dst[i] += sM[i];
            }
        }
    }

    // Apply smooth raised-cosine fade-out envelope (1.0 -> 0.0) and sum into renderScratchBuffer_
    const double invTotal = 1.0 / static_cast<double>(std::max<int64_t>(1, crossfadeOut_.totalFadeSamples));
    constexpr double kPi = 3.14159265358979323846;
    for (int ch = 0; ch < numOutputChannels; ++ch) {
        const float* src = crossfadeScratchBuffer_.getReadPointer(ch);
        float* dst = renderScratchBuffer_.getWritePointer(ch) + scratchOffset;
        for (int i = 0; i < samplesToRender; ++i) {
            const double p = std::clamp(static_cast<double>(crossfadeOut_.elapsedSamples + i) * invTotal, 0.0, 1.0);
            const float env = static_cast<float>(0.5 * (1.0 + std::cos(kPi * p)));
            dst[i] += src[i] * env;
        }
    }

    crossfadeOut_.outTrackPos += samplesToRender;
    crossfadeOut_.elapsedSamples += samplesToRender;
    if (crossfadeOut_.elapsedSamples >= crossfadeOut_.totalFadeSamples) {
        clearCrossfadeOutLocked();
    }
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
        const int n = std::clamp(activePeakChannels_.load(std::memory_order_relaxed), 1, kMaxMeterChannels);
        for (int ch = 0; ch < n; ++ch) {
            const float prev = outputPeaksAtomic_[static_cast<size_t>(ch)].load(std::memory_order_relaxed);
            if (prev > 1e-4f) {
                outputPeaksAtomic_[static_cast<size_t>(ch)].store(prev * 0.82f, std::memory_order_relaxed);
            } else if (prev != 0.0f) {
                outputPeaksAtomic_[static_cast<size_t>(ch)].store(0.0f, std::memory_order_relaxed);
            }
        }
        return;
    }

    // Ensure scratch buffer matches output channel count and sample size
    if (renderScratchBuffer_.getNumChannels() != numOutputChannels ||
        renderScratchBuffer_.getNumSamples() < numSamples) {
        renderScratchBuffer_.setSize(numOutputChannels, numSamples, false, false, true);
    }
    renderScratchBuffer_.clear();

    // 2. Render backing tracks & metronome (splitting cleanly at section jumps, click stops, or song transitions)
    const bool playTracks = useTracks_.load(std::memory_order_relaxed) && hasTracks_.load(std::memory_order_relaxed);
    const bool structuredTimeline = isStructuredTimelineMode();
    const SongTransitionMode transitionMode = clampTransitionMode(transitionModeAtomic_.load(std::memory_order_relaxed));
    const int transitionCrossfadeBars = clampCrossfadeBars(transitionCrossfadeBarsAtomic_.load(std::memory_order_relaxed));
    const bool hasNextSong = hasNextSongAtomic_.load(std::memory_order_relaxed);

    {
        std::unique_lock<std::mutex> lock(audioMutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            const int64_t audioStartFrame = (structuredTimeline && hasLeadingCountSection_.load(std::memory_order_relaxed))
                                                ? 0
                                                : metronome_.getTotalCountInFrames();

            int crossfadeRenderStart = 0;
            bool shouldTriggerAsync = false;

            if (!awaitingTransitionSwap_) {
                // Compute song end / crossfade plan when playing a structured timeline (tracks or trackless arrangement with sections)
                bool hasTrackPlan = false;
                SongTransitionTriggerPlan trackPlan;
                int64_t trackTriggerFrame = 0;
                int64_t trackEndFrame = 0;
                if (structuredTimeline) {
                    const int64_t maxFrames = maxSongLengthFramesAtomic_.load(std::memory_order_relaxed);
                    const int64_t secBars = static_cast<int64_t>(sectionTotalBars_.load(std::memory_order_relaxed));
                    int64_t trackBars = 0;
                    if (playTracks && maxFrames > 0) {
                        const double audioDurationBars = cachedTrackDurationBars_.load(std::memory_order_relaxed);
                        const int64_t audioBars = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(audioDurationBars)));
                        trackBars = std::max(audioBars, secBars);
                    } else if (!playTracks && secBars > 0) {
                        trackBars = secBars;
                    }
                    if (trackBars > 0) {
                        trackPlan = computeSongTransitionPlan(metronome_.getPrependedCountBars(),
                                                              trackBars,
                                                              transitionMode,
                                                              transitionCrossfadeBars,
                                                              hasNextSong);
                        trackTriggerFrame = metronome_.getFrameForBar(static_cast<double>(trackPlan.triggerBar));
                        trackEndFrame = metronome_.getFrameForBar(static_cast<double>(trackPlan.endBar));
                        hasTrackPlan = (trackEndFrame > 0 && trackTriggerFrame > 0);
                    }
                }

                auto handleTrackTriggerAtFrame = [&](int64_t reachedFrame, int renderedOffset) {
                    if (trackPlan.isCrossfade && hasNextSong) {
                        // Snapshot outgoing song's backing tracks into CrossfadeOutTail for smooth fade-out
                        const int origTempo = originalTrackTempo_.load(std::memory_order_relaxed);
                        const int playTempo = metronome_.getPlaybackTempo();
                        const double rawRate = (origTempo > 0 && playTempo > 0)
                                                   ? (static_cast<double>(playTempo) / static_cast<double>(origTempo))
                                                   : 1.0;
                        crossfadeOut_.tracks = loadedTracks_;
                        crossfadeOut_.stretchBuses = std::move(stretchBuses_);
                        crossfadeOut_.maxSongLengthFrames = maxSongLengthFrames_;
                        crossfadeOut_.outTrackPos = std::max<int64_t>(0, reachedFrame - audioStartFrame);
                        crossfadeOut_.elapsedSamples = 0;
                        crossfadeOut_.totalFadeSamples = std::max<int64_t>(1, trackEndFrame - reachedFrame);
                        crossfadeOut_.exactRate = std::clamp(rawRate, kEngineMinSpeedRatio, kEngineMaxSpeedRatio);
                        crossfadeOut_.semitones = pitchSemitones_.load(std::memory_order_relaxed);
                        crossfadeOut_.active = !crossfadeOut_.tracks.empty();

                        metronome_.reset();
                        clearQueuedSectionLocked();
                        trackMuteUntilFrame_ = 0;
                        isSectionStartCountInAtomic_.store(false, std::memory_order_relaxed);
                        trackJumpFadeSamplesRemaining_ = 0;
                        awaitingTransitionSwap_ = true;
                        crossfadeRenderStart = renderedOffset;
                        pendingTransitionTrigger_.store(static_cast<int>(TransitionTrigger::CrossfadeNext), std::memory_order_release);
                        shouldTriggerAsync = true;
                    } else if (hasNextSong && transitionMode == SongTransitionMode::AutoplayNextSong) {
                        metronome_.reset();
                        clearQueuedSectionLocked();
                        trackMuteUntilFrame_ = 0;
                        isSectionStartCountInAtomic_.store(false, std::memory_order_relaxed);
                        trackJumpFadeSamplesRemaining_ = 0;
                        awaitingTransitionSwap_ = true;
                        pendingTransitionTrigger_.store(static_cast<int>(TransitionTrigger::AutoplayNext), std::memory_order_release);
                        shouldTriggerAsync = true;
                    } else if (hasNextSong && transitionMode == SongTransitionMode::CueNextSong) {
                        isPlaying_.store(false);
                        isPaused_.store(false);
                        metronome_.reset();
                        clearQueuedSectionLocked();
                        trackMuteUntilFrame_ = 0;
                        isSectionStartCountInAtomic_.store(false, std::memory_order_relaxed);
                        trackJumpFadeSamplesRemaining_ = 0;
                        pendingTransitionTrigger_.store(static_cast<int>(TransitionTrigger::CueNext), std::memory_order_release);
                        shouldTriggerAsync = true;
                    } else {
                        isPlaying_.store(false);
                        isPaused_.store(false);
                        metronome_.reset();
                        clearQueuedSectionLocked();
                        trackMuteUntilFrame_ = 0;
                        isSectionStartCountInAtomic_.store(false, std::memory_order_relaxed);
                        trackJumpFadeSamplesRemaining_ = 0;
                    }
                };

                int rendered = 0;
                while (rendered < numSamples) {
                    const int remaining = numSamples - rendered;
                    const int64_t curFrame = metronome_.getMetronomeFrame();

                    int segmentSamples = remaining;
                    bool jumpAfterSegment = false;
                    int64_t jumpTargetFrame = 0;

                    if (structuredTimeline && trackMuteUntilFrame_ > 0) {
                        if (curFrame >= trackMuteUntilFrame_) {
                            trackMuteUntilFrame_ = 0;
                            isSectionStartCountInAtomic_.store(false, std::memory_order_relaxed);
                            trackJumpFadeSamplesRemaining_ = kTrackJumpFadeSamples;
                        } else if (curFrame + segmentSamples > trackMuteUntilFrame_) {
                            segmentSamples = static_cast<int>(trackMuteUntilFrame_ - curFrame);
                        }
                    }

                    if (structuredTimeline && queuedSectionIndex_ >= 0 && queuedJumpBoundaryBar_ >= 0) {
                        const int64_t boundaryFrame = metronome_.getFrameForBar(static_cast<double>(queuedJumpBoundaryBar_));
                        const int targetIdx = queuedSectionIndex_;
                        if (targetIdx < 0 || targetIdx >= static_cast<int>(sectionSpans_.size())) {
                            clearQueuedSectionLocked();
                        } else if (curFrame >= boundaryFrame) {
                            jumpTargetFrame = metronome_.getFrameForBar(static_cast<double>(sectionSpans_[static_cast<size_t>(targetIdx)].startBar));
                            metronome_.jumpToFrame(jumpTargetFrame);
                            clearQueuedSectionLocked();
                            trackMuteUntilFrame_ = 0;
                            isSectionStartCountInAtomic_.store(false, std::memory_order_relaxed);
                            trackJumpFadeSamplesRemaining_ = kTrackJumpFadeSamples;
                            continue;
                        } else if (curFrame + segmentSamples >= boundaryFrame) {
                            segmentSamples = static_cast<int>(boundaryFrame - curFrame);
                            jumpAfterSegment = true;
                            jumpTargetFrame = metronome_.getFrameForBar(static_cast<double>(sectionSpans_[static_cast<size_t>(targetIdx)].startBar));
                        }
                    }

                    bool triggerTrackTransitionAfterSegment = false;
                    if (hasTrackPlan && !jumpAfterSegment) {
                        if (curFrame >= trackTriggerFrame) {
                            handleTrackTriggerAtFrame(curFrame, rendered);
                            break;
                        }
                        if (curFrame + segmentSamples >= trackTriggerFrame) {
                            segmentSamples = static_cast<int>(trackTriggerFrame - curFrame);
                            triggerTrackTransitionAfterSegment = true;
                        }
                    }

                    bool stopClickAfterSegment = false;
                    if (!structuredTimeline && clickLoopStopBar_ > 0) {
                        const int64_t stopFrame = metronome_.getFrameForBar(static_cast<double>(clickLoopStopBar_));
                        if (curFrame >= stopFrame) {
                            clickLoopStopBar_ = -1;
                            clickLoopEnabled_.store(true, std::memory_order_relaxed);
                            metronome_.reset();
                            if (hasNextSong && (transitionMode == SongTransitionMode::AutoplayNextSong ||
                                                transitionMode == SongTransitionMode::Crossfade)) {
                                awaitingTransitionSwap_ = true;
                                pendingTransitionTrigger_.store(static_cast<int>(TransitionTrigger::AutoplayNext), std::memory_order_release);
                                shouldTriggerAsync = true;
                            } else if (hasNextSong && transitionMode == SongTransitionMode::CueNextSong) {
                                isPlaying_.store(false);
                                isPaused_.store(false);
                                pendingTransitionTrigger_.store(static_cast<int>(TransitionTrigger::CueNext), std::memory_order_release);
                                shouldTriggerAsync = true;
                            } else {
                                isPlaying_.store(false);
                                isPaused_.store(false);
                            }
                            break;
                        }
                        if (curFrame + segmentSamples >= stopFrame) {
                            segmentSamples = static_cast<int>(stopFrame - curFrame);
                            stopClickAfterSegment = true;
                        }
                    }

                    renderSegmentLocked(rendered, segmentSamples, numOutputChannels, playTracks, audioStartFrame);
                    rendered += segmentSamples;

                    if (stopClickAfterSegment) {
                        clickLoopStopBar_ = -1;
                        clickLoopEnabled_.store(true, std::memory_order_relaxed);
                        metronome_.reset();
                        if (hasNextSong && (transitionMode == SongTransitionMode::AutoplayNextSong ||
                                            transitionMode == SongTransitionMode::Crossfade)) {
                            awaitingTransitionSwap_ = true;
                            pendingTransitionTrigger_.store(static_cast<int>(TransitionTrigger::AutoplayNext), std::memory_order_release);
                            shouldTriggerAsync = true;
                        } else if (hasNextSong && transitionMode == SongTransitionMode::CueNextSong) {
                            isPlaying_.store(false);
                            isPaused_.store(false);
                            pendingTransitionTrigger_.store(static_cast<int>(TransitionTrigger::CueNext), std::memory_order_release);
                            shouldTriggerAsync = true;
                        } else {
                            isPlaying_.store(false);
                            isPaused_.store(false);
                        }
                        break;
                    }

                    if (triggerTrackTransitionAfterSegment) {
                        handleTrackTriggerAtFrame(trackTriggerFrame, rendered);
                        break;
                    }

                    if (jumpAfterSegment) {
                        metronome_.jumpToFrame(jumpTargetFrame);
                        clearQueuedSectionLocked();
                        trackMuteUntilFrame_ = 0;
                        isSectionStartCountInAtomic_.store(false, std::memory_order_relaxed);
                        trackJumpFadeSamplesRemaining_ = kTrackJumpFadeSamples;
                    }
                }
            }

            // Mix any active outgoing crossfade tail smoothly underneath the incoming song
            if (crossfadeOut_.active && crossfadeRenderStart < numSamples) {
                renderCrossfadeOutLocked(crossfadeRenderStart, numSamples - crossfadeRenderStart, numOutputChannels);
            }

            if (shouldTriggerAsync) {
                triggerAsyncUpdate();
            }
        }
    }

    // 4. Apply master gain and clamp to [-0.95, 0.95] (SIMD-accelerated)
    const float mg = masterGain_.load();
    for (int ch = 0; ch < numOutputChannels; ++ch) {
        float* dst = outputChannelData[ch];
        const float* src = renderScratchBuffer_.getReadPointer(ch);
        if (!dst) continue;
        juce::FloatVectorOperations::copyWithMultiply(dst, src, mg, numSamples);
        juce::FloatVectorOperations::clip(dst, dst, -0.95f, 0.95f, numSamples);
    }

    // 5. Lock-free atomic peak metering
    const int meterCh = std::min(numOutputChannels, kMaxMeterChannels);
    activePeakChannels_.store(std::max(1, meterCh), std::memory_order_relaxed);
    for (int ch = 0; ch < meterCh; ++ch) {
        float peak = 0.0f;
        if (outputChannelData[ch]) {
            auto range = juce::FloatVectorOperations::findMinAndMax(outputChannelData[ch], numSamples);
            peak = std::max(std::abs(range.getStart()), std::abs(range.getEnd()));
        }
        const float prev = outputPeaksAtomic_[static_cast<size_t>(ch)].load(std::memory_order_relaxed);
        outputPeaksAtomic_[static_cast<size_t>(ch)].store(std::max(prev * 0.82f, peak), std::memory_order_relaxed);
    }
}

} // namespace tracks
