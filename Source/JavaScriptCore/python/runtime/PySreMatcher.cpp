/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PySreMatcher.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// ---- Positions and sets ----

template<typename Char>
static ALWAYS_INLINE bool sreIsAt(const SreState& state, const Char* ptr, SreCode at)
{
    const Char* beginning = static_cast<const Char*>(state.beginning);
    const Char* end = static_cast<const Char*>(state.end);
    // Whether there is a word character on one side of the position and not on the other.
    auto isBoundary = [&](auto isWord) {
        bool before = ptr > beginning && isWord(ptr[-1]);
        bool after = ptr < end && isWord(ptr[0]);
        return before != after;
    };
    switch (at) {
    case SreAtBeginning:
    case SreAtBeginningString:
        return ptr == beginning;
    case SreAtBeginningLine:
        return ptr == beginning || sreIsLineBreak(ptr[-1]);
    case SreAtEnd:
        return ptr == end || (end - ptr == 1 && sreIsLineBreak(ptr[0]));
    case SreAtEndLine:
        return ptr == end || (ptr < static_cast<const Char*>(state.limit) && sreIsLineBreak(ptr[0]));
    case SreAtEndString:
        return ptr == end;
    case SreAtBoundary:
        return isBoundary(sreIsWord);
    case SreAtNonBoundary:
        return !isBoundary(sreIsWord);
    case SreAtLocBoundary:
        return isBoundary(sreIsLocaleWord);
    case SreAtLocNonBoundary:
        return !isBoundary(sreIsLocaleWord);
    case SreAtUniBoundary:
        return isBoundary(sreIsUnicodeWord);
    case SreAtUniNonBoundary:
        return !isBoundary(sreIsUnicodeWord);
    }
    return false;
}

// Whether the character is in the set: a list of members that ends with FAILURE.
static bool sreIsInSet(const SreCode* set, SreCode ch)
{
    bool found = true;
    while (true) {
        switch (*set++) {
        case SreOpFailure:
            return !found;
        case SreOpLiteral:
            if (ch == set[0])
                return found;
            ++set;
            break;
        case SreOpCategory:
            if (sreIsInCategory(set[0], ch))
                return found;
            ++set;
            break;
        case SreOpCharset:
            // A bit for each of the first 256 characters.
            if (ch < 256 && (set[ch / sreCodeBits] & (1u << (ch % sreCodeBits))))
                return found;
            set += 256 / sreCodeBits;
            break;
        case SreOpRange:
            if (set[0] <= ch && ch <= set[1])
                return found;
            set += 2;
            break;
        case SreOpRangeUniIgnore: {
            // The character has been lowered.
            if (set[0] <= ch && ch <= set[1])
                return found;
            SreCode upper = sreUpperUnicode(ch);
            if (set[0] <= upper && upper <= set[1])
                return found;
            set += 2;
            break;
        }
        case SreOpNegate:
            found = !found;
            break;
        case SreOpBigCharset: {
            // <number of blocks> <256 bytes: which block each run of 256 characters uses> <the blocks, 256 bits each>
            SreCode blockCount = *set++;
            const uint8_t* blockIndices = reinterpret_cast<const uint8_t*>(set);
            set += 256 / sizeof(SreCode);
            if (ch < 0x10000) {
                unsigned bit = blockIndices[ch >> 8] * 256 + (ch & 255);
                if (set[bit / sreCodeBits] & (1u << (bit % sreCodeBits)))
                    return found;
            }
            set += blockCount * (256 / sreCodeBits);
            break;
        }
        default:
            return false;
        }
    }
}

// The same for a byte in either of its cases in the locale. Each member is tried with both, so that a negated set leaves out
// both cases of what it names.
static bool sreIsInSetIgnoringLocaleCase(const SreCode* set, SreCode ch)
{
    SreCode lower = sreLowerLocale(ch);
    SreCode upper = sreUpperLocale(ch);
    if (lower == upper)
        return sreIsInSet(set, lower);

    bool found = true;
    auto hasBit = [&](SreCode c) { return c < 256 && (set[c / sreCodeBits] & (1u << (c % sreCodeBits))); };
    while (true) {
        switch (*set++) {
        case SreOpFailure:
            return !found;
        case SreOpLiteral:
            if (lower == set[0] || upper == set[0])
                return found;
            ++set;
            break;
        case SreOpCategory:
            if (sreIsInCategory(set[0], lower) || sreIsInCategory(set[0], upper))
                return found;
            ++set;
            break;
        case SreOpCharset:
            if (hasBit(lower) || hasBit(upper))
                return found;
            set += 256 / sreCodeBits;
            break;
        case SreOpRange:
            if ((set[0] <= lower && lower <= set[1]) || (set[0] <= upper && upper <= set[1]))
                return found;
            set += 2;
            break;
        case SreOpNegate:
            found = !found;
            break;
        default:
            return false;
        }
    }
}

// ---- Matching ----

template<typename Char> static ptrdiff_t sreMatchAt(SreState&, const SreCode* pattern, bool toplevel);

