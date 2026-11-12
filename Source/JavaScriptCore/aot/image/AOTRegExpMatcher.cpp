/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpMatcher.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTLatin1Test.h"
#include "AOTStubRegisters.h"
#include "AOTStubsRegExp.h"
#include "CCallHelpers.h"
#include "JSCJSValueInlines.h"
#include <algorithm>
#include <array>
#include <limits>
#include <wtf/ASCIICType.h>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>

namespace JSC::AOT {

using Node = RegExpMatchNode;
using Sequence = RegExpMatchNode::Sequence;
using Address = MacroAssembler::Address;
using Jump = MacroAssembler::Jump;
using JumpList = MacroAssembler::JumpList;
using Label = MacroAssembler::Label;
using TrustedImm32 = MacroAssembler::TrustedImm32;

static constexpr unsigned maxCharactersUnrolled = 4;
static constexpr unsigned maxCostOfPlanWithLoadedTable = 4;
static constexpr std::array registersOfMatcher { gprOf(StubRegister::A0), gprOf(StubRegister::A3), gprOf(StubRegister::A4), gprOf(StubRegister::T9), gprOf(StubRegister::A5), gprOf(StubRegister::T13), gprOf(StubRegister::A1) };
static constexpr std::array registersOfRecordingMatcher { gprOf(StubRegister::A0), gprOf(StubRegister::A3), gprOf(StubRegister::A4), gprOf(StubRegister::T9) };
static constexpr GPRReg characterGPR = gprOf(StubRegister::Operation);
static constexpr GPRReg endGPR = gprOf(registerOfMatcherEnd);
static constexpr GPRReg startGPR = gprOf(registerOfMatcherCharacters);
static constexpr GPRReg recordedStringGPR = gprOf(StubRegister::A2);
static constexpr GPRReg recordedStartGPR = gprOf(StubRegister::A3);
static constexpr GPRReg recordedEndGPR = gprOf(StubRegister::A4);
static constexpr GPRReg recordedCharactersGPR = gprOf(StubRegister::T9);
static_assert(registerOfMatcherCharacters == StubRegister::A0 && registersOfMatcher[0] == startGPR && registersOfRecordingMatcher[0] == startGPR && registersOfRecordingMatcher[1] == recordedStartGPR);
static_assert(areDistinctOnEveryISA(StubRegister::A0, StubRegister::A1, StubRegister::A3, StubRegister::A4, StubRegister::T9, registerOfMatcherEnd, registerOfMatcherString, registerOfMatcherRecorder, StubRegister::Operation));
static_assert(areDistinctOnEveryISA(StubRegister::A0, StubRegister::A3, StubRegister::A4, StubRegister::A5, StubRegister::T13, StubRegister::A1, StubRegister::T9, StubRegister::Operation, registerOfMatcherEnd));

namespace {

struct LoadedTable {
    GPRReg address { InvalidGPRReg };
    uint8_t mask { 0 };
};

class MatcherCompiler {
public:
    enum class Outcome : uint8_t { Done, NeedsStart, NeedsMoreRegisters };

    MatcherCompiler(CCallHelpers& jit, ImageCodeAddressing& addressing, const RegExpMatchPlan& plan, bool recordsMatches, bool keepsStart)
        : m_jit(jit)
        , m_addressing(addressing)
        , m_plan(plan)
        , m_positionRegisters(recordsMatches ? std::span<const GPRReg>(registersOfRecordingMatcher) : std::span<const GPRReg>(registersOfMatcher))
        , m_recordsMatches(recordsMatches)
        , m_keepsStart(keepsStart)
    {
        for (unsigned character = 0; character < 256; ++character) {
            if (isASCIIAlphanumeric(character) || character == '_')
                m_wordCharacters.set(character);
        }
        m_newlines.set('\n');
        m_newlines.set('\r');
    }

    Outcome compile();

private:
    struct State {
        unsigned base { 0 };
        unsigned numberOfRegisters { 1 };
        unsigned offset { 0 };
        unsigned available { 0 };
        bool isBaseAtStart { false };
        std::optional<Latin1Set> previous;
        std::optional<Latin1Set> next;
    };

    struct Exit {
        unsigned numberOfRegistersRead { 0 };
        JumpList jumps { };
    };

    using Continuation = ScopedLambda<void(State&, Exit& fails)>;

