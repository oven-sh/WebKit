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
#include "YarrRequiredText.h"
#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
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
    macro(SetRegister) \
    macro(Increment) \
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

// The steps of a lookbehind that is read backward are made where a machine leaves its loop, as the enclosure by .* is. They have its kind, so that a machine that never meets one has nothing of them.
// So is the search for the next place where a run can end in front of a back reference, which goes forward for a lazy run.
enum class FlatStepBackward : uint16_t { None, Set, NeedsInput, BackReference, LazyRunBeforeReference, GreedyRunBeforeReference, ShorterGreedyRunBeforeReference };

struct FlatOp {
    enum class Kind : uint8_t {
#define FLAT_OP_ENUMERATOR(name) name,
        FOR_EACH_FLAT_OP(FLAT_OP_ENUMERATOR)
#undef FLAT_OP_ENUMERATOR
    };
    Kind kind;
    uint16_t set { 0 }; // Index into FlatProgram::characterSets.
    uint16_t firstSet { 0 }; // Index into FlatProgram::firstSets. ChoiceByFirst: what this alternative can begin with. *BeforeKnownCharacters: what follows the run.
    uint16_t laterFirstSet { 0 }; // Index into FlatProgram::firstSets. ChoiceByFirst: what any later alternative can begin with. *RunBeforeReference: 0 if the group ended before the run, else one more than the units between the run and the end of the group.
    uint16_t registerIndex { 0 }; // DotStarEnclosure: a FlatStepBackward.
    uint16_t memoRow { 0 }; // One more than this op's first row of the table of failed searches. 0: the search from here on depends on more than the position, or makes no choice.
    uint32_t argument { 0 }; // Lookahead ops: the register that holds the frame depth at the lookahead's begin. ChoiceByTable: where this alternative's row of FlatProgram::dispatchEntries begins.
    uint32_t target { 0 }; // *RunBeforeReference: the units between the run and the reference, and from bit 16 up the register that holds the least end of a greedy run.
    uint32_t needed { 0 }; // ChoiceByTable: where its table's part of FlatProgram::dispatchClasses begins. FlatStepBackward::BackReference: whether it ignores case. GreedyRunBeforeReference: the least length of the run.
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

enum class FlatStartScan : uint8_t { None, OneCharacter, Table, TableBehindPair, RequiredTextsAlone };

class FlatProgram {
    WTF_MAKE_TZONE_ALLOCATED(FlatProgram);
    WTF_MAKE_NONCOPYABLE(FlatProgram);
public:
    FlatProgram() = default;

    JS_EXPORT_PRIVATE size_t estimatedSizeInBytes() const;
    bool hasStartTable() const { return startScan == FlatStartScan::Table || startScan == FlatStartScan::TableBehindPair; }
    // Whether the tables of where a match can begin say as much.
    bool startScanCovers(const RequiredText& required) const
    {
        unsigned depth = startScan == FlatStartScan::OneCharacter ? 1 : hasStartTable() ? startTableDepth : 0;
        if (required.isAllThatMatchesAtEndHold || required.greatestDistance >= depth)
            return false;
        return required.greatestDistance + (required.text.isEmpty() ? 1 : required.text.size() - required.indexInText) <= depth;
    }

    // Whether they leave few attempts for a text to save.
    bool hasSelectiveStartScan() const
    {
        std::array<uint64_t, 2> asciiMembers { };
        bool matchesNonASCII = false;
        if (startScan == FlatStartScan::OneCharacter) {
            if (onlyStartCharacter < 128)
                asciiMembers[onlyStartCharacter >> 6] |= 1ull << (onlyStartCharacter & 63);
            else
                matchesNonASCII = true;
        } else if (hasStartTable()) {
            for (unsigned character = 0; character < startTable.size(); ++character) {
                if (!(startTable[character] & 1))
                    continue;
                if (character < 128)
                    asciiMembers[character >> 6] |= 1ull << (character & 63);
                else
                    matchesNonASCII = true;
            }
        } else
            return false;
        return rarityOfUnits(asciiMembers, matchesNonASCII) >= leastRarityWorthSearchingFor;
    }

