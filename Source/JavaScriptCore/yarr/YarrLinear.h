/*
 * Copyright (C) 2026 Oven-sh Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include "Yarr.h"
#include "YarrPattern.h"
#include <wtf/BitSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace Yarr {

// The non-backtracking matcher, selected by Options::useRegExpLinearEngine().
//
// A YarrPattern is compiled to a flat program for a nondeterministic automaton. A match moves
// every state the automaton can be in over the subject together, one character at a time, and
// enters no state twice at one position. So the steps of a match grow with the length of the
// subject and with nothing else: a position costs at most
// LinearProgram::m_maximumStepsPerPosition of them, a number the compiler takes from the
// program. The backtracking engines retry a position once per way of reaching it, which is
// exponential for /(a*)*b/.
//
// The states of a position are kept in the order backtracking would try them. The match that is
// found, and what each group captured, are therefore the ones the backtracking engines report.
//
// What a backreference matches depends on what a group captured, which is not a state of the
// automaton, and the compiler does not expand the ones whose group can only capture a few
// strings. A lookaround is run to its end at every position that asserts it, so it is accepted
// only when what it can match has a bounded length, and when the lookarounds of the pattern,
// inside one another, do not cost a position more steps than the largest program without a
// lookaround does. The compiler refuses the other patterns and says why.
//
// The engine of a RegExp is chosen once, when it is compiled: RegExp::compile() asks this
// matcher first, and a pattern it refuses is compiled for the JIT and the bytecode interpreter
// as it is without the option. No match changes engine part of the way through.
//
// The bound is for one match. A global loop (replace, matchAll, split) runs one match per
// result, and each of them can read the subject to its end.

enum class LinearRefusal : uint8_t {
    None,
    BackReference,
    UnboundedLookaround,
    NestingTooDeep,
    ProgramTooLarge,
    WorkingMemoryTooLarge,
    LookaroundTooCostly,
    UnsupportedTerm,
};

JS_EXPORT_PRIVATE ASCIILiteral NODELETE linearRefusalName(LinearRefusal);

struct LinearInstruction {
    enum class Opcode : uint8_t {
        // A state waits on one of these for the next character.
        Character, // a: the character
        CharacterEither, // a, b: the two characters (an ASCII letter under /i)
        CharacterClass, // a: index into LinearProgram::m_classes
        Match,

        // The rest are followed while the states of a position are collected.
        Jump, // a: target
        Split, // a: the target backtracking tries first, b: the other one
        Save, // a: slot of the output vector, set to the position
        ClearSlots, // [a, b): slots set to offsetNoMatch
        StoreSlot, // a: slot, b: value
        LoopBegin, // an iteration that is allowed to match nothing starts here
        LoopEnd, // and ends here; the state dies if the iteration consumed nothing
        AssertBOL,
        AssertEOL,
        AssertWordBoundary,
        Lookaround, // a: index into LinearProgram::m_codes
    };

    enum Flag : uint8_t {
        Invert = 1 << 0,
        Multiline = 1 << 1,
        UnicodeIgnoreCase = 1 << 2,
    };

    bool isConsuming() const { return opcode <= Opcode::Match; }
    bool invert() const { return flags & Invert; }
    bool multiline() const { return flags & Multiline; }
    bool unicodeIgnoreCase() const { return flags & UnicodeIgnoreCase; }

    Opcode opcode { Opcode::Match };
    uint8_t flags { 0 };
    unsigned a { 0 };
    unsigned b { 0 };
};

struct LinearCharacterClass {
    bool contains(char32_t character) const
    {
        if (character < latin1.size())
            return latin1.get(character);
        size_t low = 0;
        size_t high = ranges.size();
        while (low < high) {
            size_t middle = low + (high - low) / 2;
            if (character > ranges[middle].end)
                low = middle + 1;
            else if (character < ranges[middle].begin)
                high = middle;
            else
                return true;
        }
        return false;
    }

    WTF::BitSet<256> latin1;
    // The members above U+00FF, sorted. No two ranges overlap or touch.
    Vector<CharacterRange> ranges;
};

// The pattern, or the body of one lookaround.
struct LinearCode {
    Vector<LinearInstruction> instructions;
    MatchDirection direction { Forward };
    // What a lookaround hands to the state that asserted it: the slots of its groups,
    // [firstSlot, endSlot), and the slots of duplicated group names when it has such a group.
    unsigned firstSlot { 0 };
    unsigned endSlot { 0 };
    bool writesSlots { false };
    bool writesDuplicateNameSlots { false };
};

class LinearProgram {
    WTF_MAKE_TZONE_ALLOCATED(LinearProgram);
    WTF_MAKE_NONCOPYABLE(LinearProgram);
public:
    // What one match cost. Both numbers depend only on the program and the subject.
    struct Statistics {
        // Instructions executed, over every state. At most m_maximumStepsPerPosition for each
        // position the match reads.
        uint64_t steps { 0 };
        // Bytes of working memory the matcher held at the end of the match. At most
        // m_maximumScratchBytes.
        size_t scratchBytes { 0 };
    };

    LinearProgram() = default;

    // Same contract as Yarr::interpret(): the offset of the match, or offsetNoMatch.
    unsigned match(StringView input, unsigned start, unsigned* output, Statistics* = nullptr) const;

    size_t instructionCount() const;
    size_t estimatedSizeInBytes() const;
    void dump(PrintStream&) const;

    // m_codes[0] is the pattern. A Lookaround instruction names one of the others.
    Vector<LinearCode> m_codes;
    Vector<LinearCharacterClass> m_classes;
    // The subject's Latin-1 characters that can begin a match. Full when a match can be empty
    // or begin with any character.
    WTF::BitSet<256> m_firstCharacters;
    bool m_firstCharacterCanBeWide { true };
    // Slots of the output vector: two per group, then one per duplicated group name.
    unsigned m_captureSlotCount { 0 };
    unsigned m_slotCount { 0 };
    bool m_decodeSurrogatePairs { false };
    bool m_sticky { false };
    // Every alternative begins with a ^ that only the start of the subject satisfies.
    bool m_anchoredAtStart { false };
    // A match that starts at `start` takes at most this many steps for each of the positions
    // start, ..., length of the subject. At most four times
    // Options::maximumRegExpLinearProgramSize(): the compiler refuses the program otherwise.
    uint64_t m_maximumStepsPerPosition { 0 };
    // A match holds at most this much working memory, whatever the subject is. At most
    // Options::maximumRegExpLinearWorkingMemory(): the compiler refuses the program otherwise.
    uint64_t m_maximumScratchBytes { 0 };
};

// Null, with the reason in the LinearRefusal, for a pattern the matcher cannot run.
std::unique_ptr<LinearProgram> compileLinear(YarrPattern&, LinearRefusal&);

} } // namespace JSC::Yarr

#endif // USE(BUN_JSC_ADDITIONS)
