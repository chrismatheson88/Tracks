#include "AudioAttachmentService.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <set>

namespace fs = std::filesystem;

namespace tracks {

fs::path AudioAttachmentService::getProjectRoot() {
    // Navigate from app bundle or executable to repository/working root
    auto appFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    fs::path p(appFile.getFullPathName().toStdString());
    // In build/Tracks_artefacts/..., walk up until we see 'CMakeLists.txt', 'uploaded_tracks', or 'assets'
    fs::path cur = p.parent_path();
    while (!cur.empty() && cur != cur.parent_path()) {
        if (fs::exists(cur / "CMakeLists.txt") || fs::exists(cur / "uploaded_tracks") || fs::exists(cur / "assets")) {
            return cur;
        }
        cur = cur.parent_path();
    }
    // Fallback to current working directory
    return fs::current_path();
}

bool AudioAttachmentService::isSupportedAudioExtension(const std::string& extension) {
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".wav" || ext == ".mp3" || ext == ".m4a";
}

std::string AudioAttachmentService::sanitizeSongTitleForPath(const std::string& songTitle) {
    std::string out;
    out.reserve(songTitle.size());
    bool lastWasUnderscore = false;

    for (unsigned char ch : songTitle) {
        if (std::isalnum(ch) || ch == '-') {
            out.push_back(static_cast<char>(ch));
            lastWasUnderscore = false;
        } else if (ch == ' ' || ch == '_' || !std::isalnum(ch)) {
            if (!out.empty() && !lastWasUnderscore) {
                out.push_back('_');
                lastWasUnderscore = true;
            }
        }
    }

    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }

    return out;
}

fs::path AudioAttachmentService::getSongStorageDir(int64_t songId, const std::string& songTitle) {
    std::string folderName = std::to_string(songId);
    std::string safeTitle = sanitizeSongTitleForPath(songTitle);
    if (!safeTitle.empty()) {
        folderName += "_" + safeTitle;
    }
    return getProjectRoot() / "uploaded_tracks" / folderName;
}

fs::path AudioAttachmentService::getSongTracksDir(int64_t songId, const std::string& songTitle) {
    return getSongStorageDir(songId, songTitle) / "tracks";
}

void AudioAttachmentService::removeSongStorage(int64_t songId) {
    auto root = getProjectRoot() / "uploaded_tracks";
    if (!fs::exists(root) || !fs::is_directory(root)) {
        return;
    }

    const std::string exactName = std::to_string(songId);
    const std::string prefix = exactName + "_";

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (name == exactName || name.rfind(prefix, 0) == 0) {
            fs::remove_all(entry.path(), ec);
        }
    }
}

void AudioAttachmentService::clearSongTrackDirectory(int64_t songId, const std::string& songTitle) {
    auto dir = getSongTracksDir(songId, songTitle);
    if (fs::exists(dir)) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_regular_file()) {
                if (isSupportedAudioExtension(entry.path().extension().string())) {
                    fs::remove(entry.path(), ec);
                }
            }
        }
    }
}