// How many times in a row, up to maxCount, an item that is one character wide matches from state.ptr on.
template<typename Char>
static ptrdiff_t sreCount(SreState& state, const SreCode* pattern, ptrdiff_t maxCount)
{
    const Char* const first = static_cast<const Char*>(state.ptr);
    const Char* ptr = first;
    const Char* end = static_cast<const Char*>(state.end);
    if (maxCount < end - ptr && maxCount != static_cast<ptrdiff_t>(sreMaxRepeat))
        end = ptr + maxCount;

    SreCode literal = pattern[1];
    // A literal too large for the characters of the subject is nowhere in it.
    bool fits = static_cast<SreCode>(static_cast<Char>(literal)) == literal;

    switch (pattern[0]) {
    case SreOpIn:
        while (ptr < end && sreIsInSet(pattern + 2, *ptr))
            ++ptr;
        break;
    case SreOpAny:
        while (ptr < end && !sreIsLineBreak(*ptr))
            ++ptr;
        break;
    case SreOpAnyAll:
        ptr = end;
        break;
    case SreOpLiteral:
        if (fits) {
            while (ptr < end && *ptr == static_cast<Char>(literal))
                ++ptr;
        }
        break;
    case SreOpLiteralIgnore:
        while (ptr < end && sreLowerASCII(*ptr) == literal)
            ++ptr;
        break;
    case SreOpLiteralUniIgnore:
        while (ptr < end && sreLowerUnicode(*ptr) == literal)
            ++ptr;
        break;
    case SreOpLiteralLocIgnore:
        while (ptr < end && sreMatchesIgnoringLocaleCase(literal, *ptr))
            ++ptr;
        break;
    case SreOpNotLiteral:
        if (!fits)
            ptr = end;
        else {
            while (ptr < end && *ptr != static_cast<Char>(literal))
                ++ptr;
        }
        break;
    case SreOpNotLiteralIgnore:
        while (ptr < end && sreLowerASCII(*ptr) != literal)
            ++ptr;
        break;
    case SreOpNotLiteralUniIgnore:
        while (ptr < end && sreLowerUnicode(*ptr) != literal)
            ++ptr;
        break;
    case SreOpNotLiteralLocIgnore:
        while (ptr < end && !sreMatchesIgnoringLocaleCase(literal, *ptr))
            ++ptr;
        break;
    default:
        // Anything else is for the matcher, once for each character.
        while (static_cast<const Char*>(state.ptr) < end) {
            ptrdiff_t result = sreMatchAt<Char>(state, pattern, false);
            if (result < 0)
                return result;
            if (!result)
                break;
        }
        return static_cast<const Char*>(state.ptr) - first;
    }
    return ptr - first;
}

enum SreResume : uint8_t {
    SreResumeNone,
    SreResumeBranch,
    SreResumeRepeatOneLiteral,
    SreResumeRepeatOne,
    SreResumeMinRepeatOne,
    SreResumeRepeat,
    SreResumeMaxUntilMinimum,
    SreResumeMaxUntilItem,
    SreResumeMaxUntilTail,
    SreResumeMinUntilMinimum,
    SreResumeMinUntilTail,
    SreResumeMinUntilItem,
    SreResumePossessiveMinimum,
    SreResumePossessiveItem,
    SreResumeAtomicGroup,
    SreResumeAssert,
    SreResumeAssertNot,
};

#define SRE_FAIL do { result = 0; goto finished; } while (0)
#define SRE_SUCCEED do { result = 1; goto finished; } while (0)

#define SRE_SAVE_LAST_MARK() do { \
        frame->lastMark = state.lastMark; \
        frame->lastIndex = state.lastIndex; \
    } while (0)
#define SRE_RESTORE_LAST_MARK() do { \
        state.lastMark = frame->lastMark; \
        state.lastIndex = frame->lastIndex; \
    } while (0)
#define SRE_PUSH_MARKS() do { \
        if (!state.pushMarks(frame->lastMark)) [[unlikely]] \
            return sreErrorMemory; \
    } while (0)

// Matches `next` from state.ptr on, as a call of the matcher by itself would, and goes on after `label` with the outcome in
// `result`. The local variables that are still needed then are kept in the frame.
#define SRE_MATCH(resumePoint, label, next, isToplevel) \
    nextPattern = (next); \
    nextToplevel = (isToplevel); \
    frame->pattern = pattern; \
    frame->ptr = ptr; \
    frame = state.frames.push(); \
    if (!frame) [[unlikely]] \
        return sreErrorMemory; \
    frame->resume = resumePoint; \
    frame->toplevel = nextToplevel; \
    pattern = nextPattern; \
    goto entrance; \
    label: \
    pattern = frame->pattern; \
    ptr = static_cast<const Char*>(frame->ptr);

// When the marks are saved: a failed attempt may have moved the marks of groups, and whoever goes on to try something else has
// to put them back. lastMark alone is enough for that outside any REPEAT, since marks beyond it do not count; inside one, an
// earlier iteration may have set the same marks, and those values must come back.

