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

#include <algorithm>
#include <bit>
#include <limits>
#include <ranges>
#include <span>
#include <tuple>
#include <wtf/ASCIICType.h>
#include <wtf/Hasher.h>
#include <wtf/SetForScope.h>
#include <wtf/StdUnorderedMap.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace Yarr {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FlatProgram);

class FlatProgramBuilder {
public:
    FlatProgramBuilder(const YarrPattern& pattern, FlatProgramForm form)
        : m_pattern(pattern)
        , m_countsTurns(form == FlatProgramForm::CountsTurns)
        , m_findsEndsOfRuns(form == FlatProgramForm::FindsEndsOfRuns)
    {
    }

    std::expected<std::unique_ptr<FlatProgram>, FlatProgramDecline> build()
    {
        if (m_pattern.eitherUnicode()) {
            auto steps = whatStepsConsume(*m_pattern.m_body);
            if (steps == Steps::Strings)
                return makeUnexpected(FlatProgramDecline::ClassWithStrings);
            m_decodesSurrogatePairs = steps == Steps::Surrogates;
        }
        if (m_pattern.hasDuplicateNamedCaptureGroups())
            return makeUnexpected(FlatProgramDecline::DuplicateNamedGroups);
        if (m_pattern.containsUnsignedLengthPattern())
            return makeUnexpected(FlatProgramDecline::UnsignedLengthPattern);

        auto program = makeUnique<FlatProgram>();
        m_program = program.get();
        program->numSubpatterns = m_pattern.m_numSubpatterns;
        if (m_pattern.hasEndAnchoredFixedSize())
            program->endAnchoredFixedSize = m_pattern.m_endAnchoredFixedSize;
        program->triesOnlyOnce = m_pattern.sticky();
        program->decodesSurrogatePairs = m_decodesSurrogatePairs;
        program->countsTurns = m_countsTurns;

        auto& alternatives = m_pattern.m_body->m_alternatives;
        unsigned count = alternatives.size();
        if (alternatives[0]->onceThrough()) {
            unsigned triedOnce = 0;
            while (triedOnce < alternatives.size() && alternatives[triedOnce]->onceThrough())
                ++triedOnce;
            program->triesOnlyOnce |= triedOnce == alternatives.size();
            // Behind these stand copies that lack what begins with ^. The bytecode and the JIT try these and then the copies at the first place, and the copies alone from the second place on.
            // That finds what these alone find at every place, in other turns.
            if (!m_countsTurns)
                count = triedOnce;
            else if (triedOnce < alternatives.size())
                m_firstRepeatedAlternative = alternatives[triedOnce].get();
        }

        if (!emitAlternatives(alternatives.span().first(count), { }))
            return makeUnexpected(*m_decline);
        emit(Kind::Match);
        program->recordFailureOp = emit(Kind::RecordFailure);
        if (m_countsTurns && !std::ranges::contains(program->ops, Kind::CountTurn, &FlatOp::kind))
            return makeUnexpected(FlatProgramDecline::NoTurnsToCount);
        if (hasExceededIndexLimit())
            return makeUnexpected(FlatProgramDecline::TooManySetsOrRegisters);
        program->numRegisters = m_nextRegister;
        program->framesOfRegisters = (m_nextRegister * sizeof(uint32_t) + sizeof(FlatFrame) - 1) / sizeof(FlatFrame);

        assignMemoRows();
        // Machine::popChoice() can leave a RecordFailure frame behind. A capture-keyed entry needs the captures restored by the time that frame resumes.
        if (!program->captureKeyedMemoRows)
            removeUnneededLogging(*program);
        if (!m_countsTurns)
            program->stepsPerCharacterBeforeStoppingOnFullMachine = program->jitCountsTurns ? 32 : program->memoRows ? 8 : 0;
        program->needsFullMachine = std::ranges::any_of(program->ops, isStepBackward);
        program->isEnteredOutOfLine = m_countsTurns || program->needsFullMachine;
        if (!program->needsFullMachine)
            program->stepsPerCharacterBeforeStopping = program->stepsPerCharacterBeforeStoppingOnFullMachine;
        if (!m_countsTurns)
            computeStartScan(alternatives.span().first(count));
        findRunBehindFixedSteps();
        computeDirtyCaptureRanges();

        if (!program->text.isEmpty()) {
            for (unsigned i = 0; i < sizeof(uint64_t); ++i)
                program->text.append(0);
        }
        program->ops.shrinkToFit();
        program->characterSets.shrinkToFit();
        program->firstSets.shrinkToFit();
        program->dispatchClasses.shrinkToFit();
        program->dispatchEntries.shrinkToFit();
        program->wideRanges.shrinkToFit();
        program->text.shrinkToFit();
        program->wideText.shrinkToFit();
        program->turnsThatMayTakeNothing.shrinkToFit();
        program->loopsThatCountTurns.shrinkToFit();
        program->aroundMemoRow.shrinkToFit();
        program->referencedGroups.shrinkToFit();
        return program;
    }

private:
    static bool isSingleCodeUnit(char32_t character) { return U_IS_BMP(character) && !U_IS_SURROGATE(character); }

    static bool holdsSingleCodeUnits(const CharacterClass& characterClass)
    {
        if (characterClass.hasNonBMPCharacters())
            return false;
        bool hasSurrogate = std::ranges::any_of(characterClass.m_matches32, [](char32_t character) {
            return U_IS_SURROGATE(character);
        });
        hasSurrogate |= std::ranges::any_of(characterClass.m_ranges32, [](const CharacterRange& range) {
            return range.begin <= 0xdfff && range.end >= 0xd800;
        });
        return !hasSurrogate;
    }

    enum class Steps : uint8_t { SingleCodeUnits, Surrogates, Strings };

    // In unicode mode a step consumes a code point. Where no step can consume a surrogate, a code point that is consumed is one code unit, and the program for code units answers the same.
    // The parser has made classes of the letters that have other cases. RegExp::matchInline keeps a match from beginning inside a surrogate pair.
    static Steps whatStepsConsume(const PatternDisjunction& disjunction, unsigned depth = 0)
    {
        Steps steps = Steps::SingleCodeUnits;
        if (depth > maxNestingDepth)
            return steps;
        for (auto& alternative : disjunction.m_alternatives) {
            for (const PatternTerm& term : alternative->m_terms) {
                switch (term.type) {
                case PatternTerm::Type::AssertionBOL:
                case PatternTerm::Type::AssertionEOL:
                case PatternTerm::Type::AssertionWordBoundary:
                case PatternTerm::Type::NumberedBackReference:
                case PatternTerm::Type::NamedBackReference:
                case PatternTerm::Type::NumberedForwardReference:
                case PatternTerm::Type::NamedForwardReference:
                    break;
                case PatternTerm::Type::PatternCharacter:
                    if (!isSingleCodeUnit(term.patternCharacter))
                        steps = std::max(steps, Steps::Surrogates);
                    break;
                case PatternTerm::Type::CharacterClass:
                    if (term.characterClass->hasStrings())
                        return Steps::Strings;
                    if (term.invert() || !holdsSingleCodeUnits(*term.characterClass))
                        steps = std::max(steps, Steps::Surrogates);
                    break;
                case PatternTerm::Type::ParenthesesSubpattern:
                case PatternTerm::Type::ParentheticalAssertion:
                    steps = std::max(steps, whatStepsConsume(*term.parentheses.disjunction, depth + 1));
                    break;
                case PatternTerm::Type::DotStarEnclosure:
                    break;
                }
            }
        }
        return steps;
    }

    using Kind = FlatOp::Kind;
    using Alternatives = std::span<const std::unique_ptr<PatternAlternative>>;
    using SetKey = std::tuple<const CharacterClass*, bool, char32_t, char32_t>;

    static constexpr unsigned maxNestingDepth = 64;
    static constexpr unsigned maxTermsVisitedForEmptiness = 64;
    static constexpr unsigned maxSharedPrefixDepth = 32;
    static constexpr size_t maxIndexCount = std::numeric_limits<uint16_t>::max();

    // What the search goes on with behind a group that is taken exactly once.
    struct TermsAfterGroup {
        const PatternAlternative& alternative;
        unsigned begin;
        std::optional<unsigned> capturedGroup;
        const TermsAfterGroup* outer;
    };

    struct EmitContext {
        uint64_t neededAfter { 0 };
        unsigned nestingDepth { 0 };
        unsigned sharedPrefixDepth { 0 };
        // FlatProgramForm::CountsTurns: PatternTerm::inputPosition counts from here in the alternatives that are emitted, and neededAfter is what ByteCompiler::emitDisjunction() has tested for beyond them.
        unsigned initialInputPosition { 0 };
        // FlatProgramForm::CountsTurns: an alternative that needs no more than this, and neededAfter, has been tested for.
        unsigned testedFor { 0 };
        // Null where the alternatives are not those of a group that is taken exactly once.
        const TermsAfterGroup* termsAfterGroup { nullptr };
    };

    // What is left of an alternative from one of its terms on.
    struct Branch {
        const PatternAlternative* alternative;
        unsigned begin;
    };

    // Branches that begin with the same step, in their order. stepTerms is 0 where the first member begins with no step that could be shared.
    struct BranchGroup {
        Vector<Branch, 2> members;
        std::optional<FirstSetBits> first;
        unsigned stepTerms { 0 };
    };

    struct InternedKeyHash {
        size_t operator()(const SetKey& key) const { return computeHash(key); }
        size_t operator()(const FirstSetBits& set) const { return computeHash(set.bits, set.atEnd); }
    };

    bool decline(FlatProgramDecline reason)
    {
        if (!m_decline)
            m_decline = reason;
        return false;
    }

    bool hasExceededIndexLimit() const
    {
        return m_nextRegister > maxIndexCount || m_program->characterSets.size() > maxIndexCount || m_program->firstSets.size() > maxIndexCount;
    }

    unsigned emit(Kind kind, uint32_t argument = 0, uint32_t target = 0)
    {
        FlatOp op { };
        op.kind = kind;
        op.argument = argument;
        op.target = target;
        m_program->ops.append(op);
        return m_program->ops.size() - 1;
    }

    unsigned emitWithSet(Kind kind, unsigned set, uint32_t argument = 0, uint32_t target = 0)
    {
        unsigned op = emit(kind, argument, target);
        m_program->ops[op].set = static_cast<uint16_t>(set);
        return op;
    }

    unsigned emitWithRegister(Kind kind, unsigned registerIndex, uint32_t argument = 0, uint32_t target = 0)
    {
        unsigned op = emit(kind, argument, target);
        m_program->ops[op].registerIndex = static_cast<uint16_t>(registerIndex);
        return op;
    }

    unsigned here() const { return m_program->ops.size(); }

    void isLookaheadSince(unsigned begin) { m_lookaheads.append({ begin, here() }); }
    void isTurnThatMayTakeNothingSince(unsigned begin, unsigned startRegister) { m_turnsThatMayTakeNothing.append({ begin, here(), startRegister }); }
    void isLoopThatCountsTurnsSince(unsigned begin, unsigned counter, unsigned bound) { m_loopsThatCountTurns.append({ begin, here(), counter, bound }); }

