/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "YarrFlatProgram.h"

#include "Options.h"
#include "SuperSampler.h"
#include "YarrCanonicalize.h"
#include <algorithm>
#include <wtf/ASCIICType.h>
#include <wtf/Atomics.h>
#include <wtf/DataLog.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringCommon.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Yarr {

#define FLAT_OP_CASE_LABEL(name) case FlatOp::Kind::name:

namespace {

// Next: continue at op + 1. Jumped: op is already the next op to run.
enum class Step : uint8_t { Next, Jumped, Failed, Matched, NoMatch, HitLimit, NeedsFrames };

template<typename CharType>
static ALWAYS_INLINE bool textMatches(const CharType* input, const Latin1Character* expected, unsigned count, unsigned available)
{
    if constexpr (sizeof(CharType) == 1) {
        constexpr unsigned wordSize = sizeof(uint64_t);
        if (count <= wordSize) {
            if (available >= wordSize) [[likely]]
                return !((WTF::unalignedLoad<uint64_t>(input) ^ WTF::unalignedLoad<uint64_t>(expected)) << ((wordSize - count) * 8));
        } else {
            unsigned i = 0;
            for (; i + wordSize < count; i += wordSize) {
                if (WTF::unalignedLoad<uint64_t>(input + i) != WTF::unalignedLoad<uint64_t>(expected + i))
                    return false;
            }
            return WTF::unalignedLoad<uint64_t>(input + count - wordSize) == WTF::unalignedLoad<uint64_t>(expected + count - wordSize);
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        if (input[i] != expected[i])
            return false;
    }
    return true;
}

template<typename CharType>
static ALWAYS_INLINE bool casedTextMatches(const CharType* input, const Latin1Character* expected, unsigned count, unsigned available)
{
    const Latin1Character* mask = expected + count;
    if constexpr (sizeof(CharType) == 1) {
        constexpr unsigned wordSize = sizeof(uint64_t);
        auto differs = [&](unsigned at) {
            return (WTF::unalignedLoad<uint64_t>(input + at) | WTF::unalignedLoad<uint64_t>(mask + at)) ^ WTF::unalignedLoad<uint64_t>(expected + at);
        };
        if (count <= wordSize) {
            if (available >= wordSize) [[likely]]
                return !(differs(0) << ((wordSize - count) * 8));
        } else {
            for (unsigned i = 0; i + wordSize < count; i += wordSize) {
                if (differs(i))
                    return false;
            }
            return !differs(count - wordSize);
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        if ((input[i] | mask[i]) != expected[i])
            return false;
    }
    return true;
}

// Separate from Machine so that a Machine's address never escapes and the compiler can keep its members in registers.
struct FrameStorage {
    WTF_MAKE_NONCOPYABLE(FrameStorage);
    static constexpr unsigned inlineFrames = 128;

    FrameStorage() = default;

    NEVER_INLINE bool grow(size_t used, size_t needed)
    {
        size_t newCapacity = frames.size() * 2;
        while (newCapacity - used < needed)
            newCapacity *= 2;
        Vector<FlatFrame> larger;
        if (newCapacity * sizeof(FlatFrame) > Options::maxRegExpStackSize() || !larger.tryGrow(newCapacity))
            return false;
        memcpySpan(larger.mutableSpan().first(used), frames.first(used));
        overflow = WTF::move(larger);
        frames = overflow.mutableSpan();
        return true;
    }

    FlatFrame inlineStorage[inlineFrames];
    Vector<FlatFrame> overflow;
    std::span<FlatFrame> frames { inlineStorage };
};

template<typename CharType>
struct Machine {
    using Kind = FlatOp::Kind;

    static constexpr unsigned registersPerFrame = sizeof(FlatFrame) / sizeof(uint32_t);

    ALWAYS_INLINE Machine(const FlatProgram& program, std::span<const CharType> subject, unsigned start, unsigned* output)
        : program(program)
        , ops(program.ops.span().data())
        , characterSets(program.characterSets.span().data())
        , firstSets(program.firstSets.span().data())
        , dispatchClasses(program.dispatchClasses.span().data())
        , dispatchEntries(program.dispatchEntries.span().data())
        , wideRanges(program.wideRanges.span())
        , input(subject.data())
        , length(subject.size())
        , output(output)
        , slots((program.numSubpatterns + 1) * 2)
        , framesOfRegisters((program.numRegisters + registersPerFrame - 1) / registersPerFrame)
        , matchStart(start)
    {
    }

    ALWAYS_INLINE JSRegExpResult run()
    {
        if (matchStart > length)
            return JSRegExpResult::NoMatch;

        for (unsigned i = 0; i < slots; i += 2) {
            output[i] = offsetNoMatch;
            output[i + 1] = offsetNoMatch;
            WTF::compilerFence();
        }

        if (program.endAnchoredFixedSize && length >= *program.endAnchoredFixedSize)
            matchStart = std::max(matchStart, length - *program.endAnchoredFixedSize);
        remainingSteps = matchLimit;

        FrameStorage frameStorage;
        if (framesOfRegisters >= FrameStorage::inlineFrames && !frameStorage.grow(0, framesOfRegisters + 1)) [[unlikely]]
            return JSRegExpResult::ErrorNoMemory;
        setFrames(frameStorage.frames, framesOfRegisters);

        if (!beginAttempt())
            return JSRegExpResult::NoMatch;

        for (;;) {
            Step step = runOp();
            if (step == Step::Failed)
                step = backtrack();
            switch (step) {
            case Step::Next:
                ++op;
                continue;
            case Step::Jumped:
                continue;
            case Step::Matched:
                return JSRegExpResult::Match;
            case Step::NoMatch:
                return JSRegExpResult::NoMatch;
            case Step::HitLimit:
                return JSRegExpResult::ErrorHitLimit;
            case Step::NeedsFrames:
                if (!growFrames(frameStorage))
                    return JSRegExpResult::ErrorNoMemory;
                continue;
            case Step::Failed:
                break;
            }
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    ALWAYS_INLINE uint32_t* registers() { return reinterpret_cast<uint32_t*>(storage); }
    ALWAYS_INLINE uint32_t offsetOf(const FlatOp* at) const { return static_cast<uint32_t>(reinterpret_cast<const char*>(at) - reinterpret_cast<const char*>(ops)); }
    ALWAYS_INLINE const FlatOp* opOf(const FlatFrame& frame) const { return reinterpret_cast<const FlatOp*>(reinterpret_cast<const char*>(ops) + frame.op); }

    ALWAYS_INLINE bool contains(const CharacterSetBits& set, CharType character) const
    {
        return Yarr::contains(set, wideRanges, character);
    }

    ALWAYS_INLINE bool contains(const FirstSetBits& set, CharType character) const
    {
        if (sizeof(CharType) > 1 && character > 255)
            return false;
        return set.bits[static_cast<uint8_t>(character) >> 6] & (1ull << (character & 63));
    }

    ALWAYS_INLINE void setFrames(std::span<FlatFrame> frames, size_t used)
    {
        storage = frames.data();
        storageEnd = frames.data() + frames.size();
        bottom = storage + framesOfRegisters;
        top = storage + used;
    }

    ALWAYS_INLINE bool growFrames(FrameStorage& frameStorage)
    {
        size_t used = top - storage;
        if (!frameStorage.grow(used, framesNeeded))
            return false;
        setFrames(frameStorage.frames, used);
        return true;
    }

    ALWAYS_INLINE bool tryReserveFrames(size_t count)
    {
        if (static_cast<size_t>(storageEnd - top) < count) [[unlikely]] {
            framesNeeded = count;
            return false;
        }
        return true;
    }

    ALWAYS_INLINE void push(uint32_t savedPosition, uint32_t limit, uint32_t extra)
    {
        *top++ = FlatFrame { offsetOf(op), savedPosition, limit, extra };
    }

    ALWAYS_INLINE Step jumpTo(uint32_t index)
    {
        op = ops + index;
        return Step::Jumped;
    }

    ALWAYS_INLINE Step jumpIf(bool condition) { return condition ? jumpTo(op->target) : Step::Next; }
    static ALWAYS_INLINE Step nextIf(bool condition) { return condition ? Step::Next : Step::Failed; }

    ALWAYS_INLINE bool findNextStart()
    {
        switch (program.startScan) {
        case FlatStartScan::None:
            return true;
        case FlatStartScan::OneCharacter: {
            size_t found = WTF::find(std::span<const CharType> { input, length }, static_cast<CharType>(program.onlyStartCharacter), matchStart);
            if (found == notFound)
                return false;
            matchStart = found;
            return true;
        }
        case FlatStartScan::Set:
            for (; matchStart < length; ++matchStart) {
                auto character = input[matchStart];
                if (sizeof(CharType) == 1 || character < 256) {
                    if (contains(program.startSet, character))
                        return true;
                } else if (program.startMayBeWide)
                    return true;
            }
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ALWAYS_INLINE bool beginAttempt()
    {
        if (!findNextStart())
            return false;
        position = matchStart;
        op = ops;
        top = bottom;
        return true;
    }

    ALWAYS_INLINE Step consumeIf(bool condition, unsigned count)
    {
        if (!condition)
            return Step::Failed;
        position += count;
        return Step::Next;
    }

    ALWAYS_INLINE Step runCharacter() { return consumeIf(position < length && input[position] == op->argument, 1); }
    ALWAYS_INLINE Step runSet() { return consumeIf(position < length && contains(characterSets[op->set], input[position]), 1); }

    ALWAYS_INLINE Step runText()
    {
        unsigned count = op->target;
        unsigned available = length - position;
        return consumeIf(available >= count && textMatches(input + position, program.text.span().data() + op->argument, count, available), count);
    }

    ALWAYS_INLINE Step runCasedText()
    {
        unsigned count = op->target;
        unsigned available = length - position;
        return consumeIf(available >= count && casedTextMatches(input + position, program.text.span().data() + op->argument, count, available), count);
    }

    ALWAYS_INLINE Step runWideText()
    {
        unsigned count = op->target;
        if (sizeof(CharType) == 1 || length - position < count)
            return Step::Failed;
        const char16_t* expected = program.wideText.span().data() + op->argument;
        for (unsigned i = 0; i < count; ++i) {
            if (input[position + i] != expected[i])
                return Step::Failed;
        }
        position += count;
        return Step::Next;
    }

    ALWAYS_INLINE bool consumeMembers(const CharacterSetBits& set, unsigned count)
    {
        for (unsigned i = 0; i < count; ++i) {
            if (!contains(set, input[position + i]))
                return false;
        }
        position += count;
        return true;
    }

    ALWAYS_INLINE Step runSetCount() { return nextIf(length - position >= op->argument && consumeMembers(characterSets[op->set], op->argument)); }

    ALWAYS_INLINE std::optional<unsigned> consumeGreedyRun()
    {
        const CharacterSetBits& set = characterSets[op->set];
        unsigned available = length - position;
        if (available < op->needed)
            return std::nullopt;
        available -= op->needed;
        unsigned end = available > op->argument ? position + op->argument : position + available;
        unsigned begin = position;
        while (position < end && contains(set, input[position]))
            ++position;
        if (position - begin < op->target)
            return std::nullopt;
        return begin + op->target;
    }

    ALWAYS_INLINE Step runGreedyRun()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        std::optional<unsigned> least = consumeGreedyRun();
        if (!least)
            return Step::Failed;
        if (position > *least)
            push(position, *least, 0);
        return Step::Next;
    }

    ALWAYS_INLINE Step runGreedyRunBeforeKnownCharacters()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        std::optional<unsigned> least = consumeGreedyRun();
        if (!least)
            return Step::Failed;
        const FirstSetBits& following = firstSets[op->firstSet];
        while (position > *least && !(position == length ? following.atEnd : contains(following, input[position])))
            --position;
        if (position > *least)
            push(position, *least, 0);
        return Step::Next;
    }

    ALWAYS_INLINE bool consumeLazyRunMinimum()
    {
        unsigned available = length - position;
        return available >= op->needed && available - op->needed >= op->target && consumeMembers(characterSets[op->set], op->target);
    }

    ALWAYS_INLINE Step runLazyRun()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        if (!consumeLazyRunMinimum())
            return Step::Failed;
        push(position, op->target, length - op->needed);
        return Step::Next;
    }

    ALWAYS_INLINE Step runLazyRunBeforeKnownCharacters()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        if (!consumeLazyRunMinimum())
            return Step::Failed;
        const CharacterSetBits& set = characterSets[op->set];
        const FirstSetBits& following = firstSets[op->firstSet];
        unsigned count = op->target;
        while (position < length && !contains(following, input[position])) {
            if (count == op->argument || length - position <= op->needed || !contains(set, input[position]))
                return Step::Failed;
            ++count;
            ++position;
        }
        push(position, count, length - op->needed);
        return Step::Next;
    }

    ALWAYS_INLINE Step runPossessiveRun()
    {
        const CharacterSetBits& set = characterSets[op->set];
        unsigned end = length - position > op->argument ? position + op->argument : length;
        unsigned begin = position;
        while (position < end && contains(set, input[position]))
            ++position;
        return nextIf(position - begin >= op->target);
    }

    ALWAYS_INLINE std::optional<unsigned> consumeCapturedRun()
    {
        const CharacterSetBits& set = characterSets[op->set];
        unsigned begin = position;
        while (position < length && contains(set, input[position]))
            ++position;
        if (position - begin < op->target)
            return std::nullopt;
        return begin;
    }

    ALWAYS_INLINE Step runCapturedRun()
    {
        std::optional<unsigned> begin = consumeCapturedRun();
        if (!begin)
            return Step::Failed;
        output[op->argument] = *begin;
        output[op->argument + 1] = position;
        return Step::Next;
    }

    ALWAYS_INLINE Step runChoice()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        push(position, 0, 0);
        return Step::Next;
    }

    ALWAYS_INLINE Step runGreedyLoopChoice()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        push(position, registers()[op->registerIndex], 0);
        registers()[op->registerIndex] = position;
        return Step::Next;
    }

    ALWAYS_INLINE bool canBeginHere(const FirstSetBits& first) const
    {
        if (position >= length)
            return first.atEnd;
        if (sizeof(CharType) > 1 && input[position] > 255)
            return true;
        return contains(first, input[position]);
    }

    ALWAYS_INLINE Step runChoiceByFirst()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        if (!canBeginHere(firstSets[op->firstSet]))
            return jumpTo(op->target);
        if (canBeginHere(firstSets[op->laterFirstSet]))
            push(position, 0, 0);
        return Step::Next;
    }