template<typename Char>
static ptrdiff_t sreMatchAt(SreState& state, const SreCode* pattern, bool toplevel)
{
    const Char* const end = static_cast<const Char*>(state.end);
    const Char* ptr;
    ptrdiff_t result = 0;
    const SreCode* nextPattern;
    bool nextToplevel;
    SreRepeat* repeatOfTail;

    const size_t depthOnEntry = state.frames.size();
    SreFrame* frame = state.frames.push();
    if (!frame) [[unlikely]]
        return sreErrorMemory;
    frame->resume = SreResumeNone;
    frame->toplevel = toplevel;

entrance:
    ptr = static_cast<const Char*>(state.ptr);
    if (pattern[0] == SreOpInfo) {
        // <INFO> <skip> <flags> <min> ...: what is left of the subject may be too short.
        if (pattern[3] && static_cast<uintptr_t>(end - ptr) < pattern[3])
            SRE_FAIL;
        pattern += pattern[1] + 1;
    }

dispatch:
    switch (*pattern++) {
    case SreOpMark: {
        // <MARK> <index>
        int index = static_cast<int>(pattern[0]);
        if (index & 1)
            state.lastIndex = index / 2 + 1;
        if (index > state.lastMark) {
            // Those that are skipped have not been set.
            for (int i = state.lastMark + 1; i < index; ++i)
                state.marks[i] = nullptr;
            state.lastMark = index;
        }
        state.marks[index] = ptr;
        ++pattern;
        goto dispatch;
    }

    case SreOpLiteral:
        if (ptr >= end || static_cast<SreCode>(*ptr) != pattern[0])
            SRE_FAIL;
        ++pattern;
        ++ptr;
        goto dispatch;

    case SreOpNotLiteral:
        if (ptr >= end || static_cast<SreCode>(*ptr) == pattern[0])
            SRE_FAIL;
        ++pattern;
        ++ptr;
        goto dispatch;

    case SreOpSuccess:
        if (frame->toplevel && ((state.matchAll && ptr != state.end) || (state.mustAdvance && ptr == state.start)))
            SRE_FAIL;
        state.ptr = ptr;
        SRE_SUCCEED;

    case SreOpAt:
        if (!sreIsAt(state, ptr, pattern[0]))
            SRE_FAIL;
        ++pattern;
        goto dispatch;

    case SreOpCategory:
        if (ptr >= end || !sreIsInCategory(pattern[0], *ptr))
            SRE_FAIL;
        ++pattern;
        ++ptr;
        goto dispatch;

    case SreOpAny:
        if (ptr >= end || sreIsLineBreak(*ptr))
            SRE_FAIL;
        ++ptr;
        goto dispatch;

    case SreOpAnyAll:
        if (ptr >= end)
            SRE_FAIL;
        ++ptr;
        goto dispatch;

    case SreOpIn:
        // <IN> <skip> set
        if (ptr >= end || !sreIsInSet(pattern + 1, *ptr))
            SRE_FAIL;
        pattern += pattern[0];
        ++ptr;
        goto dispatch;

    case SreOpLiteralIgnore:
        if (ptr >= end || sreLowerASCII(*ptr) != pattern[0])
            SRE_FAIL;
        ++pattern;
        ++ptr;
        goto dispatch;

    case SreOpLiteralUniIgnore:
        if (ptr >= end || sreLowerUnicode(*ptr) != pattern[0])
            SRE_FAIL;
        ++pattern;
        ++ptr;
        goto dispatch;

    case SreOpLiteralLocIgnore:
        if (ptr >= end || !sreMatchesIgnoringLocaleCase(pattern[0], *ptr))
            SRE_FAIL;
        ++pattern;
        ++ptr;
        goto dispatch;

    case SreOpNotLiteralIgnore:
        if (ptr >= end || sreLowerASCII(*ptr) == pattern[0])
            SRE_FAIL;
        ++pattern;
        ++ptr;
        goto dispatch;

    case SreOpNotLiteralUniIgnore:
        if (ptr >= end || sreLowerUnicode(*ptr) == pattern[0])
            SRE_FAIL;
        ++pattern;
        ++ptr;
        goto dispatch;

    case SreOpNotLiteralLocIgnore:
        if (ptr >= end || sreMatchesIgnoringLocaleCase(pattern[0], *ptr))
            SRE_FAIL;
        ++pattern;
        ++ptr;
        goto dispatch;

    case SreOpInIgnore:
        if (ptr >= end || !sreIsInSet(pattern + 1, sreLowerASCII(*ptr)))
            SRE_FAIL;
        pattern += pattern[0];
        ++ptr;
        goto dispatch;

    case SreOpInUniIgnore:
        if (ptr >= end || !sreIsInSet(pattern + 1, sreLowerUnicode(*ptr)))
            SRE_FAIL;
        pattern += pattern[0];
        ++ptr;
        goto dispatch;

    case SreOpInLocIgnore:
        if (ptr >= end || !sreIsInSetIgnoringLocaleCase(pattern + 1, *ptr))
            SRE_FAIL;
        pattern += pattern[0];
        ++ptr;
        goto dispatch;

    case SreOpJump:
    case SreOpInfo:
        // <JUMP> <offset>
        pattern += pattern[0];
        goto dispatch;

    case SreOpBranch:
        // <BRANCH> <skip> alternative <JUMP> ... <skip> alternative <JUMP> ... <0>
        SRE_SAVE_LAST_MARK();
        if (state.repeat)
            SRE_PUSH_MARKS();
        for (; pattern[0]; pattern += pattern[0]) {
            // An alternative that begins with a character that is not there needs no trying.
            if (pattern[1] == SreOpLiteral && (ptr >= end || static_cast<SreCode>(*ptr) != pattern[2]))
                continue;
            if (pattern[1] == SreOpIn && (ptr >= end || !sreIsInSet(pattern + 3, *ptr)))
                continue;
            state.ptr = ptr;
            SRE_MATCH(SreResumeBranch, resumeBranch, pattern + 1, frame->toplevel)
            if (result) {
                if (state.repeat)
                    state.dropMarks(frame->lastMark);
                SRE_SUCCEED;
            }
            if (state.repeat)
                state.restoreMarks(frame->lastMark);
            SRE_RESTORE_LAST_MARK();
        }
        if (state.repeat)
            state.dropMarks(frame->lastMark);
        SRE_FAIL;

    case SreOpRepeatOne:
        // <REPEAT_ONE> <skip> <min> <max> item <SUCCESS> tail
        // The item is one character wide: take as many as there are, then give them back one by one until the tail matches.
        if (static_cast<ptrdiff_t>(pattern[1]) > end - ptr)
            SRE_FAIL;
        state.ptr = ptr;
        result = sreCount<Char>(state, pattern + 3, pattern[2]);
        if (result < 0)
            return result;
        frame = &state.frames.last();
        frame->count = result;
        ptr += frame->count;
        if (frame->count < static_cast<ptrdiff_t>(pattern[1]))
            SRE_FAIL;

        if (pattern[pattern[0]] == SreOpSuccess && ptr == state.end && !(frame->toplevel && state.mustAdvance && ptr == state.start)) {
            // There is no tail, and nothing left for one to match.
            state.ptr = ptr;
            SRE_SUCCEED;
        }

        SRE_SAVE_LAST_MARK();
        if (state.repeat)
            SRE_PUSH_MARKS();
        if (pattern[pattern[0]] == SreOpLiteral) {
            // The tail can only match where its first character is.
            frame->u.character = pattern[pattern[0] + 1];
            while (true) {
                while (frame->count >= static_cast<ptrdiff_t>(pattern[1]) && (ptr >= end || static_cast<SreCode>(*ptr) != frame->u.character)) {
                    --ptr;
                    --frame->count;
                }
                if (frame->count < static_cast<ptrdiff_t>(pattern[1]))
                    break;
                state.ptr = ptr;
                SRE_MATCH(SreResumeRepeatOneLiteral, resumeRepeatOneLiteral, pattern + pattern[0], frame->toplevel)
                if (result) {
                    if (state.repeat)
                        state.dropMarks(frame->lastMark);
                    SRE_SUCCEED;
                }
                if (state.repeat)
                    state.restoreMarks(frame->lastMark);
                SRE_RESTORE_LAST_MARK();
                --ptr;
                --frame->count;
            }
        } else {
            while (frame->count >= static_cast<ptrdiff_t>(pattern[1])) {
                state.ptr = ptr;
                SRE_MATCH(SreResumeRepeatOne, resumeRepeatOne, pattern + pattern[0], frame->toplevel)
                if (result) {
                    if (state.repeat)
                        state.dropMarks(frame->lastMark);
                    SRE_SUCCEED;
                }
                if (state.repeat)
                    state.restoreMarks(frame->lastMark);
                SRE_RESTORE_LAST_MARK();
                --ptr;
                --frame->count;
            }
        }
        if (state.repeat)
            state.dropMarks(frame->lastMark);
        SRE_FAIL;

    case SreOpMinRepeatOne:
        // <MIN_REPEAT_ONE> <skip> <min> <max> item <SUCCESS> tail
        // Take as few as allowed, then one more each time the tail does not match.
        if (static_cast<ptrdiff_t>(pattern[1]) > end - ptr)
            SRE_FAIL;
        state.ptr = ptr;
        if (!pattern[1])
            frame->count = 0;
        else {
            result = sreCount<Char>(state, pattern + 3, pattern[1]);
            if (result < 0)
                return result;
            frame = &state.frames.last();
            if (result < static_cast<ptrdiff_t>(pattern[1]))
                SRE_FAIL;
            frame->count = result;
            ptr += frame->count;
        }

        if (pattern[pattern[0]] == SreOpSuccess && !(frame->toplevel && ((state.matchAll && ptr != state.end) || (state.mustAdvance && ptr == state.start)))) {
            state.ptr = ptr;
            SRE_SUCCEED;
        }

        SRE_SAVE_LAST_MARK();
        if (state.repeat)
            SRE_PUSH_MARKS();
        while (pattern[2] == sreMaxRepeat || frame->count <= static_cast<ptrdiff_t>(pattern[2])) {
            state.ptr = ptr;
            SRE_MATCH(SreResumeMinRepeatOne, resumeMinRepeatOne, pattern + pattern[0], frame->toplevel)
            if (result) {
                if (state.repeat)
                    state.dropMarks(frame->lastMark);
                SRE_SUCCEED;
            }
            if (state.repeat)
                state.restoreMarks(frame->lastMark);
            SRE_RESTORE_LAST_MARK();

            state.ptr = ptr;
            result = sreCount<Char>(state, pattern + 3, 1);
            if (result < 0)
                return result;
            frame = &state.frames.last();
            if (!result)
                break;
            ++ptr;
            ++frame->count;
        }
        if (state.repeat)
            state.dropMarks(frame->lastMark);
        SRE_FAIL;

    case SreOpPossessiveRepeatOne:
        // <POSSESSIVE_REPEAT_ONE> <skip> <min> <max> item <SUCCESS> tail
        // Take as many as there are, and give none back.
        if (static_cast<ptrdiff_t>(pattern[1]) > end - ptr)
            SRE_FAIL;
        state.ptr = ptr;
        result = sreCount<Char>(state, pattern + 3, pattern[2]);
        if (result < 0)
            return result;
        frame = &state.frames.last();
        frame->count = result;
        ptr += frame->count;
        if (frame->count < static_cast<ptrdiff_t>(pattern[1]))
            SRE_FAIL;
        pattern += pattern[0];
        if (*pattern == SreOpSuccess && ptr == state.end && !(frame->toplevel && state.mustAdvance && ptr == state.start)) {
            state.ptr = ptr;
            SRE_SUCCEED;
        }
        goto dispatch;

    case SreOpRepeat:
        // <REPEAT> <skip> <min> <max> item <MAX_UNTIL or MIN_UNTIL> tail
        // This only says that a repeat has begun. The UNTIL, to which it jumps, does the work, each time it is reached.
        frame->u.repeat = state.allocateRepeat();
        if (!frame->u.repeat)
            return sreErrorMemory;
        frame->u.repeat->count = -1;
        frame->u.repeat->pattern = pattern;
        frame->u.repeat->previous = state.repeat;
        frame->u.repeat->lastPosition = nullptr;
        state.repeat = frame->u.repeat;

        state.ptr = ptr;
        SRE_MATCH(SreResumeRepeat, resumeRepeat, pattern + pattern[0], frame->toplevel)
        state.repeat = frame->u.repeat->previous;
        state.freeRepeat(frame->u.repeat);
        if (result)
            SRE_SUCCEED;
        SRE_FAIL;

    case SreOpMaxUntil:
        // The end of the item of a greedy repeat; the tail follows.
        frame->u.repeat = state.repeat;
        if (!frame->u.repeat)
            return sreErrorState;
        state.ptr = ptr;
        frame->count = frame->u.repeat->count + 1;

        if (frame->count < static_cast<ptrdiff_t>(frame->u.repeat->pattern[1])) {
            // Not enough yet: the item must match again.
            frame->u.repeat->count = frame->count;
            SRE_MATCH(SreResumeMaxUntilMinimum, resumeMaxUntilMinimum, frame->u.repeat->pattern + 3, frame->toplevel)
            if (result)
                SRE_SUCCEED;
            frame->u.repeat->count = frame->count - 1;
            state.ptr = ptr;
            SRE_FAIL;
        }

        if ((frame->count < static_cast<ptrdiff_t>(frame->u.repeat->pattern[2]) || frame->u.repeat->pattern[2] == sreMaxRepeat) && state.ptr != frame->u.repeat->lastPosition) {
            // Enough, but more is better. (Not if the last iteration consumed nothing: so would all that follow.)
            frame->u.repeat->count = frame->count;
            SRE_SAVE_LAST_MARK();
            SRE_PUSH_MARKS();
            frame->savedLastPosition = frame->u.repeat->lastPosition;
            frame->u.repeat->lastPosition = state.ptr;
            SRE_MATCH(SreResumeMaxUntilItem, resumeMaxUntilItem, frame->u.repeat->pattern + 3, frame->toplevel)
            frame->u.repeat->lastPosition = frame->savedLastPosition;
            if (result) {
                state.dropMarks(frame->lastMark);
                SRE_SUCCEED;
            }
            state.popMarks(frame->lastMark);
            SRE_RESTORE_LAST_MARK();
            frame->u.repeat->count = frame->count - 1;
            state.ptr = ptr;
        }

        // No more of the item: on to the tail, which is outside this repeat.
        state.repeat = frame->u.repeat->previous;
        SRE_MATCH(SreResumeMaxUntilTail, resumeMaxUntilTail, pattern, frame->toplevel)
        state.repeat = frame->u.repeat;
        if (result)
            SRE_SUCCEED;
        state.ptr = ptr;
        SRE_FAIL;

    case SreOpMinUntil:
        // The end of the item of a lazy repeat; the tail follows.
        frame->u.repeat = state.repeat;
        if (!frame->u.repeat)
            return sreErrorState;
        state.ptr = ptr;
        frame->count = frame->u.repeat->count + 1;

        if (frame->count < static_cast<ptrdiff_t>(frame->u.repeat->pattern[1])) {
            frame->u.repeat->count = frame->count;
            SRE_MATCH(SreResumeMinUntilMinimum, resumeMinUntilMinimum, frame->u.repeat->pattern + 3, frame->toplevel)
            if (result)
                SRE_SUCCEED;
            frame->u.repeat->count = frame->count - 1;
            state.ptr = ptr;
            SRE_FAIL;
        }

        // Enough: the tail first.
        state.repeat = frame->u.repeat->previous;
        SRE_SAVE_LAST_MARK();
        if (state.repeat)
            SRE_PUSH_MARKS();
        SRE_MATCH(SreResumeMinUntilTail, resumeMinUntilTail, pattern, frame->toplevel)
        repeatOfTail = state.repeat;
        state.repeat = frame->u.repeat;
        if (result) {
            if (repeatOfTail)
                state.dropMarks(frame->lastMark);
            SRE_SUCCEED;
        }
        if (repeatOfTail)
            state.popMarks(frame->lastMark);
        SRE_RESTORE_LAST_MARK();
        state.ptr = ptr;

        if ((frame->count >= static_cast<ptrdiff_t>(frame->u.repeat->pattern[2]) && frame->u.repeat->pattern[2] != sreMaxRepeat) || state.ptr == frame->u.repeat->lastPosition)
            SRE_FAIL;

        frame->u.repeat->count = frame->count;
        frame->savedLastPosition = frame->u.repeat->lastPosition;
        frame->u.repeat->lastPosition = state.ptr;
        SRE_MATCH(SreResumeMinUntilItem, resumeMinUntilItem, frame->u.repeat->pattern + 3, frame->toplevel)
        frame->u.repeat->lastPosition = frame->savedLastPosition;
        if (result)
            SRE_SUCCEED;
        frame->u.repeat->count = frame->count - 1;
        state.ptr = ptr;
        SRE_FAIL;

    case SreOpPossessiveRepeat:
        // <POSSESSIVE_REPEAT> <skip> <min> <max> item <SUCCESS> tail
        state.ptr = ptr;

        // Only so that what is inside knows it is in a repeat, and saves the marks.
        frame->u.repeat = state.allocateRepeat();
        if (!frame->u.repeat)
            return sreErrorMemory;
        frame->u.repeat->count = -1;
        frame->u.repeat->pattern = nullptr;
        frame->u.repeat->previous = state.repeat;
        frame->u.repeat->lastPosition = nullptr;
        state.repeat = frame->u.repeat;

        frame->count = 0;
        while (frame->count < static_cast<ptrdiff_t>(pattern[1])) {
            SRE_MATCH(SreResumePossessiveMinimum, resumePossessiveMinimum, pattern + 3, false)
            if (!result) {
                state.ptr = ptr;
                state.repeat = frame->u.repeat->previous;
                state.freeRepeat(frame->u.repeat);
                SRE_FAIL;
            }
            ++frame->count;
        }

        // From here on `ptr` is where the last iteration began: one that consumed nothing ends the loop.
        ptr = nullptr;
        while ((frame->count < static_cast<ptrdiff_t>(pattern[2]) || pattern[2] == sreMaxRepeat) && state.ptr != ptr) {
            SRE_SAVE_LAST_MARK();
            SRE_PUSH_MARKS();
            ptr = static_cast<const Char*>(state.ptr);
            SRE_MATCH(SreResumePossessiveItem, resumePossessiveItem, pattern + 3, false)
            if (!result) {
                state.popMarks(frame->lastMark);
                SRE_RESTORE_LAST_MARK();
                state.ptr = ptr;
                break;
            }
            state.dropMarks(frame->lastMark);
            ++frame->count;
        }

        state.repeat = frame->u.repeat->previous;
        state.freeRepeat(frame->u.repeat);
        pattern += pattern[0] + 1;
        ptr = static_cast<const Char*>(state.ptr);
        goto dispatch;

    case SreOpAtomicGroup:
        // <ATOMIC_GROUP> <skip> group <SUCCESS> tail
        state.ptr = ptr;
        SRE_MATCH(SreResumeAtomicGroup, resumeAtomicGroup, pattern + 1, false)
        if (!result) {
            state.ptr = ptr;
            SRE_FAIL;
        }
        pattern += pattern[0];
        ptr = static_cast<const Char*>(state.ptr);
        goto dispatch;

#define SRE_GROUP_REFERENCE(sameCharacter) { \
        int mark = static_cast<int>(pattern[0]) * 2; \
        if (mark >= state.lastMark) \
            SRE_FAIL; \
        const Char* group = static_cast<const Char*>(state.marks[mark]); \
        const Char* groupEnd = static_cast<const Char*>(state.marks[mark + 1]); \
        if (!group || !groupEnd || groupEnd < group) \
            SRE_FAIL; \
        for (; group < groupEnd; ++group, ++ptr) { \
            if (ptr >= end || !(sameCharacter)) \
                SRE_FAIL; \
        } \
        ++pattern; \
        goto dispatch; \
    }

    case SreOpGroupRef:
        SRE_GROUP_REFERENCE(*ptr == *group)
    case SreOpGroupRefIgnore:
        SRE_GROUP_REFERENCE(sreLowerASCII(*ptr) == sreLowerASCII(*group))
    case SreOpGroupRefUniIgnore:
        SRE_GROUP_REFERENCE(sreLowerUnicode(*ptr) == sreLowerUnicode(*group))
    case SreOpGroupRefLocIgnore:
        SRE_GROUP_REFERENCE(sreLowerLocale(*ptr) == sreLowerLocale(*group))

#undef SRE_GROUP_REFERENCE

    case SreOpGroupRefExists: {
        // <GROUPREF_EXISTS> <group> <skip> then <JUMP> else
        int mark = static_cast<int>(pattern[0]) * 2;
        bool exists = false;
        if (mark < state.lastMark) {
            const void* group = state.marks[mark];
            const void* groupEnd = state.marks[mark + 1];
            exists = group && groupEnd && groupEnd >= group;
        }
        pattern += exists ? 2 : pattern[1];
        goto dispatch;
    }

    case SreOpAssert:
        // <ASSERT> <skip> <how far back> pattern <SUCCESS>
        if (static_cast<uintptr_t>(ptr - static_cast<const Char*>(state.beginning)) < pattern[1])
            SRE_FAIL;
        state.ptr = ptr - pattern[1];
        SRE_MATCH(SreResumeAssert, resumeAssert, pattern + 2, false)
        if (!result)
            SRE_FAIL;
        pattern += pattern[0];
        goto dispatch;

    case SreOpAssertNot:
        // <ASSERT_NOT> <skip> <how far back> pattern <SUCCESS>
        if (static_cast<uintptr_t>(ptr - static_cast<const Char*>(state.beginning)) >= pattern[1]) {
            state.ptr = ptr - pattern[1];
            SRE_SAVE_LAST_MARK();
            if (state.repeat)
                SRE_PUSH_MARKS();
            SRE_MATCH(SreResumeAssertNot, resumeAssertNot, pattern + 2, false)
            if (result) {
                if (state.repeat)
                    state.dropMarks(frame->lastMark);
                SRE_FAIL;
            }
            if (state.repeat)
                state.popMarks(frame->lastMark);
            SRE_RESTORE_LAST_MARK();
        }
        pattern += pattern[0];
        goto dispatch;

    case SreOpFailure:
        SRE_FAIL;

    default:
        return sreErrorIllegal;
    }

