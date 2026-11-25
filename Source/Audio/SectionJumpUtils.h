#pragma once

#include "../Models/Song.h"
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

} // namespace tracks