AudioDirectoryInspectionResult AudioAttachmentService::inspectAudioDirectory(const std::string& directoryPath) {
    AudioDirectoryInspectionResult result;
    fs::path dir(directoryPath);

    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        result.error = "Select a valid directory.";
        return result;
    }

    std::vector<fs::path> audioFiles;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            if (isSupportedAudioExtension(entry.path().extension().string())) {
                audioFiles.push_back(entry.path());
            }
        }
    }

    if (audioFiles.empty()) {
        result.error = "No WAV, MP3, or M4A files were found in the selected directory.";
        return result;
    }

    std::sort(audioFiles.begin(), audioFiles.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    struct AudioDetails {
        std::string name;
        int64_t frames;
        double sampleRate;
        double duration;
    };
    std::vector<AudioDetails> details;

    for (const auto& file : audioFiles) {
        juce::File jFile(file.string());
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(jFile));
        if (!reader) {
            result.mismatches.push_back(file.filename().string() + ": could not read audio file");
            continue;
        }

        int numChannels = static_cast<int>(reader->numChannels);
        int64_t lengthInSamples = reader->lengthInSamples;
        double sampleRate = reader->sampleRate;
        double duration = sampleRate > 0 ? static_cast<double>(lengthInSamples) / sampleRate : 0.0;

        details.push_back({ file.filename().string(), lengthInSamples, sampleRate, duration });

        std::string channelKind = (numChannels == 1) ? "mono" : (numChannels == 2) ? "stereo" : (std::to_string(numChannels) + "-channel");
        result.channelDetails.push_back(file.filename().string() + ": " + channelKind);

        SongTrack track;
        track.name = file.stem().string();
        track.filePath = fs::absolute(file).string();
        track.outputChannel = 0; // Default to Ch 1 (or Stereo Ch 1-2 for stereo files)
        track.stereoOutput = (numChannels >= 2);
        track.gain = 1.0f;
        track.muted = false;
        track.numChannels = numChannels;
        result.tracks.push_back(track);
    }

    if (result.tracks.empty()) {
        result.error = "No readable WAV, MP3, or M4A files were found in the selected directory.";
        return result;
    }

    // Check duration consistency
    std::set<int64_t> roundedDurations;
    for (const auto& d : details) {
        roundedDurations.insert(static_cast<int64_t>(std::round(d.duration * 1000.0)));
    }

    if (roundedDurations.size() > 1) {
        std::ostringstream oss;
        oss << "Audio lengths differ — ";
        for (size_t i = 0; i < details.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << details[i].name << ": " << std::fixed << std::setprecision(3) << details[i].duration << "s";
        }
        result.mismatches.insert(result.mismatches.begin(), oss.str());
    }

    return result;
}

int AudioAttachmentService::getAudioChannelCount(const std::string& filePath) {
    if (filePath.empty()) return 0;
    juce::File jFile(filePath);
    if (!jFile.existsAsFile()) return 0;

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(jFile));
    return reader ? static_cast<int>(reader->numChannels) : 0;
}

std::vector<SongTrack> AudioAttachmentService::copyTracksToSongDirectory(int64_t songId,
                                                                         const std::string& songTitle,
                                                                         const std::vector<SongTrack>& tracks) {
    auto destSongDir = getSongStorageDir(songId, songTitle);
    auto destDir = destSongDir / "tracks";
    fs::create_directories(destDir);

    std::vector<SongTrack> copied;
    std::set<std::string> keptFilenames;

    for (const auto& track : tracks) {
        fs::path src(track.filePath);
        if (!fs::exists(src) || !fs::is_regular_file(src)) {
            continue;
        }

        fs::path dest = destDir / src.filename();
        std::error_code ec;
        bool sameFile = fs::exists(dest, ec) && fs::equivalent(src, dest, ec);
        if (!sameFile) {
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        }

        keptFilenames.insert(dest.filename().string());

        SongTrack updated = track;
        updated.name = src.stem().string();
        updated.filePath = fs::absolute(dest).string();
        copied.push_back(updated);
    }

    // Remove any stale audio files in destDir that are no longer part of this song
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(destDir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (isSupportedAudioExtension(entry.path().extension().string()) &&
            keptFilenames.find(entry.path().filename().string()) == keptFilenames.end()) {
            fs::remove(entry.path(), ec);
        }
    }

    // Clean up any legacy/renamed storage directories for this song ID (e.g. old "4" or "4_Old_Title")
    auto root = getProjectRoot() / "uploaded_tracks";
    const std::string exactName = std::to_string(songId);
    const std::string prefix = exactName + "_";
    const std::string canonicalName = destSongDir.filename().string();

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (name != canonicalName && (name == exactName || name.rfind(prefix, 0) == 0)) {
            fs::remove_all(entry.path(), ec);
        }
    }

    return copied;
}

} // namespace tracks