finished:
    {
        uint8_t resume = frame->resume;
        state.frames.shrink(state.frames.size() - 1);
        if (state.frames.size() == depthOnEntry)
            return result;
        frame = &state.frames.last();
        switch (resume) {
        case SreResumeBranch:
            goto resumeBranch;
        case SreResumeRepeatOneLiteral:
            goto resumeRepeatOneLiteral;
        case SreResumeRepeatOne:
            goto resumeRepeatOne;
        case SreResumeMinRepeatOne:
            goto resumeMinRepeatOne;
        case SreResumeRepeat:
            goto resumeRepeat;
        case SreResumeMaxUntilMinimum:
            goto resumeMaxUntilMinimum;
        case SreResumeMaxUntilItem:
            goto resumeMaxUntilItem;
        case SreResumeMaxUntilTail:
            goto resumeMaxUntilTail;
        case SreResumeMinUntilMinimum:
            goto resumeMinUntilMinimum;
        case SreResumeMinUntilTail:
            goto resumeMinUntilTail;
        case SreResumeMinUntilItem:
            goto resumeMinUntilItem;
        case SreResumePossessiveMinimum:
            goto resumePossessiveMinimum;
        case SreResumePossessiveItem:
            goto resumePossessiveItem;
        case SreResumeAtomicGroup:
            goto resumeAtomicGroup;
        case SreResumeAssert:
            goto resumeAssert;
        case SreResumeAssertNot:
            goto resumeAssertNot;
        }
    }
    return sreErrorIllegal;
}