    void setRequiredTexts(const RequiredTexts& texts)
    {
        requiredTexts = texts;
        if (texts.isEmpty())
            return;
        // A text at a fixed distance names the one place to try, and the attempt there fails as soon as the table would.
        bool isTextAtFixedDistance = texts.size() == 1 && !texts[0].text.isEmpty() && texts[0].leastDistance == texts[0].greatestDistance;
        if (startScan == FlatStartScan::None || isTextAtFixedDistance || (mayMatchOnlyAtInputEnd && texts[0].isAllThatMatchesAtEndHold))
            startScan = FlatStartScan::RequiredTextsAlone;
        // The search that skips starts takes it that an attempt was made one place in front of where it is to begin.
        if (std::exchange(skipsStartsBehindFailedRuns, false))
            firstStartFoundByTablesAlone = 0;
    }

    void setRequiredTextsAskedLate(const RequiredTexts& texts)
    {
        requiredTexts = texts;
        asksRequiredTextsLate = true;
    }

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
    // Some matches begin with a $ that holds at the end of the input alone. The tables say nothing about them, and they are shorter than startTableDepth.
    bool mayMatchOnlyAtInputEnd { false };
    // In front of this place there is more to ask than the tables. 1: mayMatchOnlyAtInputStart. The greatest number: skipsStartsBehindFailedRuns.
    unsigned firstStartFoundByTablesAlone { 0 };
    bool skipsStartsBehindFailedRuns { false };
    // How every match begins: steps of one unit and runs without an upper bound. Only the last, a run, may have a choice of where to end. See findStartBehindFailedRun().
    struct StepOrRun {
        uint16_t set { 0 };
        bool isRun { false };
    };
    static constexpr unsigned maxStepsAndRunsAtStart = 8;
    std::array<StepOrRun, maxStepsAndRunsAtStart> stepsAndRunsAtStart { };
    uint8_t numberOfStepsAndRunsAtStart { 0 };
    // The same for the search that knows one run. With several runs there are more steps than a subject has units: that search finds no room for them and hands over, at no cost to the others.
    struct RunBehindFixedSteps {
        static constexpr unsigned maxSteps = 4;
        static constexpr unsigned moreStepsThanUnits = 1u << 31;
        std::array<uint16_t, maxSteps> stepSets { };
        uint16_t runSet { 0 };
        unsigned steps { 0 };
    };
    RunBehindFixedSteps runBehindFixedSteps;
    static constexpr unsigned maxStartTableDepth = 4;
    // For sixteen characters at once. Bit i is set in the entry of a character's low four bits and in the entry of its high four bits if the character can be at place i, and for some that cannot.
    // The bits of the places from startTableDepth on are set everywhere.
    alignas(16) std::array<uint8_t, 16> startTableOfLowBits { };
    alignas(16) std::array<uint8_t, 16> startTableOfHighBits { };
    // FlatStartScan::TableBehindPair: two places of the table that each take one unit, or one letter in both cases. In a long subject the table is asked only where such a pair stands.
    struct StartPair {
        Latin1Character first { 0 };
        Latin1Character second { 0 };
        uint8_t firstCaseMask { 0 };
        uint8_t secondCaseMask { 0 };
        uint8_t firstPlace { 0 };
        uint8_t secondPlace { 0 };
    };
    StartPair startPair;
    static constexpr unsigned leastLengthSearchedByStartPair = 64;
    FlatStartScan startScan { FlatStartScan::None };
    // No attempt is made where these rule a match out.
    RequiredTexts requiredTexts;
    // `\/[^\/]*\.log` on a path: the start scan is selective, so the texts are not asked at every start. But it stops at every `/` and each attempt walks a folder's name: together they walk the subject.
    // Once the attempts have come this many units without a match, and as many are left, the texts are looked for, one time: the search walks sixteen units at a time.
    bool asksRequiredTextsLate { false };
    static constexpr unsigned unitsWalkedBeforeTextsAreAsked = 128;
    // Of a program that makes one attempt, with a run in front of the text: `^(?:.*\/)?[^\/]*\.pem$` walks a path once for each of its folders.
    // A long subject is looked through for the text first: the run alone costs more. A shorter one when the attempt has gone back this often, and then the attempt begins anew: a short attempt pays nothing.
    std::optional<RequiredText> textOfEveryMatch;
    static constexpr unsigned leastLengthLookedThroughFirst = 256;
    static constexpr unsigned stepsBeforeLookingForText = 9;
    uint8_t stepsPerCharacterBehindLookForText { 0 };

