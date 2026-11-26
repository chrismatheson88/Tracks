#pragma once

#include "../Models/Song.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace tracks {

// Analytical piecewise constant / linear-ramp tempo map converting bidirectionally
// between musical pulse coordinates and audio sample frames with zero cumulative drift.
class TempoMap {
public:
    struct Segment {
        double startPulse = 0.0;
        double endPulse = std::numeric_limits<double>::infinity();
        double startBpm = 120.0;
        double endBpm = 120.0;
        double startTimeSec = 0.0;
        double endTimeSec = std::numeric_limits<double>::infinity();
    };

    TempoMap() {
        rebuild(44100.0, 120.0, 1.0, "4/4", 2, {});
    }

    static void parseTimeSignature(const std::string& timeSignature,
                                   int& outBeatsPerBar,
                                   int& outBeatDivision,
                                   bool& outIsCompound,
                                   int& outPulseCountPerBar) {
        int bpb = 4;
        int div = 4;
        auto slashPos = timeSignature.find('/');
        if (slashPos != std::string::npos) {
            try {
                bpb = std::stoi(timeSignature.substr(0, slashPos));
                div = std::stoi(timeSignature.substr(slashPos + 1));
            } catch (...) {
                bpb = 4;
                div = 4;
            }
        }
        outBeatsPerBar = std::max(1, bpb);
        outBeatDivision = std::max(1, div);
        outIsCompound = (outBeatDivision == 8 && (outBeatsPerBar == 6 || outBeatsPerBar == 12));
        outPulseCountPerBar = outIsCompound ? ((outBeatsPerBar / 3) * 3) : outBeatsPerBar;
    }

    void rebuild(double sampleRate,
                 double baseBpm,
                 double tempoScale,
                 const std::string& timeSignature,
                 int prependedCountBars,
                 const std::vector<TempoPoint>& points) {
        sampleRate_ = (sampleRate > 0.0) ? sampleRate : 44100.0;
        baseBpm_ = std::clamp(baseBpm > 0.0 ? baseBpm : 120.0, 20.0, 300.0);
        tempoScale_ = std::clamp(tempoScale > 0.0 ? tempoScale : 1.0, 0.25, 4.0);
        prependedCountBars_ = std::max(0, prependedCountBars);
        parseTimeSignature(timeSignature, beatsPerBar_, beatDivision_, isCompound_, pulseCountPerBar_);
        pulsesPerTempoBeat_ = isCompound_ ? 3.0 : 1.0;

        // Sort sanitized copy of tempo points by their musical pulse position on the timeline
        struct ResolvedPoint {
            double pulse = 0.0;
            double bpm = 120.0;
            double rampPulses = 0.0;
            size_t originalOrder = 0;
        };

        std::vector<ResolvedPoint> resolved;
        resolved.reserve(points.size());
        for (size_t i = 0; i < points.size(); ++i) {
            const auto& pt = points[i];
            int clampedBar = std::max(1, pt.bar);
            double clampedBeat = std::clamp(pt.beat, 1.0, static_cast<double>(pulseCountPerBar_) + 0.9999);
            double audioBarZero = static_cast<double>(clampedBar - 1);
            double beatZero = clampedBeat - 1.0;
            double timelinePulse = (static_cast<double>(prependedCountBars_) + audioBarZero) * static_cast<double>(pulseCountPerBar_) + beatZero;
            double clampedBpm = std::clamp(pt.bpm, 20.0, 300.0);
            double rampPulses = std::max(0.0, pt.rampBars) * static_cast<double>(pulseCountPerBar_);
            resolved.push_back({ timelinePulse, clampedBpm, rampPulses, i });
        }

        std::stable_sort(resolved.begin(), resolved.end(),
                         [](const ResolvedPoint& a, const ResolvedPoint& b) {
                             if (std::abs(a.pulse - b.pulse) > 1e-6) return a.pulse < b.pulse;
                             return a.originalOrder < b.originalOrder;
                         });

        // Deduplicate exact same start pulse (keep the last one defined at that position)
        std::vector<ResolvedPoint> deduped;
        deduped.reserve(resolved.size());
        for (const auto& rp : resolved) {
            if (!deduped.empty() && std::abs(deduped.back().pulse - rp.pulse) <= 1e-6) {
                deduped.back() = rp;
            } else {
                deduped.push_back(rp);
            }
        }

        hasTempoAutomation_ = !deduped.empty();

        // Determine initial BPM for count-in & pre-first-event span:
        // If there's an instant Step event right at Audio Bar 1 Beat 1, count-in matches that tempo;
        // otherwise it uses baseBpm_.
        const double audioStartPulse = static_cast<double>(prependedCountBars_) * static_cast<double>(pulseCountPerBar_);
        double initialRawBpm = baseBpm_;
        if (!deduped.empty() && std::abs(deduped.front().pulse - audioStartPulse) <= 1e-6 && deduped.front().rampPulses <= 1e-6) {
            initialRawBpm = deduped.front().bpm;
        }

        segments_.clear();
        double curPulse = 0.0;
        double curRawBpm = initialRawBpm;

        auto pushSegment = [&](double p0, double p1, double rawB0, double rawB1) {
            if (p1 <= p0 + 1e-9) return;
            Segment seg;
            seg.startPulse = p0;
            seg.endPulse = p1;
            seg.startBpm = std::clamp(rawB0 * tempoScale_, 5.0, 1200.0);
            seg.endBpm = std::clamp(rawB1 * tempoScale_, 5.0, 1200.0);
            seg.startTimeSec = segments_.empty() ? 0.0 : segments_.back().endTimeSec;
            seg.endTimeSec = seg.startTimeSec + segmentDurationSec(p1 - p0, seg.startBpm, seg.endBpm);
            segments_.push_back(seg);
        };

        for (size_t i = 0; i < deduped.size(); ++i) {
            const auto& pt = deduped[i];
            if (pt.pulse > curPulse + 1e-9) {
                pushSegment(curPulse, pt.pulse, curRawBpm, curRawBpm);
                curPulse = pt.pulse;
            }

            if (pt.rampPulses > 1e-6) {
                double rampEnd = pt.pulse + pt.rampPulses;
                // Truncate ramp cleanly if another tempo event begins before this ramp finishes
                if (i + 1 < deduped.size() && deduped[i + 1].pulse < rampEnd) {
                    double nextPulse = std::max(pt.pulse, deduped[i + 1].pulse);
                    double frac = (nextPulse - pt.pulse) / pt.rampPulses;
                    double midBpm = curRawBpm + (pt.bpm - curRawBpm) * frac;
                    pushSegment(pt.pulse, nextPulse, curRawBpm, midBpm);
                    curPulse = nextPulse;
                    curRawBpm = midBpm;
                } else {
                    pushSegment(pt.pulse, rampEnd, curRawBpm, pt.bpm);
                    curPulse = rampEnd;
                    curRawBpm = pt.bpm;
                }
            } else {
                curRawBpm = pt.bpm;
            }
        }

        // Trailing infinite constant segment from curPulse onwards
        Segment tail;
        tail.startPulse = curPulse;
        tail.endPulse = std::numeric_limits<double>::infinity();
        tail.startBpm = std::clamp(curRawBpm * tempoScale_, 5.0, 1200.0);
        tail.endBpm = tail.startBpm;
        tail.startTimeSec = segments_.empty() ? 0.0 : segments_.back().endTimeSec;
        tail.endTimeSec = std::numeric_limits<double>::infinity();
        segments_.push_back(tail);

        audioStartTimeSec_ = pulseToSeconds(audioStartPulse);
    }

