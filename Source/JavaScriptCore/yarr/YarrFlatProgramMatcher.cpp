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
#include <wtf/SIMDHelpers.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringCommon.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Yarr {

#define FLAT_OP_CASE_LABEL(name) case FlatOp::Kind::name:

namespace {

// Next: continue at op + 1. Jumped: op is already the next op to run.
enum class Step : uint8_t { Next, Jumped, Failed, Matched, NoMatch, HitLimit, NeedsFrames, AtEnclosure };

template<typename CharType>
static ALWAYS_INLINE bool textMatches(const CharType* input, const Latin1Character* expected, unsigned count, unsigned available)
{
    if constexpr (sizeof(CharType) == 1) {
        constexpr unsigned wordSize = sizeof(uint64_t);
        if (count <= wordSize) {
            unsigned unused = (wordSize - count) * 8;
            if (available >= wordSize) [[likely]]
                return !((WTF::unalignedLoad<uint64_t>(input) ^ WTF::unalignedLoad<uint64_t>(expected)) << unused);
            // The end of the subject is near: its first and its last four, or two, which may overlap.
            auto sameAtBothEnds = [&]<typename Part>(Part) {
                return WTF::unalignedLoad<Part>(input) == WTF::unalignedLoad<Part>(expected) && WTF::unalignedLoad<Part>(input + count - sizeof(Part)) == WTF::unalignedLoad<Part>(expected + count - sizeof(Part));
            };
            if (count >= sizeof(uint32_t))
                return sameAtBothEnds(uint32_t { });
            if (count >= sizeof(uint16_t))
                return sameAtBothEnds(uint16_t { });
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
            unsigned unused = (wordSize - count) * 8;
            if (available >= wordSize) [[likely]]
                return !(differs(0) << unused);
            auto sameAtBothEnds = [&]<typename Part>(Part) {
                auto same = [&](unsigned at) {
                    return static_cast<Part>(WTF::unalignedLoad<Part>(input + at) | WTF::unalignedLoad<Part>(mask + at)) == WTF::unalignedLoad<Part>(expected + at);
                };
                return same(0) && same(count - sizeof(Part));
            };
            if (count >= sizeof(uint32_t))
                return sameAtBothEnds(uint32_t { });
            if (count >= sizeof(uint16_t))
                return sameAtBothEnds(uint16_t { });
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
        auto frames = this->frames();
        size_t newCapacity = frames.size() * 2;
        while (newCapacity - used < needed)
            newCapacity *= 2;
        Vector<FlatFrame> larger;
        if (newCapacity * sizeof(FlatFrame) > Options::maxRegExpStackSize() || !larger.tryGrow(newCapacity))
            return false;
        memcpySpan(larger.mutableSpan().first(used), frames.first(used));
        overflow = WTF::move(larger);
        return true;
    }

    std::span<FlatFrame> frames() { return overflow.isEmpty() ? std::span<FlatFrame> { inlineStorage } : overflow.mutableSpan(); }

    FlatFrame inlineStorage[inlineFrames];
    Vector<FlatFrame> overflow;
};

template<typename CharType>
static ALWAYS_INLINE bool isInFirstSet(const FirstSetBits& set, CharType character)
{
    if (sizeof(CharType) > 1 && character > 255)
        return false;
    return set.bits[static_cast<uint8_t>(character) >> 6] & (1ull << (character & 63));
}

// At which of sixteen characters from `begin` on a match can begin by FlatProgram::startTable.
struct StartCandidates {
    static constexpr unsigned stride = SIMD::stride<uint8_t>;
    static constexpr unsigned bitsPerCharacter = SIMD::bitsPerLaneInMask<uint8_t>;
    static constexpr uint64_t oneBitPerCharacter = ~0ull / ((1ull << bitsPerCharacter) - 1);

    // The first from `from` on.
    ALWAYS_INLINE std::optional<unsigned> find(unsigned from) const
    {
        if (from - begin >= stride)
            return std::nullopt;
        uint64_t rest = mask >> ((from - begin) * bitsPerCharacter);
        if (!rest)
            return std::nullopt;
        return from + std::countr_zero(rest) / bitsPerCharacter;
    }

    unsigned begin { std::numeric_limits<unsigned>::max() - stride };
    uint64_t mask { 0 };
};

// The first place from `from` on where a match can begin by the tables. Past the end: none.
template<typename CharType>
static ALWAYS_INLINE unsigned findStartByTables(const FlatProgram& program, const CharType* input, unsigned length, unsigned from, StartCandidates& candidates)
{
    if (auto found = candidates.find(from))
        return *found;
    unsigned none = length + 1;
    if (!from && program.mayMatchOnlyAtInputStart)
        return 0;
    if (program.startScan == FlatStartScan::OneCharacter) {
        size_t found = WTF::find(std::span { input, length }, static_cast<CharType>(program.onlyStartCharacter), from);
        return found == notFound ? none : found;
    }
    unsigned depth = program.startTableDepth;
    if (length < depth)
        return none;
    unsigned last = length - depth;
    auto entryOf = [&](unsigned at) -> unsigned {
        unsigned character = input[at];
        return program.startTable[sizeof(CharType) == 1 || character < 256 ? character : 256];
    };
    // Behind a surrogate pair the places of a match are no longer those of the subject: the attempt is made.
    auto canBeginAt = [&](unsigned at) {
        for (unsigned place = 0; place < depth; ++place) {
            if (sizeof(CharType) > 1 && U16_IS_SURROGATE(input[at + place]) && program.decodesSurrogatePairs)
                return true;
            if (!(entryOf(at + place) >> place & 1))
                return false;
        }
        return true;
    };
    if constexpr (sizeof(CharType) == 1) {
        constexpr unsigned stride = StartCandidates::stride;
        static_assert(FlatProgram::maxStartTableDepth == 4 && FlatProgram::maxStartTableDepth <= stride);
        auto lowBits = SIMD::load(program.startTableOfLowBits.data());
        auto highBits = SIMD::load(program.startTableOfHighBits.data());
        auto placesOf = [&](const uint8_t* characters) {
            auto sixteen = SIMD::load(characters);
            return simde_vandq_u8(simde_vqtbl1q_u8(lowBits, simde_vandq_u8(sixteen, simde_vdupq_n_u8(0xf))), simde_vqtbl1q_u8(highBits, simde_vshrq_n_u8(sixteen, 4)));
        };
        auto nextPlaces = lowBits;
        unsigned nextPlacesBegin = none;
        if (from - candidates.begin < stride)
            from = candidates.begin + stride;
        for (;; from += stride) {
            if (from > last)
                return none;
            const uint8_t* characters = std::bit_cast<const uint8_t*>(input + from);
            uint8_t endOfInput[2 * stride] { };
            if (length - from < 2 * stride) {
                memcpy(endOfInput, characters, length - from);
                characters = endOfInput;
            }
            auto places = nextPlacesBegin == from ? nextPlaces : placesOf(characters);
            nextPlaces = placesOf(characters + stride);
            nextPlacesBegin = from + stride;
            auto found = simde_vandq_u8(places, simde_vdupq_n_u8(1));
            found = simde_vorrq_u8(found, simde_vandq_u8(simde_vextq_u8(places, nextPlaces, 1), simde_vdupq_n_u8(2)));
            found = simde_vorrq_u8(found, simde_vandq_u8(simde_vextq_u8(places, nextPlaces, 2), simde_vdupq_n_u8(4)));
            found = simde_vorrq_u8(found, simde_vandq_u8(simde_vextq_u8(places, nextPlaces, 3), simde_vdupq_n_u8(8)));
            uint64_t mask = SIMD::laneMask(simde_vceqq_u8(found, simde_vdupq_n_u8(0xf))) & StartCandidates::oneBitPerCharacter;
            if (last - from < stride - 1)
                mask &= (1ull << ((last - from + 1) * StartCandidates::bitsPerCharacter)) - 1;
            for (uint64_t rest = mask; rest; rest &= rest - 1) {
                if (!canBeginAt(from + std::countr_zero(rest) / StartCandidates::bitsPerCharacter))
                    mask &= ~(rest & -rest);
            }
            if (!mask)
                continue;
            candidates.begin = from;
            candidates.mask = mask;
            return from + std::countr_zero(mask) / StartCandidates::bitsPerCharacter;
        }
    }
    for (; from <= last; ++from) {
        if (canBeginAt(from))
            return from;
    }
    return none;
}

struct StartSearch {
    StartCandidates candidates { };
    RequiredTextPlaces requiredTextPlaces { };
};

// The first place from `from` on where a match can begin. Past the end: none. Out of line so that its loops do not take registers from Machine::run().
template<typename CharType>
static NEVER_INLINE unsigned findStartOfMatch(const FlatProgram& program, const CharType* input, unsigned length, unsigned from, StartSearch& search)
{
    if (program.requiredTexts.isEmpty())
        return findStartByTables(program, input, length, from, search.candidates);
    for (;;) {
        from = nextStartWithRequiredTexts(program.requiredTexts, std::span { input, length }, from, search.requiredTextPlaces);
        if (from > length || program.startScan == FlatStartScan::RequiredTextsAlone)
            return from;
        unsigned next = findStartByTables(program, input, length, from, search.candidates);
        if (next == from || next > length)
            return next;
        from = next;
    }
}

// The characters around the last one from 256 up that was looked for in a set, as far as the set treats them alike: the letters of one script lie side by side.
struct WideNeighbors {
    template<typename Character>
    ALWAYS_INLINE bool contains(std::span<const CharacterRange> wideRanges, const CharacterSetBits& wanted, Character character)
    {
        if (set != &wanted || character < first || character > last) [[unlikely]]
            find(wideRanges, wanted, character);
        return areInSet;
    }

    template<typename Character>
    NEVER_INLINE void find(std::span<const CharacterRange> wideRanges, const CharacterSetBits& wanted, Character character)
    {
        auto ranges = wideRanges.subspan(wanted.firstWideRange, wanted.wideRangeCount);
        auto next = std::ranges::upper_bound(ranges, character, { }, &CharacterRange::begin);
        bool isInRange = next != ranges.begin() && character <= (next - 1)->end;
        set = &wanted;
        first = next == ranges.begin() ? 0x100 : isInRange ? (next - 1)->begin : (next - 1)->end + 1;
        last = isInRange ? (next - 1)->end : next == ranges.end() ? 0x10ffff : next->begin - 1;
        areInSet = isInRange != wanted.invertsWideRanges;
    }

    const CharacterSetBits* set { nullptr };
    char32_t first { 0 };
    char32_t last { 0 };
    bool areInSet { false };
};

// As Interpreter::tryConsumeBackReference(). Only outside unicode mode is a character from 128 up never the same as an ASCII one.
static ALWAYS_INLINE bool areEqualIgnoringCase(char32_t expected, char32_t actual, bool isUnicodeMode)
{
    if (expected == actual)
        return true;
    if (!isUnicodeMode && (isASCII(expected) || isASCII(actual)))
        return toASCIIUpper(expected) == toASCIIUpper(actual);
    return areCanonicallyEquivalent(expected, actual, isUnicodeMode ? CanonicalMode::Unicode : CanonicalMode::UCS2);
}

static constexpr char32_t noCharacter = std::numeric_limits<char32_t>::max();

// As Interpreter::InputStream::reread(): see FlatProgram::decodesSurrogatePairs.
static NEVER_INLINE char32_t characterAtSurrogate(const char16_t* input, unsigned length, unsigned at)
{
    char32_t unit = input[at];
    if (U16_IS_SURROGATE_LEAD(unit))
        return at + 1 < length && U16_IS_TRAIL(input[at + 1]) ? U16_GET_SUPPLEMENTARY(unit, input[at + 1]) : unit;
    return at && U16_IS_LEAD(input[at - 1]) ? noCharacter : unit;
}

// See FlatProgram::referencedGroups.
static NEVER_INLINE bool holdsBoundsOfReferencedGroups(const FlatProgram& program, const uint32_t* bounds, const unsigned* output)
{
    for (unsigned group : program.referencedGroups) {
        if (*bounds++ != output[group] || *bounds++ != output[group + 1])
            return false;
    }
    return true;
}

static NEVER_INLINE void storeBoundsOfReferencedGroups(const FlatProgram& program, uint32_t* bounds, const unsigned* output)
{
    for (unsigned group : program.referencedGroups) {
        *bounds++ = output[group];
        *bounds++ = output[group + 1];
    }
}

// What a match seldom uses. Separate from Machine for the same reason as FrameStorage.
template<typename CharType>
struct Enclosure {
    static constexpr unsigned lineStartIsUnsatisfiable = std::numeric_limits<unsigned>::max();

    // As Interpreter::matchDotStarEnclosure().
    NEVER_INLINE bool widen(const FlatProgram& program, const FlatOp& op, const CharType* characters, unsigned expressionBegin, unsigned expressionEnd, unsigned* output)
    {
        std::span input { characters, length };
        const CharacterSetBits& newlines = program.characterSets[op.set];
        auto wideRanges = program.wideRanges.span();
        bool isMultiline = op.argument & FlatProgram::enclosureIsMultiline;
        bool beginsLine = op.argument & FlatProgram::enclosureBeginsLine;
        auto isNewline = [&](unsigned at) {
            return contains(newlines, wideRanges, input[at]);
        };
        auto isLineStart = [&](unsigned at) {
            return !at || (isMultiline && isNewline(at - 1));
        };
        auto rejectForLineStart = [&] {
            if (!isMultiline)
                noNewlineBefore = lineStartIsUnsatisfiable;
            return false;
        };
        if (noNewlineBefore == lineStartIsUnsatisfiable)
            return false;

        unsigned begin = startOffset;
        unsigned end = input.size();
        if (op.argument & FlatProgram::enclosureDotMatchesAll) {
            if (beginsLine && !isLineStart(begin)) {
                if (!isMultiline)
                    return rejectForLineStart();
                begin = std::max(begin, noNewlineBefore);
                while (begin < expressionBegin && !isNewline(begin))
                    ++begin;
                if (begin >= expressionBegin) {
                    noNewlineBefore = std::max(noNewlineBefore, expressionBegin);
                    return false;
                }
                ++begin;
            }
        } else {
            unsigned bound = std::max(startOffset, noNewlineBefore);
            if (expressionBegin > bound) {
                for (begin = expressionBegin - 1; !isNewline(begin); --begin) {
                    if (begin == bound) {
                        noNewlineBefore = expressionBegin;
                        begin = startOffset - 1;
                        break;
                    }
                }
                ++begin;
            }
            if (beginsLine && !isLineStart(begin))
                return rejectForLineStart();
            end = expressionEnd;
            while (end != input.size() && !isNewline(end))
                ++end;
            if ((op.argument & FlatProgram::enclosureEndsLine) && end != input.size() && !isMultiline)
                return false;
        }
        output[0] = begin;
        output[1] = end;
        return true;
    }

    // A step of a lookbehind stands behind a Choice that goes on behind the step, and always fails. So the search goes on from the frame of the Choice: at the position that the step has written there,
    // or, where the step cannot be made, not at all.
    NEVER_INLINE void readBackward(const FlatProgram& program, const FlatOp& op, const CharType* input, const unsigned* output, FlatFrame& frame)
    {
        unsigned reached = positionBehindStepBackward(program, op, input, frame.position, output);
        if (reached != cannotReadBackward) {
            frame.position = reached;
            return;
        }
        frame.op = program.recordFailureOp * sizeof(FlatOp);
        frame.extra = recordsNothing;
    }

    static constexpr unsigned cannotReadBackward = std::numeric_limits<unsigned>::max();
    static constexpr uint32_t recordsNothing = std::numeric_limits<uint32_t>::max();

    unsigned positionBehindStepBackward(const FlatProgram& program, const FlatOp& op, const CharType* input, unsigned position, const unsigned* output)
    {
        switch (static_cast<FlatStepBackward>(op.registerIndex)) {
        case FlatStepBackward::NeedsInput:
            return position >= op.needed ? position : cannotReadBackward;
        case FlatStepBackward::Set: {
            // As Interpreter::InputStream::tryReadBackward(): what is read ends at the position even where that lies inside a surrogate pair.
            if (!position)
                return cannotReadBackward;
            char32_t character = input[--position];
            if constexpr (sizeof(CharType) > 1) {
                if (U16_IS_TRAIL(character) && program.decodesSurrogatePairs && position && U16_IS_LEAD(input[position - 1]))
                    character = U16_GET_SUPPLEMENTARY(static_cast<char32_t>(input[--position]), character);
            }
            const CharacterSetBits& set = program.characterSets[op.set];
            bool isMember = character > 0xff ? wideRangesContain(program.wideRanges.span(), set, character) : contains(set, program.wideRanges.span(), static_cast<Latin1Character>(character));
            return isMember ? position : cannotReadBackward;
        }
        case FlatStepBackward::BackReference: {
            unsigned begin = output[op.argument];
            unsigned end = output[op.argument + 1];
            if (begin == offsetNoMatch || end == offsetNoMatch)
                return position;
            unsigned count = end - begin;
            if (position < count)
                return cannotReadBackward;
            position -= count;
            for (unsigned i = 0; i < count; ++i) {
                if (input[begin + i] != input[position + i] && !(op.needed && areEqualIgnoringCase(input[begin + i], input[position + i], op.target)))
                    return cannotReadBackward;
            }
            return position;
        }
        case FlatStepBackward::None:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ALWAYS_INLINE JSRegExpResult resultUnlessGivenUp(const FlatProgram& program, JSRegExpResult result) const
    {
        if constexpr (sizeof(CharType) > 1) {
            if (hasMetSurrogate && program.decodesSurrogatePairs)
                return JSRegExpResult::JITCodeFailure;
        }
        return result;
    }

    const unsigned startOffset;
    const unsigned length;
    const unsigned steps;
    unsigned noNewlineBefore { startOffset };
    StartSearch startSearch { };
    struct Nothing { };
    NO_UNIQUE_ADDRESS std::conditional_t<sizeof(CharType) == 1, Nothing, bool> hasMetSurrogate { }; // See FlatProgram::decodesSurrogatePairs.
    NO_UNIQUE_ADDRESS std::conditional_t<sizeof(CharType) == 1, Nothing, WideNeighbors> neighbors { };
};

template<typename CharType, bool isFull> static JSRegExpResult matchBehindLimit(const FlatProgram&, const CharType* input, const Enclosure<CharType>&, unsigned* output);

// remembers: keeps the table of failed searches. See FlatMatchPolicy.
// isFull: can do what few programs need: see FlatProgram::needsFullMachine. It keeps the table where it is given one. The others are kept free of that, since whatever is added to run() costs them registers.
template<typename CharType, bool remembers, bool isFull = false>
struct Machine {
    using Kind = FlatOp::Kind;

    ALWAYS_INLINE Machine(const FlatProgram& program, std::span<const CharType> subject, unsigned start, unsigned* output, Enclosure<CharType>& enclosure, uint32_t* memo = nullptr)
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
        , framesOfRegisters(program.framesOfRegisters)
        , enclosure(enclosure)
        , matchStart(start)
        , remainingSteps(enclosure.steps)
        , memo(memo)
    {
        ASSERT(isFull || remembers == !!memo);
    }

    ALWAYS_INLINE JSRegExpResult run()
    {
        if (matchStart > length)
            return JSRegExpResult::NoMatch;

        FrameStorage frameStorage;
        if (framesOfRegisters >= FrameStorage::inlineFrames) [[unlikely]] {
            if (!frameStorage.grow(0, framesOfRegisters + 1))
                return JSRegExpResult::ErrorNoMemory;
            setFrames(frameStorage.frames(), framesOfRegisters);
        } else
            setFrames(frameStorage.inlineStorage, framesOfRegisters);

        // Whoever finds a match sets the first two.
        for (unsigned i = 2; i < slots; i += 2) {
            output[i] = offsetNoMatch;
            output[i + 1] = offsetNoMatch;
            WTF::compilerFence();
        }

        // The size is in code units: a pattern in unicode mode has one only where every step takes a known number of them. The bytecode interpreter and the JIT begin at the same place.
        if (program.endAnchoredFixedSize && length >= *program.endAnchoredFixedSize)
            matchStart = std::max(matchStart, length - *program.endAnchoredFixedSize);

        if (!beginAttempt())
            return JSRegExpResult::NoMatch;

        for (;;) {
            Step step = runOp();
            if (step == Step::AtEnclosure)
                step = leaveMainLoop() ? Step::Matched : Step::Failed;
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
                if constexpr (!remembers)
                    return matchBehindLimit<CharType, false>(program, input, enclosure, output);
                if (isFull && !memo)
                    return matchBehindLimit<CharType, true>(program, input, enclosure, output);
                return JSRegExpResult::ErrorHitLimit;
            case Step::NeedsFrames:
                if (!growFrames(frameStorage))
                    return JSRegExpResult::ErrorNoMemory;
                continue;
            case Step::Failed:
            case Step::AtEnclosure:
                break;
            }
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    ALWAYS_INLINE bool leaveMainLoop()
    {
        if constexpr (isFull) {
            if (op->registerIndex) {
                enclosure.readBackward(program, *op, input, output, top[-1]);
                return false;
            }
        }
        return enclosure.widen(program, *op, input, matchStart, position, output);
    }

    // 0: the search from there has not begun. Else one more than the turns that it began before it failed.
    ALWAYS_INLINE uint32_t& memoEntry(unsigned row, unsigned at)
    {
        ASSERT(row && row <= program.memoRows && at <= length);
        return memo[static_cast<size_t>(row - 1) * (static_cast<size_t>(length) + 1) + at];
    }

    ALWAYS_INLINE size_t memoIndex(unsigned row, unsigned at) const
    {
        ASSERT(row && row <= program.memoRows && at <= length);
        return static_cast<size_t>(row - 1) * (static_cast<size_t>(length) + 1) + at;
    }

    // The bounds of the referenced groups lie behind all the entries, in the entries' order.
    ALWAYS_INLINE uint32_t* boundsOfMemoEntry(size_t index) { return memo + memoIndex(program.memoRows, length) + 1 + index * 2 * program.referencedGroups.size(); }

    // 0: the search from there has not begun while the referenced groups held what they hold now. Else one more than the turns that it began before it failed.
    ALWAYS_INLINE uint32_t findFailedSearch(unsigned row, unsigned at)
    {
        size_t index = memoIndex(row, at);
        uint32_t entry = memo[index];
        if constexpr (isFull) {
            if (entry && !holdsBoundsOfReferencedGroups(program, boundsOfMemoEntry(index), output))
                return 0;
        }
        return entry;
    }

    ALWAYS_INLINE void enterSearch(unsigned row, unsigned at, uint32_t value)
    {
        size_t index = memoIndex(row, at);
        memo[index] = value;
        if constexpr (isFull)
            storeBoundsOfReferencedGroups(program, boundsOfMemoEntry(index), output);
    }
    ALWAYS_INLINE bool isMemoized(const FlatOp* at) const
    {
        if constexpr (isFull)
            return memo && at->memoRow;
        return remembers && at->memoRow;
    }

    ALWAYS_INLINE bool takeTurnsOfFailedSearch(uint32_t entry)
    {
        uint32_t turns = entry - 1;
        if (!turns)
            return true;
        if (program.remainingTurns <= turns) {
            program.remainingTurns = 0;
            return false;
        }
        program.remainingTurns -= turns;
        return true;
    }

    static constexpr uint32_t dependsOnWhereTurnBegan = Enclosure<CharType>::recordsNothing;

    ALWAYS_INLINE void recordFailedSearch(unsigned row, const FlatFrame& frame)
    {
        if constexpr (isFull) {
            if (frame.extra != dependsOnWhereTurnBegan)
                enterSearch(row, frame.position, 1 + (frame.extra - program.remainingTurns));
        } else {
            if (frame.extra != dependsOnWhereTurnBegan)
                memoEntry(row, frame.position) = 1 + (frame.extra - program.remainingTurns);
        }
    }

    ALWAYS_INLINE bool isInsideLookahead(const FlatOp* at) const { return program.aroundMemoRow[at->memoRow].isInsideLookahead; }

    ALWAYS_INLINE unsigned memoRowOf(const FlatOp* at)
    {
        unsigned index = 0;
        for (unsigned loop = program.aroundMemoRow[at->memoRow].loopThatCountsTurns; loop; loop = program.loopsThatCountTurns[loop].enclosing) {
            auto& counted = program.loopsThatCountTurns[loop];
            index = index * (counted.bound + 1) + std::min<unsigned>(registers()[counted.registerIndex], counted.bound);
        }
        return at->memoRow + index;
    }

    ALWAYS_INLINE bool turnsAroundHaveTakenSomething(unsigned row)
    {
        for (unsigned turn = program.aroundMemoRow[row].turnThatMayTakeNothing; turn; turn = program.turnsThatMayTakeNothing[turn].enclosing) {
            if (registers()[program.turnsThatMayTakeNothing[turn].registerIndex] == position)
                return false;
        }
        return true;
    }

    ALWAYS_INLINE Step pushChoice(uint32_t limit)
    {
        if constexpr (isFull) {
            if (isMemoized(op)) [[unlikely]] {
                if (!turnsAroundHaveTakenSomething(op->memoRow)) {
                    push(position, limit, dependsOnWhereTurnBegan);
                    return Step::Next;
                }
                unsigned row = memoRowOf(op);
                if (uint32_t entry = findFailedSearch(row, position))
                    return takeTurnsOfFailedSearch(entry) ? Step::Failed : Step::HitLimit;
                enterSearch(row, position, !isInsideLookahead(op));
                push(position, limit, program.remainingTurns);
                return Step::Next;
            }
            push(position, limit, 0);
            return Step::Next;
        } else {
            if (isMemoized(op)) [[unlikely]] {
                if (!turnsAroundHaveTakenSomething(op->memoRow)) {
                    push(position, limit, dependsOnWhereTurnBegan);
                    return Step::Next;
                }
                uint32_t& entry = memoEntry(memoRowOf(op), position);
                if (entry)
                    return takeTurnsOfFailedSearch(entry) ? Step::Failed : Step::HitLimit;
                entry = !isInsideLookahead(op);
                push(position, limit, program.remainingTurns);
                return Step::Next;
            }
            push(position, limit, 0);
            return Step::Next;
        }
    }

    // The last alternative is about to begin. A program that counts turns has to hear when that has failed too.
    ALWAYS_INLINE void popChoice(const FlatOp* from)
    {
        if (isMemoized(from) && top[-1].extra != dependsOnWhereTurnBegan && (program.countsTurns || isInsideLookahead(from))) [[unlikely]] {
            top[-1].op = offsetOf(ops + program.recordFailureOp);
            top[-1].limit = memoRowOf(from);
            return;
        }
        --top;
    }

    ALWAYS_INLINE uint32_t* registers() { return reinterpret_cast<uint32_t*>(storage); }
    ALWAYS_INLINE uint32_t offsetOf(const FlatOp* at) const { return static_cast<uint32_t>(reinterpret_cast<const char*>(at) - reinterpret_cast<const char*>(ops)); }
    ALWAYS_INLINE const FlatOp* opOf(const FlatFrame& frame) const { return reinterpret_cast<const FlatOp*>(reinterpret_cast<const char*>(ops) + frame.op); }

    ALWAYS_INLINE bool contains(const CharacterSetBits& set, CharType character) const
    {
        if constexpr (sizeof(CharType) > 1) {
            if (character > 0xff) [[unlikely]] {
                enclosure.hasMetSurrogate |= U16_IS_SURROGATE(character);
                return enclosure.neighbors.contains(wideRanges, set, character);
            }
        }
        return Yarr::contains(set, wideRanges, static_cast<Latin1Character>(character));
    }

    ALWAYS_INLINE char32_t characterAt(unsigned at) const
    {
        char32_t unit = input[at];
        if constexpr (isFull && sizeof(CharType) > 1) {
            if (U16_IS_SURROGATE(unit) && program.decodesSurrogatePairs) [[unlikely]]
                return characterAtSurrogate(input, length, at);
        }
        return unit;
    }

    ALWAYS_INLINE bool isInsidePair(unsigned at) const { return isFull && sizeof(CharType) > 1 && at < length && characterAt(at) == noCharacter; }

    // How many units the character at `at`, which is before `end`, takes. 0: it is not in the set, or ends behind `end`.
    ALWAYS_INLINE unsigned widthOfMember(const CharacterSetBits& set, unsigned at, unsigned end) const
    {
        if constexpr (!isFull)
            return contains(set, input[at]);
        if constexpr (sizeof(CharType) > 1) {
            if (input[at] > 0xff) [[unlikely]] {
                char32_t character = input[at];
                unsigned width = 1;
                if (U16_IS_SURROGATE(character) && program.decodesSurrogatePairs) [[unlikely]] {
                    character = characterAt(at);
                    width = U16_LENGTH(character);
                    if (character == noCharacter || width > end - at)
                        return 0;
                }
                return enclosure.neighbors.contains(wideRanges, set, character) ? width : 0;
            }
        }
        return Yarr::contains(set, wideRanges, static_cast<Latin1Character>(input[at]));
    }

    ALWAYS_INLINE void advance(unsigned width) { position += isFull ? width : 1; }

    // How many units the character in front of `at`, which is not 0, takes.
    ALWAYS_INLINE unsigned widthBefore(unsigned at) const
    {
        if constexpr (isFull && sizeof(CharType) > 1) {
            if (U16_IS_TRAIL(input[at - 1]) && program.decodesSurrogatePairs && at > 1 && U16_IS_LEAD(input[at - 2])) [[unlikely]]
                return 2;
        }
        return 1;
    }

    ALWAYS_INLINE bool contains(const FirstSetBits& set, CharType character) const { return isInFirstSet(set, character); }

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
        setFrames(frameStorage.frames(), used);
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
        if (program.startScan == FlatStartScan::None)
            return true;
        matchStart = findStartOfMatch(program, input, length, matchStart, enclosure.startSearch);
        return matchStart <= length;
    }

    ALWAYS_INLINE bool beginAttempt()
    {
        if constexpr (isFull) {
            if (!findNextStart())
                return false;
            position = matchStart;
            op = ops;
            top = bottom;
            return true;
        } else {
            if (!findNextStart())
                return false;
            if constexpr (sizeof(CharType) > 1) {
                enclosure.hasMetSurrogate |= matchStart < length && U16_IS_SURROGATE(input[matchStart]);
                if (enclosure.hasMetSurrogate && program.decodesSurrogatePairs)
                    return false;
            }
            position = matchStart;
            op = ops;
            top = bottom;
            return true;
        }
    }

    ALWAYS_INLINE Step consumeIf(bool condition, unsigned count)
    {
        if (!condition)
            return Step::Failed;
        position += count;
        return Step::Next;
    }

    ALWAYS_INLINE Step runCharacter() { return consumeIf(position < length && input[position] == op->argument, 1); }

    ALWAYS_INLINE Step runSet()
    {
        if constexpr (!isFull)
            return consumeIf(position < length && contains(characterSets[op->set], input[position]), 1);
        unsigned width = position < length ? widthOfMember(characterSets[op->set], position, length) : 0;
        return consumeIf(width && hasNeededBehind(position + width), width);
    }

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

    // FlatProgramForm::CountsTurns: a surrogate pair has taken a unit more than was tested for. As Interpreter::InputStream::readChecked().
    ALWAYS_INLINE bool hasNeededBehind(unsigned at) const { return !isFull || length - at >= op->needed; }

    // The caller has tested for as many units.
    ALWAYS_INLINE bool consumeMembers(const CharacterSetBits& set, unsigned count)
    {
        if constexpr (isFull) {
            unsigned at = position;
            for (unsigned i = 0; i < count; ++i) {
                unsigned width = at < length ? widthOfMember(set, at, length) : 0;
                if (!width)
                    return false;
                at += width;
            }
            position = at;
            return true;
        } else {
            for (unsigned i = 0; i < count; ++i) {
                if (!contains(set, input[position + i]))
                    return false;
            }
            position += count;
            return true;
        }
    }

    ALWAYS_INLINE Step runSetCount() { return nextIf(length - position >= op->argument && consumeMembers(characterSets[op->set], op->argument) && hasNeededBehind(position)); }

    // A search that comes into the same long run again does not walk it again.
    ALWAYS_INLINE void consumeMembersUpTo(unsigned end)
    {
        if constexpr (isFull) {
            constexpr unsigned leastLengthToKnow = 32;
            const CharacterSetBits& set = characterSets[op->set];
            auto consume = [&] {
                while (position < end) {
                    unsigned width = widthOfMember(set, position, end);
                    if (!width)
                        break;
                    position += width;
                }
            };
            if constexpr (!remembers)
                return consume();
            if (isFull && !memo)
                return consume();
            unsigned begin = position;
            // An attempt may begin inside a surrogate pair.
            bool isInKnownRun = op->set == knownRunSet && begin - knownRunBegin <= knownRunEnd - knownRunBegin && !isInsidePair(begin);
            if (isInKnownRun) {
                position = std::min(knownRunEnd, end);
                position -= isInsidePair(position);
            }
            consume();
            if (isInKnownRun)
                knownRunEnd = std::max(knownRunEnd, position);
            else if (position - begin >= leastLengthToKnow) {
                knownRunSet = op->set;
                knownRunBegin = begin;
                knownRunEnd = position;
            }
        } else {
            constexpr unsigned leastLengthToKnow = 32;
            const CharacterSetBits& set = characterSets[op->set];
            if constexpr (!remembers) {
                while (position < end && contains(set, input[position]))
                    ++position;
                return;
            }
            unsigned begin = position;
            bool isInKnownRun = op->set == knownRunSet && begin - knownRunBegin <= knownRunEnd - knownRunBegin;
            if (isInKnownRun)
                position = std::min(knownRunEnd, end);
            while (position < end && contains(set, input[position]))
                ++position;
            if (isInKnownRun)
                knownRunEnd = std::max(knownRunEnd, position);
            else if (position - begin >= leastLengthToKnow) {
                knownRunSet = op->set;
                knownRunBegin = begin;
                knownRunEnd = position;
            }
        }
    }

    ALWAYS_INLINE std::optional<unsigned> consumeGreedyRun()
    {
        if constexpr (isFull) {
            unsigned available = length - position;
            if (available < op->needed)
                return std::nullopt;
            available -= op->needed;
            return consumeRun(position + available, op->target, op->argument);
        } else {
            unsigned available = length - position;
            if (available < op->needed)
                return std::nullopt;
            available -= op->needed;
            unsigned end = available > op->argument ? position + op->argument : position + available;
            unsigned begin = position;
            consumeMembersUpTo(end);
            if (position - begin < op->target)
                return std::nullopt;
            return begin + op->target;
        }
    }

    // Takes as many members as it can, up to `most`, that end at `end` or before. Answers where it stood behind the first `least`, or nothing where there are fewer.
    ALWAYS_INLINE std::optional<unsigned> consumeRun(unsigned end, unsigned least, unsigned most)
    {
        unsigned begin = position;
        if (!program.decodesSurrogatePairs) [[likely]] {
            consumeMembersUpTo(end - begin > most ? begin + most : end);
            if (position - begin < least)
                return std::nullopt;
            return begin + least;
        }
        // As many characters are no longer as many units.
        if (end - begin < least || !consumeMembers(characterSets[op->set], least) || position > end)
            return std::nullopt;
        unsigned behindLeast = position;
        if (most == quantifyInfinite)
            consumeMembersUpTo(end);
        else {
            for (unsigned taken = least; taken < most && position < end; ++taken) {
                unsigned width = widthOfMember(characterSets[op->set], position, end);
                if (!width)
                    break;
                position += width;
            }
        }
        return behindLeast;
    }

    ALWAYS_INLINE Step runGreedyRun()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        std::optional<unsigned> least = consumeGreedyRun();
        if (!least)
            return Step::Failed;
        if (isMemoized(op)) [[unlikely]] {
            push(position, *least, 0);
            return goOnFromGreedyRun<false>(op);
        }
        if (position > *least)
            push(position, *least, 0);
        return Step::Next;
    }

    // The top frame is the run's. Goes down from position to the first place that the search has not gone on from before.
    template<bool knowsFollowingCharacters>
    ALWAYS_INLINE Step goOnFromGreedyRun(const FlatOp* from)
    {
        if constexpr (isFull) {
            FlatFrame& frame = top[-1];
            unsigned row = memoRowOf(from);
            for (;; position -= widthBefore(position)) {
                if (!knowsFollowingCharacters || (position == length ? firstSets[from->firstSet].atEnd : contains(firstSets[from->firstSet], input[position]))) {
                    if (!turnsAroundHaveTakenSomething(from->memoRow)) {
                        frame.extra = dependsOnWhereTurnBegan;
                        break;
                    }
                    uint32_t entry = findFailedSearch(row, position);
                    if (!entry) {
                        enterSearch(row, position, !isInsideLookahead(from));
                        frame.extra = program.remainingTurns;
                        break;
                    }
                    if (!takeTurnsOfFailedSearch(entry))
                        return Step::HitLimit;
                }
                if (position == frame.limit) {
                    --top;
                    return Step::Failed;
                }
            }
            frame.position = position;
            op = from;
            return Step::Next;
        } else {
            FlatFrame& frame = top[-1];
            unsigned row = memoRowOf(from);
            for (;; --position) {
                if (!knowsFollowingCharacters || (position == length ? firstSets[from->firstSet].atEnd : contains(firstSets[from->firstSet], input[position]))) {
                    if (!turnsAroundHaveTakenSomething(from->memoRow)) {
                        frame.extra = dependsOnWhereTurnBegan;
                        break;
                    }
                    uint32_t& entry = memoEntry(row, position);
                    if (!entry) {
                        entry = !isInsideLookahead(from);
                        frame.extra = program.remainingTurns;
                        break;
                    }
                    if (!takeTurnsOfFailedSearch(entry))
                        return Step::HitLimit;
                }
                if (position == frame.limit) {
                    --top;
                    return Step::Failed;
                }
            }
            frame.position = position;
            op = from;
            return Step::Next;
        }
    }

    template<bool knowsFollowingCharacters>
    ALWAYS_INLINE Step resumeMemoizedGreedyRun(const FlatOp* from)
    {
        if constexpr (isFull) {
            const FlatFrame& frame = top[-1];
            recordFailedSearch(memoRowOf(from), frame);
            if (frame.position == frame.limit) {
                --top;
                return Step::Failed;
            }
            position = frame.position - widthBefore(frame.position);
            return goOnFromGreedyRun<knowsFollowingCharacters>(from);
        } else {
            const FlatFrame& frame = top[-1];
            recordFailedSearch(memoRowOf(from), frame);
            if (frame.position == frame.limit) {
                --top;
                return Step::Failed;
            }
            position = frame.position - 1;
            return goOnFromGreedyRun<knowsFollowingCharacters>(from);
        }
    }

    // Takes one more character into a lazy run that has taken `taken`, if it ends at `end` or before.
    ALWAYS_INLINE bool extendLazyRun(const FlatOp* from, unsigned taken, unsigned end)
    {
        unsigned width = taken != from->argument && position < end ? widthOfMember(characterSets[from->set], position, end) : 0;
        if (!width)
            return false;
        advance(width);
        return true;
    }

    // The top frame is the run's: its limit is how many characters the run has taken up to position.
    template<bool knowsFollowingCharacters>
    ALWAYS_INLINE Step goOnFromLazyRun(const FlatOp* from)
    {
        if constexpr (isFull) {
            FlatFrame& frame = top[-1];
            unsigned row = memoRowOf(from);
            for (;;) {
                if (!knowsFollowingCharacters || position == length || contains(firstSets[from->firstSet], input[position])) {
                    if (!turnsAroundHaveTakenSomething(from->memoRow)) {
                        frame.extra = dependsOnWhereTurnBegan;
                        break;
                    }
                    uint32_t entry = findFailedSearch(row, position);
                    if (!entry) {
                        enterSearch(row, position, !isInsideLookahead(from));
                        frame.extra = program.remainingTurns;
                        break;
                    }
                    if (!takeTurnsOfFailedSearch(entry))
                        return Step::HitLimit;
                }
                if (!extendLazyRun(from, frame.limit, length - from->needed)) {
                    --top;
                    return Step::Failed;
                }
                ++frame.limit;
            }
            frame.position = position;
            op = from;
            return Step::Next;
        } else {
            FlatFrame& frame = top[-1];
            unsigned row = memoRowOf(from);
            for (;;) {
                if (!knowsFollowingCharacters || position == length || contains(firstSets[from->firstSet], input[position])) {
                    if (!turnsAroundHaveTakenSomething(from->memoRow)) {
                        frame.extra = dependsOnWhereTurnBegan;
                        break;
                    }
                    uint32_t& entry = memoEntry(row, position);
                    if (!entry) {
                        entry = !isInsideLookahead(from);
                        frame.extra = program.remainingTurns;
                        break;
                    }
                    if (!takeTurnsOfFailedSearch(entry))
                        return Step::HitLimit;
                }
                if (frame.limit == from->argument || position >= length - from->needed || !contains(characterSets[from->set], input[position])) {
                    --top;
                    return Step::Failed;
                }
                ++frame.limit;
                ++position;
            }
            frame.position = position;
            op = from;
            return Step::Next;
        }
    }

    template<bool knowsFollowingCharacters>
    ALWAYS_INLINE Step resumeMemoizedLazyRun(const FlatOp* from)
    {
        if constexpr (isFull) {
            FlatFrame& frame = top[-1];
            recordFailedSearch(memoRowOf(from), frame);
            position = frame.position;
            if (!extendLazyRun(from, frame.limit, length - from->needed)) {
                --top;
                return Step::Failed;
            }
            ++frame.limit;
            return goOnFromLazyRun<knowsFollowingCharacters>(from);
        } else {
            FlatFrame& frame = top[-1];
            recordFailedSearch(memoRowOf(from), frame);
            position = frame.position;
            if (frame.limit == from->argument || position >= length - from->needed || !contains(characterSets[from->set], input[position])) {
                --top;
                return Step::Failed;
            }
            ++frame.limit;
            ++position;
            return goOnFromLazyRun<knowsFollowingCharacters>(from);
        }
    }

    ALWAYS_INLINE Step runGreedyRunBeforeKnownCharacters()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        std::optional<unsigned> least = consumeGreedyRun();
        if (!least)
            return Step::Failed;
        if (isMemoized(op)) [[unlikely]] {
            push(position, *least, 0);
            return goOnFromGreedyRun<true>(op);
        }
        const FirstSetBits& following = firstSets[op->firstSet];
        while (position > *least && !(position == length ? following.atEnd : contains(following, input[position])))
            --position;
        if (position > *least)
            push(position, *least, 0);
        return Step::Next;
    }

    ALWAYS_INLINE bool consumeLazyRunMinimum()
    {
        if constexpr (isFull) {
            unsigned available = length - position;
            return available >= op->needed && available - op->needed >= op->target && consumeMembers(characterSets[op->set], op->target) && hasNeededBehind(position);
        } else {
            unsigned available = length - position;
            return available >= op->needed && available - op->needed >= op->target && consumeMembers(characterSets[op->set], op->target);
        }
    }

    ALWAYS_INLINE Step runLazyRun()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        if (!consumeLazyRunMinimum())
            return Step::Failed;
        push(position, op->target, length - op->needed);
        if (isMemoized(op)) [[unlikely]]
            return goOnFromLazyRun<false>(op);
        return Step::Next;
    }

    ALWAYS_INLINE Step runLazyRunBeforeKnownCharacters()
    {
        if constexpr (isFull) {
            if (!tryReserveFrames(1))
                return Step::NeedsFrames;
            if (!consumeLazyRunMinimum())
                return Step::Failed;
            if (isMemoized(op)) [[unlikely]] {
                push(position, op->target, 0);
                return goOnFromLazyRun<true>(op);
            }
            const FirstSetBits& following = firstSets[op->firstSet];
            unsigned taken = op->target;
            if (!program.decodesSurrogatePairs) {
                const CharacterSetBits& set = characterSets[op->set];
                unsigned begin = position;
                unsigned limit = length - op->needed;
                if (op->argument - op->target < limit - position)
                    limit = position + (op->argument - op->target);
                while (position < length && !contains(following, input[position])) {
                    if (position >= limit || !contains(set, input[position]))
                        return Step::Failed;
                    ++position;
                }
                taken += position - begin;
            } else {
                for (; position < length && !contains(following, input[position]); ++taken) {
                    if (!extendLazyRun(op, taken, length - op->needed))
                        return Step::Failed;
                }
            }
            push(position, taken, length - op->needed);
            return Step::Next;
        } else {
            if (!tryReserveFrames(1))
                return Step::NeedsFrames;
            if (!consumeLazyRunMinimum())
                return Step::Failed;
            if (isMemoized(op)) [[unlikely]] {
                push(position, op->target, 0);
                return goOnFromLazyRun<true>(op);
            }
            const CharacterSetBits& set = characterSets[op->set];
            const FirstSetBits& following = firstSets[op->firstSet];
            unsigned begin = position;
            unsigned limit = length - op->needed;
            if (op->argument - op->target < limit - position)
                limit = position + (op->argument - op->target);
            while (position < length && !contains(following, input[position])) {
                if (position >= limit || !contains(set, input[position]))
                    return Step::Failed;
                ++position;
            }
            push(position, op->target + (position - begin), length - op->needed);
            return Step::Next;
        }
    }

    ALWAYS_INLINE Step runPossessiveRun()
    {
        if constexpr (isFull) {
            return nextIf(!!consumeRun(length, op->target, op->argument));
        } else {
            unsigned end = length - position > op->argument ? position + op->argument : length;
            unsigned begin = position;
            consumeMembersUpTo(end);
            return nextIf(position - begin >= op->target);
        }
    }

    ALWAYS_INLINE std::optional<unsigned> consumeCapturedRun()
    {
        if constexpr (isFull) {
            unsigned begin = position;
            if (!consumeRun(length, op->target, quantifyInfinite))
                return std::nullopt;
            return begin;
        } else {
            unsigned begin = position;
            consumeMembersUpTo(length);
            if (position - begin < op->target)
                return std::nullopt;
            return begin;
        }
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
        return pushChoice(0);
    }

    ALWAYS_INLINE Step runGreedyLoopChoice()
    {
        if (!tryReserveFrames(1))
            return Step::NeedsFrames;
        Step step = pushChoice(registers()[op->registerIndex]);
        if (step == Step::Next)
            registers()[op->registerIndex] = position;
        return step;
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
            return pushChoice(0);
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
            return pushChoice(0);
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

    ALWAYS_INLINE Step runStepBack()
    {
        if constexpr (isFull) {
            if (position < op->argument)
                return Step::Failed;
            position -= op->argument;
            return Step::Next;
        } else {
            if (position < op->argument)
                return Step::Failed;
            if constexpr (sizeof(CharType) > 1) {
                // The step is in characters: see FlatProgram::decodesSurrogatePairs.
                for (unsigned i = 1; i <= op->argument; ++i)
                    enclosure.hasMetSurrogate |= U16_IS_SURROGATE(input[position - i]);
            }
            position -= op->argument;
            return Step::Next;
        }
    }
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

    ALWAYS_INLINE Step runDotStarEnclosure() { return Step::AtEnclosure; }

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

    template<bool ignoresCase>
    ALWAYS_INLINE Step consumeCapturedText()
    {
        if constexpr (isFull) {
            unsigned begin = output[op->argument];
            unsigned end = output[op->argument + 1];
            if (begin == offsetNoMatch || end == offsetNoMatch)
                return Step::Next;
            unsigned count = end - begin;
            if (length - position < count)
                return Step::Failed;
            for (unsigned i = 0; i < count; ++i) {
                char32_t expected = characterAt(begin + i);
                char32_t actual = characterAt(position + i);
                if (expected == noCharacter || actual == noCharacter || (ignoresCase ? !areEqualIgnoringCase(expected, actual, op->target) : expected != actual))
                    return Step::Failed;
                i += U16_LENGTH(expected) - 1;
            }
            position += count;
            return Step::Next;
        } else {
            unsigned begin = output[op->argument];
            unsigned end = output[op->argument + 1];
            if (begin == offsetNoMatch || end == offsetNoMatch)
                return Step::Next;
            unsigned count = end - begin;
            if (length - position < count)
                return Step::Failed;
            for (unsigned i = 0; i < count; ++i) {
                if (ignoresCase ? !areEqualIgnoringCase(input[begin + i], input[position + i], op->target) : input[begin + i] != input[position + i])
                    return Step::Failed;
            }
            position += count;
            return Step::Next;
        }
    }

    ALWAYS_INLINE Step runBackReference() { return consumeCapturedText<false>(); }
    ALWAYS_INLINE Step runBackReferenceIgnoringCase() { return consumeCapturedText<true>(); }

    ALWAYS_INLINE Step resumeChoice()
    {
        const FlatOp* from = opOf(top[-1]);
        position = top[-1].position;
        popChoice(from);
        return jumpTo(from->target);
    }

    ALWAYS_INLINE Step runRecordFailure() { RELEASE_ASSERT_NOT_REACHED(); }

    ALWAYS_INLINE Step resumeRecordFailure()
    {
        const FlatFrame& frame = *--top;
        recordFailedSearch(frame.limit, frame);
        return Step::Failed;
    }

    ALWAYS_INLINE Step resumeChoiceByFirst() { return resumeChoice(); }
    ALWAYS_INLINE Step resumeChoiceByTable() { return resumeChoice(); }
    ALWAYS_INLINE Step resumeNegativeLookaheadBegin() { return resumeChoice(); }

    ALWAYS_INLINE Step resumeGreedyLoopChoice()
    {
        const FlatFrame& frame = top[-1];
        const FlatOp* from = opOf(frame);
        position = frame.position;
        registers()[from->registerIndex] = frame.limit;
        popChoice(from);
        return jumpTo(from->target);
    }

    ALWAYS_INLINE Step resumeGreedyRun()
    {
        if constexpr (isFull) {
            FlatFrame* frame = top - 1;
            const FlatOp* from = opOf(*frame);
            if (isMemoized(from)) [[unlikely]]
                return resumeMemoizedGreedyRun<false>(from);
            position = frame->position -= widthBefore(frame->position);
            if (position == frame->limit)
                --top;
            op = from;
            return Step::Next;
        } else {
            FlatFrame* frame = top - 1;
            const FlatOp* from = opOf(*frame);
            if (isMemoized(from)) [[unlikely]]
                return resumeMemoizedGreedyRun<false>(from);
            position = --frame->position;
            if (position == frame->limit)
                --top;
            op = from;
            return Step::Next;
        }
    }

    ALWAYS_INLINE Step resumeGreedyRunBeforeKnownCharacters()
    {
        FlatFrame* frame = top - 1;
        const FlatOp* from = opOf(*frame);
        if (isMemoized(from)) [[unlikely]]
            return resumeMemoizedGreedyRun<true>(from);
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
        if constexpr (isFull) {
            FlatFrame* frame = top - 1;
            const FlatOp* from = opOf(*frame);
            if (isMemoized(from)) [[unlikely]]
                return resumeMemoizedLazyRun<true>(from);
            position = frame->position;
            do {
                if (!extendLazyRun(from, frame->limit, frame->extra)) {
                    --top;
                    return Step::Failed;
                }
                ++frame->limit;
            } while (position < length && !contains(firstSets[from->firstSet], input[position]));
            frame->position = position;
            op = from;
            return Step::Next;
        } else {
            FlatFrame* frame = top - 1;
            const FlatOp* from = opOf(*frame);
            if (isMemoized(from)) [[unlikely]]
                return resumeMemoizedLazyRun<true>(from);
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
    }

    ALWAYS_INLINE Step resumeLazyRun()
    {
        if constexpr (isFull) {
            FlatFrame* frame = top - 1;
            const FlatOp* from = opOf(*frame);
            if (isMemoized(from)) [[unlikely]]
                return resumeMemoizedLazyRun<false>(from);
            position = frame->position;
            if (extendLazyRun(from, frame->limit, frame->extra)) {
                ++frame->limit;
                frame->position = position;
                op = from;
                return Step::Next;
            }
            --top;
            return Step::Failed;
        } else {
            FlatFrame* frame = top - 1;
            const FlatOp* from = opOf(*frame);
            if (isMemoized(from)) [[unlikely]]
                return resumeMemoizedLazyRun<false>(from);
            if (frame->limit < from->argument && frame->position < frame->extra && contains(characterSets[from->set], input[frame->position])) {
                ++frame->limit;
                position = ++frame->position;
                op = from;
                return Step::Next;
            }
            --top;
            return Step::Failed;
        }
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
        case Kind::RecordFailure:
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
        case Kind::RecordFailure:
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
    Enclosure<CharType>& enclosure;
    unsigned matchStart;
    unsigned remainingSteps;
    uint32_t* const memo;
    // Every character of [knownRunBegin, knownRunEnd) is in the set of this index.
    unsigned knownRunSet { std::numeric_limits<unsigned>::max() };
    unsigned knownRunBegin { 0 };
    unsigned knownRunEnd { 0 };

    const FlatOp* op { nullptr };
    unsigned position { 0 };
    FlatFrame* top { nullptr };

    FlatFrame* storage { nullptr };
    FlatFrame* storageEnd { nullptr };
    FlatFrame* bottom { nullptr };
    size_t framesNeeded { 0 };
};

template<typename CharType>
static NEVER_INLINE JSRegExpResult runFullMachine(const FlatProgram& program, const CharType* input, Enclosure<CharType>& enclosure, unsigned* output, uint32_t* memo)
{
    return Machine<CharType, true, true>(program, std::span { input, enclosure.length }, enclosure.startOffset, output, enclosure, memo).run();
}

template<typename CharType, bool isFull = false>
static NEVER_INLINE JSRegExpResult matchForgetting(const FlatProgram& program, const CharType* input, unsigned length, unsigned start, unsigned* output, unsigned steps)
{
    Enclosure<CharType> enclosure { start, length, steps };
    if constexpr (isFull)
        return runFullMachine(program, input, enclosure, output, nullptr);
    JSRegExpResult result = Machine<CharType, false>(program, std::span { input, length }, start, output, enclosure).run();
    return enclosure.resultUnlessGivenUp(program, result);
}

template<typename CharType, bool isFull = false>
static NEVER_INLINE JSRegExpResult matchRemembering(const FlatProgram& program, const CharType* input, unsigned length, unsigned start, unsigned* output)
{
    constexpr uint64_t maxMemoSize = 32 * MB;
    uint64_t entries = static_cast<uint64_t>(program.memoRows) * (static_cast<uint64_t>(length) + 1);
    if constexpr (isFull)
        entries *= program.memoEntrySize();
    Vector<uint32_t> memo;
    if (!entries || entries * sizeof(uint32_t) > maxMemoSize || !memo.tryGrow(entries))
        return matchForgetting<CharType, isFull>(program, input, length, start, output, matchLimit);
    zeroSpan(memo.mutableSpan());
    Enclosure<CharType> enclosure { start, length, matchLimit };
    if constexpr (isFull)
        return runFullMachine(program, input, enclosure, output, memo.mutableSpan().data());
    JSRegExpResult result = Machine<CharType, true>(program, std::span { input, length }, start, output, enclosure, memo.mutableSpan().data()).run();
    return enclosure.resultUnlessGivenUp(program, result);
}

template<typename CharType, bool isFull>
static NEVER_INLINE JSRegExpResult matchBehindLimit(const FlatProgram& program, const CharType* input, const Enclosure<CharType>& enclosure, unsigned* output)
{
    if (enclosure.steps == matchLimit || program.jitCountsTurns)
        return JSRegExpResult::ErrorHitLimit;
    if (isFull || !program.referencedGroups.isEmpty())
        return matchRemembering<CharType, true>(program, input, enclosure.length, enclosure.startOffset, output);
    return matchRemembering(program, input, enclosure.length, enclosure.startOffset, output);
}

template<typename CharType, bool isFull>
static JSRegExpResult matchByPolicy(const FlatProgram& program, std::span<const CharType> input, unsigned start, unsigned* output, FlatMatchPolicy policy)
{
    unsigned length = input.size();
    unsigned stepsPerCharacter = isFull ? program.stepsPerCharacterBeforeStoppingOnFullMachine : program.stepsPerCharacterBeforeStopping;
    if (policy == FlatMatchPolicy::HandsOverEarly && stepsPerCharacter)
        return matchForgetting<CharType, isFull>(program, input.data(), length, start, output, static_cast<unsigned>(std::min<uint64_t>(matchLimit, stepsPerCharacter * (static_cast<uint64_t>(length) + 32))));
    if (policy != FlatMatchPolicy::RunsToMatchLimit && (program.countsTurns || policy == FlatMatchPolicy::RemembersFromStart)) {
        if (isFull || !program.referencedGroups.isEmpty())
            return matchRemembering<CharType, true>(program, input.data(), length, start, output);
        return matchRemembering(program, input.data(), length, start, output);
    }
    return matchForgetting<CharType, isFull>(program, input.data(), length, start, output, matchLimit);
}

// Whatever is not the common case.
template<typename CharType>
static NEVER_INLINE JSRegExpResult matchOutOfLine(const FlatProgram& program, std::span<const CharType> input, unsigned start, unsigned* output, FlatMatchPolicy policy)
{
    if (program.needsFullMachine)
        return matchByPolicy<CharType, true>(program, input, start, output, policy);
    return matchByPolicy<CharType, false>(program, input, start, output, policy);
}

template<typename CharType>
static ALWAYS_INLINE JSRegExpResult match(const FlatProgram& program, std::span<const CharType> input, unsigned start, unsigned* output, FlatMatchPolicy policy)
{
    unsigned length = input.size();
    if (policy == FlatMatchPolicy::HandsOverEarly && program.stepsPerCharacterBeforeStopping) [[likely]]
        return matchForgetting(program, input.data(), length, start, output, static_cast<unsigned>(std::min<uint64_t>(matchLimit, program.stepsPerCharacterBeforeStopping * (static_cast<uint64_t>(length) + 32))));
    if (policy != FlatMatchPolicy::HandsOverEarly || program.isEnteredOutOfLine)
        return matchOutOfLine(program, input, start, output, policy);
    return matchForgetting(program, input.data(), length, start, output, matchLimit);
}

} // anonymous namespace

JSRegExpResult matchFlatProgram(const FlatProgram& program, std::span<const Latin1Character> input, unsigned start, unsigned* output, FlatMatchPolicy policy)
{
    return match(program, input, start, output, policy);
}

JSRegExpResult matchFlatProgram(const FlatProgram& program, StringView input, unsigned start, unsigned* output, FlatMatchPolicy policy)
{
    SuperSamplerScope superSamplerScope(false);
    if (input.is8Bit())
        return match(program, input.span8(), start, output, policy);
    return match(program, input.span16(), start, output, policy);
}

JSRegExpResult matchFlatProgramBehindSurrogate(const FlatProgram& program, StringView input, unsigned start, unsigned* output, FlatMatchPolicy policy)
{
    return matchByPolicy<char16_t, true>(program, input.span16(), start, output, policy);
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