    void setTextOfEveryMatch(RequiredText&& text)
    {
        textOfEveryMatch = WTF::move(text);
        stepsPerCharacterBehindLookForText = std::exchange(stepsPerCharacterBeforeStopping, 0);
        isEnteredOutOfLine = true;
    }
    Latin1Character onlyStartCharacter { 0 };
    // The first character of every match, if there is only one. Unlike onlyStartCharacter it can be wide, and the table may be deeper than one place.
    std::optional<char32_t> onlyStart;
    unsigned numSubpatterns { 0 };
    unsigned numRegisters { 0 };
    unsigned framesOfRegisters { 0 }; // The registers lie below the frames, in as many frames' room.
    std::optional<unsigned> endAnchoredFixedSize;
    bool triesOnlyOnce { false };
    // For each op, the capture slots [begin, end) that an attempt can have written by the time it gets there. The full machine resets only those after a failed attempt.
    struct SlotRange {
        uint32_t begin { 0 };
        uint32_t end { 0 };
    };
    Vector<SlotRange> dirtyCaptureRanges;
    // FlatProgramForm::CountsTurns: the op that every attempt but the first begins at. In front of it stand the alternatives that are tried at the first place alone.
    unsigned laterAttemptsBeginAt { 0 };
    // Of a pattern in unicode mode with a step that can consume a surrogate. There a lead surrogate and the trail surrogate behind it are one character, and that trail surrogate alone is none.
    // A match that meets a surrogate, in a step that tests for a class or goes back, where an attempt begins or where one might, is made again by the full machine.
    bool decodesSurrogatePairs { false };
    // Whether the JIT takes turns off matchLimit for this pattern, and so can give a match up.
    bool jitCountsTurns { false };
    bool countsTurns { false };
    // A fast program that has a table to start over with, or something behind it that counts turns, stops at this many steps back for each character of the subject and 32 more: see FlatMatchPolicy.
    // What counts turns takes longer over a step, so it pays later. 0: it does not stop early.
    uint8_t stepsPerCharacterBeforeStopping { 0 };
    // A lookbehind with a group, a run or a reference is read backward, a surrogate pair is taken as one character, and the table is kept beside references, by a machine of its own,
    // so that the machines for everything else stay small. needsFullMachine: from the start, and then stepsPerCharacterBeforeStopping is 0.
    uint8_t stepsPerCharacterBeforeStoppingOnFullMachine { 0 };
    // FlatProgramForm::Fast: whether the pattern has a program of the form FindsEndsOfRuns, and that program. A search that has taken the steps above is made again with it.
    bool hasRunBeforeReference { false };
    std::unique_ptr<FlatProgram> programThatFindsEndsOfRuns;
    bool needsFullMachine { false };
    bool isEnteredOutOfLine { false };
    // The pattern is one set, or a run of at least one of its members: [aeiou], \d+. A match needs no machine.
    bool isOneSetOrRun { false };
    // `^(?:break|case|catch)$`: the subject is one of the texts or there is no match. A match needs no machine: a text is found by its length, its first unit and its last.
    struct WholeTexts {
        struct Text {
            uint32_t begin { 0 };
            uint32_t length { 0 };
        };
        static ALWAYS_INLINE unsigned placeOf(unsigned length, char16_t first, char16_t last) { return (first * 31u + last) * 31u + length; }

