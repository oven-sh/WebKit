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
#include <span>
#include <tuple>
#include <wtf/ASCIICType.h>
#include <wtf/Hasher.h>
#include <wtf/StdUnorderedMap.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace Yarr {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FlatProgram);

class FlatProgramBuilder {
public:
    explicit FlatProgramBuilder(const YarrPattern& pattern)
        : m_pattern(pattern)
    {
    }

    std::expected<std::unique_ptr<FlatProgram>, FlatProgramDecline> build()
    {
        if (m_pattern.eitherUnicode())
            return makeUnexpected(FlatProgramDecline::UnicodeMode);
        if (m_pattern.m_containsModifiers)
            return makeUnexpected(FlatProgramDecline::Modifiers);
        if (m_pattern.m_containsLookbehinds)
            return makeUnexpected(FlatProgramDecline::Lookbehind);
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

        auto& alternatives = m_pattern.m_body->m_alternatives;
        unsigned count = alternatives.size();
        if (alternatives[0]->onceThrough()) {
            count = 0;
            while (count < alternatives.size() && alternatives[count]->onceThrough())
                ++count;
            program->triesOnlyOnce |= count == alternatives.size();
        }

        if (!emitAlternatives(alternatives.span().first(count), { }))
            return makeUnexpected(*m_decline);
        emit(Kind::Match);
        if (hasExceededIndexLimit())
            return makeUnexpected(FlatProgramDecline::TooManySetsOrRegisters);
        program->numRegisters = m_nextRegister;

        removeUnneededLogging(*program);
        computeStartScan(alternatives.span().first(count));

        if (!program->text.isEmpty()) {
            for (unsigned i = 0; i < sizeof(uint64_t); ++i)
                program->text.append(0);
        }
        program->ops.shrinkToFit();
        program->characterSets.shrinkToFit();
        program->firstSets.shrinkToFit();
        program->wideRanges.shrinkToFit();
        program->text.shrinkToFit();
        program->wideText.shrinkToFit();
        return program;
    }

private:
    using Kind = FlatOp::Kind;
    using Alternatives = std::span<const std::unique_ptr<PatternAlternative>>;
    using SetKey = std::tuple<const CharacterClass*, bool, char16_t, char16_t>;

    static constexpr unsigned maxNestingDepth = 64;
    static constexpr unsigned maxTermsVisitedForEmptiness = 64;
    static constexpr size_t maxIndexCount = std::numeric_limits<uint16_t>::max();

    struct EmitContext {
        uint64_t neededAfter { 0 };
        unsigned nestingDepth { 0 };
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
    void setTarget(unsigned op) { m_program->ops[op].target = here(); }

