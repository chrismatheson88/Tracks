#pragma once

#include "../Models/Song.h"
#include "TempoMap.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace tracks {

struct MidiTempoImportResult {
    bool success = false;
    std::string message;
    std::optional<int> detectedBaseTempo;
    std::vector<TempoPoint> points;
};

inline MidiTempoImportResult parseMidiTempoMap(const juce::MidiFile& midiFile,
                                               const std::string& timeSignature,
                                               int fallbackBaseTempo = 120) {
    MidiTempoImportResult result;

    const short timeFormat = midiFile.getTimeFormat();
    if (timeFormat <= 0) {
        result.message = "Unsupported MIDI timing format (SMPTE timecode is not supported; export using ticks-per-quarter-note).";
        return result;
    }

    const double tpq = static_cast<double>(timeFormat);
    int beatsPerBar = 4;
    int beatDivision = 4;
    bool isCompound = false;
    int pulseCountPerBar = 4;
    TempoMap::parseTimeSignature(timeSignature, beatsPerBar, beatDivision, isCompound, pulseCountPerBar);

    const double quarterNotesPerBar = static_cast<double>(beatsPerBar) * (4.0 / static_cast<double>(beatDivision));
    if (quarterNotesPerBar <= 0.0) {
        result.message = "Invalid arrangement time signature.";
        return result;
    }

    struct RawMidiTempo {
        double tick = 0.0;
        double bpm = 120.0;
        size_t order = 0;
    };

    std::vector<RawMidiTempo> rawEvents;
    size_t seqOrder = 0;
    for (int t = 0; t < midiFile.getNumTracks(); ++t) {
        const auto* track = midiFile.getTrack(t);
        if (!track) continue;
        for (int e = 0; e < track->getNumEvents(); ++e) {
            const auto* ev = track->getEventPointer(e);
            if (!ev) continue;
            const auto& msg = ev->message;
            if (msg.isTempoMetaEvent()) {
                const double secPerQuarter = msg.getTempoSecondsPerQuarterNote();
                if (secPerQuarter <= 0.0) continue;
                // In compound meters (6/8, 12/8), Tracks defines BPM as dotted-quarter notes (= 1.5 quarters)
                double bpm = isCompound ? (40.0 / secPerQuarter) : (60.0 / secPerQuarter);
                bpm = std::clamp(bpm, 20.0, 300.0);
                rawEvents.push_back({ std::max(0.0, msg.getTimeStamp()), bpm, seqOrder++ });
            }
        }
    }

    if (rawEvents.empty()) {
        result.message = "No tempo events found in the selected MIDI file.";
        return result;
    }

    std::stable_sort(rawEvents.begin(), rawEvents.end(),
                     [](const RawMidiTempo& a, const RawMidiTempo& b) {
                         if (std::abs(a.tick - b.tick) > 1e-6) return a.tick < b.tick;
                         return a.order < b.order;
                     });

    // Deduplicate events at the exact same tick (keep last)
    std::vector<RawMidiTempo> uniqueTickEvents;
    uniqueTickEvents.reserve(rawEvents.size());
    for (const auto& ev : rawEvents) {
        if (!uniqueTickEvents.empty() && std::abs(uniqueTickEvents.back().tick - ev.tick) <= 1e-6) {
            uniqueTickEvents.back() = ev;
        } else {
            uniqueTickEvents.push_back(ev);
        }
    }

    double baseTempo = static_cast<double>(std::clamp(fallbackBaseTempo > 0 ? fallbackBaseTempo : 120, 20, 300));
    if (uniqueTickEvents.front().tick <= 1e-6) {
        int roundedFirst = std::clamp(static_cast<int>(std::lround(uniqueTickEvents.front().bpm)), 20, 300);
        result.detectedBaseTempo = roundedFirst;
        baseTempo = static_cast<double>(roundedFirst);
    }

    double lastEmittedBpm = baseTempo;
    for (size_t i = 0; i < uniqueTickEvents.size(); ++i) {
        const auto& ev = uniqueTickEvents[i];
        const double totalQuarterNotes = ev.tick / tpq;
        const double barFloat = totalQuarterNotes / quarterNotesPerBar;
        int barZeroIdx = static_cast<int>(std::floor(barFloat + 1e-9));
        double fracBar = std::max(0.0, barFloat - static_cast<double>(barZeroIdx));
        double beatOneIndexed = 1.0 + fracBar * static_cast<double>(pulseCountPerBar);
        // Round beat cleanly to nearest 0.01 to avoid 1.0000001 noise
        beatOneIndexed = std::round(beatOneIndexed * 100.0) / 100.0;
        if (beatOneIndexed >= static_cast<double>(pulseCountPerBar) + 0.995) {
            barZeroIdx += 1;
            beatOneIndexed = 1.0;
        }
        double roundedBpm = std::round(ev.bpm * 100.0) / 100.0;

        // Skip redundant initial event at Bar 1 Beat 1 if it already equals the detected integer base tempo
        if (barZeroIdx == 0 && std::abs(beatOneIndexed - 1.0) <= 1e-3 &&
            std::abs(roundedBpm - baseTempo) <= 0.05) {
            lastEmittedBpm = baseTempo;
            continue;
        }

        // Skip consecutive events with essentially identical BPM
        if (!result.points.empty() && std::abs(roundedBpm - lastEmittedBpm) < 0.05) {
            continue;
        }

        TempoPoint pt;
        pt.bar = barZeroIdx + 1;
        pt.beat = beatOneIndexed;
        pt.bpm = roundedBpm;
        pt.rampBars = 0.0;
        pt.orderIndex = static_cast<int>(result.points.size());
        result.points.push_back(pt);
        lastEmittedBpm = roundedBpm;
    }

    result.success = true;
    if (result.points.empty()) {
        result.message = "Imported constant MIDI tempo (" +
                         std::to_string(result.detectedBaseTempo.value_or(fallbackBaseTempo)) + " BPM, no tempo changes).";
    } else {
        result.message = "Imported " + std::to_string(result.points.size()) +
                         (result.points.size() == 1 ? " tempo change" : " tempo changes") +
                         " from MIDI.";
    }
    return result;
}

inline MidiTempoImportResult importMidiTempoMapFromFile(const std::string& midiFilePath,
                                                        const std::string& timeSignature,
                                                        int fallbackBaseTempo = 120) {
    MidiTempoImportResult result;
    juce::File file(midiFilePath);
    if (!file.existsAsFile()) {
        result.message = "Selected MIDI file does not exist.";
        return result;
    }

    juce::FileInputStream stream(file);
    if (!stream.openedOk()) {
        result.message = "Could not open the selected MIDI file.";
        return result;
    }

    juce::MidiFile midiFile;
    if (!midiFile.readFrom(stream)) {
        result.message = "Selected file is not a valid Standard MIDI File (.mid).";
        return result;
    }

    return parseMidiTempoMap(midiFile, timeSignature, fallbackBaseTempo);
}

} // namespace tracks