#undef SRE_FAIL
#undef SRE_SUCCEED
#undef SRE_SAVE_LAST_MARK
#undef SRE_RESTORE_LAST_MARK
#undef SRE_PUSH_MARKS
#undef SRE_MATCH

// ---- Searching ----

template<typename Char>
static ptrdiff_t sreSearchFrom(SreState& state, const SreCode* pattern)
{
    const Char* ptr = static_cast<const Char*>(state.start);
    const Char* end = static_cast<const Char*>(state.end);
    if (ptr > end)
        return 0;

    SreCode flags = 0;
    ptrdiff_t prefixLength = 0;
    ptrdiff_t prefixSkip = 0;
    const SreCode* prefix = nullptr;
    const SreCode* overlap = nullptr;
    const SreCode* charset = nullptr;

    if (pattern[0] == SreOpInfo) {
        // <INFO> <skip> <flags> <min> <max> what every match begins with
        flags = pattern[2];
        if (pattern[3] && static_cast<uintptr_t>(end - ptr) < pattern[3])
            return 0;
        if (pattern[3] > 1) {
            // No match begins too near the end.
            end -= pattern[3] - 1;
            if (end <= ptr)
                end = ptr;
        }
        if (flags & SreInfoPrefix) {
            // <length> <how much of it need not be matched again> <the characters> <the overlap table>
            prefixLength = pattern[5];
            prefixSkip = pattern[6];
            prefix = pattern + 7;
            overlap = prefix + prefixLength - 1;
        } else if (flags & SreInfoCharset)
            charset = pattern + 5;
        pattern += 1 + pattern[1];
    }

    auto forgetGroups = [&] { state.lastMark = state.lastIndex = -1; };
    ptrdiff_t status = 0;

    if (prefixLength == 1) {
        // Every match begins with one known character.
        Char first = static_cast<Char>(prefix[0]);
        if (static_cast<SreCode>(first) != prefix[0])
            return 0;
        end = static_cast<const Char*>(state.end);
        state.mustAdvance = false;
        while (ptr < end) {
            while (*ptr != first) {
                if (++ptr >= end)
                    return 0;
            }
            state.start = ptr;
            state.ptr = ptr + prefixSkip;
            if (flags & SreInfoLiteral)
                return 1;
            status = sreMatchAt<Char>(state, pattern + 2 * prefixSkip, false);
            if (status)
                return status;
            ++ptr;
            forgetGroups();
        }
        return 0;
    }

    if (prefixLength > 1) {
        // Every match begins with a known string. The overlap table says how much of it is still matched after a mismatch.
        end = static_cast<const Char*>(state.end);
        if (prefixLength > end - ptr)
            return 0;
        for (ptrdiff_t i = 0; i < prefixLength; ++i) {
            if (static_cast<SreCode>(static_cast<Char>(prefix[i])) != prefix[i])
                return 0;
        }
        while (ptr < end) {
            Char first = static_cast<Char>(prefix[0]);
            while (*ptr++ != first) {
                if (ptr >= end)
                    return 0;
            }
            if (ptr >= end)
                return 0;

            ptrdiff_t matched = 1;
            state.mustAdvance = false;
            do {
                if (*ptr == static_cast<Char>(prefix[matched])) {
                    if (++matched != prefixLength) {
                        if (++ptr >= end)
                            return 0;
                        continue;
                    }
                    state.start = ptr - (prefixLength - 1);
                    state.ptr = ptr - (prefixLength - prefixSkip - 1);
                    if (flags & SreInfoLiteral)
                        return 1;
                    status = sreMatchAt<Char>(state, pattern + 2 * prefixSkip, false);
                    if (status)
                        return status;
                    if (++ptr >= end)
                        return 0;
                    forgetGroups();
                }
                matched = overlap[matched];
            } while (matched);
        }
        return 0;
    }

    if (charset) {
        // Every match begins with a character of a known set.
        end = static_cast<const Char*>(state.end);
        state.mustAdvance = false;
        while (true) {
            while (ptr < end && !sreIsInSet(charset, *ptr))
                ++ptr;
            if (ptr >= end)
                return 0;
            state.start = ptr;
            state.ptr = ptr;
            status = sreMatchAt<Char>(state, pattern, false);
            if (status)
                return status;
            ++ptr;
            forgetGroups();
        }
    }

    state.start = state.ptr = ptr;
    status = sreMatchAt<Char>(state, pattern, true);
    state.mustAdvance = false;
    if (!status && pattern[0] == SreOpAt && (pattern[1] == SreAtBeginning || pattern[1] == SreAtBeginningString)) {
        // It can match nowhere else.
        state.start = state.ptr = end;
        return 0;
    }
    while (!status && ptr < end) {
        ++ptr;
        forgetGroups();
        state.start = state.ptr = ptr;
        status = sreMatchAt<Char>(state, pattern, false);
    }
    return status;
}

