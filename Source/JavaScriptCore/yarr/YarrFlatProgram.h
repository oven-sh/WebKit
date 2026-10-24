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

#pragma once

#include "Yarr.h"
#include "YarrCharacterSetBits.h"
#include "YarrPattern.h"
#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace Yarr {

#define FOR_EACH_FLAT_OP_PUSHING_FRAME(macro) \
    macro(Choice) \
    macro(ChoiceByFirst) \
    macro(ChoiceByTable) \
    macro(GreedyLoopChoice) \
    macro(NegativeLookaheadBegin) \
    macro(GreedyRun) \
    macro(LazyRun) \
    macro(GreedyRunBeforeKnownCharacters) \
    macro(LazyRunBeforeKnownCharacters) \
    macro(LoggedCaptureStart) \
    macro(LoggedCaptureEnd) \
    macro(LoggedCaptureEmpty) \
    macro(LoggedCapturedRun) \
    macro(LoggedClearCaptures) \
    macro(LoggedSavePosition) \
    macro(LoggedSetRegister) \
    macro(LoggedIncrement) \
    macro(RecordFailure)

#define FOR_EACH_FLAT_OP_WITHOUT_FRAME(macro) \
    macro(Character) \
    macro(Text) \
    macro(CasedText) \
    macro(WideText) \
    macro(Set) \
    macro(SetCount) \
    macro(PossessiveRun) \
    macro(CapturedRun) \
    macro(Jump) \
    macro(CaptureStart) \
    macro(CaptureEnd) \
    macro(CaptureEmpty) \
    macro(ClearCaptures) \
    macro(SavePosition) \
    macro(FailIfEmpty) \
    macro(NeedsInput) \
    macro(NeedsInputElseJump) \
    macro(StepBack) \
    macro(InputStart) \
    macro(LineStart) \
    macro(InputEnd) \
    macro(LineEnd) \
    macro(WordBoundary) \
    macro(NotWordBoundary) \
    macro(Match) \
    macro(DotStarEnclosure) \
    macro(JumpIfBelow) \
    macro(JumpIfNotBelow) \
    macro(LoopBack) \
    macro(LoopBackUnlessEmpty) \
    macro(CountTurn) \
    macro(LookaheadBegin) \
    macro(LookaheadEnd) \
    macro(LookaheadEndKeepingCaptures) \
    macro(NegativeLookaheadEnd) \
    macro(BackReference) \
    macro(BackReferenceIgnoringCase)

#define FOR_EACH_FLAT_OP(macro) \
    FOR_EACH_FLAT_OP_PUSHING_FRAME(macro) \
    FOR_EACH_FLAT_OP_WITHOUT_FRAME(macro)

struct FlatOp {
    enum class Kind : uint8_t {
#define FLAT_OP_ENUMERATOR(name) name,
        FOR_EACH_FLAT_OP(FLAT_OP_ENUMERATOR)
#undef FLAT_OP_ENUMERATOR
    };
    Kind kind;
    uint16_t set { 0 }; // Index into FlatProgram::characterSets.
    uint16_t firstSet { 0 }; // Index into FlatProgram::firstSets. ChoiceByFirst: what this alternative can begin with. *BeforeKnownCharacters: what follows the run.
    uint16_t laterFirstSet { 0 }; // Index into FlatProgram::firstSets. ChoiceByFirst: what any later alternative can begin with.
    uint16_t registerIndex { 0 };
    uint16_t memoRow { 0 }; // One more than this op's first row of the table of failed searches. 0: the search from here on depends on more than the position, or makes no choice.
    uint32_t argument { 0 }; // Lookahead ops: the register that holds the frame depth at the lookahead's begin. ChoiceByTable: where this alternative's row of FlatProgram::dispatchEntries begins.
    uint32_t target { 0 };
    uint32_t needed { 0 }; // ChoiceByTable: where its table's part of FlatProgram::dispatchClasses begins.
};

// Says nothing about characters from 256 up.
struct FirstSetBits {
    std::array<uint64_t, 4> bits { };
    bool atEnd { false };

    friend bool operator==(const FirstSetBits&, const FirstSetBits&) = default;
};