    double pulseToSeconds(double pulse) const {
        if (pulse <= 0.0 || segments_.empty()) return 0.0;
        const Segment& seg = findSegmentByPulse(pulse);
        return seg.startTimeSec + segmentElapsedSec(seg, pulse);
    }

    double pulseToFrameDouble(double pulse) const {
        return pulseToSeconds(pulse) * sampleRate_;
    }

    int64_t pulseToFrame(double pulse) const {
        if (pulse <= 0.0) return 0;
        return static_cast<int64_t>(std::llround(pulseToFrameDouble(pulse)));
    }

    double secondsToPulse(double sec) const {
        if (sec <= 0.0 || segments_.empty()) return 0.0;
        const Segment& seg = findSegmentByTime(sec);
        return segmentPulseAtTime(seg, sec);
    }

    double frameToPulse(int64_t frame) const {
        if (frame <= 0) return 0.0;
        return secondsToPulse(static_cast<double>(frame) / sampleRate_);
    }

    int64_t barToFrame(double timelineBar) const {
        return pulseToFrame(timelineBar * static_cast<double>(pulseCountPerBar_));
    }

    double barToFrameDouble(double timelineBar) const {
        return pulseToFrameDouble(timelineBar * static_cast<double>(pulseCountPerBar_));
    }

    double frameToBarDouble(int64_t frame) const {
        if (frame <= 0 || pulseCountPerBar_ <= 0) return 0.0;
        return frameToPulse(frame) / static_cast<double>(pulseCountPerBar_);
    }

    // Offset in frames relative to the start of the audio file (Audio Bar 0 = timeline bar prependedCountBars_)
    double audioBarToFrameOffsetDouble(double audioBarZeroIdx) const {
        double p = (static_cast<double>(prependedCountBars_) + std::max(0.0, audioBarZeroIdx)) *
                   static_cast<double>(pulseCountPerBar_);
        double tSec = std::max(0.0, pulseToSeconds(p) - audioStartTimeSec_);
        return tSec * sampleRate_;
    }