ptrdiff_t sreMatch(SreState& state, const SreCode* code)
{
    switch (state.characterSize) {
    case 1:
        return sreMatchAt<uint8_t>(state, code, true);
    case 2:
        return sreMatchAt<char16_t>(state, code, true);
    default:
        return sreMatchAt<char32_t>(state, code, true);
    }
}

ptrdiff_t sreSearch(SreState& state, const SreCode* code)
{
    switch (state.characterSize) {
    case 1:
        return sreSearchFrom<uint8_t>(state, code);
    case 2:
        return sreSearchFrom<char16_t>(state, code);
    default:
        return sreSearchFrom<char32_t>(state, code);
    }
}

// ---- Validation ----
//
// All jumps in generated code go forward, and they nest. Each construct is checked to end where its skip says, with what
// must be there.

namespace {

class SreValidator {
public:
    explicit SreValidator(size_t groups)
        : m_groups(groups)
    {
    }

    enum Outcome { Valid, Invalid, EndsWithJump };

    static bool set(const SreCode* code, const SreCode* end)
    {
        while (code < end) {
            switch (*code++) {
            case SreOpNegate:
                break;
            case SreOpLiteral:
                if (end - code < 1)
                    return false;
                ++code;
                break;
            case SreOpRange:
            case SreOpRangeUniIgnore:
                if (end - code < 2)
                    return false;
                code += 2;
                break;
            case SreOpCharset:
                if (static_cast<size_t>(end - code) < 256 / sreCodeBits)
                    return false;
                code += 256 / sreCodeBits;
                break;
            case SreOpBigCharset: {
                if (end - code < 1)
                    return false;
                SreCode blockCount = *code++;
                if (static_cast<size_t>(end - code) < 256 / sizeof(SreCode))
                    return false;
                const uint8_t* blockIndices = reinterpret_cast<const uint8_t*>(code);
                for (unsigned i = 0; i < 256; ++i) {
                    if (blockIndices[i] >= blockCount)
                        return false;
                }
                code += 256 / sizeof(SreCode);
                size_t size = static_cast<size_t>(blockCount) * (256 / sreCodeBits);
                if (static_cast<size_t>(end - code) < size)
                    return false;
                code += size;
                break;
            }
            case SreOpCategory:
                if (end - code < 1 || *code > SreCategoryUniNotLineBreak)
                    return false;
                ++code;
                break;
            default:
                return false;
            }
        }
        return true;
    }