    ALWAYS_INLINE Step runChoiceByTable()
    {
        uint32_t entry = FlatProgram::thisAndLaterAlternativesBeginHere;
        if (position >= length)
            entry = dispatchEntries[op->argument + dispatchClasses[op->needed + 256]];
        else if (sizeof(CharType) == 1 || input[position] <= 255)
            entry = dispatchEntries[op->argument + dispatchClasses[op->needed + static_cast<unsigned>(input[position])]];
        if (entry == FlatProgram::onlyThisAlternativeBeginsHere)
            return Step::Next;
        if (entry == FlatProgram::thisAndLaterAlternativesBeginHere) {
            if (!tryReserveFrames(1))
                return Step::NeedsFrames;
            push(position, 0, 0);
            return Step::Next;
        }
        if (entry == FlatProgram::noAlternativeBeginsHere)
            return Step::Failed;
        return jumpTo(entry - FlatProgram::firstDispatchTarget);
    }

    ALWAYS_INLINE Step runJump() { return jumpTo(op->target); }
    ALWAYS_INLINE Step runJumpIfBelow() { return jumpIf(registers()[op->registerIndex] < op->argument); }
    ALWAYS_INLINE Step runJumpIfNotBelow() { return jumpIf(registers()[op->registerIndex] >= op->argument); }
    ALWAYS_INLINE Step runNeedsInputElseJump() { return jumpIf(length - position < op->needed); }