    // Backward liveness: the capture bounds that a backreference can read after each op before they are overwritten. Bit 2 * i is the start of m_referencedGroups[i], and bit 2 * i + 1 is its end.
    // A search inside a lookahead stops at the end of the lookahead.
    Vector<uint64_t> computeLiveCaptureBounds()
    {
        auto& ops = m_program->ops;
        Vector<uint64_t> result(FillWith { }, ops.size(), 0);
        auto& groups = m_referencedGroups;
        if (groups.isEmpty())
            return result;

        auto boundsInSlots = [&](unsigned firstSlot, unsigned lastSlot) {
            uint64_t bounds = 0;
            for (unsigned i = 0; i < groups.size(); ++i) {
                for (unsigned bound = 0; bound < 2; ++bound) {
                    if (groups[i] + bound >= firstSlot && groups[i] + bound <= lastSlot)
                        bounds |= 1ull << (2 * i + bound);
                }
            }
            return bounds;
        };
        struct Effect {
            uint64_t reads;
            uint64_t sets;
            bool fallsThrough;
            bool jumpsToTarget;
        };
        auto effects = ops.map([&](const FlatOp& op) -> Effect {
            switch (op.kind) {
            case Kind::BackReference:
            case Kind::BackReferenceIgnoringCase:
                return { boundsInSlots(op.argument, op.argument + 1), 0, true, false };
            case Kind::DotStarEnclosure:
                // This either ends the search or is a backward step, which always fails.
                if (static_cast<FlatStepBackward>(op.registerIndex) >= FlatStepBackward::BackReference)
                    return { boundsInSlots(op.argument, op.argument + 1), 0, false, false };
                return { 0, 0, false, false };
            case Kind::CaptureStart:
            case Kind::CaptureEnd:
            case Kind::LoggedCaptureStart:
            case Kind::LoggedCaptureEnd:
                return { 0, boundsInSlots(op.argument, op.argument), true, false };
            case Kind::CaptureEmpty:
            case Kind::CapturedRun:
            case Kind::LoggedCaptureEmpty:
            case Kind::LoggedCapturedRun:
                return { 0, boundsInSlots(op.argument, op.argument + 1), true, false };
            case Kind::ClearCaptures:
            case Kind::LoggedClearCaptures:
                return { 0, boundsInSlots(op.argument << 1, (op.target << 1) + 1), true, false };
            // Every dispatch target of a ChoiceByTable is also reachable through its chain of targets.
            case Kind::Choice:
            case Kind::ChoiceByFirst:
            case Kind::ChoiceByTable:
            case Kind::GreedyLoopChoice:
            case Kind::NegativeLookaheadBegin:
            case Kind::JumpIfBelow:
            case Kind::JumpIfNotBelow:
            case Kind::NeedsInputElseJump:
                return { 0, 0, true, true };
            case Kind::Jump:
            case Kind::LoopBack:
            case Kind::LoopBackUnlessEmpty:
                return { 0, 0, false, true };
            case Kind::Match:
            case Kind::RecordFailure:
            case Kind::NegativeLookaheadEnd:
                return { 0, 0, false, false };
            case Kind::GreedyRun:
            case Kind::LazyRun:
            case Kind::GreedyRunBeforeKnownCharacters:
            case Kind::LazyRunBeforeKnownCharacters:
            case Kind::LoggedSavePosition:
            case Kind::LoggedSetRegister:
            case Kind::LoggedIncrement:
            case Kind::Character:
            case Kind::Text:
            case Kind::CasedText:
            case Kind::WideText:
            case Kind::Set:
            case Kind::SetCount:
            case Kind::PossessiveRun:
            case Kind::SavePosition:
            case Kind::SetRegister:
            case Kind::Increment:
            case Kind::FailIfEmpty:
            case Kind::NeedsInput:
            case Kind::StepBack:
            case Kind::InputStart:
            case Kind::LineStart:
            case Kind::InputEnd:
            case Kind::LineEnd:
            case Kind::WordBoundary:
            case Kind::NotWordBoundary:
            case Kind::CountTurn:
            case Kind::LookaheadBegin:
            case Kind::LookaheadEnd:
            case Kind::LookaheadEndKeepingCaptures:
                return { 0, 0, true, false };
            }
            RELEASE_ASSERT_NOT_REACHED();
        });

        // Index + 1 into m_lookaheads, which lists inner lookaheads first.
        Vector<unsigned> innermostLookahead(FillWith { }, ops.size(), 0);
        for (unsigned i = m_lookaheads.size(); i--;) {
            auto [begin, end] = m_lookaheads[i];
            std::ranges::fill(innermostLookahead.mutableSpan().subspan(begin + 1, end - begin - 2), i + 1);
        }
        Vector<uint64_t> live(FillWith { }, ops.size(), 0);
        auto solve = [&](unsigned begin, unsigned end, unsigned lookahead) {
            std::ranges::fill(live.mutableSpan().subspan(begin, end - begin), 0);
            bool changed = true;
            while (changed) {
                changed = false;
                for (unsigned i = end; i-- > begin;) {
                    const Effect& effect = effects[i];
                    uint64_t behind = 0;
                    if (effect.fallsThrough && i + 1 < end)
                        behind |= live[i + 1];
                    if (effect.jumpsToTarget && ops[i].target < end)
                        behind |= live[ops[i].target];
                    uint64_t here = (behind & ~effect.sets) | effect.reads;
                    changed |= here != live[i];
                    live[i] = here;
                }
            }
            for (unsigned i = begin; i < end; ++i) {
                if (innermostLookahead[i] == lookahead)
                    result[i] = live[i];
            }
        };
        solve(0, ops.size(), 0);
        for (unsigned i = 0; i < m_lookaheads.size(); ++i)
            solve(m_lookaheads[i].first + 1, m_lookaheads[i].second - 1, i + 1);
        return result;
    }

    template<typename Functor>
    void forEachSuccessor(unsigned i, const Functor& functor) const
    {
        const FlatOp& op = m_program->ops[i];
        switch (op.kind) {
        case Kind::Choice:
        case Kind::ChoiceByFirst:
        case Kind::ChoiceByTable:
        case Kind::GreedyLoopChoice:
        case Kind::NegativeLookaheadBegin:
        case Kind::JumpIfBelow:
        case Kind::JumpIfNotBelow:
        case Kind::NeedsInputElseJump:
            functor(i + 1);
            functor(op.target);
            break;
        case Kind::Jump:
        case Kind::LoopBack:
        case Kind::LoopBackUnlessEmpty:
            functor(op.target);
            break;
        default:
            functor(i + 1);
            break;
        }
    }

    static constexpr unsigned minGroupsForDirtyRanges = 8;

    // Backtracking into a choice undoes what its earlier alternatives wrote, so a write only reaches the ops after it on a path through the ops.
    void computeDirtyCaptureRanges()
    {
        bool mayRunOnFullMachine = m_program->needsFullMachine || m_program->decodesSurrogatePairs || m_program->captureKeyedMemoRows;
        if (!mayRunOnFullMachine || m_program->triesOnlyOnce || m_program->numSubpatterns < minGroupsForDirtyRanges)
            return;
        auto& ops = m_program->ops;
        using SlotRange = FlatProgram::SlotRange;
        auto include = [](SlotRange& range, SlotRange other) {
            if (other.begin == other.end)
                return false;
            SlotRange joined = range.begin == range.end ? other : SlotRange { std::min(range.begin, other.begin), std::max(range.end, other.end) };
            bool changed = joined.begin != range.begin || joined.end != range.end;
            range = joined;
            return changed;
        };
        auto writtenBy = [](const FlatOp& op) -> SlotRange {
            switch (op.kind) {
            case Kind::CaptureStart:
            case Kind::CaptureEnd:
            case Kind::LoggedCaptureStart:
            case Kind::LoggedCaptureEnd:
                return { op.argument, op.argument + 1 };
            case Kind::CaptureEmpty:
            case Kind::CapturedRun:
            case Kind::LoggedCaptureEmpty:
            case Kind::LoggedCapturedRun:
                return { op.argument, op.argument + 2 };
            default:
                return { };
            }
        };
        Vector<SlotRange> ranges(ops.size() + 1);
        bool changed = true;
        while (changed) {
            changed = false;
            for (unsigned i = 0; i < ops.size(); ++i) {
                SlotRange after = ranges[i];
                include(after, writtenBy(ops[i]));
                forEachSuccessor(i, [&](unsigned successor) {
                    changed |= include(ranges[successor], after) && successor <= i;
                });
            }
        }
        ranges.shrink(ops.size());
        m_program->dirtyCaptureRanges = WTF::move(ranges);
    }

    // An op with a single fixed-width path from the start of an attempt runs once per attempt, at a position where no other attempt runs it.
    struct SinglePathOp {
        bool hasSinglePath { false };
        // Capture bounds set along the path, which differ in every attempt. Same bits as computeLiveCaptureBounds().
        uint64_t boundsSetOnPath { 0 };
    };
    Vector<SinglePathOp> findSinglePathOps()
    {
        auto& ops = m_program->ops;
        Vector<SinglePathOp> result(ops.size());
        if (m_decodesSurrogatePairs)
            return result;
        auto boundsInSlots = [&](unsigned firstSlot, unsigned lastSlot) {
            uint64_t bounds = 0;
            for (unsigned i = 0; i < m_referencedGroups.size(); ++i) {
                for (unsigned bound = 0; bound < 2; ++bound) {
                    if (m_referencedGroups[i] + bound >= firstSlot && m_referencedGroups[i] + bound <= lastSlot)
                        bounds |= 1ull << (2 * i + bound);
                }
            }
            return bounds;
        };
        // The start of an attempt counts as a predecessor.
        Vector<unsigned> predecessorCounts(FillWith { }, ops.size() + 1, 0);
        ++predecessorCounts[0];
        if (m_program->laterAttemptsBeginAt)
            ++predecessorCounts[m_program->laterAttemptsBeginAt];
        for (unsigned i = 0; i < ops.size(); ++i) {
            forEachSuccessor(i, [&](unsigned successor) {
                ++predecessorCounts[successor];
            });
        }
        Vector<std::pair<unsigned, uint64_t>, 8> worklist { { 0, 0 } };
        while (!worklist.isEmpty()) {
            auto [i, bounds] = worklist.takeLast();
            for (bool continues = true; continues; ++i) {
                const FlatOp& op = ops[i];
                // The last Choice of tryEmitRunBeforeReference() is the frame of the run.
                if (i + 1 < ops.size() && (op.kind == Kind::Jump || op.kind == Kind::Choice) && isRunBeforeReference(ops[i + 1 + (op.kind == Kind::Jump)])) {
                    unsigned frameOfRun = i;
                    while (ops[frameOfRun].kind != Kind::Choice || isRunBeforeReference(ops[frameOfRun + 1]))
                        ++frameOfRun;
                    result[frameOfRun] = { true, bounds };
                    break;
                }
                if (predecessorCounts[i] > 1)
                    break;
                switch (op.kind) {
                case Kind::Character:
                case Kind::Text:
                case Kind::CasedText:
                case Kind::WideText:
                case Kind::Set:
                case Kind::SetCount:
                case Kind::NeedsInput:
                case Kind::InputStart:
                case Kind::LineStart:
                case Kind::InputEnd:
                case Kind::LineEnd:
                case Kind::WordBoundary:
                case Kind::NotWordBoundary:
                case Kind::LookaheadBegin:
                    break;
                case Kind::CaptureStart:
                case Kind::CaptureEnd:
                case Kind::LoggedCaptureStart:
                case Kind::LoggedCaptureEnd:
                    bounds |= boundsInSlots(op.argument, op.argument);
                    break;
                case Kind::CaptureEmpty:
                case Kind::LoggedCaptureEmpty:
                    bounds |= boundsInSlots(op.argument, op.argument + 1);
                    break;
                case Kind::Choice:
                case Kind::ChoiceByFirst:
                case Kind::ChoiceByTable:
                    if (isStepBackward(ops[i + 1])) {
                        continues = false;
                        break;
                    }
                    result[i] = { true, bounds };
                    worklist.append({ op.target, bounds });
                    break;
                case Kind::GreedyRun:
                case Kind::LazyRun:
                case Kind::GreedyRunBeforeKnownCharacters:
                case Kind::LazyRunBeforeKnownCharacters:
                    result[i] = { true, bounds };
                    continues = false;
                    break;
                default:
                    continues = false;
                    break;
                }
            }
        }
        return result;
    }