struct FlatFrame {
    uint32_t op;
    uint32_t position;
    uint32_t limit;
    uint32_t extra;
};

enum class FlatStartScan : uint8_t { None, OneCharacter, Table };

class FlatProgram {
    WTF_MAKE_TZONE_ALLOCATED(FlatProgram);
    WTF_MAKE_NONCOPYABLE(FlatProgram);
public:
    FlatProgram() = default;

    JS_EXPORT_PRIVATE size_t estimatedSizeInBytes() const;

    Vector<FlatOp> ops;
    Vector<CharacterSetBits> characterSets;
    Vector<FirstSetBits> firstSets;
    // A disjunction of more than two alternatives has a table. Its part of dispatchClasses gives every Latin-1 character, and the end of the input, a class: the characters of a class begin the same alternatives.
    // An alternative's row of dispatchEntries says for every class which alternative, from this one on, is the first that can begin with it: this one, or the op to go on at.
    static constexpr unsigned dispatchClassesPerTable = 257;
    static constexpr uint32_t onlyThisAlternativeBeginsHere = 0;
    static constexpr uint32_t thisAndLaterAlternativesBeginHere = 1;
    static constexpr uint32_t noAlternativeBeginsHere = 2;
    static constexpr uint32_t firstDispatchTarget = 3;
    Vector<uint16_t> dispatchClasses;
    Vector<uint32_t> dispatchEntries;
    Vector<CharacterRange> wideRanges;
    Vector<Latin1Character> text;
    Vector<char16_t> wideText;
    // Bit i of a character's entry: a match can have that character at its place i. The last entry stands for every character from 256 up. No match is shorter than startTableDepth.
    std::array<uint8_t, 257> startTable { };
    uint8_t startTableDepth { 0 };
    // Some matches begin with a ^ that holds at the start of the input alone. The tables say nothing about them.
    bool mayMatchOnlyAtInputStart { false };
    static constexpr unsigned maxStartTableDepth = 4;
    // For sixteen characters at once. Bit i is set in the entry of a character's low four bits and in the entry of its high four bits if the character can be at place i, and for some that cannot.
    // The bits of the places from startTableDepth on are set everywhere.
    alignas(16) std::array<uint8_t, 16> startTableOfLowBits { };
    alignas(16) std::array<uint8_t, 16> startTableOfHighBits { };
    FlatStartScan startScan { FlatStartScan::None };
    Latin1Character onlyStartCharacter { 0 };
    unsigned numSubpatterns { 0 };
    unsigned numRegisters { 0 };
    unsigned framesOfRegisters { 0 }; // The registers lie below the frames, in as many frames' room.
    std::optional<unsigned> endAnchoredFixedSize;
    bool triesOnlyOnce { false };
    // Of a pattern in unicode mode with a step that can consume a surrogate pair. A subject of 8-bit characters has none. In any other subject a match that meets a surrogate, in a step
    // that tests for a class or goes back, where an attempt begins or where one might, is given up: JSRegExpResult::JITCodeFailure. The bytecode interpreter makes it.
    bool givesUpAtSurrogates { false };
    // Whether the JIT takes turns off matchLimit for this pattern, and so can give a match up.
    bool jitCountsTurns { false };
    bool countsTurns { false };
    // A fast program that has a table to start over with, or something behind it that counts turns, stops at this many steps back for each character of the subject and 32 more: see FlatMatchPolicy.
    // What counts turns takes longer over a step, so it pays later. 0: it does not stop early.
    uint8_t stepsPerCharacterBeforeStopping { 0 };
    unsigned memoRows { 0 };
    // Inside a turn of a group that may take nothing, the rest of the search depends on the position alone only once the turn has taken something: once the register that holds where the turn began
    // differs from the position. An op's row names the innermost such turn around it, which names the next one out. 0: none.
    struct TurnThatMayTakeNothing {
        uint16_t registerIndex;
        uint16_t enclosing;
    };
    Vector<TurnThatMayTakeNothing> turnsThatMayTakeNothing;
    // Inside a loop that counts its turns the rest of the search depends on the count too, up to a bound from which on every count is treated alike. An op there has a row for each count
    // up to the bound, for each count of the next such loop out, and so on: FlatOp::memoRow is the first of them.
    struct LoopThatCountsTurns {
        uint16_t registerIndex;
        uint16_t bound;
        uint16_t enclosing;
    };
    Vector<LoopThatCountsTurns> loopsThatCountTurns;
    // Inside a lookahead a search ends in a third way: the lookahead is through, and what is left of the search is dropped. So a place there is entered in the table when the search from it
    // has failed, not when it begins.
    struct AroundMemoRow {
        uint16_t turnThatMayTakeNothing;
        uint16_t loopThatCountsTurns;
        bool isInsideLookahead;
    };
    Vector<AroundMemoRow> aroundMemoRow;
    unsigned recordFailureOp { 0 };
    static constexpr uint32_t enclosureBeginsLine = 1;
    static constexpr uint32_t enclosureEndsLine = 2;
    static constexpr uint32_t enclosureIsMultiline = 4;
    static constexpr uint32_t enclosureDotMatchesAll = 8;
    // FlatProgramForm::CountsTurns: whoever starts a match sets this to matchLimit, and keeps every other thread from starting one.
    mutable unsigned remainingTurns { 0 };
};