    ALWAYS_INLINE Step runCaptureStart()
    {
        output[op->argument] = position;
        return Step::Next;
    }

    ALWAYS_INLINE Step runCaptureEnd() { return runCaptureStart(); }

    ALWAYS_INLINE Step runCaptureEmpty()
    {
        output[op->argument] = position;
        output[op->argument + 1] = position;
        return Step::Next;
    }

    ALWAYS_INLINE Step runClearCaptures()
    {
        for (unsigned i = op->argument << 1, end = (op->target << 1) + 2; i < end; ++i)
            output[i] = offsetNoMatch;
        return Step::Next;
    }

    ALWAYS_INLINE Step runSavePosition()
    {
        registers()[op->registerIndex] = position;
        return Step::Next;
    }

    ALWAYS_INLINE bool isAtWordBoundary() const
    {
        const CharacterSetBits& set = characterSets[op->set];
        bool before = position && contains(set, input[position - 1]);
        bool after = position < length && contains(set, input[position]);
        return before != after;
    }

    ALWAYS_INLINE Step runFailIfEmpty() { return nextIf(registers()[op->registerIndex] != position); }
    ALWAYS_INLINE Step runNeedsInput() { return nextIf(length - position >= op->needed); }
    ALWAYS_INLINE Step runInputStart() { return nextIf(!position); }
    ALWAYS_INLINE Step runLineStart() { return nextIf(!position || contains(characterSets[op->set], input[position - 1])); }
    ALWAYS_INLINE Step runInputEnd() { return nextIf(position == length); }
    ALWAYS_INLINE Step runLineEnd() { return nextIf(position == length || contains(characterSets[op->set], input[position])); }
    ALWAYS_INLINE Step runWordBoundary() { return nextIf(isAtWordBoundary()); }
    ALWAYS_INLINE Step runNotWordBoundary() { return nextIf(!isAtWordBoundary()); }

