/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpBitParallel.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "CCallHelpers.h"
#include "YarrJITRegisters.h"
#include "YarrPattern.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <unicode/utf16.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

using Address = MacroAssembler::Address;
using BaseIndex = MacroAssembler::BaseIndex;
using Jump = MacroAssembler::Jump;
using JumpList = MacroAssembler::JumpList;
using Label = MacroAssembler::Label;
using TrustedImm32 = MacroAssembler::TrustedImm32;
using TrustedImm64 = MacroAssembler::TrustedImm64;

static constexpr unsigned maxStates = 63;
static constexpr unsigned maxAlternatives = 31;
static constexpr unsigned maxCodePoints = 256;
static constexpr unsigned unbounded = std::numeric_limits<unsigned>::max();
static constexpr unsigned unitsOfCodePoint = 2;

namespace {

using CodePointRanges = Vector<Yarr::CharacterRange, 4>;

struct AlternativeOfCodePoints {
    Vector<CodePointRanges, 8> sets;
    unsigned maxRepeats { 0 };
    Vector<BitParallelRegExp::Group, 2> groups;
};

} // anonymous namespace

static std::optional<CodePointRanges> codePointsOf(const Yarr::PatternTerm& term)
{
    CodePointRanges ranges;
    if (term.type == Yarr::PatternTerm::Type::PatternCharacter) {
        if (term.ignoreCase())
            return std::nullopt;
        ranges.append({ term.patternCharacter, term.patternCharacter });
    } else {
        const Yarr::CharacterClass& characterClass = *term.characterClass;
        if (term.invert() || characterClass.hasStrings() || characterClass.m_anyCharacter || !characterClass.m_matches8.isEmpty() || !characterClass.m_ranges8.isEmpty())
            return std::nullopt;
        for (char32_t character : characterClass.m_matches32)
            ranges.append({ character, character });
        ranges.appendVector(characterClass.m_ranges32);
    }
    bool areSupplementary = !ranges.isEmpty() && std::ranges::all_of(ranges, [](const Yarr::CharacterRange& range) {
        return range.begin <= range.end && !U_IS_BMP(range.begin);
    });
    if (!areSupplementary)
        return std::nullopt;
    return ranges;
}