// Fast: leaves out every part of the search that it can tell will fail. It gives up at matchLimit steps, which says nothing about where the JIT gives up.
// CountsTurns: makes the same search as the bytecode, which tests for input where the JIT does, and takes one off matchLimit where the JIT does: see jitCountsTurnsOf(). Where no turn is left there is no match.
// It still gives up at matchLimit steps between two turns.
enum class FlatProgramForm : bool { Fast, CountsTurns };

// A search that goes back far more often than its subject is long is making the same searches again. From an op that has a memoRow the search depends on the position alone, so where it has failed once
// it fails again: a table of rows by positions says where, and how many turns the failed search began. With the table a match takes time in proportion to ops times positions.
// A program that counts turns keeps the table from the start: it takes the turns of a search that it leaves out off matchLimit, and so gives up exactly where it would without the table.
// A fast program of a pattern whose turns the JIT does not count starts over with the table. Any other fast program gives up, since with the table its steps would say nothing about the JIT's turns:
// HandsOverEarly. RunsToMatchLimit is for where nothing stands behind it that counts turns. RemembersFromStart is for the checking mode.
enum class FlatMatchPolicy : uint8_t { HandsOverEarly, RunsToMatchLimit, RemembersFromStart };

enum class FlatProgramDecline : uint8_t {
    UnicodeMode,
    Lookbehind,
    DuplicateNamedGroups,
    UnsignedLengthPattern,
    CopiedGroup,
    NestedTooDeep,
    TooManySetsOrRegisters,
    NoTurnsToCount,
};

struct RegExpMatchOutcome {
    JSRegExpResult result;
    const unsigned* output;
};

std::expected<std::unique_ptr<FlatProgram>, FlatProgramDecline> compileFlatProgram(const YarrPattern&, FlatProgramForm = FlatProgramForm::Fast);
// Whether the JIT takes one off matchLimit for each turn that this repeated group begins: it does where it keeps a context for each turn (YarrGenerator::opCompileParenthesesSubpattern()).
bool jitCountsTurnsOf(const PatternTerm&);
void removeUnneededLogging(FlatProgram&);
JSRegExpResult matchFlatProgram(const FlatProgram&, StringView input, unsigned start, unsigned* output, FlatMatchPolicy = FlatMatchPolicy::HandsOverEarly);
JS_EXPORT_PRIVATE JSRegExpResult matchFlatProgram(const FlatProgram&, std::span<const Latin1Character> input, unsigned start, unsigned* output, FlatMatchPolicy = FlatMatchPolicy::HandsOverEarly);
void validateFlatProgramResult(const FlatProgram&, StringView input, unsigned start, RegExpMatchOutcome flatProgramOutcome, RegExpMatchOutcome interpreterOutcome);
ASCIILiteral description(FlatProgramDecline);

} } // namespace JSC::Yarr