    Outcome sequence(const SreCode* code, const SreCode* end)
    {
        if (code > end)
            return Invalid;
        SreCode skip;
        SreCode argument;
        // A skip counts from the word it is in, and must stay within the sequence. (`slack`: by how much it may pass the end.)
        auto readSkip = [&](SreCode slack = 0) {
            if (code >= end)
                return false;
            skip = *code;
            if (static_cast<SreCode>(skip - slack) > static_cast<uintptr_t>(end - code))
                return false;
            ++code;
            return true;
        };
        auto readArgument = [&] {
            if (code >= end)
                return false;
            argument = *code++;
            return true;
        };
        auto readOp = [&](SreCode expected) { return code < end && *code++ == expected; };

        while (code < end) {
            SreCode op = *code++;
            switch (op) {
            case SreOpMark:
                if (!readArgument() || argument >= 2 * m_groups)
                    return Invalid;
                break;

            case SreOpLiteral:
            case SreOpNotLiteral:
            case SreOpLiteralIgnore:
            case SreOpNotLiteralIgnore:
            case SreOpLiteralUniIgnore:
            case SreOpNotLiteralUniIgnore:
            case SreOpLiteralLocIgnore:
            case SreOpNotLiteralLocIgnore:
                if (!readArgument())
                    return Invalid;
                break;

            case SreOpSuccess:
            case SreOpFailure:
            case SreOpAny:
            case SreOpAnyAll:
                break;

            case SreOpAt:
                if (!readArgument() || argument > SreAtUniNonBoundary)
                    return Invalid;
                break;

            case SreOpIn:
            case SreOpInIgnore:
            case SreOpInUniIgnore:
            case SreOpInLocIgnore:
                if (!readSkip() || skip < 2 || !set(code, code + skip - 2) || code[skip - 2] != SreOpFailure)
                    return Invalid;
                code += skip - 1;
                break;

            case SreOpInfo: {
                // <INFO> <skip> <flags> <min> <max> and then a prefix or a set
                if (!readSkip() || !skip)
                    return Invalid;
                const SreCode* next = code + skip - 1;
                if (!readArgument())
                    return Invalid;
                SreCode flags = argument;
                if (!readArgument() || !readArgument())
                    return Invalid;
                if (flags & ~(SreInfoPrefix | SreInfoLiteral | SreInfoCharset))
                    return Invalid;
                if ((flags & SreInfoPrefix) && (flags & SreInfoCharset))
                    return Invalid;
                if ((flags & SreInfoLiteral) && !(flags & SreInfoPrefix))
                    return Invalid;
                if (code > next)
                    return Invalid;
                if (flags & SreInfoPrefix) {
                    if (!readArgument())
                        return Invalid;
                    SreCode prefixLength = argument;
                    if (!readArgument() || code > next)
                        return Invalid;
                    // A search goes on after the LITERALs that match what it need not match again.
                    if (argument > prefixLength || 2 * static_cast<uintptr_t>(argument) > static_cast<uintptr_t>(end - next))
                        return Invalid;
                    if (prefixLength > static_cast<uintptr_t>(next - code))
                        return Invalid;
                    code += prefixLength;
                    if (prefixLength > static_cast<uintptr_t>(next - code))
                        return Invalid;
                    for (SreCode i = 0; i < prefixLength; ++i) {
                        if (code[i] >= prefixLength)
                            return Invalid;
                    }
                    code += prefixLength;
                }
                if (flags & SreInfoCharset) {
                    if (next - code < 1 || !set(code, next - 1) || next[-1] != SreOpFailure)
                        return Invalid;
                    code = next;
                } else if (code != next)
                    return Invalid;
                break;
            }

            case SreOpBranch: {
                // Every alternative ends with a JUMP to the same place, which is where the BRANCH ends.
                const SreCode* target = nullptr;
                while (true) {
                    if (!readSkip())
                        return Invalid;
                    if (!skip)
                        break;
                    if (skip < 3 || sequence(code, code + skip - 3) != Valid)
                        return Invalid;
                    code += skip - 3;
                    if (!readOp(SreOpJump) || !readSkip())
                        return Invalid;
                    if (!target)
                        target = code + skip - 1;
                    else if (code + skip - 1 != target)
                        return Invalid;
                }
                if (code != target)
                    return Invalid;
                break;
            }

            case SreOpRepeatOne:
            case SreOpMinRepeatOne:
            case SreOpPossessiveRepeatOne: {
                if (!readSkip() || !readArgument())
                    return Invalid;
                SreCode minimum = argument;
                if (!readArgument() || minimum > argument)
                    return Invalid;
                if (skip < 4 || sequence(code, code + skip - 4) != Valid)
                    return Invalid;
                code += skip - 4;
                if (!readOp(SreOpSuccess))
                    return Invalid;
                break;
            }

            case SreOpRepeat:
            case SreOpPossessiveRepeat: {
                if (!readSkip() || !readArgument())
                    return Invalid;
                SreCode minimum = argument;
                if (!readArgument() || minimum > argument)
                    return Invalid;
                if (skip < 3 || sequence(code, code + skip - 3) != Valid)
                    return Invalid;
                code += skip - 3;
                if (code >= end)
                    return Invalid;
                SreCode last = *code++;
                if (op == SreOpPossessiveRepeat ? last != SreOpSuccess : last != SreOpMaxUntil && last != SreOpMinUntil)
                    return Invalid;
                break;
            }

            case SreOpAtomicGroup:
                if (!readSkip() || skip < 2 || sequence(code, code + skip - 2) != Valid)
                    return Invalid;
                code += skip - 2;
                if (!readOp(SreOpSuccess))
                    return Invalid;
                break;

            case SreOpGroupRef:
            case SreOpGroupRefIgnore:
            case SreOpGroupRefUniIgnore:
            case SreOpGroupRefLocIgnore:
                if (!readArgument() || argument >= m_groups)
                    return Invalid;
                break;

            case SreOpGroupRefExists: {
                // <GROUPREF_EXISTS> <group> <skip> then [<JUMP> <skip> else]. The skip counts from the word before it.
                // Whether there is an `else` shows only in that `then` ends with a JUMP.
                if (!readArgument() || argument >= m_groups)
                    return Invalid;
                if (!readSkip(1) || !skip)
                    return Invalid;
                --code;
                Outcome outcome = sequence(code + 1, code + skip - 1);
                if (outcome == EndsWithJump) {
                    code += skip - 2;
                    if (!readSkip() || !skip)
                        return Invalid;
                    outcome = sequence(code, code + skip - 1);
                }
                if (outcome != Valid)
                    return Invalid;
                code += skip - 1;
                break;
            }

            case SreOpAssert:
            case SreOpAssertNot:
                // <ASSERT> <skip> <how far back> pattern <SUCCESS>
                if (!readSkip() || !readArgument())
                    return Invalid;
                --code;
                if (skip < 3 || sequence(code + 1, code + skip - 2) != Valid)
                    return Invalid;
                code += skip - 2;
                if (!readOp(SreOpSuccess))
                    return Invalid;
                break;

            case SreOpJump:
                // Only as the last thing in a sequence; whoever asked checks where it goes.
                return code + 1 == end ? EndsWithJump : Invalid;

            default:
                return Invalid;
            }
        }
        return Valid;
    }

private:
    size_t m_groups;
};

}

bool sreValidate(const SreCode* code, size_t size, ptrdiff_t groups)
{
    if (groups < 0 || static_cast<size_t>(groups) > sreMaxGroups || !size || code[size - 1] != SreOpSuccess)
        return false;
    return SreValidator(static_cast<size_t>(groups)).sequence(code, code + size - 1) == SreValidator::Valid;
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