    GPRReg registerAt(unsigned number)
    {
        if (number < m_positionRegisters.size())
            return m_positionRegisters[number];
        m_needsMoreRegisters = true;
        return m_positionRegisters[0];
    }
    GPRReg allocate(State& state) { return registerAt(state.numberOfRegisters++); }
    GPRReg temporaryBehind(const State& state) { return registerAt(state.numberOfRegisters); }
    GPRReg baseOf(const State& state) { return registerAt(state.base); }
    bool canAdvanceBase(const State& state, const Exit& fails) const { return state.base + 1 == state.numberOfRegisters && state.base >= fails.numberOfRegistersRead; }

    void moveAddress(const State&, int64_t distance, GPRReg to);
    Jump branchOnAddress(MacroAssembler::RelationalCondition, const State&, int64_t distance, GPRReg other);
    GPRReg moveToPosition(State&, bool keepsBase);
    GPRReg start();

    LoadedTable loadTable(const Latin1Set&, unsigned numberOfRegister);
    void failUnlessCharacterIsIn(GPRReg base, int32_t offset, const Latin1Set&, JumpList& fails, const LoadedTable& = { });
    void failUnlessIsAtStart(const State&, JumpList& isAtStart);

    void requireCharacters(std::span<const Node> sequence, unsigned minimumLengthAfter, State&, Exit& fails);
    void emitLiterals(std::span<const Node>, State&, Exit& fails);
    void emitFixedCharacters(const Node&, State&, Exit& fails);
    void emitRun(const Node&, std::span<const Node> rest, unsigned minimumLengthAfter, State&&, Exit& fails, const Continuation&);
    void emitGroup(const Node&, std::span<const Node> rest, unsigned minimumLengthAfter, State&&, Exit& fails, const Continuation&);
    void emitGroupLoop(const Node&, std::span<const Node> rest, unsigned minimumLengthAfter, State&&, Exit& fails, const Continuation&);
    void emitAlternatives(std::span<const Sequence>, bool mayBeLeftOut, unsigned minimumLengthAfter, const State&, Exit& fails, const Continuation& matches, const Continuation& isLeftOut);
    void emitLookahead(const Node&, const State&, Exit& fails);
    void emitWordBoundary(const Node&, std::span<const Node> rest, const State&, Exit& fails);
    void emitStartOfLine(const State&, Exit& fails);
    void emitSequence(std::span<const Node>, unsigned minimumLengthAfter, State&&, Exit& fails, const Continuation&);
    void emitMatch(const State&);
    void emitMiss();

    static State behind(const State&);
    std::optional<bool> isWordCharacter(const std::optional<Latin1Set>&) const;

