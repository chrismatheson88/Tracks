#pragma once

#include "../Models/Song.h"
#include "../Models/SetlistEntry.h"
#include <algorithm>
#include <string>
#include <vector>

namespace tracks {

struct SectionSpan {
    std::string name;
    std::string normalizedKey;
    bool isCount = false;
    int startBar = 0; // Metronome bar index (inclusive)
    int endBar = 0;   // Metronome bar index (exclusive)
};

struct SectionContext {
    int currentSectionIndex = -1; // -1 when in prepended 2-bar count-in before Section 0
    int naturalNextIndex = 0;     // Section index that will play naturally when current finishes (or N for song end)
    int boundaryBar = 0;          // Metronome bar where current section finishes
};

inline std::vector<SectionSpan> buildSectionSpans(const std::vector<SongSection>& sections,
                                                  bool hasLeadingCountSection) {
    std::vector<SectionSpan> spans;
    spans.reserve(sections.size());
    int cursorBar = hasLeadingCountSection ? 0 : 2;
    for (const auto& sec : sections) {
        int len = std::max(1, sec.lengthBars);
        SectionSpan span;
        span.name = sec.name;
        span.normalizedKey = sec.normalizedKey();
        span.isCount = sec.isCountSection();
        span.startBar = cursorBar;
        span.endBar = cursorBar + len;
        spans.push_back(std::move(span));
        cursorBar = span.endBar;
    }
    return spans;
}

inline SectionContext computeSectionContext(const std::vector<SectionSpan>& spans, int currentBar) {
    SectionContext ctx;
    const int n = static_cast<int>(spans.size());
    if (n == 0) {
        return ctx;
    }

    if (currentBar < spans.front().startBar) {
        ctx.currentSectionIndex = -1;
        ctx.naturalNextIndex = 0;
        ctx.boundaryBar = spans.front().startBar;
        return ctx;
    }

    for (int i = 0; i < n; ++i) {
        if (currentBar >= spans[static_cast<size_t>(i)].startBar &&
            currentBar < spans[static_cast<size_t>(i)].endBar) {
            ctx.currentSectionIndex = i;
            ctx.naturalNextIndex = i + 1;
            ctx.boundaryBar = spans[static_cast<size_t>(i)].endBar;
            return ctx;
        }
    }

    // Past the last defined section (trailing audio tail): jump at the end of the current bar.
    ctx.currentSectionIndex = n - 1;
    ctx.naturalNextIndex = n;
    ctx.boundaryBar = currentBar + 1;
    return ctx;
}

// Compute the updated queued section index (-1 means no override / play natural next)
// given the current section context, currently queued section (-1 if none), and delta (-1 or +1).
inline int computeNextQueuedSection(int sectionCount,
                                    const SectionContext& ctx,
                                    int currentQueuedIndex,
                                    int delta) {
    if (sectionCount <= 0 || delta == 0) {
        return -1;
    }

    const int cursor = (currentQueuedIndex >= 0 && currentQueuedIndex < sectionCount)
                           ? currentQueuedIndex
                           : ctx.naturalNextIndex;

    const int maxTarget = (ctx.naturalNextIndex >= sectionCount) ? sectionCount : (sectionCount - 1);
    const int nextTarget = std::clamp(cursor + delta, 0, maxTarget);

    if (nextTarget == ctx.naturalNextIndex || nextTarget >= sectionCount) {
        return -1;
    }
    return nextTarget;
}

inline bool canSkipSectionInDirection(int sectionCount,
                                      const SectionContext& ctx,
                                      int currentQueuedIndex,
                                      int delta) {
    if (sectionCount <= 0 || delta == 0) {
        return false;
    }

    const int cursor = (currentQueuedIndex >= 0 && currentQueuedIndex < sectionCount)
                           ? currentQueuedIndex
                           : ctx.naturalNextIndex;
    const int maxTarget = (ctx.naturalNextIndex >= sectionCount) ? sectionCount : (sectionCount - 1);
    const int nextTarget = std::clamp(cursor + delta, 0, maxTarget);
    return nextTarget != cursor;
}

inline bool canRepeatCurrentSection(int sectionCount, const SectionContext& ctx) {
    return sectionCount > 0 && ctx.currentSectionIndex >= 0 && ctx.currentSectionIndex < sectionCount;
}

// Return the index of the first non-count section (the section that naturally begins after any leading count-in),
// or 0 if all sections are count sections, or -1 if spans is empty.
inline int getDefaultStartSectionIndex(const std::vector<SectionSpan>& spans) {
    const int n = static_cast<int>(spans.size());
    if (n == 0) return -1;
    for (int i = 0; i < n; ++i) {
        if (!spans[static_cast<size_t>(i)].isCount) {
            return i;
        }
    }
    return 0;
}

// Compute the updated pre-play start section index (-1 means normal start from the beginning of the song)
// when stepping back/forward while stopped.
inline int computeNextStartSection(const std::vector<SectionSpan>& spans,
                                   int currentStartIndex,
                                   int delta) {
    const int n = static_cast<int>(spans.size());
    if (n == 0 || delta == 0) return -1;

    const int baseIdx = getDefaultStartSectionIndex(spans);
    if (baseIdx < 0) return -1;

    const int cursor = (currentStartIndex >= baseIdx && currentStartIndex < n) ? currentStartIndex : baseIdx;
    const int next = std::clamp(cursor + delta, baseIdx, n - 1);
    return (next == baseIdx) ? -1 : next;
}

inline bool canStepStartSection(const std::vector<SectionSpan>& spans,
                                int currentStartIndex,
                                int delta) {
    const int n = static_cast<int>(spans.size());
    if (n == 0 || delta == 0) return false;

    const int baseIdx = getDefaultStartSectionIndex(spans);
    if (baseIdx < 0) return false;

    const int cursor = (currentStartIndex >= baseIdx && currentStartIndex < n) ? currentStartIndex : baseIdx;
    const int next = std::clamp(cursor + delta, baseIdx, n - 1);
    return next != cursor;
}

// Toggle repeating the currently playing section:
// - If already queued to repeat currentSectionIndex, cancels and returns -1.
// - Otherwise queues currentSectionIndex (or returns -1 if not currently inside a section).
inline int computeToggleRepeatCurrentSection(int sectionCount,
                                             const SectionContext& ctx,
                                             int currentQueuedIndex) {
    if (!canRepeatCurrentSection(sectionCount, ctx)) {
        return -1;
    }
    if (currentQueuedIndex == ctx.currentSectionIndex) {
        return -1;
    }
    return ctx.currentSectionIndex;
}

// Compute the updated pre-play start section index when clicking a specific section directly while stopped.
// Clicking the default first non-count section, a leading count section before it, or the already-selected
// start section resets back to normal song start (-1).
inline int computeDirectStartSection(const std::vector<SectionSpan>& spans,
                                     int currentStartIndex,
                                     int clickedIndex) {
    const int n = static_cast<int>(spans.size());
    if (n == 0 || clickedIndex < 0 || clickedIndex >= n) return -1;

    const int baseIdx = getDefaultStartSectionIndex(spans);
    if (baseIdx < 0 || clickedIndex <= baseIdx) return -1;
    if (currentStartIndex == clickedIndex) return -1;
    return clickedIndex;
}

// Compute the updated queued section index when clicking a specific section directly while playing.
// Clicking the already-queued section or the section that will naturally play next cancels any override (-1).
// Clicking the currently playing section queues a repeat of that section.
inline int computeDirectQueuedSection(int sectionCount,
                                      const SectionContext& ctx,
                                      int currentQueuedIndex,
                                      int clickedIndex) {
    if (sectionCount <= 0 || clickedIndex < 0 || clickedIndex >= sectionCount) {
        return -1;
    }
    if (clickedIndex == currentQueuedIndex || clickedIndex == ctx.naturalNextIndex) {
        return -1;
    }
    return clickedIndex;
}

struct SongTransitionTriggerPlan {
    int64_t endBar = 0;       // Timeline bar where the current song ends
    int64_t triggerBar = 0;   // Timeline bar where transition fires (< endBar for Crossfade 1/2 bar)
    bool isCrossfade = false; // True when firing early with outgoing track fade-out
};

inline SongTransitionTriggerPlan computeSongTransitionPlan(int64_t prependedCountBars,
                                                           int64_t trackBars,
                                                           SongTransitionMode mode,
                                                           int crossfadeBars,
                                                           bool hasNextSong) {
    SongTransitionTriggerPlan plan;
    const int64_t safePrepended = std::max<int64_t>(0, prependedCountBars);
    const int64_t safeTrackBars = std::max<int64_t>(1, trackBars);
    plan.endBar = safePrepended + safeTrackBars;
    plan.triggerBar = plan.endBar;
    plan.isCrossfade = false;

    if (!hasNextSong) {
        return plan;
    }

    const int xfBars = getCrossfadeBarsForTransition(mode, crossfadeBars);
    if (xfBars > 0 && safeTrackBars > 1) {
        const int64_t effectiveXf = std::min<int64_t>(xfBars, safeTrackBars - 1);
        plan.triggerBar = plan.endBar - effectiveXf;
        plan.isCrossfade = true;
    }
    return plan;
}

} // namespace tracks