    void assignMemoRows()
    {
        if (m_searchDependsOnMoreThanPosition)
            return;
        auto& ops = m_program->ops;
        Vector<bool> hasTooManyRows(FillWith { }, ops.size(), false);
        Vector<bool> isInsideLookahead(FillWith { }, ops.size(), false);
        for (auto [begin, end] : m_lookaheads)
            std::ranges::fill(isInsideLookahead.mutableSpan().subspan(begin, end - begin), true);
        // An inner turn ends, and so is listed, before the one around it.
        Vector<uint16_t> turnAround(FillWith { }, ops.size(), 0);
        auto& turns = m_program->turnsThatMayTakeNothing;
        turns.append(FlatProgram::TurnThatMayTakeNothing { 0, 0 });
        for (auto [begin, end, startRegister] : m_turnsThatMayTakeNothing.span() | std::views::reverse) {
            turns.append(FlatProgram::TurnThatMayTakeNothing { static_cast<uint16_t>(startRegister), turnAround[begin] });
            std::ranges::fill(turnAround.mutableSpan().subspan(begin, end - begin), static_cast<uint16_t>(turns.size() - 1));
        }
        constexpr unsigned maxRowsOfOp = 1024;
        Vector<uint16_t> loopAround(FillWith { }, ops.size(), 0);
        Vector<unsigned> rowsOfOp(FillWith { }, ops.size(), 1);
        auto& loops = m_program->loopsThatCountTurns;
        loops.append(FlatProgram::LoopThatCountsTurns { 0, 0, 0 });
        for (auto [begin, end, counter, bound] : m_loopsThatCountTurns.span() | std::views::reverse) {
            uint64_t rows = static_cast<uint64_t>(rowsOfOp[begin]) * (static_cast<uint64_t>(bound) + 1);
            if (rows > maxRowsOfOp) {
                std::ranges::fill(hasTooManyRows.mutableSpan().subspan(begin, end - begin), true);
                continue;
            }
            loops.append(FlatProgram::LoopThatCountsTurns { static_cast<uint16_t>(counter), static_cast<uint16_t>(bound), loopAround[begin] });
            std::ranges::fill(loopAround.mutableSpan().subspan(begin, end - begin), static_cast<uint16_t>(loops.size() - 1));
            std::ranges::fill(rowsOfOp.mutableSpan().subspan(begin, end - begin), static_cast<unsigned>(rows));
        }
        // A row only helps if the same search can start twice. A single-path choice never repeats, and neither does a single-path run whose search depends on a bound set along the path.
        auto liveBoundsOfAllGroups = computeLiveCaptureBounds();
        auto singlePathOps = findSinglePathOps();
        auto canHaveRows = [&](unsigned i) {
            switch (ops[i].kind) {
            case Kind::Choice:
            case Kind::ChoiceByFirst:
            case Kind::ChoiceByTable:
                if (singlePathOps[i].hasSinglePath)
                    return false;
                [[fallthrough]];
            case Kind::GreedyLoopChoice:
            case Kind::GreedyRun:
            case Kind::LazyRun:
            case Kind::GreedyRunBeforeKnownCharacters:
            case Kind::LazyRunBeforeKnownCharacters:
                if (liveBoundsOfAllGroups[i] & singlePathOps[i].boundsSetOnPath)
                    return false;
                // The step behind such a Choice changes the position in its frame.
                return !hasTooManyRows[i] && !isStepBackward(ops[i + 1]);
            default:
                return false;
            }
        };
        // Entries hold the bounds of at most maxReferencedGroups groups. Past that, ops with live bounds get no row.
        Vector<uint16_t> liveCaptureBounds(FillWith { }, ops.size(), 0);
        Vector<bool> hasTooManyLiveBounds(FillWith { }, ops.size(), false);
        {
            uint64_t liveAtSomeRow = 0;
            for (unsigned i = 0; i < ops.size(); ++i)
                liveAtSomeRow |= canHaveRows(i) ? liveBoundsOfAllGroups[i] : 0;
            Vector<unsigned, FlatProgram::maxReferencedGroups> liveGroups;
            for (unsigned group = 0; group < m_referencedGroups.size(); ++group) {
                if ((liveAtSomeRow >> (2 * group)) & 3)
                    liveGroups.append(group);
            }
            bool hasTooManyLiveGroups = liveGroups.size() > FlatProgram::maxReferencedGroups;
            for (unsigned i = 0; i < ops.size(); ++i) {
                if (hasTooManyLiveGroups) {
                    hasTooManyLiveBounds[i] = !!liveBoundsOfAllGroups[i];
                    continue;
                }
                for (unsigned index = 0; index < liveGroups.size(); ++index)
                    liveCaptureBounds[i] |= static_cast<uint16_t>(((liveBoundsOfAllGroups[i] >> (2 * liveGroups[index])) & 3) << (2 * index));
            }
            if (!hasTooManyLiveGroups) {
                for (unsigned group : liveGroups)
                    m_program->referencedGroups.append(m_referencedGroups[group]);
            }
        }
        auto hasRunSuffixRow = [&](unsigned i) {
            switch (ops[i].kind) {
            case Kind::GreedyRun:
            case Kind::LazyRun:
            case Kind::GreedyRunBeforeKnownCharacters:
            case Kind::LazyRunBeforeKnownCharacters:
                return ops[i].argument == quantifyInfinite && !liveCaptureBounds[i];
            default:
                return false;
            }
        };
        Vector<unsigned> opsWithRows;
        for (unsigned i = 0; i < ops.size() && m_program->memoRows + 2 * maxRowsOfOp < maxIndexCount; ++i) {
            if (!canHaveRows(i) || hasTooManyLiveBounds[i])
                continue;
            opsWithRows.append(i);
            (liveCaptureBounds[i] ? m_program->captureKeyedMemoRows : m_program->positionKeyedMemoRows) += rowsOfOp[i];
            if (hasRunSuffixRow(i))
                m_program->runSuffixMemoRows += rowsOfOp[i];
            m_program->memoRows = m_program->positionKeyedMemoRows + m_program->runSuffixMemoRows + m_program->captureKeyedMemoRows;
        }
        m_program->aroundMemoRow.grow(m_program->memoRows + 1);
        unsigned nextPositionKeyedRow = 1;
        unsigned nextRunSuffixRow = nextPositionKeyedRow + m_program->positionKeyedMemoRows;
        unsigned nextCaptureKeyedRow = nextRunSuffixRow + m_program->runSuffixMemoRows;
        for (unsigned i : opsWithRows) {
            unsigned& next = liveCaptureBounds[i] ? nextCaptureKeyedRow : nextPositionKeyedRow;
            FlatProgram::AroundMemoRow around { turnAround[i], loopAround[i], isInsideLookahead[i], liveCaptureBounds[i], 0 };
            if (hasRunSuffixRow(i)) {
                around.runSuffixRowOffset = static_cast<uint16_t>(nextRunSuffixRow - next);
                nextRunSuffixRow += rowsOfOp[i];
            }
            ops[i].memoRow = static_cast<uint16_t>(next);
            std::ranges::fill(m_program->aroundMemoRow.mutableSpan().subspan(next, rowsOfOp[i]), around);
            next += rowsOfOp[i];
        }
    }
    void setTarget(unsigned op) { m_program->ops[op].target = here(); }

    // In units, as YarrPatternConstructor::setupAlternativeOffsets() counts them.
    static uint64_t unitsOfOne(const PatternTerm& term)
    {
        if (term.type == PatternTerm::Type::PatternCharacter)
            return U16_LENGTH(term.patternCharacter);
        return term.characterClass->hasOneCharacterSize() && !term.invert() && term.characterClass->hasNonBMPCharacters() ? 2 : 1;
    }