        Vector<char16_t> units;
        Vector<Text> texts;
        // The index of a text and one more, or 0. A text that finds its place taken stands at the next free one.
        Vector<uint32_t> places;
        unsigned greatestLength { 0 };
    };
    std::optional<WholeTexts> wholeTexts;
    // Rows are numbered in this order: position-keyed, run-suffix, capture-keyed.
    unsigned memoRows { 0 };
    unsigned positionKeyedMemoRows { 0 };
    // An unbounded run retries its continuation at every position of a stretch of matching characters. A run-suffix row marks the positions from which every later retry in the stretch has failed,
    // with the turns those retries used, so runs that overlap scan the stretch once. These rows are dropped first when the table would be too large.
    unsigned runSuffixMemoRows { 0 };
    // Rows whose search also depends on capture bounds that a backreference can still read. See AroundMemoRow::liveCaptureBounds.
    unsigned captureKeyedMemoRows { 0 };
    // The output index of the start of each group that has live bounds.
    Vector<uint32_t> referencedGroups;
    static constexpr unsigned maxReferencedGroups = 8;
    unsigned captureKeyedSlotSize() const { return 2 + 2 * referencedGroups.size(); }
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
    // Every row of an op shares one value.
    struct AroundMemoRow {
        uint16_t turnThatMayTakeNothing { 0 };
        uint16_t loopThatCountsTurns { 0 };
        bool isInsideLookahead { false };
        // Bit 2 * i is the start of referencedGroups[i], and bit 2 * i + 1 is its end.
        uint16_t liveCaptureBounds { 0 };
        // The distance to this row's run-suffix row, or 0 if it has none.
        uint16_t runSuffixRowOffset { 0 };
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
// FindsEndsOfRuns: Fast, but for a run in front of a back reference: see FlatProgramBuilder::tryEmitRunBeforeReference(). It needs the full machine, which costs a short search more than the form saves.
enum class FlatProgramForm : uint8_t { Fast, CountsTurns, FindsEndsOfRuns };

// A search that goes back far more often than its subject is long is making the same searches again. From an op that has a memoRow the search depends on the position alone, so where it has failed once
// it fails again: a table of rows by positions says where, and how many turns the failed search began. With the table a match takes time in proportion to ops times positions.
// A program that counts turns keeps the table from the start: it takes the turns of a search that it leaves out off matchLimit, and so gives up exactly where it would without the table.
// A fast program of a pattern whose turns the JIT does not count starts over with the table. Any other fast program gives up, since with the table its steps would say nothing about the JIT's turns:
// HandsOverEarly. RemembersFromStart is for the checking mode.
enum class FlatMatchPolicy : uint8_t { HandsOverEarly, RemembersFromStart };

enum class FlatProgramDecline : uint8_t {
    ClassWithStrings,
    Lookbehind,
    DuplicateNamedGroups,
    UnsignedLengthPattern,
    NestedTooDeep,
    TooManySetsOrRegisters,
    NoTurnsToCount,
};

struct RegExpMatchOutcome {
    JSRegExpResult result;
    const unsigned* output;
};

std::expected<std::unique_ptr<FlatProgram>, FlatProgramDecline> compileFlatProgram(const YarrPattern&, FlatProgramForm = FlatProgramForm::Fast);
// Whether the pattern has a program of each form, and the one that counts turns keeps a table.
bool hasFlatProgramsThatRememberFailedSearches(StringView pattern, OptionSet<Flags>);
// Whether the JIT takes one off matchLimit for each turn that this repeated group begins: it does where it keeps a context for each turn (YarrGenerator::opCompileParenthesesSubpattern()).
// It depends on whether the group has anything to go back into. What has been found out about a disjunction is kept, so that each is walked once however deep groups are nested.
using KnownBacktrackableContent = UncheckedKeyHashMap<const PatternDisjunction*, bool>;
bool jitCountsTurnsOf(const PatternTerm&, KnownBacktrackableContent&);
void removeUnneededLogging(FlatProgram&);
// For ops that run with nothing to undo: each becomes the form that pushes no undo frame.
void removeLogging(std::span<FlatOp>);
JSRegExpResult matchFlatProgram(const FlatProgram&, StringView input, unsigned start, unsigned* output, FlatMatchPolicy = FlatMatchPolicy::HandsOverEarly);
JS_EXPORT_PRIVATE JSRegExpResult matchFlatProgram(const FlatProgram&, std::span<const Latin1Character> input, unsigned start, unsigned* output, FlatMatchPolicy = FlatMatchPolicy::HandsOverEarly);
// Of a program with isOneSetOrRun: the start and end of every match, as a global search finds them. False: out of memory.
bool collectMatchesOfOneSetOrRun(const FlatProgram&, StringView input, Vector<unsigned, 128>& matches);
// For a match that matchFlatProgram() has given up with JSRegExpResult::JITCodeFailure: see FlatProgram::decodesSurrogatePairs.
JSRegExpResult matchFlatProgramBehindSurrogate(const FlatProgram&, StringView input, unsigned start, unsigned* output, FlatMatchPolicy = FlatMatchPolicy::HandsOverEarly);
void validateFlatProgramResult(const FlatProgram&, StringView input, unsigned start, RegExpMatchOutcome flatProgramOutcome, RegExpMatchOutcome interpreterOutcome);
ASCIILiteral description(FlatProgramDecline);

} } // namespace JSC::Yarr
