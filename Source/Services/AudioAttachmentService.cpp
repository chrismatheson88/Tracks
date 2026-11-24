#include "AudioAttachmentService.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
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

fs::path AudioAttachmentService::getSongStorageDir(int64_t songId) {
    return getProjectRoot() / "uploaded_tracks" / std::to_string(songId);
}

fs::path AudioAttachmentService::getSongTracksDir(int64_t songId) {
    return getSongStorageDir(songId) / "tracks";
}

void AudioAttachmentService::removeSongStorage(int64_t songId) {
    auto dir = getSongStorageDir(songId);
    if (fs::exists(dir)) {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
}

void AudioAttachmentService::clearSongTrackDirectory(int64_t songId) {
    auto dir = getSongTracksDir(songId);
    if (fs::exists(dir)) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_regular_file()) {
                auto ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".wav" || ext == ".mp3") {
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
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".wav" || ext == ".mp3") {
                audioFiles.push_back(entry.path());
            }
        }
    }

    if (audioFiles.empty()) {
        result.error = "No WAV or MP3 files were found in the selected directory.";
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

    int currentChannel = 0;
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
        track.outputChannel = currentChannel;
        track.gain = 1.0f;
        track.muted = false;
        result.tracks.push_back(track);

        currentChannel += (numChannels >= 2) ? 2 : 1;
    }

    if (result.tracks.empty()) {
        result.error = "No readable WAV or MP3 files were found in the selected directory.";
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

std::vector<SongTrack> AudioAttachmentService::copyTracksToSongDirectory(int64_t songId, const std::vector<SongTrack>& tracks) {
    auto destDir = getSongTracksDir(songId);
    fs::create_directories(destDir);

    clearSongTrackDirectory(songId);

    std::vector<SongTrack> copied;
    for (const auto& track : tracks) {
        fs::path src(track.filePath);
        if (!fs::exists(src) || !fs::is_regular_file(src)) {
            continue;
        }

        fs::path dest = destDir / src.filename();
        std::error_code ec;
        fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);

        SongTrack updated = track;
        updated.name = src.stem().string();
        updated.filePath = fs::absolute(dest).string();
        copied.push_back(updated);
    }
    return copied;
}

} // namespace tracks
