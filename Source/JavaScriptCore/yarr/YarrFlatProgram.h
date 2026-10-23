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
    macro(LoggedIncrement)

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
    macro(InputStart) \
    macro(LineStart) \
    macro(InputEnd) \
    macro(LineEnd) \
    macro(WordBoundary) \
    macro(NotWordBoundary) \
    macro(Match) \
    macro(JumpIfBelow) \
    macro(JumpIfNotBelow) \
    macro(LoopBack) \
    macro(LoopBackUnlessEmpty) \
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

enum class FlatStartScan : uint8_t { None, OneCharacter, Set };

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
    FirstSetBits startSet;
    bool startMayBeWide { false };
    FlatStartScan startScan { FlatStartScan::None };
    Latin1Character onlyStartCharacter { 0 };
    unsigned numSubpatterns { 0 };
    unsigned numRegisters { 0 };
    std::optional<unsigned> endAnchoredFixedSize;
    bool triesOnlyOnce { false };
    // Of a pattern in unicode mode with a step that can consume a surrogate pair. A subject of 8-bit characters has none. Any other subject goes to the bytecode interpreter.
    bool runsOnlyOn8BitSubjects { false };
};

enum class FlatProgramDecline : uint8_t {
    UnicodeMode,
    Modifiers,
    Lookbehind,
    DuplicateNamedGroups,
    UnsignedLengthPattern,
    CopiedGroup,
    DotStarEnclosure,
    NestedTooDeep,
    TooManySetsOrRegisters,
};

struct RegExpMatchOutcome {
    JSRegExpResult result;
    const unsigned* output;
};

std::expected<std::unique_ptr<FlatProgram>, FlatProgramDecline> compileFlatProgram(const YarrPattern&);
// Whether the JIT takes one off matchLimit for each turn that this repeated group begins: it does where it keeps a context for each turn (YarrGenerator::opCompileParenthesesSubpattern()).
bool jitCountsTurnsOf(const PatternTerm&);
void removeUnneededLogging(FlatProgram&);
JSRegExpResult matchFlatProgram(const FlatProgram&, StringView input, unsigned start, unsigned* output);
JS_EXPORT_PRIVATE JSRegExpResult matchFlatProgram(const FlatProgram&, std::span<const Latin1Character> input, unsigned start, unsigned* output);
void validateFlatProgramResult(const FlatProgram&, StringView input, unsigned start, RegExpMatchOutcome flatProgramOutcome, RegExpMatchOutcome interpreterOutcome);
ASCIILiteral description(FlatProgramDecline);

} } // namespace JSC::Yarr