    ALWAYS_INLINE Step runMatch()
    {
        output[0] = matchStart;
        output[1] = position;
        return Step::Matched;
    }

    ALWAYS_INLINE Step runLoggedCaptureStart()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        if (top != bottom)
            push(output[op->argument], 0, 0);
        output[op->argument] = position;
        return Step::Next;
    }

    ALWAYS_INLINE Step runLoggedCaptureEnd() { return runLoggedCaptureStart(); }

    ALWAYS_INLINE Step runLoggedCaptureEmpty()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        if (top != bottom)
            push(op->argument >> 1, output[op->argument], output[op->argument + 1]);
        output[op->argument] = position;
        output[op->argument + 1] = position;
        return Step::Next;
    }

    ALWAYS_INLINE Step runLoggedCapturedRun()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        std::optional<unsigned> begin = consumeCapturedRun();
        if (!begin)
            return Step::Failed;
        if (top != bottom)
            push(op->argument >> 1, output[op->argument], output[op->argument + 1]);
        output[op->argument] = *begin;
        output[op->argument + 1] = position;
        return Step::Next;
    }

    ALWAYS_INLINE Step runLoggedClearCaptures()
    {
        if (!tryReserveFrames(op->target - op->argument + 1))
            return Step::NeedsFrames;
        for (unsigned i = op->argument; i <= op->target; ++i) {
            if (output[i << 1] == offsetNoMatch && output[(i << 1) + 1] == offsetNoMatch)
                continue;
            if (top != bottom)
                push(i, output[i << 1], output[(i << 1) + 1]);
            output[i << 1] = offsetNoMatch;
            output[(i << 1) + 1] = offsetNoMatch;
        }
        return Step::Next;
    }

    ALWAYS_INLINE Step logAndSetRegister(uint32_t value)
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        if (top != bottom)
            push(registers()[op->registerIndex], 0, 0);
        registers()[op->registerIndex] = value;
        return Step::Next;
    }

    ALWAYS_INLINE Step runLoggedSavePosition() { return logAndSetRegister(position); }
    ALWAYS_INLINE Step runLoggedSetRegister() { return logAndSetRegister(op->argument); }
    ALWAYS_INLINE Step runLoggedIncrement() { return logAndSetRegister(registers()[op->registerIndex] + 1); }

    ALWAYS_INLINE Step runLoopBack()
    {
        if (!--remainingSteps) [[unlikely]]
            return Step::HitLimit;
        return jumpTo(op->target);
    }

    ALWAYS_INLINE Step runCountTurn()
    {
        remainingSteps = matchLimit;
        return --program.remainingTurns ? Step::Next : Step::HitLimit;
    }

    ALWAYS_INLINE Step runLoopBackUnlessEmpty()
    {
        if (registers()[op->registerIndex] == position)
            return Step::Failed;
        return runLoopBack();
    }

    ALWAYS_INLINE Step runLookaheadBegin()
    {
        registers()[op->registerIndex] = position;
        registers()[op->argument] = static_cast<uint32_t>(top - bottom);
        return Step::Next;
    }

    ALWAYS_INLINE Step runLookaheadEnd()
    {
        position = registers()[op->registerIndex];
        top = bottom + registers()[op->argument];
        return Step::Next;
    }

    ALWAYS_INLINE Step runLookaheadEndKeepingCaptures()
    {
        position = registers()[op->registerIndex];
        FlatFrame* newTop = bottom + registers()[op->argument];
        for (FlatFrame* frame = newTop; frame < top; ++frame) {
            if (restoresCapture(opOf(*frame)->kind))
                *newTop++ = *frame;
        }
        top = newTop;
        return Step::Next;
    }

    ALWAYS_INLINE Step runNegativeLookaheadBegin()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        registers()[op->argument] = static_cast<uint32_t>(top - bottom);
        push(position, 0, 0);
        return Step::Next;
    }

    ALWAYS_INLINE Step runNegativeLookaheadEnd()
    {
        FlatFrame* base = bottom + registers()[op->argument];
        while (top > base) {
            --top;
            undo(*opOf(*top), *top);
        }
        return Step::Failed;
    }

    static ALWAYS_INLINE bool areEqualIgnoringCase(char32_t expected, char32_t actual)
    {
        if (expected == actual)
            return true;
        if (isASCII(expected) || isASCII(actual))
            return toASCIIUpper(expected) == toASCIIUpper(actual);
        return areCanonicallyEquivalent(expected, actual, CanonicalMode::UCS2);
    }

    template<bool ignoresCase>
    ALWAYS_INLINE Step consumeCapturedText()
    {
        unsigned begin = output[op->argument];
        unsigned end = output[op->argument + 1];
        if (begin == offsetNoMatch || end == offsetNoMatch)
            return Step::Next;
        unsigned count = end - begin;
        if (length - position < count)
            return Step::Failed;
        for (unsigned i = 0; i < count; ++i) {
            if (ignoresCase ? !areEqualIgnoringCase(input[begin + i], input[position + i]) : input[begin + i] != input[position + i])
                return Step::Failed;
        }
        position += count;
        return Step::Next;
    }

    ALWAYS_INLINE Step runBackReference() { return consumeCapturedText<false>(); }
    ALWAYS_INLINE Step runBackReferenceIgnoringCase() { return consumeCapturedText<true>(); }

    ALWAYS_INLINE Step resumeChoice()
    {
        const FlatFrame& frame = *--top;
        position = frame.position;
        return jumpTo(opOf(frame)->target);
    }

    ALWAYS_INLINE Step resumeChoiceByFirst() { return resumeChoice(); }
    ALWAYS_INLINE Step resumeChoiceByTable() { return resumeChoice(); }
    ALWAYS_INLINE Step resumeNegativeLookaheadBegin() { return resumeChoice(); }

    ALWAYS_INLINE Step resumeGreedyLoopChoice()
    {
        const FlatFrame& frame = *--top;
        const FlatOp* from = opOf(frame);
        position = frame.position;
        registers()[from->registerIndex] = frame.limit;
        return jumpTo(from->target);
    }

    ALWAYS_INLINE Step resumeGreedyRun()
    {
        FlatFrame* frame = top - 1;
        const FlatOp* from = opOf(*frame);
        position = --frame->position;
        if (position == frame->limit)
            --top;
        op = from;
        return Step::Next;
    }

    ALWAYS_INLINE Step resumeGreedyRunBeforeKnownCharacters()
    {
        FlatFrame* frame = top - 1;
        const FlatOp* from = opOf(*frame);
        position = frame->position - 1;
        while (position > frame->limit && !contains(firstSets[from->firstSet], input[position]))
            --position;
        frame->position = position;
        if (position == frame->limit)
            --top;
        op = from;
        return Step::Next;
    }

    ALWAYS_INLINE Step resumeLazyRunBeforeKnownCharacters()
    {
        FlatFrame* frame = top - 1;
        const FlatOp* from = opOf(*frame);
        position = frame->position;
        do {
            if (frame->limit == from->argument || position >= frame->extra || !contains(characterSets[from->set], input[position])) {
                --top;
                return Step::Failed;
            }
            ++frame->limit;
            ++position;
        } while (position < length && !contains(firstSets[from->firstSet], input[position]));
        frame->position = position;
        op = from;
        return Step::Next;
    }

    ALWAYS_INLINE Step resumeLazyRun()
    {
        FlatFrame* frame = top - 1;
        const FlatOp* from = opOf(*frame);
        if (frame->limit < from->argument && frame->position < frame->extra && contains(characterSets[from->set], input[frame->position])) {
            ++frame->limit;
            position = ++frame->position;
            op = from;
            return Step::Next;
        }
        --top;
        return Step::Failed;
    }

    ALWAYS_INLINE Step resumeLogged()
    {
        const FlatFrame& frame = *--top;
        undo(*opOf(frame), frame);
        return Step::Failed;
    }

    ALWAYS_INLINE Step resumeLoggedCaptureStart() { return resumeLogged(); }
    ALWAYS_INLINE Step resumeLoggedCaptureEnd() { return resumeLogged(); }
    ALWAYS_INLINE Step resumeLoggedClearCaptures() { return resumeLogged(); }
    ALWAYS_INLINE Step resumeLoggedCaptureEmpty() { return resumeLogged(); }
    ALWAYS_INLINE Step resumeLoggedCapturedRun() { return resumeLogged(); }
    ALWAYS_INLINE Step resumeLoggedSavePosition() { return resumeLogged(); }
    ALWAYS_INLINE Step resumeLoggedSetRegister() { return resumeLogged(); }
    ALWAYS_INLINE Step resumeLoggedIncrement() { return resumeLogged(); }

    ALWAYS_INLINE void undo(const FlatOp& from, const FlatFrame& frame)
    {
        switch (from.kind) {
        case Kind::LoggedCaptureStart:
        case Kind::LoggedCaptureEnd:
            output[from.argument] = frame.position;
            return;
        case Kind::LoggedCaptureEmpty:
        case Kind::LoggedCapturedRun:
        case Kind::LoggedClearCaptures:
            output[frame.position << 1] = frame.limit;
            output[(frame.position << 1) + 1] = frame.extra;
            return;
        case Kind::LoggedSavePosition:
        case Kind::LoggedSetRegister:
        case Kind::LoggedIncrement:
            registers()[from.registerIndex] = frame.position;
            return;
        case Kind::GreedyLoopChoice:
            registers()[from.registerIndex] = frame.limit;
            return;
        case Kind::Choice:
        case Kind::ChoiceByFirst:
        case Kind::ChoiceByTable:
        case Kind::NegativeLookaheadBegin:
        case Kind::GreedyRun:
        case Kind::LazyRun:
        case Kind::GreedyRunBeforeKnownCharacters:
        case Kind::LazyRunBeforeKnownCharacters:
            return;
        FOR_EACH_FLAT_OP_WITHOUT_FRAME(FLAT_OP_CASE_LABEL)
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    static ALWAYS_INLINE bool restoresCapture(Kind kind)
    {
        switch (kind) {
        case Kind::LoggedCaptureStart:
        case Kind::LoggedCaptureEnd:
        case Kind::LoggedCaptureEmpty:
        case Kind::LoggedCapturedRun:
        case Kind::LoggedClearCaptures:
            return true;
        case Kind::Choice:
        case Kind::ChoiceByFirst:
        case Kind::ChoiceByTable:
        case Kind::GreedyLoopChoice:
        case Kind::NegativeLookaheadBegin:
        case Kind::GreedyRun:
        case Kind::LazyRun:
        case Kind::GreedyRunBeforeKnownCharacters:
        case Kind::LazyRunBeforeKnownCharacters:
        case Kind::LoggedSavePosition:
        case Kind::LoggedSetRegister:
        case Kind::LoggedIncrement:
            return false;
        FOR_EACH_FLAT_OP_WITHOUT_FRAME(FLAT_OP_CASE_LABEL)
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ALWAYS_INLINE Step runOp()
    {
        switch (op->kind) {
#define FLAT_OP_CASE(name) case Kind::name: return run##name();
        FOR_EACH_FLAT_OP(FLAT_OP_CASE)
#undef FLAT_OP_CASE
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ALWAYS_INLINE Step resumeTopFrame()
    {
        switch (opOf(top[-1])->kind) {
#define FLAT_OP_CASE(name) case Kind::name: return resume##name();
        FOR_EACH_FLAT_OP_PUSHING_FRAME(FLAT_OP_CASE)
#undef FLAT_OP_CASE
        FOR_EACH_FLAT_OP_WITHOUT_FRAME(FLAT_OP_CASE_LABEL)
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ALWAYS_INLINE Step backtrack()
    {
        for (;;) {
            if (top == bottom) {
                if (program.triesOnlyOnce || matchStart >= length)
                    return Step::NoMatch;
                ++matchStart;
                for (unsigned i = 2; i < slots; ++i)
                    output[i] = offsetNoMatch;
                return beginAttempt() ? Step::Jumped : Step::NoMatch;
            }
            if (!--remainingSteps) [[unlikely]]
                return Step::HitLimit;
            Step step = resumeTopFrame();
            if (step != Step::Failed)
                return step;
        }
    }

    const FlatProgram& program;
    const FlatOp* const ops;
    const CharacterSetBits* const characterSets;
    const FirstSetBits* const firstSets;
    const uint16_t* const dispatchClasses;
    const uint32_t* const dispatchEntries;
    const std::span<const CharacterRange> wideRanges;
    const CharType* const input;
    const unsigned length;
    unsigned* const output;
    const unsigned slots;
    const size_t framesOfRegisters;
    unsigned matchStart;
    unsigned remainingSteps { 0 };

    const FlatOp* op { nullptr };
    unsigned position { 0 };
    FlatFrame* top { nullptr };

    FlatFrame* storage { nullptr };
    FlatFrame* storageEnd { nullptr };
    FlatFrame* bottom { nullptr };
    size_t framesNeeded { 0 };
};

} // anonymous namespace

JSRegExpResult matchFlatProgram(const FlatProgram& program, std::span<const Latin1Character> input, unsigned start, unsigned* output)
{
    return Machine<Latin1Character>(program, input, start, output).run();
}

JSRegExpResult matchFlatProgram(const FlatProgram& program, StringView input, unsigned start, unsigned* output)
{
    SuperSamplerScope superSamplerScope(false);
    if (input.is8Bit())
        return matchFlatProgram(program, input.span8(), start, output);
    return Machine<char16_t>(program, input.span16(), start, output).run();
}

void validateFlatProgramResult(const FlatProgram& program, StringView input, unsigned start, RegExpMatchOutcome flat, RegExpMatchOutcome interpreter)
{
    auto isMatchOrNoMatch = [](JSRegExpResult result) {
        return result == JSRegExpResult::Match || result == JSRegExpResult::NoMatch;
    };
    if (!isMatchOrNoMatch(flat.result) || !isMatchOrNoMatch(interpreter.result))
        return;
    bool same = flat.result == interpreter.result;
    for (unsigned i = 0; same && flat.result == JSRegExpResult::Match && i <= program.numSubpatterns; ++i) {
        same = flat.output[i << 1] == interpreter.output[i << 1];
        if (same && interpreter.output[i << 1] != offsetNoMatch)
            same = flat.output[(i << 1) + 1] == interpreter.output[(i << 1) + 1];
    }
    if (!same) {
        dataLogLn("validateRegExpFlatProgram: results differ at start ", start, ": flat program ", static_cast<int>(flat.result), " interpreter ", static_cast<int>(interpreter.result));
        for (unsigned i = 0; i <= program.numSubpatterns; ++i)
            dataLogLn("  group ", i, ": flat program ", static_cast<int>(flat.output[i << 1]), ",", static_cast<int>(flat.output[(i << 1) + 1]), " interpreter ", static_cast<int>(interpreter.output[i << 1]), ",", static_cast<int>(interpreter.output[(i << 1) + 1]));
        dataLogLn("  subject: ", input);
        CRASH();
    }
}

#undef FLAT_OP_CASE_LABEL

} }

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