static bool addTerm(const Yarr::PatternTerm& term, Vector<AlternativeOfCodePoints>& sequences)
{
    bool isAfterRepeats = std::ranges::any_of(sequences, [](const AlternativeOfCodePoints& sequence) { return sequence.maxRepeats; });
    if (isAfterRepeats || term.matchDirection() != Yarr::Forward)
        return false;
    switch (term.type) {
    case Yarr::PatternTerm::Type::PatternCharacter:
    case Yarr::PatternTerm::Type::CharacterClass: {
        std::optional<CodePointRanges> set = codePointsOf(term);
        if (!set)
            return false;
        bool isFixed = term.quantityMinCount == term.quantityMaxCount;
        if (isFixed ? !term.quantityMinCount || term.quantityMinCount.value() > maxStates : term.quantityType != Yarr::QuantifierType::Greedy || term.quantityMinCount)
            return false;
        for (AlternativeOfCodePoints& sequence : sequences) {
            if (isFixed) {
                for (unsigned count = 0; count < term.quantityMinCount; ++count)
                    sequence.sets.append(*set);
                continue;
            }
            if (sequence.sets.isEmpty() || sequence.sets.last() != *set)
                return false;
            sequence.maxRepeats = term.quantityMaxCount == Yarr::quantifyInfinite ? unbounded : term.quantityMaxCount.value();
        }
        return true;
    }
    case Yarr::PatternTerm::Type::ParenthesesSubpattern: {
        if (term.quantityMinCount != 1 || term.quantityMaxCount != 1)
            return false;
        Vector<AlternativeOfCodePoints> results;
        for (const AlternativeOfCodePoints& sequence : sequences) {
            for (auto& alternative : term.parentheses.disjunction->m_alternatives) {
                Vector<AlternativeOfCodePoints> inner { sequence };
                for (const Yarr::PatternTerm& innerTerm : alternative->m_terms) {
                    if (!addTerm(innerTerm, inner))
                        return false;
                }
                if (results.size() + inner.size() > maxAlternatives)
                    return false;
                for (AlternativeOfCodePoints& result : inner) {
                    if (term.capture())
                        result.groups.append({ .group = term.parentheses.subpatternId, .first = safeCast<unsigned>(sequence.sets.size()), .last = safeCast<unsigned>(result.sets.size()), .includesRepeats = !!result.maxRepeats });
                    results.append(WTF::move(result));
                }
            }
        }
        sequences = WTF::move(results);
        return true;
    }
    case Yarr::PatternTerm::Type::AssertionBOL:
    case Yarr::PatternTerm::Type::AssertionEOL:
    case Yarr::PatternTerm::Type::AssertionWordBoundary:
    case Yarr::PatternTerm::Type::NumberedBackReference:
    case Yarr::PatternTerm::Type::NamedBackReference:
    case Yarr::PatternTerm::Type::NumberedForwardReference:
    case Yarr::PatternTerm::Type::NamedForwardReference:
    case Yarr::PatternTerm::Type::ParentheticalAssertion:
    case Yarr::PatternTerm::Type::DotStarEnclosure:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::unique_ptr<const BitParallelRegExp> planBitParallelRegExp(const Yarr::YarrPattern& pattern)
{
    if (!pattern.eitherUnicode() || pattern.sticky() || pattern.m_containsBackreferences || pattern.m_containsLookbehinds || pattern.m_numDuplicateNamedCaptureGroups)
        return nullptr;

    Vector<AlternativeOfCodePoints> sequences;
    for (auto& alternative : pattern.m_body->m_alternatives) {
        Vector<AlternativeOfCodePoints> own { AlternativeOfCodePoints { } };
        if (alternative->onceThrough() || !std::ranges::all_of(alternative->m_terms, [&](const Yarr::PatternTerm& term) { return addTerm(term, own); }))
            return nullptr;
        sequences.appendVector(WTF::move(own));
        if (sequences.size() > maxAlternatives)
            return nullptr;
    }

    auto plan = makeUnique<BitParallelRegExp>();
    plan->numberOfAlternatives = safeCast<unsigned>(sequences.size());
    plan->numberOfGroups = pattern.m_numSubpatterns;
    char32_t firstCodePoint = std::numeric_limits<char32_t>::max();
    char32_t lastCodePoint = 0;
    for (unsigned number = 0; number < sequences.size(); ++number) {
        AlternativeOfCodePoints& sequence = sequences[number];
        if (sequence.sets.isEmpty() || sequence.sets.size() != sequences[0].sets.size() || sequence.maxRepeats != sequences[0].maxRepeats)
            return nullptr;
        for (const CodePointRanges& set : sequence.sets) {
            for (const Yarr::CharacterRange& range : set) {
                firstCodePoint = std::min(firstCodePoint, range.begin);
                lastCodePoint = std::max(lastCodePoint, range.end);
            }
        }
        for (BitParallelRegExp::Group group : sequence.groups) {
            group.alternative = number;
            plan->groups.append(group);
        }
    }
    plan->length = safeCast<unsigned>(sequences[0].sets.size());
    plan->maxRepeats = sequences[0].maxRepeats;
    if (plan->numberOfStates() > maxStates || lastCodePoint - firstCodePoint >= maxCodePoints || U16_LEAD(firstCodePoint) != U16_LEAD(lastCodePoint))
        return nullptr;

    plan->firstCodePoint = firstCodePoint;
    plan->states.fill(0, lastCodePoint - firstCodePoint + 1);
    for (unsigned alternative = 0; alternative < plan->numberOfAlternatives; ++alternative) {
        for (unsigned position = 0; position < plan->length; ++position) {
            for (const Yarr::CharacterRange& range : sequences[alternative].sets[position]) {
                for (char32_t codePoint = range.begin; codePoint <= range.end; ++codePoint)
                    plan->states[codePoint - firstCodePoint] |= 1ULL << (position * plan->numberOfAlternatives + alternative);
            }
        }
    }
    return plan;
}

namespace {

class BitParallelEmitter {
public:
    BitParallelEmitter(CCallHelpers& jit, ImageCodeAddressing& addressing, const BitParallelRegExp& plan)
        : m_jit(jit)
        , m_addressing(addressing)
        , m_plan(plan)
    {
    }

    void emit();

private:
    using Registers = Yarr::YarrJITDefaultRegisters;

    static constexpr GPRReg codePointGPR = Registers::regT0;
    static constexpr GPRReg stateGPR = Registers::regT1;
    static constexpr GPRReg tableGPR = Registers::regT2;
    static constexpr GPRReg lastStartGPR = Registers::regT3;
    static constexpr GPRReg indexGPR = Registers::index;

    static BaseIndex unitAt(GPRReg index, unsigned offset = 0) { return { Registers::input, index, MacroAssembler::TimesTwo, static_cast<int32_t>(offset * sizeof(char16_t)) }; }

    unsigned firstAcceptingState() const { return (m_plan.length - 1) * m_plan.numberOfAlternatives; }
    unsigned bytesOfStates() const;
    void moveAddressOfTable();
    void loadStates(GPRReg dest);
    Jump loadCodePoint();
    void enterStates();
    Jump branchIfAccepts();
    void emitStartsOfMatches(JumpList& matches, JumpList& fails);
    void emitRestOfMatches(JumpList& matches, JumpList& fails);
    void emitMatch();
    void emitFailure();

    CCallHelpers& m_jit;
    ImageCodeAddressing& m_addressing;
    const BitParallelRegExp& m_plan;
};

} // anonymous namespace

unsigned BitParallelEmitter::bytesOfStates() const
{
    unsigned states = m_plan.numberOfStates();
    return states <= 8 ? 1 : states <= 16 ? 2 : states <= 32 ? 4 : 8;
}

void BitParallelEmitter::moveAddressOfTable()
{
    unsigned bytes = bytesOfStates();
    Vector<uint8_t> table;
    for (uint64_t states : m_plan.states) {
        for (unsigned byte = 0; byte < bytes; ++byte)
            table.append(static_cast<uint8_t>(states >> (8 * byte)));
    }
    m_addressing.moveAddressOfData(table.span(), tableGPR);
}

void BitParallelEmitter::loadStates(GPRReg dest)
{
    switch (bytesOfStates()) {
    case 1:
        m_jit.load8(BaseIndex(tableGPR, codePointGPR, MacroAssembler::TimesOne), dest);
        return;
    case 2:
        m_jit.load16(BaseIndex(tableGPR, codePointGPR, MacroAssembler::TimesTwo), dest);
        return;
    case 4:
        m_jit.load32(BaseIndex(tableGPR, codePointGPR, MacroAssembler::TimesFour), dest);
        return;
    case 8:
        m_jit.load64(BaseIndex(tableGPR, codePointGPR, MacroAssembler::TimesEight), dest);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Jump BitParallelEmitter::loadCodePoint()
{
    uint32_t first = static_cast<uint32_t>(U16_TRAIL(m_plan.firstCodePoint)) << 16 | U16_LEAD(m_plan.firstCodePoint);
    m_jit.load32(unitAt(indexGPR), codePointGPR);
    m_jit.sub32(TrustedImm32(static_cast<int32_t>(first)), codePointGPR);
    m_jit.rotateRight32(TrustedImm32(16), codePointGPR);
    return m_jit.branch32(MacroAssembler::AboveOrEqual, codePointGPR, TrustedImm32(static_cast<int32_t>(m_plan.states.size())));
}

void BitParallelEmitter::enterStates()
{
    loadStates(codePointGPR);
    m_jit.and64(codePointGPR, stateGPR);
    m_jit.add32(TrustedImm32(unitsOfCodePoint), indexGPR);
}

Jump BitParallelEmitter::branchIfAccepts()
{
    if (!firstAcceptingState())
        return m_jit.branchTest64(MacroAssembler::NonZero, stateGPR);
    if (firstAcceptingState() < 31 && m_plan.numberOfStates() <= 32)
        return m_jit.branch32(MacroAssembler::AboveOrEqual, stateGPR, TrustedImm32(1 << firstAcceptingState()));
    m_jit.urshift64(stateGPR, TrustedImm32(firstAcceptingState()), codePointGPR);
    return m_jit.branchTest64(MacroAssembler::NonZero, codePointGPR);
}

void BitParallelEmitter::emitStartsOfMatches(JumpList& matches, JumpList& fails)
{
    m_jit.move(TrustedImm32(0), stateGPR);
    Label next = m_jit.label();
    Jump isOutside = loadCodePoint();
    if (m_plan.length > 1)
        m_jit.lshift64(TrustedImm32(m_plan.numberOfAlternatives), stateGPR);
    m_jit.or64(TrustedImm32((1 << m_plan.numberOfAlternatives) - 1), stateGPR);
    enterStates();
    matches.append(branchIfAccepts());
    m_jit.branch32(MacroAssembler::LessThanOrEqual, indexGPR, lastStartGPR).linkTo(next, &m_jit);
    Jump hasNoPlaceLeft = m_jit.jump();

    isOutside.link(&m_jit);
    m_jit.move(TrustedImm32(0), stateGPR);
    m_jit.load16(unitAt(indexGPR), codePointGPR);
    m_jit.add32(TrustedImm32(1), indexGPR);
    m_jit.and32(TrustedImm32(0xfc00), codePointGPR);
    Jump isNoLead = m_jit.branch32(MacroAssembler::NotEqual, codePointGPR, TrustedImm32(0xd800));
    m_jit.load16(unitAt(indexGPR), codePointGPR);
    m_jit.and32(TrustedImm32(0xfc00), codePointGPR);
    Jump isNoTrail = m_jit.branch32(MacroAssembler::NotEqual, codePointGPR, TrustedImm32(0xdc00));
    m_jit.add32(TrustedImm32(1), indexGPR);
    isNoLead.link(&m_jit);
    isNoTrail.link(&m_jit);
    m_jit.branch32(MacroAssembler::LessThanOrEqual, indexGPR, lastStartGPR).linkTo(next, &m_jit);
    fails.append(m_jit.jump());

    hasNoPlaceLeft.link(&m_jit);
}

void BitParallelEmitter::emitRestOfMatches(JumpList& matches, JumpList& fails)
{
    if (m_plan.length == 1) {
        fails.append(m_jit.jump());
        return;
    }
    Label next = m_jit.label();
    fails.append(m_jit.branchTest64(MacroAssembler::Zero, stateGPR));
    fails.append(loadCodePoint());
    m_jit.lshift64(TrustedImm32(m_plan.numberOfAlternatives), stateGPR);
    enterStates();
    matches.append(branchIfAccepts());
    m_jit.jump().linkTo(next, &m_jit);
}

void BitParallelEmitter::emitMatch()
{
    const bool needsAlternative = m_plan.maxRepeats || !m_plan.groups.isEmpty();
    const GPRReg startGPR = codePointGPR;
    m_jit.sub32(indexGPR, TrustedImm32(m_plan.length * unitsOfCodePoint), startGPR);
    m_jit.store32(startGPR, Address(Registers::output));
    if (needsAlternative) {
        if (firstAcceptingState())
            m_jit.urshift64(TrustedImm32(firstAcceptingState()), stateGPR);
        m_jit.move(stateGPR, lastStartGPR);
        m_jit.neg64(lastStartGPR);
        m_jit.and64(lastStartGPR, stateGPR);
        if (firstAcceptingState())
            m_jit.lshift64(TrustedImm32(firstAcceptingState()), stateGPR);
    }
    if (m_plan.maxRepeats) {
        JumpList isDone;
        auto repeat = [&] {
            m_jit.add32(TrustedImm32(unitsOfCodePoint), indexGPR, codePointGPR);
            isDone.append(m_jit.branch32(MacroAssembler::Above, codePointGPR, Registers::length));
            isDone.append(loadCodePoint());
            loadStates(codePointGPR);
            isDone.append(m_jit.branchTest64(MacroAssembler::Zero, codePointGPR, stateGPR));
            m_jit.add32(TrustedImm32(unitsOfCodePoint), indexGPR);
        };
        constexpr unsigned maxRepeatsUnrolled = 2;
        if (m_plan.maxRepeats <= maxRepeatsUnrolled) {
            for (unsigned count = 0; count < m_plan.maxRepeats; ++count)
                repeat();
        } else {
            const bool isBounded = m_plan.maxRepeats != unbounded;
            if (isBounded)
                m_jit.move(TrustedImm32(m_plan.maxRepeats), lastStartGPR);
            Label next = m_jit.label();
            repeat();
            if (isBounded)
                m_jit.branchSub32(MacroAssembler::NonZero, TrustedImm32(1), lastStartGPR).linkTo(next, &m_jit);
            else
                m_jit.jump().linkTo(next, &m_jit);
        }
        isDone.link(&m_jit);
        m_jit.load32(Address(Registers::output), startGPR);
    }
    m_jit.store32(indexGPR, Address(Registers::output, sizeof(int32_t)));

    auto startOf = [](unsigned group) { return Address(Registers::output, group * 2 * sizeof(int32_t)); };
    auto endOf = [](unsigned group) { return Address(Registers::output, (group * 2 + 1) * sizeof(int32_t)); };
    for (unsigned group = 1; group <= m_plan.numberOfGroups; ++group)
        m_jit.store64(TrustedImm64(-1), startOf(group));
    for (const BitParallelRegExp::Group& group : m_plan.groups) {
        m_jit.move(TrustedImm64(1LL << (firstAcceptingState() + group.alternative)), lastStartGPR);
        Jump isOther = m_jit.branchTest64(MacroAssembler::Zero, stateGPR, lastStartGPR);
        m_jit.add32(TrustedImm32(group.first * unitsOfCodePoint), startGPR, lastStartGPR);
        m_jit.store32(lastStartGPR, startOf(group.group));
        if (group.includesRepeats)
            m_jit.store32(indexGPR, endOf(group.group));
        else {
            m_jit.add32(TrustedImm32(group.last * unitsOfCodePoint), startGPR, lastStartGPR);
            m_jit.store32(lastStartGPR, endOf(group.group));
        }
        isOther.link(&m_jit);
    }
    m_jit.move(indexGPR, Registers::returnRegister2);
    m_jit.move(startGPR, Registers::returnRegister);
    m_jit.tagReturnAddress();
    m_jit.ret();
}

void BitParallelEmitter::emitFailure()
{
    m_jit.move(TrustedImm64(static_cast<int64_t>(WTF::notFound)), Registers::returnRegister);
    m_jit.move(TrustedImm32(0), Registers::returnRegister2);
    m_jit.tagReturnAddress();
    m_jit.ret();
}

void BitParallelEmitter::emit()
{
    JumpList fails;
    JumpList matches;
    JumpList startsWithinPair;

    Jump startsAtSubject = m_jit.branchTest32(MacroAssembler::Zero, indexGPR);
    fails.append(m_jit.branch32(MacroAssembler::AboveOrEqual, indexGPR, Registers::length));
    m_jit.load16(unitAt(indexGPR), codePointGPR);
    m_jit.and32(TrustedImm32(0xfc00), codePointGPR);
    startsWithinPair.append(m_jit.branch32(MacroAssembler::Equal, codePointGPR, TrustedImm32(0xdc00)));
    startsAtSubject.link(&m_jit);
    m_jit.sub32(Registers::length, TrustedImm32(m_plan.length * unitsOfCodePoint), lastStartGPR);
    fails.append(m_jit.branch32(MacroAssembler::LessThan, lastStartGPR, indexGPR));
    moveAddressOfTable();

    emitStartsOfMatches(matches, fails);
    emitRestOfMatches(matches, fails);
    matches.link(&m_jit);
    emitMatch();
    fails.link(&m_jit);
    emitFailure();

    startsWithinPair.link(&m_jit);
}

void emitBitParallelRegExp(CCallHelpers& jit, ImageCodeAddressing& addressing, const BitParallelRegExp& plan)
{
    BitParallelEmitter(jit, addressing, plan).emit();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