    static uint64_t minimumSize(const PatternTerm& term)
    {
        switch (term.type) {
        case PatternTerm::Type::PatternCharacter:
        case PatternTerm::Type::CharacterClass:
            return static_cast<unsigned>(term.quantityMinCount) * unitsOfOne(term);
        case PatternTerm::Type::ParenthesesSubpattern:
            return static_cast<uint64_t>(static_cast<unsigned>(term.quantityMinCount)) * term.parentheses.disjunction->m_minimumSize;
        case PatternTerm::Type::AssertionBOL:
        case PatternTerm::Type::AssertionEOL:
        case PatternTerm::Type::AssertionWordBoundary:
        case PatternTerm::Type::NumberedBackReference:
        case PatternTerm::Type::NamedBackReference:
        case PatternTerm::Type::NumberedForwardReference:
        case PatternTerm::Type::NamedForwardReference:
        case PatternTerm::Type::ParentheticalAssertion:
        case PatternTerm::Type::DotStarEnclosure:
            return 0;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void setNeeded(unsigned op, uint64_t needed)
    {
        m_program->ops[op].needed = static_cast<uint32_t>(std::min<uint64_t>(needed, std::numeric_limits<uint32_t>::max()));
    }

    unsigned emitWithNeeded(Kind kind, uint64_t needed)
    {
        unsigned op = emit(kind);
        setNeeded(op, needed);
        return op;
    }

    static bool isStepBackward(const FlatOp& op) { return op.kind == Kind::DotStarEnclosure && op.registerIndex; }
    static bool isRunBeforeReference(const FlatOp& op) { return isStepBackward(op) && static_cast<FlatStepBackward>(op.registerIndex) >= FlatStepBackward::LazyRunBeforeReference; }

    // See Enclosure::readBackward().
    FlatOp& emitStepBackward(FlatStepBackward step)
    {
        unsigned choice = emit(Kind::Choice);
        unsigned op = emitWithRegister(Kind::DotStarEnclosure, std::to_underlying(step));
        setTarget(choice);
        return m_program->ops[op];
    }

    void emitNeedsInput(uint64_t needed)
    {
        if (!m_readsBackward) {
            emitWithNeeded(Kind::NeedsInput, needed);
            return;
        }
        emitStepBackward(FlatStepBackward::NeedsInput).needed = static_cast<uint32_t>(std::min<uint64_t>(needed, std::numeric_limits<uint32_t>::max()));
    }
    unsigned emitNeedsInputElseJump(uint64_t needed) { return emitWithNeeded(Kind::NeedsInputElseJump, needed); }

    // In a lookbehind each alternative tests for itself.
    uint64_t neededToEnter(const PatternDisjunction& disjunction, uint64_t neededAfter) const
    {
        return !m_readsBackward && disjunction.m_minimumSize && neededAfter ? disjunction.m_minimumSize + neededAfter : 0;
    }

    static bool hasAllLatin1Characters(const FirstSetBits& set)
    {
        return !~(set.bits[0] & set.bits[1] & set.bits[2] & set.bits[3]);
    }

    static void addAll(FirstSetBits& set, const FirstSetBits& other)
    {
        for (unsigned i = 0; i < set.bits.size(); ++i)
            set.bits[i] |= other.bits[i];
        set.atEnd |= other.atEnd;
    }

    std::optional<FirstSetBits> latin1Bits(CharacterSetID id, bool& hasWideMembers) const
    {
        if (id == CharacterSetID::Unknown)
            return std::nullopt;
        const CharacterSet& set = m_pattern.m_firstFollowSets.set(id);
        FirstSetBits result;
        result.atEnd = set.includesEndOfInput;
        for (auto [begin, end] : set.ranges) {
            if (begin <= 0xff)
                addLatin1Range(result.bits, begin, std::min<char32_t>(end, 0xff));
            hasWideMembers |= end > 0xff;
        }
        return result;
    }

    // The analysis gives up on a set that takes a walk over many terms, as the first set of a group of a hundred alternatives does. Each of those alternatives has its own.
    std::optional<FirstSetBits> firstBits(const PatternAlternative& alternative, bool& hasWideMembers, unsigned depth = 0) const
    {
        if (auto known = latin1Bits(alternative.m_firstSet, hasWideMembers))
            return known;
        if (depth >= maxNestingDepth || alternative.m_terms.isEmpty() || alternative.matchDirection() != Forward)
            return std::nullopt;
        const PatternTerm& term = alternative.m_terms[0];
        if (term.type != PatternTerm::Type::ParenthesesSubpattern || !term.quantityMinCount || term.matchDirection() != Forward)
            return std::nullopt;
        FirstSetBits result;
        for (auto& nested : term.parentheses.disjunction->m_alternatives) {
            auto first = firstBits(*nested, hasWideMembers, depth + 1);
            if (!first)
                return std::nullopt;
            addAll(result, *first);
        }
        return result;
    }

    static constexpr unsigned endOfInput = 256;

    template<typename Functor>
    static void forEachMember(const FirstSetBits& set, const Functor& functor)
    {
        for (unsigned word = 0; word < set.bits.size(); ++word) {
            for (uint64_t bits = set.bits[word]; bits; bits &= bits - 1)
                functor(word * 64 + std::countr_zero(bits));
        }
        if (set.atEnd)
            functor(endOfInput);
    }

    void fillDispatchTable(std::span<const unsigned> choices, std::span<const std::optional<FirstSetBits>> first)
    {
        using PerClass = std::array<uint16_t, FlatProgram::dispatchClassesPerTable>;
        PerClass classes { };
        PerClass sizes { };
        PerClass memberCounts { };
        PerClass classOfMembers { };
        Vector<uint16_t, 16> touched;
        unsigned classCount = 1;
        sizes[0] = FlatProgram::dispatchClassesPerTable;
        for (auto& set : first) {
            if (!set)
                continue;
            forEachMember(*set, [&](unsigned member) {
                if (!memberCounts[classes[member]]++)
                    touched.append(classes[member]);
            });
            forEachMember(*set, [&](unsigned member) {
                uint16_t whole = classes[member];
                if (memberCounts[whole] == sizes[whole])
                    return;
                if (!classOfMembers[whole])
                    classOfMembers[whole] = classCount++;
                classes[member] = classOfMembers[whole];
            });
            for (uint16_t whole : touched) {
                if (uint16_t part = std::exchange(classOfMembers[whole], 0)) {
                    sizes[part] = memberCounts[whole];
                    sizes[whole] -= memberCounts[whole];
                }
                memberCounts[whole] = 0;
            }
            touched.shrink(0);
        }
        auto& ops = m_program->ops;
        auto& entries = m_program->dispatchEntries;
        unsigned firstClass = m_program->dispatchClasses.size();
        m_program->dispatchClasses.append(std::span<const uint16_t> { classes });
        unsigned firstRow = entries.size();
        entries.grow(firstRow + choices.size() * classCount);

        auto forEachClassThatBegins = [&](const std::optional<FirstSetBits>& set, const auto& functor) {
            if (set) {
                forEachMember(*set, [&](unsigned member) {
                    functor(classes[member]);
                });
                return;
            }
            for (unsigned i = 0; i < classCount; ++i)
                functor(i);
        };

        Vector<uint32_t, 64> next;
        next.fill(FlatProgram::noAlternativeBeginsHere, classCount);
        forEachClassThatBegins(first[choices.size()], [&](unsigned i) {
            next[i] = ops[choices.back()].target + FlatProgram::firstDispatchTarget;
        });
        for (unsigned alternative = choices.size(); alternative--;) {
            auto row = entries.mutableSpan().subspan(firstRow + alternative * classCount, classCount);
            for (unsigned i = 0; i < classCount; ++i)
                row[i] = next[i];
            // Where this alternative alone is left, the earlier ones go past its choice.
            uint32_t here = choices[alternative] + FlatProgram::firstDispatchTarget;
            uint32_t pastChoice = here + 1;
            forEachClassThatBegins(first[alternative], [&](unsigned i) {
                if (next[i] == here || next[i] == pastChoice)
                    return;
                bool isOnlyOne = next[i] == FlatProgram::noAlternativeBeginsHere;
                row[i] = isOnlyOne ? FlatProgram::onlyThisAlternativeBeginsHere : FlatProgram::thisAndLaterAlternativesBeginHere;
                next[i] = isOnlyOne ? pastChoice : here;
            });
            ops[choices[alternative]].argument = firstRow + alternative * classCount;
            ops[choices[alternative]].needed = firstClass;
        }
    }

    unsigned internFirstSet(const FirstSetBits& set)
    {
        auto [entry, isNew] = m_firstSetIndexes.try_emplace(set, m_program->firstSets.size());
        if (isNew)
            m_program->firstSets.append(set);
        return entry->second;
    }

    static constexpr unsigned maxStartTableDepth = FlatProgram::maxStartTableDepth;
    static constexpr unsigned maxTermsVisitedForStartTable = 256;

    struct StartOfMatch {
        std::array<FirstSetBits, maxStartTableDepth> at { };
        std::array<bool, maxStartTableDepth> mayBeWide { };
        unsigned depth { maxStartTableDepth };
        unsigned leastSizeOfMatch { 0 };
        unsigned termsLeftToVisit { maxTermsVisitedForStartTable };
        bool mayMatchOnlyAtInputStart { false };
        std::optional<char32_t> onlyWideStart;
        bool hasSeveralWideStarts { false };

        void addWideStarts(char32_t begin, char32_t end)
        {
            hasSeveralWideStarts |= begin != end || (onlyWideStart && *onlyWideStart != begin);
            onlyWideStart = begin;
        }

        void allowAnythingFrom(unsigned place)
        {
            hasSeveralWideStarts |= !place;
            depth = std::min(depth, std::max(place, leastSizeOfMatch));
            for (; place < depth; ++place) {
                at[place].bits.fill(~0ull);
                mayBeWide[place] = true;
            }
        }
    };

    // What follows an alternative of a group. Without an alternative: anything.
    struct AfterGroup {
        const PatternAlternative* alternative;
        unsigned term;
        const AfterGroup* parent;
    };

    // Assertions only take matches away.
    void collectStartOfMatch(StartOfMatch& start, const PatternAlternative& alternative, unsigned i, unsigned place, const AfterGroup* after) const
    {
        auto& terms = alternative.m_terms;
        for (; place < start.depth; ++i) {
            if (!start.termsLeftToVisit)
                return start.allowAnythingFrom(place);
            --start.termsLeftToVisit;
            if (i == terms.size()) {
                if (!after) {
                    start.depth = place;
                    return;
                }
                if (!after->alternative)
                    return start.allowAnythingFrom(place);
                return collectStartOfMatch(start, *after->alternative, after->term, place, after->parent);
            }
            const PatternTerm& term = terms[i];
            switch (term.type) {
            case PatternTerm::Type::AssertionBOL:
                if (term.multiline())
                    break;
                start.mayMatchOnlyAtInputStart |= !place;
                return;
            case PatternTerm::Type::AssertionEOL:
            case PatternTerm::Type::AssertionWordBoundary:
            case PatternTerm::Type::ParentheticalAssertion:
            case PatternTerm::Type::NumberedForwardReference:
            case PatternTerm::Type::NamedForwardReference:
            case PatternTerm::Type::DotStarEnclosure:
                break;
            case PatternTerm::Type::NumberedBackReference:
            case PatternTerm::Type::NamedBackReference:
                return start.allowAnythingFrom(place);
            case PatternTerm::Type::PatternCharacter:
            case PatternTerm::Type::CharacterClass: {
                bool hasWideMembers = false;
                auto set = isAtom(term) ? latin1Bits(term.m_matchedSet, hasWideMembers) : std::nullopt;
                if (!set)
                    return start.allowAnythingFrom(place);
                for (unsigned count = 0; count < term.quantityMaxCount && place < start.depth; ++count, ++place) {
                    if (term.quantityType != QuantifierType::FixedCount)
                        collectStartOfMatch(start, alternative, i + 1, place, after);
                    addAll(start.at[place], *set);
                    start.mayBeWide[place] |= hasWideMembers;
                    if (!place && hasWideMembers) {
                        for (auto [begin, end] : m_pattern.m_firstFollowSets.set(term.m_matchedSet).ranges) {
                            if (end > 0xff)
                                start.addWideStarts(std::max<char32_t>(begin, 0x100), end);
                        }
                    }
                }
                break;
            }
            case PatternTerm::Type::ParenthesesSubpattern: {
                if (term.matchDirection() != Forward)
                    return start.allowAnythingFrom(place);
                if (!term.quantityMaxCount)
                    break;
                if (!term.quantityMinCount)
                    collectStartOfMatch(start, alternative, i + 1, place, after);
                AfterGroup afterThis { term.quantityMaxCount == 1 ? &alternative : nullptr, i + 1, after };
                for (auto& nested : term.parentheses.disjunction->m_alternatives)
                    collectStartOfMatch(start, *nested, 0, place, &afterThis);
                return;
            }
            }
        }
    }

    static bool dependsOnMoreThanPosition(const PatternTerm& term)
    {
        switch (term.type) {
        case PatternTerm::Type::NumberedBackReference:
        case PatternTerm::Type::NamedBackReference:
        case PatternTerm::Type::DotStarEnclosure:
            return true;
        case PatternTerm::Type::ParenthesesSubpattern:
        case PatternTerm::Type::ParentheticalAssertion:
            return std::ranges::any_of(term.parentheses.disjunction->m_alternatives, [](auto& alternative) {
                return std::ranges::any_of(alternative->m_terms, dependsOnMoreThanPosition);
            });
        default:
            return false;
        }
    }

    static bool isTakenOnce(const PatternTerm& term)
    {
        return term.quantityType == QuantifierType::FixedCount && term.quantityMinCount == 1 && term.quantityMaxCount == 1 && term.parentheses.disjunction->m_alternatives.size() == 1;
    }

    // See findStartBehindFailedRun(). The claim there needs an attempt that got through the steps to fail only after the run has tried all its ends, and the search from an end
    // to depend on the position alone. So the run is the first choice of the only alternative, in no repeated group, with no back reference behind it.
    // A lookahead is not gone back into: with the run in one, the attempt has to match as soon as the lookahead does. Nothing may stand behind it but a reference to a group
    // that is all of it, which reads again what the lookahead has just read: (?=(..))\1, an atomic group by other means.
    // The JIT counts turns in the attempts that are left out here, and gives a match up by their number.
    void findRunBehindFixedSteps()
    {
        auto& alternatives = m_pattern.m_body->m_alternatives;
        bool hasTables = m_program->startScan == FlatStartScan::OneCharacter || m_program->startScan == FlatStartScan::Table;
        if (!hasTables || m_countsTurns || m_program->jitCountsTurns || m_pattern.eitherUnicode() || m_program->endAnchoredFixedSize || m_program->mayMatchOnlyAtInputStart)
            return;
        if (alternatives.size() != 1 || alternatives[0]->onceThrough())
            return;

        FlatProgram::RunBehindFixedSteps found;
        // About the terms behind the groups that are open.
        bool standsInFrontOfNothing = true;
        bool standsInFrontOfReference = false;
        std::span<const PatternTerm> terms = alternatives[0]->m_terms.span();
        for (;;) {
            if (terms.empty())
                return;
            const PatternTerm& term = terms.front();
            terms = terms.subspan(1);
            if (term.matchDirection() != Forward)
                return;
            switch (term.type) {
            case PatternTerm::Type::PatternCharacter:
            case PatternTerm::Type::CharacterClass: {
                unsigned set = setForTerm(term);
                if (hasExceededIndexLimit())
                    return;
                if (term.quantityType != QuantifierType::FixedCount) {
                    if (term.quantityMaxCount != quantifyInfinite)
                        return;
                    if (standsInFrontOfReference || std::ranges::any_of(terms, dependsOnMoreThanPosition))
                        return;
                    found.runSet = set;
                    m_program->runBehindFixedSteps = found;
                    m_program->hasRunBehindFixedSteps = true;
                    m_program->firstStartFoundByTablesAlone = std::numeric_limits<unsigned>::max();
                    return;
                }
                if (term.quantityMaxCount > FlatProgram::RunBehindFixedSteps::maxSteps - found.steps)
                    return;
                for (unsigned i = 0; i < term.quantityMaxCount; ++i)
                    found.stepSets[found.steps++] = set;
                continue;
            }
            case PatternTerm::Type::ParenthesesSubpattern:
                if (!isTakenOnce(term))
                    return;
                standsInFrontOfNothing &= terms.empty();
                standsInFrontOfReference |= std::ranges::any_of(terms, dependsOnMoreThanPosition);
                break;
            case PatternTerm::Type::ParentheticalAssertion: {
                if (term.invert() || term.parentheses.disjunction->m_alternatives.size() != 1)
                    return;
                auto& inside = term.parentheses.disjunction->m_alternatives[0]->m_terms;
                if (!terms.empty()) {
                    const PatternTerm& reference = terms.front();
                    bool readsWholeLookahead = reference.type == PatternTerm::Type::NumberedBackReference && reference.quantityType == QuantifierType::FixedCount && reference.quantityMaxCount == 1
                        && inside.size() == 1 && inside[0].type == PatternTerm::Type::ParenthesesSubpattern && inside[0].capture() && isTakenOnce(inside[0])
                        && inside[0].parentheses.subpatternId == reference.backReferenceSubpatternId;
                    if (!readsWholeLookahead || terms.size() > 1)
                        return;
                }
                if (!standsInFrontOfNothing)
                    return;
                break;
            }
            default:
                return;
            }
            terms = term.parentheses.disjunction->m_alternatives[0]->m_terms.span();
        }
    }

    void computeStartScan(Alternatives alternatives)
    {
        if (m_program->triesOnlyOnce)
            return;
        StartOfMatch start;
        start.leastSizeOfMatch = alternatives[0]->m_minimumSize;
        for (auto& alternative : alternatives)
            start.leastSizeOfMatch = std::min(start.leastSizeOfMatch, alternative->m_minimumSize);
        for (auto& alternative : alternatives)
            collectStartOfMatch(start, *alternative, 0, 0, nullptr);

        // The analysis knows what a lookahead lets through.
        FirstSetBits first;
        bool firstMayBeWide = false;
        bool knowsFirst = start.depth;
        for (auto& alternative : alternatives) {
            auto known = knowsFirst ? firstBits(*alternative, firstMayBeWide) : std::nullopt;
            knowsFirst = known && !known->atEnd;
            if (knowsFirst)
                addAll(first, *known);
        }
        if (knowsFirst) {
            for (unsigned i = 0; i < first.bits.size(); ++i)
                start.at[0].bits[i] &= first.bits[i];
            start.mayBeWide[0] &= firstMayBeWide;
        }

        while (start.depth && start.mayBeWide[start.depth - 1] && hasAllLatin1Characters(start.at[start.depth - 1]))
            --start.depth;
        if (!start.depth)
            return;

        m_program->startScan = FlatStartScan::Table;
        m_program->startTableDepth = start.depth;
        m_program->mayMatchOnlyAtInputStart = start.mayMatchOnlyAtInputStart;
        m_program->firstStartFoundByTablesAlone = start.mayMatchOnlyAtInputStart;
        unsigned firstCount = 0;
        for (unsigned place = 0; place < start.depth; ++place) {
            forEachMember(start.at[place], [&](unsigned member) {
                m_program->startTable[member] |= 1 << place;
                m_program->startTableOfLowBits[member & 15] |= 1 << place;
                m_program->startTableOfHighBits[(member >> 4) & 15] |= 1 << place;
                if (!place) {
                    ++firstCount;
                    m_program->onlyStartCharacter = static_cast<Latin1Character>(member);
                }
            });
            if (start.mayBeWide[place])
                m_program->startTable[256] |= 1 << place;
        }
        for (unsigned i = 0; i < 16; ++i) {
            m_program->startTableOfLowBits[i] |= 0xf << start.depth;
            m_program->startTableOfHighBits[i] |= 0xf << start.depth;
        }
        if (firstCount == 1 && !start.mayBeWide[0] && start.depth == 1)
            m_program->startScan = FlatStartScan::OneCharacter;
        if (firstCount == 1 && !start.mayBeWide[0])
            m_program->onlyStart = m_program->onlyStartCharacter;
        else if (!firstCount && start.mayBeWide[0] && !start.hasSeveralWideStarts)
            m_program->onlyStart = start.onlyWideStart;
    }

    unsigned internSet(const CharacterClass* characterClass, bool invert, char32_t first, char32_t second)
    {
        ASSERT(characterClass || !invert);
        auto [entry, isNew] = m_setIndexes.try_emplace(SetKey { characterClass, invert, first, second }, m_program->characterSets.size());
        if (!isNew)
            return entry->second;

        CharacterSetBits set = characterClass ? buildCharacterSetBits(*characterClass, invert, m_program->wideRanges) : buildCharacterSetBits(first, second, m_program->wideRanges);
        m_program->characterSets.append(set);
        return entry->second;
    }

    unsigned newlineSet()
    {
        return internSet(m_pattern.newlineCharacterClass(), false, 0, 0);
    }

    static bool isCased(const PatternTerm& term)
    {
        return term.ignoreCase() && isASCIIAlpha(term.patternCharacter);
    }

    unsigned setForTerm(const PatternTerm& term)
    {
        if (term.type == PatternTerm::Type::CharacterClass)
            return internSet(term.characterClass, term.invert(), 0, 0);
        char32_t character = term.patternCharacter;
        if (isCased(term))
            return internSet(nullptr, false, toASCIILower(character), toASCIIUpper(character));
        return internSet(nullptr, false, character, character);
    }

    struct CaptureRange {
        unsigned first;
        unsigned last;
    };

    static std::optional<CaptureRange> captureRange(const PatternTerm& term)
    {
        if (!term.containsAnyCaptures())
            return std::nullopt;
        return CaptureRange { term.parentheses.subpatternId, term.parentheses.lastSubpatternId };
    }

    unsigned emitAlternativeChoice(const std::optional<FirstSetBits>& first, const FirstSetBits& later)
    {
        if (!first || hasAllLatin1Characters(*first))
            return emit(Kind::Choice);
        unsigned choice = emit(Kind::ChoiceByFirst);
        m_program->ops[choice].firstSet = static_cast<uint16_t>(internFirstSet(*first));
        m_program->ops[choice].laterFirstSet = static_cast<uint16_t>(internFirstSet(later));
        return choice;
    }

    std::optional<FirstSetBits> firstBits(const Branch& branch, bool& hasWideMembers) const
    {
        if (!branch.begin)
            return firstBits(*branch.alternative, hasWideMembers);
        FirstSetBits result;
        auto& terms = branch.alternative->m_terms;
        for (unsigned i = branch.begin; i < terms.size(); ++i) {
            const PatternTerm& term = terms[i];
            if (!isAtom(term))
                return std::nullopt;
            if (!term.quantityMaxCount)
                continue;
            auto set = latin1Bits(term.m_matchedSet, hasWideMembers);
            if (!set)
                return std::nullopt;
            addAll(result, *set);
            if (term.quantityMinCount)
                return result;
        }
        return std::nullopt;
    }

    static bool intersects(const FirstSetBits& a, const FirstSetBits& b)
    {
        return (a.bits[0] & b.bits[0]) || (a.bits[1] & b.bits[1]) || (a.bits[2] & b.bits[2]) || (a.bits[3] & b.bits[3]) || (a.atEnd && b.atEnd);
    }

    // A step takes its characters in one way only: a fixed number of them, or as many as there are. So (step a | step b) is step (a | b).
    unsigned termsOfStepAt(const Branch& branch)
    {
        auto& terms = branch.alternative->m_terms;
        if (branch.begin >= terms.size() || !isAtom(terms[branch.begin]))
            return 0;
        auto run = runAt(*branch.alternative, branch.begin);
        if (!run)
            return 1;
        return run->kind == Kind::PossessiveRun ? run->terms : 0;
    }

    static bool beginWithSameTerms(const Branch& a, const Branch& b, unsigned count)
    {
        for (unsigned i = 0; i < count; ++i) {
            const PatternTerm& one = a.alternative->m_terms[a.begin + i];
            const PatternTerm& other = b.alternative->m_terms[b.begin + i];
            if (!matchesSameCharacters(one, other) || one.quantityType != other.quantityType || one.quantityMinCount != other.quantityMinCount || one.quantityMaxCount != other.quantityMaxCount)
                return false;
        }
        return true;
    }

    // A branch joins an earlier group over the heads of the groups between them only if none of those can begin where it can: then it does not matter which of them is tried first.
    Vector<BranchGroup, 8> groupBranches(std::span<const Branch> branches, bool sharesSteps)
    {
        Vector<BranchGroup, 8> groups;
        for (const Branch& branch : branches) {
            bool hasWideMembers = false;
            auto first = firstBits(branch, hasWideMembers);
            unsigned stepTerms = sharesSteps ? termsOfStepAt(branch) : 0;
            BranchGroup* home = nullptr;
            bool mayPassOthers = first && !hasWideMembers && !first->atEnd;
            for (unsigned i = stepTerms ? groups.size() : 0; i--;) {
                BranchGroup& group = groups[i];
                if (group.stepTerms == stepTerms && beginWithSameTerms(group.members[0], branch, stepTerms)) {
                    home = &group;
                    break;
                }
                if (!mayPassOthers || !group.first || intersects(*group.first, *first))
                    break;
            }
            if (!home) {
                groups.append(BranchGroup { { branch }, first, stepTerms });
                continue;
            }
            home->members.append(branch);
            if (home->first && first)
                addAll(*home->first, *first);
            else
                home->first = std::nullopt;
        }
        return groups;
    }

    bool emitBranchGroup(const BranchGroup& group, EmitContext context)
    {
        const Branch& leader = group.members[0];
        if (group.members.size() == 1)
            return emitAlternative(*leader.alternative, context, leader.begin);

        unsigned shared = group.stepTerms;
        for (;;) {
            unsigned stepTerms = termsOfStepAt({ leader.alternative, leader.begin + shared });
            if (!stepTerms)
                break;
            bool isShared = true;
            for (unsigned i = 1; isShared && i < group.members.size(); ++i) {
                Branch rest { group.members[i].alternative, group.members[i].begin + shared };
                isShared = termsOfStepAt(rest) == stepTerms && beginWithSameTerms({ leader.alternative, leader.begin + shared }, rest, stepTerms);
            }
            if (!isShared)
                break;
            shared += stepTerms;
        }

        for (unsigned i = leader.begin, end = leader.begin + shared; i < end;) {
            if (auto run = runAt(*leader.alternative, i)) {
                emitRun(*run, 0);
                i += run->terms;
            } else
                i += emitFixedAtoms(*leader.alternative, i, end);
        }
        auto rest = group.members.map<Vector<Branch, 8>>([&](const Branch& member) {
            return Branch { member.alternative, member.begin + shared };
        });
        ++context.sharedPrefixDepth;
        return emitBranches(rest.span(), context);
    }

    bool emitBranches(std::span<const Branch> branches, EmitContext context)
    {
        if (hasExceededIndexLimit())
            return decline(FlatProgramDecline::TooManySetsOrRegisters);
        auto groups = groupBranches(branches, branches.size() > 2 && context.sharedPrefixDepth < maxSharedPrefixDepth);
        unsigned count = groups.size();
        if (count == 1)
            return emitBranchGroup(groups[0], context);

        Vector<std::optional<FirstSetBits>, 8> first(count);
        Vector<FirstSetBits, 8> later(count);
        for (unsigned i = 0; i < count; ++i)
            first[i] = groups[i].first;
        bool usesTable = count > 2;
        FirstSetBits unknown;
        unknown.bits.fill(~0ull);
        unknown.atEnd = true;
        for (unsigned i = count - 1; i--;) {
            later[i] = later[i + 1];
            addAll(later[i], first[i + 1].value_or(unknown));
        }

        unsigned smallestMinimumSize = branches[0].alternative->m_minimumSize;
        for (auto& branch : branches)
            smallestMinimumSize = std::min(smallestMinimumSize, branch.alternative->m_minimumSize);

        auto emitChecked = [&](const BranchGroup& group) {
            const Branch& leader = group.members[0];
            if (group.members.size() == 1 && !leader.begin && context.neededAfter && leader.alternative->m_minimumSize > smallestMinimumSize)
                emitNeedsInput(leader.alternative->m_minimumSize + context.neededAfter);
            return emitBranchGroup(group, context);
        };

        Vector<unsigned, 8> jumpsToEnd;
        Vector<unsigned, 8> choices;
        for (unsigned i = 0; i + 1 < count; ++i) {
            unsigned choice = usesTable ? emit(Kind::ChoiceByTable) : emitAlternativeChoice(first[i], later[i]);
            choices.append(choice);
            if (!emitChecked(groups[i]))
                return false;
            jumpsToEnd.append(emit(Kind::Jump));
            setTarget(choice);
        }
        if (usesTable)
            fillDispatchTable(choices.span(), first.span());
        if (!emitChecked(groups[count - 1]))
            return false;
        for (unsigned jump : jumpsToEnd)
            setTarget(jump);
        return true;
    }

    bool emitAlternativesInOrder(Alternatives alternatives, EmitContext context)
    {
        Vector<unsigned, 8> jumpsToEnd;
        for (auto& alternative : alternatives) {
            bool isLast = &alternative == &alternatives.back();
            if (alternative.get() == m_firstRepeatedAlternative)
                m_program->laterAttemptsBeginAt = here();
            unsigned choice = isLast ? 0 : emit(Kind::Choice);
            if (alternative->m_minimumSize > context.testedFor)
                emitNeedsInput(alternative->m_minimumSize + context.neededAfter);
            if (!emitAlternative(*alternative, context))
                return false;
            if (isLast)
                break;
            jumpsToEnd.append(emit(Kind::Jump));
            setTarget(choice);
        }
        for (unsigned jump : jumpsToEnd)
            setTarget(jump);
        return true;
    }

    bool emitAlternatives(Alternatives alternatives, EmitContext context)
    {
        if (context.nestingDepth >= maxNestingDepth)
            return decline(FlatProgramDecline::NestedTooDeep);
        ++context.nestingDepth;
        if (m_countsTurns || m_readsBackward)
            return emitAlternativesInOrder(alternatives, context);
        if (alternatives.size() == 1)
            return emitAlternative(*alternatives[0], context);
        Vector<Branch, 8> branches;
        branches.reserveInitialCapacity(alternatives.size());
        for (auto& alternative : alternatives)
            branches.append(Branch { alternative.get(), 0 });
        return emitBranches(branches.span(), context);
    }

    bool emitGroupBody(const PatternTerm& term, EmitContext context)
    {
        unsigned slot = term.parentheses.subpatternId << 1;
        auto& alternatives = term.parentheses.disjunction->m_alternatives;
        if (term.capture() && alternatives.size() == 1 && alternatives[0]->matchDirection() == Forward) {
            auto& terms = alternatives[0]->m_terms;
            if (terms.isEmpty()) {
                emit(Kind::LoggedCaptureEmpty, slot);
                return true;
            }
            if (auto run = runAt(*alternatives[0], 0); run && run->terms == terms.size() && run->kind == Kind::PossessiveRun && run->max == quantifyInfinite) {
                emitWithSet(Kind::LoggedCapturedRun, run->set, slot, run->min);
                return true;
            }
        }
        if (term.capture())
            emit(Kind::LoggedCaptureStart, slot + m_readsBackward);
        if (!emitAlternatives(alternatives.span(), context))
            return false;
        if (term.capture())
            emit(Kind::LoggedCaptureEnd, slot + !m_readsBackward);
        return true;
    }

    bool emitOptionalGroup(const PatternTerm& term, EmitContext context)
    {
        bool canBeEmpty = !term.parentheses.disjunction->m_minimumSize;
        unsigned start = canBeEmpty ? m_nextRegister++ : 0;

        auto emitChecked = [&] {
            unsigned begin = here();
            if (canBeEmpty)
                emitWithRegister(Kind::LoggedSavePosition, start);
            if (!emitGroupBody(term, context))
                return false;
            if (canBeEmpty) {
                emitWithRegister(Kind::FailIfEmpty, start);
                isTurnThatMayTakeNothingSince(begin, start);
            }
            return true;
        };

        uint64_t needed = neededToEnter(*term.parentheses.disjunction, context.neededAfter);
        if (term.quantityType == QuantifierType::Greedy) {
            std::optional<unsigned> check;
            if (needed)
                check = emitNeedsInputElseJump(needed);
            unsigned choice = emit(Kind::Choice);
            if (!emitChecked())
                return false;
            setTarget(choice);
            if (check)
                setTarget(*check);
            return true;
        }
        unsigned choice = emit(Kind::Choice);
        unsigned jump = emit(Kind::Jump);
        setTarget(choice);
        if (needed)
            emitNeedsInput(needed);
        if (!emitChecked())
            return false;
        setTarget(jump);
        return true;
    }

    struct Loop {
        unsigned min;
        unsigned max;
        bool isLazy;
        bool canBeEmpty;
        uint64_t neededForAnotherIteration;
        std::optional<CaptureRange> captures;
        bool countsTurns { false };
        bool makesNoChoice { false };
    };

    static bool leavesFrameToResumeAt(const FlatOp& op)
    {
        switch (op.kind) {
        case Kind::Choice:
        case Kind::ChoiceByFirst:
        case Kind::ChoiceByTable:
        case Kind::GreedyLoopChoice:
        case Kind::NegativeLookaheadBegin:
        case Kind::GreedyRun:
        case Kind::LazyRun:
        case Kind::GreedyRunBeforeKnownCharacters:
        case Kind::LazyRunBeforeKnownCharacters:
            return true;
        default:
            return false;
        }
    }

    template<typename Body>
    bool emitLoop(const Loop& loop, const Body& emitBody)
    {
        auto [min, max, isLazy, canBeEmpty, neededForAnotherIteration, captures, countsTurns, makesNoChoice] = loop;
        bool needsCounter = min > 1 || max != quantifyInfinite;
        unsigned counter = needsCounter ? m_nextRegister++ : 0;
        unsigned start = canBeEmpty ? m_nextRegister++ : 0;

        std::optional<unsigned> jumpToBody;
        std::optional<unsigned> jumpToMandatory;
        Vector<unsigned, 3> jumpsToExit;
        // The first turn clears them.
        if (makesNoChoice && captures) {
            for (unsigned group = captures->first; group <= captures->last; ++group)
                emit(Kind::LoggedCaptureEmpty, group << 1);
        }
        unsigned loopBegin = here();
        if (needsCounter)
            emitWithRegister(Kind::LoggedSetRegister, counter, 0);
        else if (min) {
            if (canBeEmpty)
                emitWithRegister(Kind::LoggedSetRegister, start, offsetNoMatch);
            if (countsTurns)
                emit(Kind::CountTurn);
            jumpToBody = emit(Kind::Jump);
        }

        unsigned head = here();
        if (needsCounter && min)
            jumpToMandatory = emitWithRegister(Kind::JumpIfBelow, counter, min);
        if (min == max)
            jumpsToExit.append(emit(Kind::Jump));
        else {
            if (max != quantifyInfinite)
                jumpsToExit.append(emitWithRegister(Kind::JumpIfNotBelow, counter, max));
            if (isLazy) {
                unsigned choice = emit(Kind::Choice);
                jumpsToExit.append(emit(Kind::Jump));
                setTarget(choice);
                if (countsTurns)
                    emit(Kind::CountTurn);
                if (neededForAnotherIteration)
                    emitNeedsInput(neededForAnotherIteration);
                if (canBeEmpty)
                    emitWithRegister(Kind::LoggedSavePosition, start);
            } else {
                if (countsTurns)
                    emit(Kind::CountTurn);
                if (canBeEmpty)
                    jumpsToExit.append(emitWithRegister(Kind::GreedyLoopChoice, start));
                else {
                    if (neededForAnotherIteration)
                        jumpsToExit.append(emitNeedsInputElseJump(neededForAnotherIteration));
                    jumpsToExit.append(emit(Kind::Choice));
                }
            }
        }
        if (jumpToMandatory) {
            if (canBeEmpty || countsTurns) {
                std::optional<unsigned> skip;
                if (min != max)
                    skip = emit(Kind::Jump);
                setTarget(*jumpToMandatory);
                if (countsTurns)
                    emit(Kind::CountTurn);
                if (canBeEmpty)
                    emitWithRegister(Kind::LoggedSetRegister, start, offsetNoMatch);
                if (skip)
                    setTarget(*skip);
            } else
                setTarget(*jumpToMandatory);
        }
        if (jumpToBody)
            setTarget(*jumpToBody);
        unsigned turnBegin = here();
        if (needsCounter)
            emitWithRegister(Kind::LoggedIncrement, counter);
        if (captures)
            emit(Kind::LoggedClearCaptures, captures->first, captures->last);
        if (!emitBody())
            return false;
        // A failure in a loop that leaves no frame to resume at goes back to before the loop. Its registers are set again on the way in, and what its captures held is logged in front of it,
        // so that no turn keeps a frame. Its turns are not steps of a search either: each takes input.
        bool keepsNoFrames = min == max && (makesNoChoice || !captures) && std::ranges::none_of(m_program->ops.span().subspan(loopBegin), leavesFrameToResumeAt);
        if (keepsNoFrames)
            removeLogging(m_program->ops.mutableSpan().subspan(loopBegin));
        if (canBeEmpty)
            emitWithRegister(Kind::LoopBackUnlessEmpty, start, 0, head);
        else
            emit(keepsNoFrames ? Kind::Jump : Kind::LoopBack, 0, head);
        if (canBeEmpty)
            isTurnThatMayTakeNothingSince(turnBegin, start);
        if (needsCounter)
            isLoopThatCountsTurnsSince(loopBegin, counter, max == quantifyInfinite ? min : max);
        for (unsigned jump : jumpsToExit)
            setTarget(jump);
        return true;
    }

    enum class InputConsumption : uint8_t { Unknown, None };

    static InputConsumption computeInputConsumption(const PatternDisjunction& disjunction, unsigned& termsLeftToVisit)
    {
        for (auto& alternative : disjunction.m_alternatives) {
            for (auto& term : alternative->m_terms) {
                if (!termsLeftToVisit)
                    return InputConsumption::Unknown;
                --termsLeftToVisit;
                switch (term.type) {
                case PatternTerm::Type::PatternCharacter:
                case PatternTerm::Type::CharacterClass:
                    if (term.quantityMaxCount)
                        return InputConsumption::Unknown;
                    break;
                case PatternTerm::Type::ParenthesesSubpattern:
                    if (term.quantityMaxCount && computeInputConsumption(*term.parentheses.disjunction, termsLeftToVisit) == InputConsumption::Unknown)
                        return InputConsumption::Unknown;
                    break;
                case PatternTerm::Type::NumberedBackReference:
                case PatternTerm::Type::NamedBackReference:
                case PatternTerm::Type::DotStarEnclosure:
                    return InputConsumption::Unknown;
                case PatternTerm::Type::AssertionBOL:
                case PatternTerm::Type::AssertionEOL:
                case PatternTerm::Type::AssertionWordBoundary:
                case PatternTerm::Type::NumberedForwardReference:
                case PatternTerm::Type::NamedForwardReference:
                case PatternTerm::Type::ParentheticalAssertion:
                    break;
                }
            }
        }
        return InputConsumption::None;
    }

    // The JIT still runs a group that is left out here, and counts the turns of the groups inside it.
    void noteTurnsCountedInside(const PatternDisjunction& leftOut)
    {
        Vector<const PatternDisjunction*, 8> worklist { &leftOut };
        while (!worklist.isEmpty() && !m_program->jitCountsTurns) {
            for (auto& alternative : worklist.takeLast()->m_alternatives) {
                for (auto& term : alternative->m_terms) {
                    if (term.type == PatternTerm::Type::ParenthesesSubpattern && term.quantityMaxCount)
                        m_program->jitCountsTurns |= jitCountsTurnsOf(term, m_knownBacktrackableContent);
                    if (term.type == PatternTerm::Type::ParenthesesSubpattern || term.type == PatternTerm::Type::ParentheticalAssertion)
                        worklist.append(term.parentheses.disjunction);
                }
            }
        }
    }

    bool emitGroup(const PatternTerm& term, EmitContext context)
    {
        unsigned min = term.quantityMinCount;
        unsigned max = term.quantityMaxCount;
        const PatternDisjunction& disjunction = *term.parentheses.disjunction;
        if (!max)
            return true;
        bool jitCountsTurns = jitCountsTurnsOf(term, m_knownBacktrackableContent);
        m_program->jitCountsTurns |= jitCountsTurns;
        bool makesNoChoice = term.quantityType == QuantifierType::FixedCount && !jitCountsTurns;
        if (m_countsTurns) {
            bool isTestedForByParent = max == 1 && term.quantityType == QuantifierType::FixedCount;
            context.initialInputPosition = term.inputPosition - (isTestedForByParent ? disjunction.m_minimumSize : 0);
            if (isTestedForByParent || (!min && neededToEnter(disjunction, context.neededAfter)))
                context.testedFor = disjunction.m_minimumSize;
        }
        // Where turns are counted: only a group none of whose turns is.
        if ((!m_countsTurns || makesNoChoice) && (min != 1 || max != 1)) {
            unsigned termsLeftToVisit = maxTermsVisitedForEmptiness;
            if (computeInputConsumption(disjunction, termsLeftToVisit) == InputConsumption::None) {
                if (min)
                    return emitGroupBody(term, context);
                noteTurnsCountedInside(disjunction);
                return true;
            }
        }
        // In a lookbehind X{1,2} is X? in front of X. It takes another turn of the same group, which begins without what the first has captured.
        if (max == 1 && !term.parentheses.isCopy)
            return min ? emitGroupBody(term, context) : emitOptionalGroup(term, context);
        Loop loop { min, max, term.quantityType == QuantifierType::NonGreedy, !disjunction.m_minimumSize, neededToEnter(disjunction, context.neededAfter), captureRange(term), m_countsTurns && jitCountsTurns, makesNoChoice };
        return emitLoop(loop, [&] {
            return emitGroupBody(term, context);
        });
    }

    bool emitAssertion(const PatternTerm& term)
    {
        switch (term.type) {
        case PatternTerm::Type::AssertionBOL:
            if (term.multiline())
                emitWithSet(Kind::LineStart, newlineSet());
            else
                emit(Kind::InputStart);
            return true;
        case PatternTerm::Type::AssertionEOL:
            if (term.multiline())
                emitWithSet(Kind::LineEnd, newlineSet());
            else
                emit(Kind::InputEnd);
            return true;
        case PatternTerm::Type::AssertionWordBoundary:
            emitWithSet(term.invert() ? Kind::NotWordBoundary : Kind::WordBoundary, internSet(m_pattern.eitherUnicode() && term.ignoreCase() ? m_pattern.wordUnicodeIgnoreCaseCharCharacterClass() : m_pattern.wordcharCharacterClass(), false, 0, 0));
            return true;
        default:
            return false;
        }
    }

    static std::optional<uint32_t> fixedWidth(const PatternAlternative& alternative)
    {
        uint64_t width = 0;
        for (const PatternTerm& term : alternative.m_terms) {
            bool isCharacter = term.type == PatternTerm::Type::PatternCharacter || term.type == PatternTerm::Type::CharacterClass;
            bool isAssertion = term.type == PatternTerm::Type::AssertionBOL || term.type == PatternTerm::Type::AssertionEOL || term.type == PatternTerm::Type::AssertionWordBoundary;
            if (!isAssertion && (!isCharacter || term.quantityType != QuantifierType::FixedCount))
                return std::nullopt;
            width += isCharacter ? static_cast<unsigned>(term.quantityMaxCount) : 0;
        }
        if (width > std::numeric_limits<int32_t>::max())
            return std::nullopt;
        return static_cast<uint32_t>(width);
    }

    // Characters and classes that are each taken a fixed number of times, and assertions between them, match the same whichever way they are read: so a lookbehind that consists of nothing else
    // steps back by as many characters as an alternative takes and reads it forward. Only where a character is a unit.
    bool emitAlternativesOfFixedWidthBehind(Alternatives alternatives)
    {
        Vector<unsigned, 8> jumpsToEnd;
        for (auto& alternative : alternatives) {
            uint32_t width = *fixedWidth(*alternative);
            bool isLast = &alternative == &alternatives.back();
            unsigned choice = isLast ? 0 : emit(Kind::Choice);
            emit(Kind::StepBack, width);
            for (const PatternTerm& term : alternative->m_terms) {
                if (!emitAssertion(term))
                    emitFixedAtom(term);
            }
            if (isLast)
                break;
            jumpsToEnd.append(emit(Kind::Jump));
            setTarget(choice);
        }
        for (unsigned jump : jumpsToEnd)
            setTarget(jump);
        return true;
    }

    bool emitLookahead(const PatternTerm& term, EmitContext context)
    {
        Alternatives alternatives = term.parentheses.disjunction->m_alternatives.span();
        context.initialInputPosition = term.inputPosition;
        SetForScope readsBackward(m_readsBackward, term.matchDirection() == Backward);
        auto emitBody = [&] {
            if (!m_readsBackward)
                return emitAlternatives(alternatives, context);
            bool hasFixedWidth = std::ranges::all_of(alternatives, [](auto& alternative) {
                return !!fixedWidth(*alternative);
            });
            if (hasFixedWidth && !m_decodesSurrogatePairs)
                return emitAlternativesOfFixedWidthBehind(alternatives);
            // As ByteCompiler::emitDisjunction(): one that must fail does not test for what its shortest alternative needs.
            context.testedFor = term.parentheses.disjunction->m_minimumSize;
            if (context.testedFor && !term.invert())
                emitNeedsInput(context.testedFor);
            return emitAlternatives(alternatives, context);
        };
        unsigned frameDepthRegister = m_nextRegister++;
        if (term.invert()) {
            unsigned begin = emit(Kind::NegativeLookaheadBegin, frameDepthRegister);
            if (!emitBody())
                return false;
            emit(Kind::NegativeLookaheadEnd, frameDepthRegister);
            isLookaheadSince(begin);
            setTarget(begin);
            return true;
        }

        unsigned positionRegister = m_nextRegister++;
        unsigned begin = emitWithRegister(Kind::LookaheadBegin, positionRegister, frameDepthRegister);
        if (!emitBody())
            return false;
        emitWithRegister(term.containsAnyCaptures() ? Kind::LookaheadEndKeepingCaptures : Kind::LookaheadEnd, positionRegister, frameDepthRegister);
        isLookaheadSince(begin);
        return true;
    }

    struct Run {
        Kind kind;
        unsigned set;
        unsigned firstSet;
        unsigned min;
        unsigned max;
        unsigned terms;
    };

    static bool isAtom(const PatternTerm& term)
    {
        return (term.type == PatternTerm::Type::PatternCharacter || term.type == PatternTerm::Type::CharacterClass) && term.matchDirection() == Forward;
    }

    static bool matchesSameCharacters(const PatternTerm& a, const PatternTerm& b)
    {
        if (a.type != b.type)
            return false;
        if (a.type == PatternTerm::Type::PatternCharacter)
            return a.patternCharacter == b.patternCharacter && isCased(a) == isCased(b);
        return a.characterClass == b.characterClass && a.invert() == b.invert();
    }

    std::optional<Run> runAt(const PatternAlternative& alternative, unsigned i)
    {
        auto& terms = alternative.m_terms;
        if (!isAtom(terms[i]))
            return std::nullopt;
        unsigned min = 0;
        unsigned counted = i;
        if (terms[i].quantityType == QuantifierType::FixedCount) {
            min = terms[i].quantityMaxCount;
            counted = i + 1;
            if (counted == terms.size() || !isAtom(terms[counted]) || terms[counted].quantityType == QuantifierType::FixedCount || !matchesSameCharacters(terms[i], terms[counted]))
                return std::nullopt;
        }
        const PatternTerm& term = terms[counted];
        unsigned set = setForTerm(term);
        unsigned extra = term.quantityMaxCount;
        unsigned max = extra == quantifyInfinite || extra > quantifyInfinite - min ? quantifyInfinite : min + extra;
        bool isGreedy = term.quantityType == QuantifierType::Greedy;
        Run run { isGreedy ? Kind::GreedyRun : Kind::LazyRun, set, 0, min, max, counted + 1 - i };
        if (m_countsTurns) {
            // Only tests of the next term's that fail are left out: no turn begins before one.
            if (counted + 1 == terms.size())
                return run;
            const PatternTerm& next = terms[counted + 1];
            if (!isAtom(next) || next.quantityType != QuantifierType::FixedCount || !next.quantityMaxCount)
                return run;
            auto hasWideMembers = [](const CharacterSetBits& bits) {
                return bits.wideRangeCount || bits.invertsWideRanges;
            };
            CharacterSetBits following = m_program->characterSets[setForTerm(next)];
            CharacterSetBits members = m_program->characterSets[set];
            bool areComplements = term.type == PatternTerm::Type::CharacterClass && next.type == PatternTerm::Type::CharacterClass && term.characterClass == next.characterClass && term.invert() != next.invert();
            bool areDisjoint = areComplements || !hasWideMembers(members) || !hasWideMembers(following);
            for (unsigned word = 0; word < members.bits.size(); ++word)
                areDisjoint &= !(members.bits[word] & following.bits[word]);
            if (areDisjoint) {
                run.kind = Kind::PossessiveRun;
                return run;
            }
            if (hasWideMembers(following))
                return run;
            run.kind = isGreedy ? Kind::GreedyRunBeforeKnownCharacters : Kind::LazyRunBeforeKnownCharacters;
            run.firstSet = internFirstSet({ following.bits, false });
            return run;
        }
        if ((isGreedy && term.possessive()) || m_pattern.m_firstFollowSets.areDisjoint(term.m_matchedSet, term.m_followSet)) {
            run.kind = Kind::PossessiveRun;
            return run;
        }
        bool hasWideMembers = false;
        auto following = latin1Bits(term.m_followSet, hasWideMembers);
        if (!following || hasWideMembers || hasAllLatin1Characters(*following))
            return run;
        run.kind = isGreedy ? Kind::GreedyRunBeforeKnownCharacters : Kind::LazyRunBeforeKnownCharacters;
        run.firstSet = internFirstSet(*following);
        return run;
    }

    struct ReferenceAfterRun {
        unsigned group;
        unsigned distance;
        // Set if the run lies in the group.
        std::optional<unsigned> distanceToEndOfGroup;
    };

    // A back reference that follows the run after a fixed number of units.
    static std::optional<ReferenceAfterRun> findReferenceAfterRun(const PatternAlternative& alternativeOfRun, unsigned begin, const TermsAfterGroup* termsAfterGroup)
    {
        constexpr unsigned maxDistance = 64;
        unsigned distance = 0;
        Vector<std::pair<unsigned, unsigned>, 4> groupsEnded;
        const PatternAlternative* alternative = &alternativeOfRun;
        for (;;) {
            for (unsigned i = begin; i < alternative->m_terms.size(); ++i) {
                const PatternTerm& term = alternative->m_terms[i];
                switch (term.type) {
                case PatternTerm::Type::AssertionBOL:
                case PatternTerm::Type::AssertionEOL:
                case PatternTerm::Type::AssertionWordBoundary:
                    break;
                case PatternTerm::Type::PatternCharacter:
                case PatternTerm::Type::CharacterClass:
                    if (term.quantityType != QuantifierType::FixedCount || term.quantityMaxCount > maxDistance - distance)
                        return std::nullopt;
                    distance += term.quantityMaxCount;
                    break;
                case PatternTerm::Type::NumberedBackReference: {
                    if (term.quantityType != QuantifierType::FixedCount || term.quantityMaxCount != 1 || term.ignoreCase())
                        return std::nullopt;
                    ReferenceAfterRun reference { term.backReferenceSubpatternId, distance, std::nullopt };
                    for (auto [group, distanceToEnd] : groupsEnded) {
                        if (group == reference.group)
                            reference.distanceToEndOfGroup = distanceToEnd;
                    }
                    return reference;
                }
                default:
                    return std::nullopt;
                }
            }
            if (!termsAfterGroup)
                return std::nullopt;
            if (termsAfterGroup->capturedGroup)
                groupsEnded.append({ *termsAfterGroup->capturedGroup, distance });
            alternative = &termsAfterGroup->alternative;
            begin = termsAfterGroup->begin;
            termsAfterGroup = termsAfterGroup->outer;
        }
    }

    // As findSinglePathOps(): nothing in front of the run makes a choice.
    bool hasEmittedOnlyFixedSteps() const
    {
        return std::ranges::all_of(m_program->ops, [](const FlatOp& op) {
            switch (op.kind) {
            case Kind::Character:
            case Kind::Text:
            case Kind::CasedText:
            case Kind::WideText:
            case Kind::Set:
            case Kind::SetCount:
            case Kind::NeedsInput:
            case Kind::InputStart:
            case Kind::LineStart:
            case Kind::InputEnd:
            case Kind::LineEnd:
            case Kind::WordBoundary:
            case Kind::NotWordBoundary:
            case Kind::LoggedCaptureStart:
            case Kind::LoggedCaptureEnd:
            case Kind::LoggedCaptureEmpty:
                return true;
            default:
                return false;
            }
        });
    }

    // In ([\"']).*?\1 and \[(.+?)\]\(\1 nearly every end of the run fails at the back reference, after a frame, a capture and the steps between. Enclosure::endOfRunBeforeReference() goes
    // from one end that can pass to the next. The Choice behind it comes back for the one after that.
    bool tryEmitRunBeforeReference(const Run& run, const PatternAlternative& alternative, unsigned termsBegin, const EmitContext& context, uint64_t neededAfter)
    {
        if (m_countsTurns || m_decodesSurrogatePairs || run.kind == Kind::PossessiveRun || run.max != quantifyInfinite || !hasEmittedOnlyFixedSteps())
            return false;
        std::optional<ReferenceAfterRun> reference = findReferenceAfterRun(alternative, termsBegin, context.termsAfterGroup);
        if (!reference)
            return false;
        m_program->hasRunBeforeReference = true;
        if (!m_findsEndsOfRuns)
            return false;
        bool isGreedy = run.kind == Kind::GreedyRun || run.kind == Kind::GreedyRunBeforeKnownCharacters;
        bool knowsWhatFollows = run.kind == Kind::GreedyRunBeforeKnownCharacters || run.kind == Kind::LazyRunBeforeKnownCharacters;
        constexpr uint64_t all = std::numeric_limits<uint64_t>::max();
        unsigned firstSet = knowsWhatFollows ? run.firstSet : internFirstSet({ { all, all, all, all }, true });

        unsigned leastEndRegister = isGreedy ? m_nextRegister++ : 0;
        auto emitSearch = [&](FlatStepBackward kind) {
            FlatOp& step = emitStepBackward(kind);
            step.set = static_cast<uint16_t>(run.set);
            step.firstSet = static_cast<uint16_t>(firstSet);
            step.laterFirstSet = static_cast<uint16_t>(reference->distanceToEndOfGroup ? *reference->distanceToEndOfGroup + 1 : 0);
            step.argument = reference->group << 1;
            step.target = reference->distance | leastEndRegister << 16;
            step.needed = isGreedy ? run.min : static_cast<uint32_t>(std::min<uint64_t>(neededAfter, std::numeric_limits<uint32_t>::max()));
        };

        // A lazy run that need not take anything tries its first end as it is.
        std::optional<unsigned> jumpToFirstEnd;
        if (isGreedy) {
            emitSearch(FlatStepBackward::GreedyRunBeforeReference);
            jumpToFirstEnd = emit(Kind::Jump);
        } else if (run.min > 1)
            emitWithSet(Kind::SetCount, run.set, run.min - 1);
        else if (!run.min)
            jumpToFirstEnd = emit(Kind::Jump);
        unsigned searchForNextEnd = here();
        emitSearch(isGreedy ? FlatStepBackward::ShorterGreedyRunBeforeReference : FlatStepBackward::LazyRunBeforeReference);
        if (jumpToFirstEnd)
            setTarget(*jumpToFirstEnd);
        emit(Kind::Choice, 0, searchForNextEnd);
        return true;
    }

    void emitRun(const Run& run, uint64_t neededAfter)
    {
        unsigned op = emitWithSet(run.kind, run.set, run.max, run.min);
        m_program->ops[op].firstSet = static_cast<uint16_t>(run.firstSet);
        if (run.kind != Kind::PossessiveRun)
            setNeeded(op, neededAfter);
        else if (m_countsTurns && neededAfter)
            emitNeedsInput(neededAfter);
    }

    // testedForFromHere: FlatProgramForm::CountsTurns, where surrogate pairs are decoded: for how many units from the term on the bytecode has tested.
    void emitFixedAtom(const PatternTerm& term, uint64_t testedForFromHere = 0)
    {
        unsigned count = term.quantityMaxCount;
        if (!count)
            return;
        if (count == 1 && term.type == PatternTerm::Type::PatternCharacter && !isCased(term) && (!m_decodesSurrogatePairs || isSingleCodeUnit(term.patternCharacter))) {
            emit(Kind::Character, term.patternCharacter);
            return;
        }
        unsigned op = count > 1 ? emitWithSet(Kind::SetCount, setForTerm(term), count) : emitWithSet(Kind::Set, setForTerm(term));
        if (testedForFromHere > count * unitsOfOne(term))
            setNeeded(op, testedForFromHere - count * unitsOfOne(term));
    }

    bool isTextCharacter(const PatternAlternative& alternative, unsigned i, bool wide) const
    {
        auto& terms = alternative.m_terms;
        if (i >= terms.size())
            return false;
        const PatternTerm& term = terms[i];
        if (term.type != PatternTerm::Type::PatternCharacter || term.matchDirection() != Forward || term.quantityType != QuantifierType::FixedCount || term.quantityMaxCount != 1)
            return false;
        if (isLatin1(term.patternCharacter) == wide || (m_decodesSurrogatePairs && U_IS_SURROGATE(term.patternCharacter)))
            return false;
        bool beginsRun = i + 1 < terms.size() && isAtom(terms[i + 1]) && terms[i + 1].quantityType != QuantifierType::FixedCount && matchesSameCharacters(term, terms[i + 1]);
        return !beginsRun;
    }

    unsigned emitFixedAtoms(const PatternAlternative& alternative, unsigned i, unsigned limit = std::numeric_limits<unsigned>::max(), uint64_t testedForFromHere = 0)
    {
        auto& terms = alternative.m_terms;
        const PatternTerm& term = terms[i];
        bool isWide = term.type == PatternTerm::Type::PatternCharacter && !isLatin1(term.patternCharacter);
        unsigned end = i;
        unsigned units = 0;
        bool hasCase = false;
        while (end < limit && isTextCharacter(alternative, end, isWide)) {
            hasCase |= isCased(terms[end]);
            units += U16_LENGTH(terms[end].patternCharacter);
            ++end;
        }
        if (units < 2) {
            emitFixedAtom(term, testedForFromHere);
            return 1;
        }
        if (isWide) {
            unsigned offset = m_program->wideText.size();
            for (unsigned j = i; j < end; ++j) {
                char32_t character = terms[j].patternCharacter;
                if (U_IS_BMP(character))
                    m_program->wideText.append(static_cast<char16_t>(character));
                else
                    m_program->wideText.appendList({ U16_LEAD(character), U16_TRAIL(character) });
            }
            emit(Kind::WideText, offset, units);
        } else {
            unsigned offset = m_program->text.size();
            for (unsigned j = i; j < end; ++j) {
                auto character = static_cast<Latin1Character>(terms[j].patternCharacter);
                m_program->text.append(isCased(terms[j]) ? toASCIILower(character) : character);
            }
            if (hasCase) {
                for (unsigned j = i; j < end; ++j)
                    m_program->text.append(isCased(terms[j]) ? 0x20 : 0);
            }
            emit(hasCase ? Kind::CasedText : Kind::Text, offset, end - i);
        }
        return end - i;
    }

    bool emitBackReference(const PatternTerm& term)
    {
        // The bytecode reads surrogate pairs in front of the position in a way of its own.
        if (m_readsBackward && m_decodesSurrogatePairs)
            return decline(FlatProgramDecline::Lookbehind);
        uint32_t group = term.backReferenceSubpatternId << 1;
        if (!m_referencedGroups.contains(group))
            m_referencedGroups.append(group);
        m_searchDependsOnMoreThanPosition |= m_referencedGroups.size() > maxTrackedReferencedGroups;
        auto emitReference = [&] {
            if (m_readsBackward) {
                FlatOp& op = emitStepBackward(FlatStepBackward::BackReference);
                op.argument = group;
                op.target = m_pattern.eitherUnicode();
                op.needed = term.ignoreCase();
            } else
                emit(term.ignoreCase() ? Kind::BackReferenceIgnoringCase : Kind::BackReference, group, m_pattern.eitherUnicode());
            return true;
        };
        unsigned max = term.quantityMaxCount;
        if (!max)
            return true;
        bool isFixed = term.quantityType == QuantifierType::FixedCount;
        if (isFixed && max == 1)
            return emitReference();
        return emitLoop({ isFixed ? max : 0, max, term.quantityType == QuantifierType::NonGreedy, true, 0, std::nullopt }, emitReference);
    }

    bool emitAtomReadBackward(const PatternTerm& term)
    {
        constexpr unsigned maxCopies = 8;
        unsigned max = term.quantityMaxCount;
        bool isFixed = term.quantityType == QuantifierType::FixedCount;
        auto emitOne = [&] {
            unsigned set = setForTerm(term);
            emitStepBackward(FlatStepBackward::Set).set = static_cast<uint16_t>(set);
            return true;
        };
        if (!isFixed || max > maxCopies)
            return emitLoop({ isFixed ? max : 0, max, term.quantityType == QuantifierType::NonGreedy, false, 0, std::nullopt }, emitOne);
        for (unsigned i = 0; i < max; ++i)
            emitOne();
        return true;
    }

    // The last term first, as ByteCompiler::emitDisjunction().
    bool emitAlternativeReadBackward(const PatternAlternative& alternative, EmitContext context)
    {
        auto& terms = alternative.m_terms;
        // FlatProgramForm::CountsTurns: the bytecode tests for no group but one that is taken exactly once.
        auto testedFor = [&](const PatternTerm& term) {
            bool isLeftOut = m_countsTurns && term.type == PatternTerm::Type::ParenthesesSubpattern && (term.quantityMaxCount != 1 || term.quantityType != QuantifierType::FixedCount);
            return isLeftOut ? 0 : minimumSize(term);
        };
        uint64_t neededAfter = context.neededAfter;
        for (auto& term : terms)
            neededAfter += testedFor(term);
        for (const PatternTerm& term : terms.span() | std::views::reverse) {
            if (hasExceededIndexLimit())
                return decline(FlatProgramDecline::TooManySetsOrRegisters);
            neededAfter -= testedFor(term);
            switch (term.type) {
            case PatternTerm::Type::AssertionBOL:
            case PatternTerm::Type::AssertionEOL:
            case PatternTerm::Type::AssertionWordBoundary:
                emitAssertion(term);
                break;
            case PatternTerm::Type::PatternCharacter:
            case PatternTerm::Type::CharacterClass:
                if (!emitAtomReadBackward(term))
                    return false;
                break;
            case PatternTerm::Type::ParenthesesSubpattern:
                if (!emitGroup(term, { neededAfter, context.nestingDepth, context.sharedPrefixDepth, 0, 0 }))
                    return false;
                break;
            case PatternTerm::Type::ParentheticalAssertion:
                if (!emitLookahead(term, { 0, context.nestingDepth, context.sharedPrefixDepth, 0, 0 }))
                    return false;
                break;
            case PatternTerm::Type::NumberedBackReference:
            case PatternTerm::Type::NamedBackReference:
                if (!emitBackReference(term))
                    return false;
                break;
            case PatternTerm::Type::NumberedForwardReference:
            case PatternTerm::Type::NamedForwardReference:
                break;
            case PatternTerm::Type::DotStarEnclosure:
                return decline(FlatProgramDecline::Lookbehind);
            }
        }
        return true;
    }

    bool emitAlternative(const PatternAlternative& alternative, EmitContext context, unsigned begin = 0)
    {
        if ((alternative.matchDirection() == Backward) != m_readsBackward)
            return decline(FlatProgramDecline::Lookbehind);
        if (m_readsBackward)
            return emitAlternativeReadBackward(alternative, context);
        auto& terms = alternative.m_terms;
        uint64_t neededAfter = context.neededAfter;
        for (unsigned i = begin; i < terms.size(); ++i)
            neededAfter += minimumSize(terms[i]);
        unsigned accounted = begin;
        auto accountThrough = [&](unsigned last) {
            while (accounted <= last)
                neededAfter -= minimumSize(terms[accounted++]);
            if (m_countsTurns)
                neededAfter = context.neededAfter + context.initialInputPosition + alternative.m_minimumSize - terms[last].inputPosition;
        };
        for (unsigned i = begin; i < terms.size(); ++i) {
            if (hasExceededIndexLimit())
                return decline(FlatProgramDecline::TooManySetsOrRegisters);
            const PatternTerm& term = terms[i];
            accountThrough(i);
            switch (term.type) {
            case PatternTerm::Type::AssertionBOL:
            case PatternTerm::Type::AssertionEOL:
            case PatternTerm::Type::AssertionWordBoundary:
                emitAssertion(term);
                break;
            case PatternTerm::Type::PatternCharacter:
            case PatternTerm::Type::CharacterClass:
                if (term.matchDirection() != Forward)
                    return decline(FlatProgramDecline::Lookbehind);
                if (auto run = runAt(alternative, i)) {
                    accountThrough(i + run->terms - 1);
                    if (!tryEmitRunBeforeReference(*run, alternative, i + run->terms, context, neededAfter))
                        emitRun(*run, neededAfter);
                    i += run->terms - 1;
                    break;
                }
                i += emitFixedAtoms(alternative, i, std::numeric_limits<unsigned>::max(), m_countsTurns && m_decodesSurrogatePairs ? neededAfter : 0) - 1;
                break;
            case PatternTerm::Type::ParenthesesSubpattern: {
                TermsAfterGroup termsAfterGroup { alternative, i + 1, term.capture() ? std::optional<unsigned>(term.parentheses.subpatternId) : std::nullopt, context.termsAfterGroup };
                bool isTakenOnce = term.quantityType == QuantifierType::FixedCount && term.quantityMaxCount == 1 && !term.parentheses.isCopy;
                if (!emitGroup(term, { neededAfter, context.nestingDepth, context.sharedPrefixDepth, 0, 0, isTakenOnce ? &termsAfterGroup : nullptr }))
                    return false;
                break;
            }
            case PatternTerm::Type::ParentheticalAssertion:
                if (!emitLookahead(term, { 0, context.nestingDepth, context.sharedPrefixDepth, 0, 0 }))
                    return false;
                break;
            case PatternTerm::Type::NumberedBackReference:
            case PatternTerm::Type::NamedBackReference:
                if (!emitBackReference(term))
                    return false;
                if (m_countsTurns && neededAfter)
                    emitNeedsInput(neededAfter);
                break;
            case PatternTerm::Type::NumberedForwardReference:
            case PatternTerm::Type::NamedForwardReference:
                break;
            case PatternTerm::Type::DotStarEnclosure:
                // Under /m whether a leading ^ holds depends on the line that the attempt began in.
                m_searchDependsOnMoreThanPosition |= term.anchors.bolAnchor && term.multiline();
                emitWithSet(Kind::DotStarEnclosure, newlineSet(), (term.anchors.bolAnchor ? FlatProgram::enclosureBeginsLine : 0) | (term.anchors.eolAnchor ? FlatProgram::enclosureEndsLine : 0)
                    | (term.multiline() ? FlatProgram::enclosureIsMultiline : 0) | (term.dotAll() ? FlatProgram::enclosureDotMatchesAll : 0));
                break;
            }
        }
        return true;
    }

    const YarrPattern& m_pattern;
    FlatProgram* m_program { nullptr };
    std::optional<FlatProgramDecline> m_decline;
    StdUnorderedMap<SetKey, unsigned, InternedKeyHash> m_setIndexes;
    StdUnorderedMap<FirstSetBits, unsigned, InternedKeyHash> m_firstSetIndexes;
    Vector<std::pair<unsigned, unsigned>, 8> m_lookaheads;
    Vector<std::tuple<unsigned, unsigned, unsigned>, 8> m_turnsThatMayTakeNothing;
    Vector<std::tuple<unsigned, unsigned, unsigned, unsigned>, 8> m_loopsThatCountTurns;
    bool m_searchDependsOnMoreThanPosition { false };
    Vector<uint32_t, 8> m_referencedGroups;
    static constexpr unsigned maxTrackedReferencedGroups = 32;
    bool m_decodesSurrogatePairs { false };
    bool m_readsBackward { false };
    unsigned m_nextRegister { 0 };
    const PatternAlternative* m_firstRepeatedAlternative { nullptr };
    KnownBacktrackableContent m_knownBacktrackableContent;
    const bool m_countsTurns;
    const bool m_findsEndsOfRuns;
};

size_t FlatProgram::estimatedSizeInBytes() const
{
    return ops.capacity() * sizeof(FlatOp) + characterSets.capacity() * sizeof(CharacterSetBits) + firstSets.capacity() * sizeof(FirstSetBits)
        + dispatchClasses.capacity() * sizeof(uint16_t) + dispatchEntries.capacity() * sizeof(uint32_t)
        + wideRanges.capacity() * sizeof(CharacterRange) + text.capacity() + wideText.capacity() * sizeof(char16_t)
        + turnsThatMayTakeNothing.capacity() * sizeof(TurnThatMayTakeNothing) + loopsThatCountTurns.capacity() * sizeof(LoopThatCountsTurns) + aroundMemoRow.capacity() * sizeof(AroundMemoRow)
        + referencedGroups.capacity() * sizeof(uint32_t)
        + (programThatFindsEndsOfRuns ? sizeof(FlatProgram) + programThatFindsEndsOfRuns->estimatedSizeInBytes() : 0);
}

// As YarrGenerator::disjunctionContainsBacktrackableContent().
static bool containsBacktrackableContent(const PatternDisjunction& disjunction, KnownBacktrackableContent& known)
{
    if (auto found = known.find(&disjunction); found != known.end())
        return found->value;
    bool result = [&] {
        if (disjunction.m_alternatives.size() > 1)
            return true;
        for (auto& alternative : disjunction.m_alternatives) {
            for (auto& term : alternative->m_terms) {
                if (term.quantityType != QuantifierType::FixedCount)
                    return true;
                if (term.type == PatternTerm::Type::NumberedBackReference || term.type == PatternTerm::Type::NamedBackReference)
                    return true;
                if ((term.type == PatternTerm::Type::ParenthesesSubpattern || term.type == PatternTerm::Type::ParentheticalAssertion) && containsBacktrackableContent(*term.parentheses.disjunction, known))
                    return true;
            }
        }
        return false;
    }();
    known.add(&disjunction, result);
    return result;
}

bool jitCountsTurnsOf(const PatternTerm& term, KnownBacktrackableContent& known)
{
    ASSERT(term.type == PatternTerm::Type::ParenthesesSubpattern);
    if ((term.quantityMaxCount == 1 && !term.parentheses.isCopy) || term.parentheses.isTerminal)
        return false;
    if (term.quantityType != QuantifierType::FixedCount)
        return true;
    return term.parentheses.disjunction->m_alternatives.size() != 1 || (term.quantityMaxCount > 1 && containsBacktrackableContent(*term.parentheses.disjunction, known));
}

std::expected<std::unique_ptr<FlatProgram>, FlatProgramDecline> compileFlatProgram(const YarrPattern& pattern, FlatProgramForm form)
{
    ASSERT(pattern.m_executionMode == ExecutionMode::IncludeSubpatterns);
    auto program = FlatProgramBuilder(pattern, form).build();
    if (!program || form != FlatProgramForm::Fast || !(*program)->hasRunBeforeReference || (*program)->jitCountsTurns)
        return program;
    if (auto second = FlatProgramBuilder(pattern, FlatProgramForm::FindsEndsOfRuns).build()) {
        // Starting over costs no table here, so it pays early.
        constexpr uint8_t stepsPerCharacter = 1;
        FlatProgram& first = **program;
        first.programThatFindsEndsOfRuns = WTF::move(*second);
        first.stepsPerCharacterBeforeStoppingOnFullMachine = stepsPerCharacter;
        if (!first.needsFullMachine)
            first.stepsPerCharacterBeforeStopping = stepsPerCharacter;
    }
    return program;
}

bool hasFlatProgramsThatRememberFailedSearches(StringView patternString, OptionSet<Flags> flags)
{
    ErrorCode errorCode { ErrorCode::NoError };
    YarrPattern analyzed(patternString, flags, errorCode, ExecutionMode::IncludeSubpatterns, PatternAnalysis::Performed);
    if (hasError(errorCode))
        return false;
    auto fast = compileFlatProgram(analyzed);
    if (!fast || !(*fast)->jitCountsTurns)
        return false;
    YarrPattern asWritten(patternString, flags, errorCode, ExecutionMode::IncludeSubpatterns, PatternAnalysis::Skipped);
    if (hasError(errorCode))
        return false;
    auto counting = compileFlatProgram(asWritten, FlatProgramForm::CountsTurns);
    return counting && (*counting)->memoRows;
}

ASCIILiteral description(FlatProgramDecline reason)
{
    switch (reason) {
    case FlatProgramDecline::ClassWithStrings:
        return "class with strings"_s;
    case FlatProgramDecline::Lookbehind:
        return "lookbehind"_s;
    case FlatProgramDecline::DuplicateNamedGroups:
        return "duplicate named groups"_s;
    case FlatProgramDecline::UnsignedLengthPattern:
        return "unsigned length pattern"_s;
    case FlatProgramDecline::NestedTooDeep:
        return "nested too deep"_s;
    case FlatProgramDecline::TooManySetsOrRegisters:
        return "too many sets or registers"_s;
    case FlatProgramDecline::NoTurnsToCount:
        return "no turns to count"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::Yarr