    CCallHelpers& m_jit;
    ImageCodeAddressing& m_addressing;
    const RegExpMatchPlan& m_plan;
    const std::span<const GPRReg> m_positionRegisters;
    const bool m_recordsMatches;
    const bool m_keepsStart;
    bool m_needsStart { false };
    bool m_needsMoreRegisters { false };
    std::optional<Label> m_isMatched;
    Latin1Set m_wordCharacters { };
    Latin1Set m_newlines { };
};

} // anonymous namespace

static bool isLiteral(const Node& node)
{
    if (node.kind != Node::Kind::Characters || node.isRun())
        return false;
    size_t count = node.characters.count();
    if (count == 1)
        return true;
    unsigned first = node.characters.findBit(0, true);
    return count == 2 && isASCIIAlpha(first) && node.characters.get(first ^ caseBitOfLatin1);
}

auto MatcherCompiler::behind(const State& state) -> State
{
    State result = state;
    result.offset = 0;
    result.available = 0;
    result.isBaseAtStart = false;
    result.previous = std::nullopt;
    result.next = std::nullopt;
    return result;
}

void MatcherCompiler::moveAddress(const State& state, int64_t distance, GPRReg to)
{
    int32_t total = static_cast<int32_t>(state.offset + distance);
    if (total)
        m_jit.add64(TrustedImm32(total), baseOf(state), to);
    else if (baseOf(state) != to)
        m_jit.move(baseOf(state), to);
}

Jump MatcherCompiler::branchOnAddress(MacroAssembler::RelationalCondition condition, const State& state, int64_t distance, GPRReg other)
{
    if (!(state.offset + distance))
        return m_jit.branch64(condition, baseOf(state), other);
    moveAddress(state, distance, characterGPR);
    return m_jit.branch64(condition, characterGPR, other);
}

GPRReg MatcherCompiler::moveToPosition(State& state, bool keepsBase)
{
    GPRReg position;
    unsigned number = state.base;
    if (keepsBase) {
        number = state.numberOfRegisters;
        position = allocate(state);
    } else
        position = baseOf(state);
    moveAddress(state, 0, position);
    state = behind(state);
    state.base = number;
    return position;
}

GPRReg MatcherCompiler::start()
{
    if (!m_keepsStart)
        m_needsStart = true;
    return startGPR;
}

std::optional<bool> MatcherCompiler::isWordCharacter(const std::optional<Latin1Set>& characters) const
{
    if (!characters || characters->isEmpty())
        return std::nullopt;
    Latin1Set others = *characters;
    others.exclude(m_wordCharacters);
    if (others.isEmpty())
        return true;
    Latin1Set words = *characters;
    words.filter(m_wordCharacters);
    if (words.isEmpty())
        return false;
    return std::nullopt;
}

LoadedTable MatcherCompiler::loadTable(const Latin1Set& set, unsigned numberOfRegister)
{
    if (numberOfRegister >= m_positionRegisters.size() || !Latin1Test::of(set).usesTable)
        return { };
    GPRReg address = registerAt(numberOfRegister);
    return { address, m_addressing.moveAddressOfLatin1Set(set, address) };
}

void MatcherCompiler::failUnlessCharacterIsIn(GPRReg base, int32_t offset, const Latin1Set& set, JumpList& fails, const LoadedTable& table)
{
    const Latin1Test test = Latin1Test::of(set);
    const Address place(base, offset);
    if (test.usesTable) {
        m_jit.load8(place, characterGPR);
        if (table.address == InvalidGPRReg) {
            fails.append(m_addressing.branchTestLatin1Set(MacroAssembler::Zero, characterGPR, set));
            return;
        }
        const MacroAssembler::BaseIndex entry(table.address, characterGPR, MacroAssembler::TimesOne);
#if CPU(X86_64)
        fails.append(m_jit.branchTest8(MacroAssembler::Zero, entry, TrustedImm32(table.mask)));
#else
        m_jit.load8(entry, characterGPR);
        fails.append(m_jit.branchTest32(MacroAssembler::Zero, characterGPR, TrustedImm32(table.mask)));
#endif
        return;
    }
    const auto isInRange = [&](const Latin1Range& range, bool isIn) -> Jump {
        auto [first, last] = range;
        if (!first)
            return m_jit.branch32(isIn ? MacroAssembler::BelowOrEqual : MacroAssembler::Above, characterGPR, TrustedImm32(last));
        if (last == 255)
            return m_jit.branch32(isIn ? MacroAssembler::AboveOrEqual : MacroAssembler::Below, characterGPR, TrustedImm32(first));
        m_jit.sub32(TrustedImm32(first), characterGPR);
        return m_jit.branch32(isIn ? MacroAssembler::BelowOrEqual : MacroAssembler::Above, characterGPR, TrustedImm32(last - first));
    };
    if (test.ranges.isEmpty() || (!test.ranges[0].first && test.ranges[0].last == 255)) {
        bool isInRanges = !test.ranges.isEmpty();
        if (isInRanges == test.isInverted)
            fails.append(m_jit.jump());
        return;
    }
    if (test.ranges.size() == 1 && test.ranges[0].first == test.ranges[0].last && !test.isFolded) {
        fails.append(m_jit.branch8(test.isInverted ? MacroAssembler::Equal : MacroAssembler::NotEqual, place, TrustedImm32(static_cast<int8_t>(test.ranges[0].first))));
        return;
    }
    m_jit.load8(place, characterGPR);
    if (test.isFolded)
        m_jit.or32(TrustedImm32(caseBitOfLatin1), characterGPR);
    if (test.ranges.size() == 1) {
        if (test.ranges[0].first == test.ranges[0].last)
            fails.append(m_jit.branch32(test.isInverted ? MacroAssembler::Equal : MacroAssembler::NotEqual, characterGPR, TrustedImm32(test.ranges[0].first)));
        else
            fails.append(isInRange(test.ranges[0], test.isInverted));
        return;
    }
    RELEASE_ASSERT(test.ranges.size() == 2 && test.ranges[0].first == test.ranges[0].last && test.ranges[1].first == test.ranges[1].last);
    Jump isFirst = m_jit.branch32(MacroAssembler::Equal, characterGPR, TrustedImm32(test.ranges[0].first));
    if (test.isInverted) {
        fails.append(isFirst);
        fails.append(m_jit.branch32(MacroAssembler::Equal, characterGPR, TrustedImm32(test.ranges[1].first)));
        return;
    }
    fails.append(m_jit.branch32(MacroAssembler::NotEqual, characterGPR, TrustedImm32(test.ranges[1].first)));
    isFirst.link(&m_jit);
}

void MatcherCompiler::requireCharacters(std::span<const Node> sequence, unsigned minimumLengthAfter, State& state, Exit& fails)
{
    if (state.available >= sequence.front().minimum)
        return;
    unsigned length = std::max(minimumLengthOf(sequence) + minimumLengthAfter, sequence.front().minimum);
    if (length == 1 && !state.offset)
        fails.jumps.append(m_jit.branch64(MacroAssembler::Equal, baseOf(state), endGPR));
    else
        fails.jumps.append(branchOnAddress(MacroAssembler::Above, state, length, endGPR));
    state.available = length;
}

void MatcherCompiler::emitLiterals(std::span<const Node> nodes, State& state, Exit& fails)
{
    Vector<uint8_t, 32> characters;
    Vector<uint8_t, 32> caseBits;
    for (const Node& node : nodes) {
        bool hasBothCases = node.characters.count() == 2;
        unsigned character = node.characters.findBit(0, true);
        for (unsigned count = 0; count < node.minimum; ++count) {
            characters.append(static_cast<uint8_t>(hasBothCases ? character | caseBitOfLatin1 : character));
            caseBits.append(hasBothCases ? caseBitOfLatin1 : 0);
        }
    }
    for (size_t position = 0; position < characters.size();) {
        size_t left = characters.size() - position;
        unsigned width = left >= 4 ? 4 : left >= 2 ? 2 : 1;
        size_t from = position;
        if (left < 4 && characters.size() >= 4) {
            width = 4;
            from = characters.size() - 4;
        }
        uint32_t expected = 0;
        uint32_t folded = 0;
        for (unsigned byte = 0; byte < width; ++byte) {
            expected |= static_cast<uint32_t>(characters[from + byte]) << (8 * byte);
            folded |= static_cast<uint32_t>(caseBits[from + byte]) << (8 * byte);
        }
        const Address place(baseOf(state), static_cast<int32_t>(state.offset + from));
        if (!folded && width == 1)
            fails.jumps.append(m_jit.branch8(MacroAssembler::NotEqual, place, TrustedImm32(static_cast<int8_t>(expected))));
        else if (!folded && width == 4)
            fails.jumps.append(m_jit.branch32(MacroAssembler::NotEqual, place, TrustedImm32(static_cast<int32_t>(expected))));
        else {
            if (width == 1)
                m_jit.load8(place, characterGPR);
            else if (width == 2)
                m_jit.load16Unaligned(place, characterGPR);
            else
                m_jit.load32(place, characterGPR);
            if (folded)
                m_jit.or32(TrustedImm32(static_cast<int32_t>(folded)), characterGPR);
            fails.jumps.append(m_jit.branch32(MacroAssembler::NotEqual, characterGPR, TrustedImm32(static_cast<int32_t>(expected))));
        }
        position = from + width;
    }
    state.offset += characters.size();
    state.available -= characters.size();
    state.previous = nodes.back().characters;
    state.next = std::nullopt;
}

void MatcherCompiler::emitFixedCharacters(const Node& node, State& state, Exit& fails)
{
    unsigned count = node.minimum;
    if (count > maxCharactersUnrolled) {
        unsigned available = state.available;
        GPRReg position = moveToPosition(state, !canAdvanceBase(state, fails));
        GPRReg limit = temporaryBehind(state);
        m_jit.add64(TrustedImm32(static_cast<int32_t>(count)), position, limit);
        const LoadedTable table = loadTable(node.characters, state.numberOfRegisters + 1);
        Label loop = m_jit.label();
        failUnlessCharacterIsIn(position, 0, node.characters, fails.jumps, table);
        m_jit.add64(TrustedImm32(1), position);
        m_jit.branch64(MacroAssembler::NotEqual, position, limit).linkTo(loop, &m_jit);
        state.available = available - count;
        state.previous = node.characters;
        return;
    }
    bool isFirstTested = state.next && [&] {
        Latin1Set others = *state.next;
        others.exclude(node.characters);
        return others.isEmpty();
    }();
    for (unsigned index = isFirstTested ? 1 : 0; index < count; ++index)
        failUnlessCharacterIsIn(baseOf(state), static_cast<int32_t>(state.offset + index), node.characters, fails.jumps);
    state.offset += count;
    state.available -= count;
    state.previous = node.characters;
    state.next = std::nullopt;
}

void MatcherCompiler::emitRun(const Node& node, std::span<const Node> rest, unsigned minimumLengthAfter, State&& state, Exit& fails, const Continuation& next)
{
    const bool isBounded = node.maximum != Node::unbounded;
    const State first = state;
    GPRReg position = moveToPosition(state, node.givesBack || !canAdvanceBase(state, fails));
    GPRReg limit = endGPR;
    auto loadLimit = [&](GPRReg to) {
        m_jit.add64(TrustedImm32(static_cast<int32_t>(node.maximum)), position, to);
        m_jit.moveConditionally64(MacroAssembler::Above, to, endGPR, endGPR, to);
        limit = to;
    };

    if (!node.isGreedy) {
        if (isBounded)
            loadLimit(allocate(state));
        Label test = m_jit.label();
        Exit retries { .numberOfRegistersRead = state.numberOfRegisters };
        emitSequence(rest, minimumLengthAfter, State(state), retries, next);
        if (retries.jumps.empty())
            return;
        retries.jumps.link(&m_jit);
        fails.jumps.append(m_jit.branch64(MacroAssembler::Equal, position, limit));
        failUnlessCharacterIsIn(position, 0, node.characters, fails.jumps);
        m_jit.add64(TrustedImm32(1), position);
        m_jit.jump().linkTo(test, &m_jit);
        return;
    }

    if (isBounded)
        loadLimit(temporaryBehind(state));
    JumpList isBehind;
    const LoadedTable table = loadTable(node.characters, state.numberOfRegisters + (isBounded ? 1 : 0));
    Jump isEntered = m_jit.jump();
    Label loop = m_jit.label();
    failUnlessCharacterIsIn(position, 0, node.characters, isBehind, table);
    m_jit.add64(TrustedImm32(1), position);
    isEntered.link(&m_jit);
    m_jit.branch64(MacroAssembler::NotEqual, position, limit).linkTo(loop, &m_jit);
    isBehind.link(&m_jit);

    if (!node.givesBack) {
        emitSequence(rest, minimumLengthAfter, WTF::move(state), fails, next);
        return;
    }

    Label test = m_jit.label();
    Exit retries { .numberOfRegistersRead = state.numberOfRegisters };
    emitSequence(rest, minimumLengthAfter, State(state), retries, next);
    if (retries.jumps.empty())
        return;
    retries.jumps.link(&m_jit);
    fails.jumps.append(branchOnAddress(MacroAssembler::Equal, first, 0, position));
    m_jit.sub64(TrustedImm32(1), position);
    m_jit.jump().linkTo(test, &m_jit);
}

void MatcherCompiler::emitAlternatives(std::span<const Sequence> alternatives, bool mayBeLeftOut, unsigned minimumLengthAfter, const State& state, Exit& fails, const Continuation& matches, const Continuation& isLeftOut)
{
    for (size_t index = 0; index < alternatives.size(); ++index) {
        if (index + 1 == alternatives.size() && !mayBeLeftOut) {
            emitSequence(alternatives[index].span(), minimumLengthAfter, State(state), fails, matches);
            return;
        }
        Exit next { .numberOfRegistersRead = std::max(fails.numberOfRegistersRead, state.numberOfRegisters) };
        emitSequence(alternatives[index].span(), minimumLengthAfter, State(state), next, matches);
        if (next.jumps.empty())
            return;
        next.jumps.link(&m_jit);
    }
    State leftOut = state;
    isLeftOut(leftOut, fails);
}

void MatcherCompiler::emitGroup(const Node& node, std::span<const Node> rest, unsigned minimumLengthAfter, State&& state, Exit& fails, const Continuation& next)
{
    const unsigned minimumLengthAfterGroup = minimumLengthOf(rest) + minimumLengthAfter;
    const bool mayBeLeftOut = !node.minimum;
    if (node.commits && (mayBeLeftOut || node.alternatives.size() > 1)) {
        State joined = behind(state);
        if (!canAdvanceBase(state, fails)) {
            joined.base = state.numberOfRegisters;
            joined.numberOfRegisters = state.numberOfRegisters + 1;
        }
        const GPRReg position = baseOf(joined);
        joined.available = std::numeric_limits<unsigned>::max();
        JumpList isMatched;
        auto join = [&](State& inner, Exit&) {
            joined.available = std::min(joined.available, inner.available);
            moveAddress(inner, 0, position);
            isMatched.append(m_jit.jump());
        };
        emitAlternatives(node.alternatives.span(), mayBeLeftOut, minimumLengthAfterGroup, state, fails, join, join);
        isMatched.link(&m_jit);
        emitSequence(rest, minimumLengthAfter, WTF::move(joined), fails, next);
        return;
    }

    auto goesOn = [&](State& inner, Exit& innerFails) {
        emitSequence(rest, minimumLengthAfter, State(inner), innerFails, next);
    };
    emitAlternatives(node.alternatives.span(), mayBeLeftOut, minimumLengthAfterGroup, state, fails, goesOn, goesOn);
}

void MatcherCompiler::emitGroupLoop(const Node& node, std::span<const Node> rest, unsigned minimumLengthAfter, State&& state, Exit& fails, const Continuation& next)
{
    const State first = state;
    GPRReg position = moveToPosition(state, node.minimum || !canAdvanceBase(state, fails));
    Label loop = m_jit.label();
    Exit ends { .numberOfRegistersRead = state.numberOfRegisters };
    auto matches = [&](State& inner, Exit&) {
        moveAddress(inner, 0, position);
        m_jit.jump().linkTo(loop, &m_jit);
    };
    emitAlternatives(node.alternatives.span(), false, 0, state, ends, matches, matches);

    ends.jumps.link(&m_jit);
    if (node.minimum)
        fails.jumps.append(branchOnAddress(MacroAssembler::Equal, first, 0, position));
    emitSequence(rest, minimumLengthAfter, WTF::move(state), fails, next);
}

void MatcherCompiler::emitLookahead(const Node& node, const State& state, Exit& fails)
{
    const bool isNegative = node.kind == Node::Kind::NegativeLookahead;
    Exit otherwise { .numberOfRegistersRead = state.numberOfRegisters };
    JumpList isMatched;
    auto matches = [&](State&, Exit&) {
        isMatched.append(m_jit.jump());
    };
    emitAlternatives(node.alternatives.span(), false, 0, state, otherwise, matches, matches);
    if (isNegative) {
        fails.jumps.append(isMatched);
        otherwise.jumps.link(&m_jit);
    } else {
        fails.jumps.append(otherwise.jumps);
        isMatched.link(&m_jit);
    }
}

void MatcherCompiler::emitWordBoundary(const Node& node, std::span<const Node> rest, const State& state, Exit& fails)
{
    const bool isBoundary = node.kind == Node::Kind::WordBoundary;
    std::optional<bool> isPrecededByWordCharacter = isWordCharacter(state.previous);
    if (state.isBaseAtStart && !state.offset)
        isPrecededByWordCharacter = false;
    std::optional<bool> isWordBehind;
    if (!rest.empty() && rest.front().kind == Node::Kind::Characters && rest.front().minimum)
        isWordBehind = isWordCharacter(rest.front().characters);
    JumpList isDone;

    auto failUnlessWordBehindIs = [&](bool isExpected) {
        if (isWordBehind) {
            if (*isWordBehind != isExpected)
                fails.jumps.append(m_jit.jump());
            return;
        }
        JumpList isNoWord;
        if (!state.available)
            isNoWord.append(branchOnAddress(MacroAssembler::Equal, state, 0, endGPR));
        failUnlessCharacterIsIn(baseOf(state), static_cast<int32_t>(state.offset), m_wordCharacters, isNoWord);
        if (isExpected) {
            fails.jumps.append(isNoWord);
            return;
        }
        fails.jumps.append(m_jit.jump());
        isNoWord.link(&m_jit);
    };
    if (isPrecededByWordCharacter) {
        failUnlessWordBehindIs(*isPrecededByWordCharacter != isBoundary);
        return;
    }
    JumpList isNoWord;
    if (!state.previous && !state.offset)
        isNoWord.append(m_jit.branch64(MacroAssembler::Equal, baseOf(state), start()));
    failUnlessCharacterIsIn(baseOf(state), static_cast<int32_t>(state.offset) - 1, m_wordCharacters, isNoWord);
    failUnlessWordBehindIs(!isBoundary);
    isDone.append(m_jit.jump());
    isNoWord.link(&m_jit);
    failUnlessWordBehindIs(isBoundary);
    isDone.link(&m_jit);
}

void MatcherCompiler::emitStartOfLine(const State& state, Exit& fails)
{
    if (state.isBaseAtStart && !state.offset)
        return;
    JumpList isAtStart;
    if (!state.previous && !state.offset)
        isAtStart.append(m_jit.branch64(MacroAssembler::Equal, baseOf(state), start()));
    failUnlessCharacterIsIn(baseOf(state), static_cast<int32_t>(state.offset) - 1, m_newlines, fails.jumps);
    isAtStart.link(&m_jit);
}

void MatcherCompiler::emitSequence(std::span<const Node> nodes, unsigned minimumLengthAfter, State&& state, Exit& fails, const Continuation& next)
{
    for (size_t index = 0; index < nodes.size(); ++index) {
        const Node& node = nodes[index];
        std::span<const Node> rest = nodes.subspan(index + 1);
        switch (node.kind) {
        case Node::Kind::Characters: {
            if (node.isRun()) {
                emitRun(node, rest, minimumLengthAfter, WTF::move(state), fails, next);
                return;
            }
            requireCharacters(nodes.subspan(index), minimumLengthAfter, state, fails);
            size_t end = index;
            unsigned length = 0;
            while (end < nodes.size() && isLiteral(nodes[end]) && length + nodes[end].minimum <= state.available)
                length += nodes[end++].minimum;
            if (length > 1) {
                emitLiterals(nodes.subspan(index, end - index), state, fails);
                index = end - 1;
            } else
                emitFixedCharacters(node, state, fails);
            break;
        }
        case Node::Kind::Group:
            if (node.maximum == Node::unbounded)
                emitGroupLoop(node, rest, minimumLengthAfter, WTF::move(state), fails, next);
            else
                emitGroup(node, rest, minimumLengthAfter, WTF::move(state), fails, next);
            return;
        case Node::Kind::StartOfSubject:
            if (!state.isBaseAtStart || state.offset)
                fails.jumps.append(branchOnAddress(MacroAssembler::NotEqual, state, 0, start()));
            break;
        case Node::Kind::StartOfLine:
            emitStartOfLine(state, fails);
            break;
        case Node::Kind::EndOfSubject:
            fails.jumps.append(branchOnAddress(MacroAssembler::NotEqual, state, 0, endGPR));
            break;
        case Node::Kind::EndOfLine: {
            Jump isAtEnd = branchOnAddress(MacroAssembler::Equal, state, 0, endGPR);
            failUnlessCharacterIsIn(baseOf(state), static_cast<int32_t>(state.offset), m_newlines, fails.jumps);
            isAtEnd.link(&m_jit);
            break;
        }
        case Node::Kind::WordBoundary:
        case Node::Kind::NoWordBoundary:
            emitWordBoundary(node, rest, state, fails);
            break;
        case Node::Kind::Lookahead:
        case Node::Kind::NegativeLookahead:
            emitLookahead(node, state, fails);
            break;
        }
    }
    next(state, fails);
}

void MatcherCompiler::emitMatch(const State& state)
{
    if (m_recordsMatches)
        moveAddress(state, 0, recordedEndGPR);
    if (m_isMatched) {
        m_jit.jump().linkTo(*m_isMatched, &m_jit);
        return;
    }
    m_isMatched = m_jit.label();
    if (!m_recordsMatches) {
        m_jit.move(TrustedImm32(static_cast<int32_t>(JSValue::encode(jsBoolean(true)))), GPRInfo::returnValueGPR);
        m_jit.ret();
        return;
    }
    if (!m_plan.isSearched)
        m_jit.move(startGPR, recordedStartGPR);
    m_jit.move(startGPR, recordedCharactersGPR);
    m_jit.move(gprOf(registerOfMatcherString), recordedStringGPR);
    m_jit.farJump(gprOf(registerOfMatcherRecorder), NoPtrTag);
}

void MatcherCompiler::emitMiss()
{
    m_jit.move(TrustedImm32(static_cast<int32_t>(JSValue::encode(jsBoolean(false)))), GPRInfo::returnValueGPR);
    m_jit.ret();
}

auto MatcherCompiler::compile() -> Outcome
{
    Exit misses { .numberOfRegistersRead = m_keepsStart ? 1u : 0u };
    State state { .isBaseAtStart = !m_plan.isSearched };
    auto matches = [&](State& state, Exit&) {
        emitMatch(state);
    };
    if (!m_plan.isSearched)
        emitAlternatives(m_plan.alternatives.span(), false, 0, state, misses, matches, matches);
    else {
        const unsigned minimumLength = m_plan.minimumLength;
        if (m_keepsStart)
            moveToPosition(state, true);
        const GPRReg begin = baseOf(state);
        if (minimumLength == 1)
            misses.jumps.append(m_jit.branch64(MacroAssembler::Equal, begin, endGPR));
        else if (minimumLength)
            misses.jumps.append(branchOnAddress(MacroAssembler::Above, state, minimumLength, endGPR));
        if (m_plan.maximumLengthToEnd) {
            m_jit.add64(TrustedImm32(-static_cast<int32_t>(*m_plan.maximumLengthToEnd)), endGPR, characterGPR);
            m_jit.moveConditionally64(MacroAssembler::Above, characterGPR, begin, characterGPR, begin);
        }
        GPRReg last = endGPR;
        if (minimumLength > 1) {
            last = allocate(state);
            m_jit.add64(TrustedImm32(-static_cast<int32_t>(minimumLength)), endGPR, last);
        }

        LoadedTable table;
        if (m_plan.firstCharacters && m_plan.cost <= maxCostOfPlanWithLoadedTable) {
            table = loadTable(*m_plan.firstCharacters, state.numberOfRegisters);
            if (table.address != InvalidGPRReg)
                allocate(state);
        }
        Jump isEntered = m_jit.jump();
        Label advances = m_jit.label();
        if (!minimumLength)
            misses.jumps.append(m_jit.branch64(MacroAssembler::Equal, begin, endGPR));
        m_jit.add64(TrustedImm32(1), begin);
        if (minimumLength == 1)
            misses.jumps.append(m_jit.branch64(MacroAssembler::Equal, begin, endGPR));
        else if (minimumLength)
            misses.jumps.append(m_jit.branch64(MacroAssembler::Above, begin, last));
        isEntered.link(&m_jit);

        Exit fails { .numberOfRegistersRead = state.numberOfRegisters };
        if (m_plan.firstCharacters) {
            failUnlessCharacterIsIn(begin, 0, *m_plan.firstCharacters, fails.jumps, table);
            state.next = m_plan.firstCharacters;
        }
        state.available = minimumLength;
        emitAlternatives(m_plan.alternatives.span(), false, 0, state, fails, matches, matches);
        fails.jumps.linkTo(advances, &m_jit);
    }
    if (!misses.jumps.empty()) {
        misses.jumps.link(&m_jit);
        emitMiss();
    }

    if (m_needsMoreRegisters)
        return Outcome::NeedsMoreRegisters;
    return m_needsStart ? Outcome::NeedsStart : Outcome::Done;
}

std::unique_ptr<RegExpCode> compileRegExpMatcher(const RegExpMatchPlan& plan, bool recordsMatches, const AssemblerPolicy& policy, BuildContext* context)
{
    if (plan.isPrefix)
        return nullptr;
    for (bool keepsStart : { false, true }) {
        if (recordsMatches && !keepsStart)
            continue;
        CCallHelpers jit;
        ImageCodeAddressing addressing(jit, PCRelativeReach::Far, policy, context);
        jit.m_assembler.buffer().putInt(plan.isSearched ? maxLengthSearchedByMatcher : std::numeric_limits<uint32_t>::max());
        jit.m_assembler.buffer().putInt(0);
        jit.tagReturnAddress();
        switch (MatcherCompiler(jit, addressing, plan, recordsMatches, keepsStart).compile()) {
        case MatcherCompiler::Outcome::NeedsStart:
            continue;
        case MatcherCompiler::Outcome::NeedsMoreRegisters:
            return nullptr;
        case MatcherCompiler::Outcome::Done: {
            auto code = addressing.takeCode();
            return code ? WTF::move(*code) : nullptr;
        }
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::unique_ptr<RegExpCode> createRegExpMatcher(const RegExpCode& code, uint32_t record)
{
    auto matcher = makeUnique<RegExpCode>();
    matcher->body = code.body;
    matcher->symbols = code.symbols;
    matcher->reach = code.reach;
    memcpySpan(matcher->body.bytes.mutableSpan().subspan(sizeOfMatcherHeader + offsetOfRecordOfMatcher), asByteSpan(record));
    return matcher;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