    double audioFrameOffsetToBarDouble(int64_t audioFrameOffset) const {
        if (audioFrameOffset <= 0 || pulseCountPerBar_ <= 0) return 0.0;
        double tSec = audioStartTimeSec_ + (static_cast<double>(audioFrameOffset) / sampleRate_);
        double pulse = secondsToPulse(tSec);
        double timelineBar = pulse / static_cast<double>(pulseCountPerBar_);
        return std::max(0.0, timelineBar - static_cast<double>(prependedCountBars_));
    }

    double getBpmAtPulse(double pulse) const {
        if (segments_.empty()) return baseBpm_ * tempoScale_;
        const Segment& seg = findSegmentByPulse(std::max(0.0, pulse));
        if (!std::isfinite(seg.endPulse) || seg.endPulse <= seg.startPulse + 1e-9 ||
            std::abs(seg.endBpm - seg.startBpm) <= 1e-6) {
            return seg.startBpm;
        }
        double frac = std::clamp((pulse - seg.startPulse) / (seg.endPulse - seg.startPulse), 0.0, 1.0);
        return seg.startBpm + (seg.endBpm - seg.startBpm) * frac;
    }

    bool hasTempoAutomation() const { return hasTempoAutomation_; }
    double getSampleRate() const { return sampleRate_; }
    double getBaseBpm() const { return baseBpm_; }
    double getTempoScale() const { return tempoScale_; }
    int getBeatsPerBar() const { return beatsPerBar_; }
    int getBeatDivision() const { return beatDivision_; }
    bool isCompound() const { return isCompound_; }
    int getPulseCountPerBar() const { return pulseCountPerBar_; }
    int getPrependedCountBars() const { return prependedCountBars_; }
    const std::vector<Segment>& getSegments() const { return segments_; }

private:
    double sampleRate_ = 44100.0;
    double baseBpm_ = 120.0;
    double tempoScale_ = 1.0;
    int beatsPerBar_ = 4;
    int beatDivision_ = 4;
    bool isCompound_ = false;
    int pulseCountPerBar_ = 4;
    double pulsesPerTempoBeat_ = 1.0;
    int prependedCountBars_ = 2;
    bool hasTempoAutomation_ = false;
    double audioStartTimeSec_ = 0.0;
    std::vector<Segment> segments_;

    double segmentDurationSec(double deltaPulse, double b0, double b1) const {
        if (deltaPulse <= 0.0) return 0.0;
        const double k = 60.0 / pulsesPerTempoBeat_;
        if (std::abs(b1 - b0) <= 1e-6) {
            return (k / b0) * deltaPulse;
        }
        return k * (deltaPulse / (b1 - b0)) * std::log(b1 / b0);
    }

    double segmentElapsedSec(const Segment& seg, double pulse) const {
        const double dp = std::max(0.0, pulse - seg.startPulse);
        if (dp <= 0.0) return 0.0;
        const double k = 60.0 / pulsesPerTempoBeat_;
        if (!std::isfinite(seg.endPulse) || std::abs(seg.endBpm - seg.startBpm) <= 1e-6) {
            return (k / seg.startBpm) * dp;
        }
        const double segPulses = seg.endPulse - seg.startPulse;
        const double dB = seg.endBpm - seg.startBpm;
        const double bpmAtP = seg.startBpm + (dB / segPulses) * dp;
        return k * (segPulses / dB) * std::log(bpmAtP / seg.startBpm);
    }

    double segmentPulseAtTime(const Segment& seg, double timeSec) const {
        const double dt = std::max(0.0, timeSec - seg.startTimeSec);
        if (dt <= 0.0) return seg.startPulse;
        const double k = 60.0 / pulsesPerTempoBeat_;
        if (!std::isfinite(seg.endPulse) || std::abs(seg.endBpm - seg.startBpm) <= 1e-6) {
            return seg.startPulse + (seg.startBpm / k) * dt;
        }
        const double segPulses = seg.endPulse - seg.startPulse;
        const double dB = seg.endBpm - seg.startBpm;
        const double bpmAtT = seg.startBpm * std::exp((dB / (k * segPulses)) * dt);
        return seg.startPulse + (segPulses / dB) * (bpmAtT - seg.startBpm);
    }

    const Segment& findSegmentByPulse(double pulse) const {
        auto it = std::upper_bound(segments_.begin(), segments_.end(), pulse,
                                   [](double p, const Segment& s) { return p < s.endPulse; });
        if (it == segments_.end()) return segments_.back();
        return *it;
    }

    const Segment& findSegmentByTime(double timeSec) const {
        auto it = std::upper_bound(segments_.begin(), segments_.end(), timeSec,
                                   [](double t, const Segment& s) { return t < s.endTimeSec; });
        if (it == segments_.end()) return segments_.back();
        return *it;
    }
};

} // namespace tracks