    static uint64_t minimumSize(const PatternTerm& term)
    {
        switch (term.type) {
        case PatternTerm::Type::PatternCharacter:
        case PatternTerm::Type::CharacterClass:
            return static_cast<unsigned>(term.quantityMinCount);
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

    unsigned emitNeedsInput(uint64_t needed) { return emitWithNeeded(Kind::NeedsInput, needed); }
    unsigned emitNeedsInputElseJump(uint64_t needed) { return emitWithNeeded(Kind::NeedsInputElseJump, needed); }

    static uint64_t neededToEnter(const PatternDisjunction& disjunction, uint64_t neededAfter)
    {
        return disjunction.m_minimumSize && neededAfter ? disjunction.m_minimumSize + neededAfter : 0;
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

    unsigned internFirstSet(const FirstSetBits& set)
    {
        auto [entry, isNew] = m_firstSetIndexes.try_emplace(set, m_program->firstSets.size());
        if (isNew)
            m_program->firstSets.append(set);
        return entry->second;
    }

    void computeStartScan(Alternatives alternatives)
    {
        if (m_program->triesOnlyOnce)
            return;
        FirstSetBits start;
        bool hasWideMembers = false;
        for (auto& alternative : alternatives) {
            auto first = latin1Bits(alternative->m_firstSet, hasWideMembers);
            if (!first)
                return;
            addAll(start, *first);
        }
        if (start.atEnd)
            return;

        m_program->startScan = FlatStartScan::Set;
        m_program->startSet = start;
        m_program->startMayBeWide = hasWideMembers;
        unsigned memberCount = 0;
        unsigned lastMember = 0;
        for (unsigned i = 0; i < start.bits.size(); ++i) {
            memberCount += std::popcount(start.bits[i]);
            if (start.bits[i])
                lastMember = i * 64 + std::countr_zero(start.bits[i]);
        }
        if (memberCount == 1 && !hasWideMembers) {
            m_program->startScan = FlatStartScan::OneCharacter;
            m_program->onlyStartCharacter = static_cast<Latin1Character>(lastMember);
        }
    }

    unsigned internSet(const CharacterClass* characterClass, bool invert, char16_t first, char16_t second)
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
        char16_t character = term.patternCharacter;
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

    bool emitAlternatives(Alternatives alternatives, EmitContext context)
    {
        if (context.nestingDepth >= maxNestingDepth)
            return decline(FlatProgramDecline::NestedTooDeep);
        ++context.nestingDepth;
        unsigned count = alternatives.size();
        if (count == 1)
            return emitAlternative(*alternatives[0], context);

        Vector<std::optional<FirstSetBits>, 8> first(count);
        Vector<FirstSetBits, 8> later(count);
        for (unsigned i = 0; i < count; ++i) {
            bool hasWideMembers = false;
            first[i] = latin1Bits(alternatives[i]->m_firstSet, hasWideMembers);
        }
        FirstSetBits unknown;
        unknown.bits.fill(~0ull);
        unknown.atEnd = true;
        for (unsigned i = count - 1; i--;) {
            later[i] = later[i + 1];
            addAll(later[i], first[i + 1].value_or(unknown));
        }

        unsigned smallestMinimumSize = alternatives[0]->m_minimumSize;
        for (auto& alternative : alternatives)
            smallestMinimumSize = std::min(smallestMinimumSize, alternative->m_minimumSize);

        auto emitChecked = [&](const PatternAlternative& alternative) {
            if (context.neededAfter && alternative.m_minimumSize > smallestMinimumSize)
                emitNeedsInput(alternative.m_minimumSize + context.neededAfter);
            return emitAlternative(alternative, context);
        };

        Vector<unsigned, 8> jumpsToEnd;
        for (unsigned i = 0; i + 1 < count; ++i) {
            unsigned choice = emitAlternativeChoice(first[i], later[i]);
            if (!emitChecked(*alternatives[i]))
                return false;
            jumpsToEnd.append(emit(Kind::Jump));
            setTarget(choice);
        }
        if (!emitChecked(*alternatives[count - 1]))
            return false;
        for (unsigned jump : jumpsToEnd)
            setTarget(jump);
        return true;
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
            emit(Kind::LoggedCaptureStart, slot);
        if (!emitAlternatives(alternatives.span(), context))
            return false;
        if (term.capture())
            emit(Kind::LoggedCaptureEnd, slot + 1);
        return true;
    }

    bool emitOptionalGroup(const PatternTerm& term, EmitContext context)
    {
        bool canBeEmpty = !term.parentheses.disjunction->m_minimumSize;
        unsigned start = canBeEmpty ? m_nextRegister++ : 0;

        auto emitChecked = [&] {
            if (canBeEmpty)
                emitWithRegister(Kind::LoggedSavePosition, start);
            if (!emitGroupBody(term, context))
                return false;
            if (canBeEmpty)
                emitWithRegister(Kind::FailIfEmpty, start);
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
    };

    template<typename Body>
    bool emitLoop(const Loop& loop, const Body& emitBody)
    {
        auto [min, max, isLazy, canBeEmpty, neededForAnotherIteration, captures] = loop;
        bool needsCounter = min > 1 || max != quantifyInfinite;
        unsigned counter = needsCounter ? m_nextRegister++ : 0;
        unsigned start = canBeEmpty ? m_nextRegister++ : 0;

        std::optional<unsigned> jumpToBody;
        std::optional<unsigned> jumpToMandatory;
        Vector<unsigned, 3> jumpsToExit;
        if (needsCounter)
            emitWithRegister(Kind::LoggedSetRegister, counter, 0);
        else if (min) {
            if (canBeEmpty)
                emitWithRegister(Kind::LoggedSetRegister, start, offsetNoMatch);
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
                if (neededForAnotherIteration)
                    emitNeedsInput(neededForAnotherIteration);
                if (canBeEmpty)
                    emitWithRegister(Kind::LoggedSavePosition, start);
            } else if (canBeEmpty)
                jumpsToExit.append(emitWithRegister(Kind::GreedyLoopChoice, start));
            else {
                if (neededForAnotherIteration)
                    jumpsToExit.append(emitNeedsInputElseJump(neededForAnotherIteration));
                jumpsToExit.append(emit(Kind::Choice));
            }
        }
        if (jumpToMandatory) {
            if (canBeEmpty) {
                std::optional<unsigned> skip;
                if (min != max)
                    skip = emit(Kind::Jump);
                setTarget(*jumpToMandatory);
                emitWithRegister(Kind::LoggedSetRegister, start, offsetNoMatch);
                if (skip)
                    setTarget(*skip);
            } else
                setTarget(*jumpToMandatory);
        }
        if (jumpToBody)
            setTarget(*jumpToBody);
        if (needsCounter)
            emitWithRegister(Kind::LoggedIncrement, counter);
        if (captures)
            emit(Kind::LoggedClearCaptures, captures->first, captures->last);
        if (!emitBody())
            return false;
        if (canBeEmpty)
            emitWithRegister(Kind::LoopBackUnlessEmpty, start, 0, head);
        else
            emit(Kind::LoopBack, 0, head);
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

    bool emitGroup(const PatternTerm& term, EmitContext context)
    {
        if (term.parentheses.isCopy)
            return decline(FlatProgramDecline::CopiedGroup);
        if (term.matchDirection() != Forward)
            return decline(FlatProgramDecline::Lookbehind);
        unsigned min = term.quantityMinCount;
        unsigned max = term.quantityMaxCount;
        const PatternDisjunction& disjunction = *term.parentheses.disjunction;
        if (!max)
            return true;
        if (min != 1 || max != 1) {
            unsigned termsLeftToVisit = maxTermsVisitedForEmptiness;
            if (computeInputConsumption(disjunction, termsLeftToVisit) == InputConsumption::None)
                return !min || emitGroupBody(term, context);
        }
        if (max == 1)
            return min ? emitGroupBody(term, context) : emitOptionalGroup(term, context);
        Loop loop { min, max, term.quantityType == QuantifierType::NonGreedy, !disjunction.m_minimumSize, neededToEnter(disjunction, context.neededAfter), captureRange(term) };
        return emitLoop(loop, [&] {
            return emitGroupBody(term, context);
        });
    }

    bool emitLookahead(const PatternTerm& term, EmitContext context)
    {
        if (term.matchDirection() != Forward)
            return decline(FlatProgramDecline::Lookbehind);
        Alternatives alternatives = term.parentheses.disjunction->m_alternatives.span();
        unsigned frameDepthRegister = m_nextRegister++;
        if (term.invert()) {
            unsigned begin = emit(Kind::NegativeLookaheadBegin, frameDepthRegister);
            if (!emitAlternatives(alternatives, context))
                return false;
            emit(Kind::NegativeLookaheadEnd, frameDepthRegister);
            setTarget(begin);
            return true;
        }

        unsigned positionRegister = m_nextRegister++;
        emitWithRegister(Kind::LookaheadBegin, positionRegister, frameDepthRegister);
        if (!emitAlternatives(alternatives, context))
            return false;
        emitWithRegister(term.containsAnyCaptures() ? Kind::LookaheadEndKeepingCaptures : Kind::LookaheadEnd, positionRegister, frameDepthRegister);
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

    void emitRun(const Run& run, uint64_t neededAfter)
    {
        unsigned op = emitWithSet(run.kind, run.set, run.max, run.min);
        m_program->ops[op].firstSet = static_cast<uint16_t>(run.firstSet);
        if (run.kind != Kind::PossessiveRun)
            setNeeded(op, neededAfter);
    }

    void emitFixedAtom(const PatternTerm& term)
    {
        unsigned count = term.quantityMaxCount;
        if (!count)
            return;
        if (count > 1)
            emitWithSet(Kind::SetCount, setForTerm(term), count);
        else if (term.type == PatternTerm::Type::PatternCharacter && !isCased(term))
            emit(Kind::Character, term.patternCharacter);
        else
            emitWithSet(Kind::Set, setForTerm(term));
    }

    static bool isTextCharacter(const PatternAlternative& alternative, unsigned i, bool wide)
    {
        auto& terms = alternative.m_terms;
        if (i >= terms.size())
            return false;
        const PatternTerm& term = terms[i];
        if (term.type != PatternTerm::Type::PatternCharacter || term.matchDirection() != Forward || term.quantityType != QuantifierType::FixedCount || term.quantityMaxCount != 1)
            return false;
        if (isLatin1(term.patternCharacter) == wide)
            return false;
        bool beginsRun = i + 1 < terms.size() && isAtom(terms[i + 1]) && terms[i + 1].quantityType != QuantifierType::FixedCount && matchesSameCharacters(term, terms[i + 1]);
        return !beginsRun;
    }

    unsigned emitFixedAtoms(const PatternAlternative& alternative, unsigned i)
    {
        auto& terms = alternative.m_terms;
        const PatternTerm& term = terms[i];
        bool isWide = term.type == PatternTerm::Type::PatternCharacter && !isLatin1(term.patternCharacter);
        unsigned end = i;
        bool hasCase = false;
        while (isTextCharacter(alternative, end, isWide)) {
            hasCase |= isCased(terms[end]);
            ++end;
        }
        if (end - i < 2) {
            emitFixedAtom(term);
            return 1;
        }
        if (isWide) {
            unsigned offset = m_program->wideText.size();
            for (unsigned j = i; j < end; ++j)
                m_program->wideText.append(static_cast<char16_t>(terms[j].patternCharacter));
            emit(Kind::WideText, offset, end - i);
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
        if (term.matchDirection() != Forward)
            return decline(FlatProgramDecline::Lookbehind);
        auto emitReference = [&] {
            emit(term.ignoreCase() ? Kind::BackReferenceIgnoringCase : Kind::BackReference, term.backReferenceSubpatternId << 1);
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

    bool emitAlternative(const PatternAlternative& alternative, EmitContext context)
    {
        if (alternative.matchDirection() != Forward)
            return decline(FlatProgramDecline::Lookbehind);
        auto& terms = alternative.m_terms;
        uint64_t neededAfter = context.neededAfter;
        for (auto& term : terms)
            neededAfter += minimumSize(term);
        unsigned accounted = 0;
        auto accountThrough = [&](unsigned last) {
            while (accounted <= last)
                neededAfter -= minimumSize(terms[accounted++]);
        };
        for (unsigned i = 0; i < terms.size(); ++i) {
            if (hasExceededIndexLimit())
                return decline(FlatProgramDecline::TooManySetsOrRegisters);
            const PatternTerm& term = terms[i];
            accountThrough(i);
            switch (term.type) {
            case PatternTerm::Type::AssertionBOL:
                if (term.multiline())
                    emitWithSet(Kind::LineStart, newlineSet());
                else
                    emit(Kind::InputStart);
                break;
            case PatternTerm::Type::AssertionEOL:
                if (term.multiline())
                    emitWithSet(Kind::LineEnd, newlineSet());
                else
                    emit(Kind::InputEnd);
                break;
            case PatternTerm::Type::AssertionWordBoundary:
                emitWithSet(term.invert() ? Kind::NotWordBoundary : Kind::WordBoundary, internSet(m_pattern.wordcharCharacterClass(), false, 0, 0));
                break;
            case PatternTerm::Type::PatternCharacter:
            case PatternTerm::Type::CharacterClass:
                if (term.matchDirection() != Forward)
                    return decline(FlatProgramDecline::Lookbehind);
                if (auto run = runAt(alternative, i)) {
                    accountThrough(i + run->terms - 1);
                    emitRun(*run, neededAfter);
                    i += run->terms - 1;
                    break;
                }
                i += emitFixedAtoms(alternative, i) - 1;
                break;
            case PatternTerm::Type::ParenthesesSubpattern:
                if (!emitGroup(term, { neededAfter, context.nestingDepth }))
                    return false;
                break;
            case PatternTerm::Type::ParentheticalAssertion:
                if (!emitLookahead(term, { 0, context.nestingDepth }))
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
                return decline(FlatProgramDecline::DotStarEnclosure);
            }
        }
        return true;
    }

    const YarrPattern& m_pattern;
    FlatProgram* m_program { nullptr };
    std::optional<FlatProgramDecline> m_decline;
    StdUnorderedMap<SetKey, unsigned, InternedKeyHash> m_setIndexes;
    StdUnorderedMap<FirstSetBits, unsigned, InternedKeyHash> m_firstSetIndexes;
    unsigned m_nextRegister { 0 };
};

size_t FlatProgram::estimatedSizeInBytes() const
{
    return ops.capacity() * sizeof(FlatOp) + characterSets.capacity() * sizeof(CharacterSetBits) + firstSets.capacity() * sizeof(FirstSetBits)
        + wideRanges.capacity() * sizeof(CharacterRange) + text.capacity() + wideText.capacity() * sizeof(char16_t);
}

std::expected<std::unique_ptr<FlatProgram>, FlatProgramDecline> compileFlatProgram(const YarrPattern& pattern)
{
    ASSERT(pattern.m_executionMode == ExecutionMode::IncludeSubpatterns);
    return FlatProgramBuilder(pattern).build();
}

ASCIILiteral description(FlatProgramDecline reason)
{
    switch (reason) {
    case FlatProgramDecline::UnicodeMode:
        return "unicode mode"_s;
    case FlatProgramDecline::Modifiers:
        return "modifiers"_s;
    case FlatProgramDecline::Lookbehind:
        return "lookbehind"_s;
    case FlatProgramDecline::DuplicateNamedGroups:
        return "duplicate named groups"_s;
    case FlatProgramDecline::UnsignedLengthPattern:
        return "unsigned length pattern"_s;
    case FlatProgramDecline::CopiedGroup:
        return "copied group"_s;
    case FlatProgramDecline::DotStarEnclosure:
        return "enclosed by .*"_s;
    case FlatProgramDecline::NestedTooDeep:
        return "nested too deep"_s;
    case FlatProgramDecline::TooManySetsOrRegisters:
        return "too many sets or registers"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::Yarr
