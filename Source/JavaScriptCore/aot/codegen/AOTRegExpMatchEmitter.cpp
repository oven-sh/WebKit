/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpMatchEmitter.h"

#if ENABLE(AOT)

#include "AOTStringEmitter.h"
#include <algorithm>
#include <array>
#include <wtf/ASCIICType.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned maxRangesCompared = 2;
static constexpr unsigned maxCharactersUnrolled = 4;
static constexpr unsigned planesOfTable = 8;
static constexpr int32_t caseBit = 0x20;

namespace {

struct CharacterRange {
    unsigned first;
    unsigned last;
};

using CharacterRanges = Vector<CharacterRange, maxRangesCompared + 1>;

struct CharacterTest {
    CharacterRanges ranges;
    bool isFolded { false };
    bool isInverted { false };
    bool usesTable { false };
};

} // anonymous namespace

static CharacterRanges rangesOf(const Latin1Set& set, bool isIn)
{
    CharacterRanges ranges;
    for (unsigned first = set.findBit(0, isIn); first < 256 && ranges.size() <= maxRangesCompared;) {
        unsigned end = set.findBit(first, !isIn);
        ranges.append({ first, end - 1 });
        first = end < 256 ? set.findBit(end, isIn) : 256;
    }
    return ranges;
}

static bool holdsBothCases(const Latin1Set& set)
{
    for (unsigned character = 0; character < 256; ++character) {
        if (set.get(character) != set.get(character ^ caseBit))
            return false;
    }
    return true;
}

static CharacterTest testOf(const Latin1Set& set)
{
    CharacterTest test { .ranges = rangesOf(set, true) };
    if (test.ranges.size() > 1 && holdsBothCases(set)) {
        Latin1Set folded;
        for (unsigned character = 0; character < 256; ++character) {
            if (character & caseBit && set.get(character))
                folded.set(character);
        }
        CharacterRanges ranges = rangesOf(folded, true);
        if (ranges.size() == 1)
            return { .ranges = WTF::move(ranges), .isFolded = true };
    }
    auto isCheap = [](const CharacterRanges& ranges) {
        return ranges.size() <= 1 || (ranges.size() == 2 && ranges[0].first == ranges[0].last && ranges[1].first == ranges[1].last);
    };
    if (isCheap(test.ranges))
        return test;
    CharacterRanges others = rangesOf(set, false);
    if (isCheap(others))
        return { .ranges = WTF::move(others), .isInverted = true };
    return { .usesTable = true };
}

static bool isSubset(const Latin1Set& part, const Latin1Set& whole)
{
    Latin1Set outside = part;
    outside.exclude(whole);
    return outside.isEmpty();
}

static bool isFixed(const RegExpMatchNode& node)
{
    return node.kind == RegExpMatchNode::Kind::Characters && !node.isRun();
}

static bool isLiteral(const RegExpMatchNode& node)
{
    if (!isFixed(node))
        return false;
    size_t count = node.characters.count();
    if (count == 1)
        return true;
    unsigned first = node.characters.findBit(0, true);
    return count == 2 && isASCIIAlpha(first) && node.characters.get(first ^ caseBit);
}

void RegExpMatchEmitter::addSetOfTable(const Latin1Set& set)
{
    if (testOf(set).usesTable && !m_setsOfTable.contains(set))
        m_setsOfTable.append(set);
}

void RegExpMatchEmitter::collectSetsOfTable(std::span<const Node> nodes)
{
    for (const Node& node : nodes) {
        switch (node.kind) {
        case Node::Kind::Characters:
            addSetOfTable(node.characters);
            break;
        case Node::Kind::WordBoundary:
        case Node::Kind::NoWordBoundary:
            addSetOfTable(m_wordCharacters);
            break;
        case Node::Kind::Group:
        case Node::Kind::Lookahead:
        case Node::Kind::NegativeLookahead:
            for (const Sequence& alternative : node.alternatives)
                collectSetsOfTable(alternative.span());
            break;
        case Node::Kind::StartOfSubject:
        case Node::Kind::StartOfLine:
        case Node::Kind::EndOfSubject:
        case Node::Kind::EndOfLine:
            break;
        }
    }
}

RegExpMatchEmitter::RegExpMatchEmitter(CodeGenerator& generator, const RegExpMatchPlan& plan, Pointer characters, Word32 length, Groups groups)
    : PrimitiveEmitter(generator)
    , m_plan(plan)
    , m_keepsGroups(groups == Groups::Kept)
    , m_start(characters.value())
    , m_end(m_out.add(characters.value(), zeroExtended(length).value()))
{
    for (unsigned character = 0; character < 256; ++character) {
        if (isASCIIAlphanumeric(character) || character == '_')
            m_wordCharacters.set(character);
    }
    m_newlines.set('\n');
    m_newlines.set('\r');

    for (const Sequence& alternative : plan.alternatives)
        collectSetsOfTable(alternative.span());
    if (plan.firstCharacters)
        addSetOfTable(*plan.firstCharacters);
    for (size_t first = 0; first < m_setsOfTable.size(); first += planesOfTable) {
        std::array<uint8_t, 256> table { };
        for (size_t plane = 0; plane < planesOfTable && first + plane < m_setsOfTable.size(); ++plane) {
            for (unsigned character = 0; character < table.size(); ++character)
                table[character] |= m_setsOfTable[first + plane].get(character) << plane;
        }
        m_tables.append(m_out.addConstantData(table, ConstantKind::Unlabeled));
    }
}

std::optional<bool> RegExpMatchEmitter::isWordCharacter(const std::optional<Latin1Set>& characters) const
{
    if (!characters || characters->isEmpty())
        return std::nullopt;
    if (isSubset(*characters, m_wordCharacters))
        return true;
    Latin1Set words = *characters;
    words.filter(m_wordCharacters);
    if (words.isEmpty())
        return false;
    return std::nullopt;
}

LValue RegExpMatchEmitter::loadCharacterAt(LValue address, ptrdiff_t offset)
{
    return m_out.load8ZeroExt32(m_out.address(m_heaps.characters8.atAnyIndex(), address, offset));
}

LValue RegExpMatchEmitter::loadCharacter(const State& state, unsigned offset)
{
    return loadCharacterAt(state.base, static_cast<ptrdiff_t>(state.offset) + offset);
}

LValue RegExpMatchEmitter::addressOf(const State& state)
{
    return state.offset ? m_out.add(state.base, static_cast<int64_t>(state.offset)) : state.base;
}

void RegExpMatchEmitter::continueIf(LValue condition, LBasicBlock otherwise)
{
    LBasicBlock continuation = newBlock();
    m_out.branch(condition, normalEdge(continuation), normalEdge(otherwise));
    m_out.appendTo(continuation);
}

Condition RegExpMatchEmitter::isInTable(LValue character, const Latin1Set& set)
{
    size_t index = m_setsOfTable.find(set);
    RELEASE_ASSERT(index != notFound);
    LValue table = m_out.relocatable(SymbolSpace::Constant, m_tables[index / planesOfTable]);
    Word32 planes(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTImage_tables, m_out.add(table, m_out.zeroExtPtr(character)))));
    return hasBits(planes, 1u << (index % planesOfTable));
}

void RegExpMatchEmitter::branchOnCharacter(LValue character, const Latin1Set& set, LBasicBlock isIn, LBasicBlock isOut)
{
    CharacterTest test = testOf(set);
    if (test.usesTable) {
        m_out.branch(isInTable(character, set).value(), normalEdge(isIn), normalEdge(isOut));
        return;
    }
    LBasicBlock isInRanges = test.isInverted ? isOut : isIn;
    LBasicBlock isOutOfRanges = test.isInverted ? isIn : isOut;
    if (test.ranges.isEmpty()) {
        m_out.jump(isOutOfRanges);
        return;
    }
    if (test.isFolded)
        character = m_out.bitOr(character, caseBit);
    for (size_t index = 0; index < test.ranges.size(); ++index) {
        auto [first, last] = test.ranges[index];
        if (!first && last == 255) {
            m_out.jump(isInRanges);
            return;
        }
        LValue isInRange = first == last ? m_out.equal(character, first)
            : !first ? m_out.belowOrEqual(character, last)
            : last == 255 ? m_out.aboveOrEqual(character, first)
            : m_out.belowOrEqual(m_out.sub(character, first), last - first);
        bool isLast = index + 1 == test.ranges.size();
        LBasicBlock next = isLast ? isOutOfRanges : newBlock();
        m_out.branch(isInRange, normalEdge(isInRanges), normalEdge(next));
        if (!isLast)
            m_out.appendTo(next);
    }
}

void RegExpMatchEmitter::requireCharacters(std::span<const Node> sequence, unsigned minimumLengthAfter, State& state, LBasicBlock fails)
{
    if (state.available >= sequence.front().minimum)
        return;
    unsigned length = std::max(minimumLengthOf(sequence) + minimumLengthAfter, sequence.front().minimum);
    continueIf(m_out.aboveOrEqual(m_out.sub(m_end, state.base), static_cast<int64_t>(state.offset) + length), fails);
    state.available = length;
}

void RegExpMatchEmitter::emitLiterals(std::span<const Node> nodes, State& state, LBasicBlock fails)
{
    Vector<uint8_t, 32> characters;
    Vector<uint8_t, 32> caseBits;
    for (const Node& node : nodes) {
        bool hasBothCases = node.characters.count() == 2;
        unsigned character = node.characters.findBit(0, true);
        for (unsigned count = 0; count < node.minimum; ++count) {
            characters.append(static_cast<uint8_t>(hasBothCases ? character | caseBit : character));
            caseBits.append(hasBothCases ? caseBit : 0);
        }
    }
    for (size_t position = 0; position < characters.size();) {
        size_t left = characters.size() - position;
        unsigned width = left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;
        size_t from = position;
        if (left < 8 && left > 4 && characters.size() >= 8) {
            width = 8;
            from = characters.size() - 8;
        } else if (left == 3 && characters.size() >= 4) {
            width = 4;
            from = characters.size() - 4;
        }
        uint64_t expected = 0;
        uint64_t folded = 0;
        for (unsigned byte = 0; byte < width; ++byte) {
            expected |= static_cast<uint64_t>(characters[from + byte]) << (8 * byte);
            folded |= static_cast<uint64_t>(caseBits[from + byte]) << (8 * byte);
        }
        LValue read = loadBytes(m_out, width, m_out.address(m_heaps.characters8.atAnyIndex(), state.base, static_cast<ptrdiff_t>(state.offset + from)));
        if (folded)
            read = m_out.bitOr(read, static_cast<int64_t>(folded));
        continueIf(m_out.equal(read, static_cast<int64_t>(expected)), fails);
        position = from + width;
    }
    state.offset += characters.size();
    state.available -= characters.size();
    state.previous = nodes.back().characters;
}

void RegExpMatchEmitter::emitFixedCharacters(const Node& node, State& state, LBasicBlock fails)
{
    unsigned count = node.minimum;
    if (count > maxCharactersUnrolled) {
        LBasicBlock loop = newBlock();
        LBasicBlock isIn = newBlock();
        LBasicBlock isMatched = newBlock();
        LValue first = addressOf(state);
        LValue limit = m_out.add(first, static_cast<int64_t>(count));
        ValueFromBlock begin = m_out.anchor(first);
        m_out.jump(loop);

        m_out.appendTo(loop);
        LValue position = m_out.phi(Machine::Int64, begin);
        branchOnCharacter(loadCharacterAt(position), node.characters, isIn, fails);

        m_out.appendTo(isIn);
        LValue next = m_out.add(position, 1);
        m_out.addIncomingToPhi(position, m_out.anchor(next));
        m_out.branch(m_out.equal(next, limit), normalEdge(isMatched), normalEdge(loop));
        m_out.appendTo(isMatched);
    } else {
        for (unsigned index = state.isFirstCharacterTested && m_plan.firstCharacters && node.characters == *m_plan.firstCharacters ? 1 : 0; index < count; ++index) {
            LBasicBlock isIn = newBlock();
            branchOnCharacter(loadCharacter(state, index), node.characters, isIn, fails);
            m_out.appendTo(isIn);
        }
    }
    state.offset += count;
    state.available -= count;
    state.previous = node.characters;
}

void RegExpMatchEmitter::emitRun(const Node& node, std::span<const Node> rest, unsigned minimumLengthAfter, State&& state, LBasicBlock fails, const Continuation& next)
{
    LValue first = addressOf(state);
    LValue limit = m_end;
    if (node.maximum != Node::unbounded)
        limit = m_out.select(m_out.below(m_out.sub(m_end, first), static_cast<int64_t>(node.maximum)), m_end, m_out.add(first, static_cast<int64_t>(node.maximum)));

    State behind = WTF::move(state);
    behind.offset = 0;
    behind.available = 0;
    behind.previous = std::nullopt;
    behind.isFirstCharacterTested = false;

    if (!node.isGreedy) {
        LBasicBlock test = newBlock();
        LBasicBlock retries = newBlock();
        LBasicBlock hasMore = newBlock();
        LBasicBlock isIn = newBlock();
        ValueFromBlock begin = m_out.anchor(first);
        m_out.jump(test);

        m_out.appendTo(test);
        LValue position = m_out.phi(Machine::Int64, begin);
        behind.base = position;
        emitSequence(rest, minimumLengthAfter, WTF::move(behind), retries, next);
        if (!retries->numberOfIncomingEdges())
            return;

        m_out.appendTo(retries);
        m_out.branch(m_out.equal(position, limit), normalEdge(fails), normalEdge(hasMore));
        m_out.appendTo(hasMore);
        branchOnCharacter(loadCharacterAt(position), node.characters, isIn, fails);
        m_out.appendTo(isIn);
        m_out.addIncomingToPhi(position, m_out.anchor(m_out.add(position, 1)));
        m_out.jump(test);
        return;
    }

    LBasicBlock loop = newBlock();
    LBasicBlock hasMore = newBlock();
    LBasicBlock isIn = newBlock();
    LBasicBlock isBehind = newBlock();
    ValueFromBlock begin = m_out.anchor(first);
    m_out.jump(loop);

    m_out.appendTo(loop);
    LValue position = m_out.phi(Machine::Int64, begin);
    m_out.branch(m_out.equal(position, limit), normalEdge(isBehind), normalEdge(hasMore));
    m_out.appendTo(hasMore);
    branchOnCharacter(loadCharacterAt(position), node.characters, isIn, isBehind);
    m_out.appendTo(isIn);
    m_out.addIncomingToPhi(position, m_out.anchor(m_out.add(position, 1)));
    m_out.jump(loop);

    m_out.appendTo(isBehind);
    behind.base = position;
    if (!node.givesBack) {
        emitSequence(rest, minimumLengthAfter, WTF::move(behind), fails, next);
        return;
    }

    LBasicBlock test = newBlock();
    LBasicBlock retries = newBlock();
    LBasicBlock hasLess = newBlock();
    ValueFromBlock longest = m_out.anchor(position);
    m_out.jump(test);

    m_out.appendTo(test);
    LValue end = m_out.phi(Machine::Int64, longest);
    behind.base = end;
    emitSequence(rest, minimumLengthAfter, WTF::move(behind), retries, next);
    if (!retries->numberOfIncomingEdges())
        return;

    m_out.appendTo(retries);
    m_out.branch(m_out.equal(end, first), normalEdge(fails), normalEdge(hasLess));
    m_out.appendTo(hasLess);
    m_out.addIncomingToPhi(end, m_out.anchor(m_out.sub(end, 1)));
    m_out.jump(test);
}

void RegExpMatchEmitter::resetInnerGroups(const Node& node, Bounds& bounds)
{
    if (!node.firstInnerGroup || !m_keepsGroups)
        return;
    for (unsigned index = 2 * node.firstInnerGroup; index < 2 * (node.lastInnerGroup + 1); ++index)
        bounds[index] = nullptr;
}

RegExpMatchEmitter::Join RegExpMatchEmitter::createJoin(unsigned firstGroup, unsigned lastGroup)
{
    Join join { .block = newBlock() };
    if (firstGroup && m_keepsGroups) {
        join.firstChangedBound = 2 * firstGroup;
        join.endOfChangedBounds = 2 * (lastGroup + 1);
    }
    return join;
}

void RegExpMatchEmitter::addTo(Join& join, const State& state)
{
    Vector<ValueFromBlock, 8> values;
    values.append(m_out.anchor(addressOf(state)));
    for (unsigned index = join.firstChangedBound; index < join.endOfChangedBounds; ++index)
        values.append(m_out.anchor(state.bounds[index] ? state.bounds[index] : m_out.int64Zero));
    m_out.jump(join.block);
    join.incoming.append(WTF::move(values));
    join.available = std::min(join.available, state.available);
}

RegExpMatchEmitter::State RegExpMatchEmitter::finish(const Join& join, const Bounds& boundsInFront)
{
    RELEASE_ASSERT(!join.incoming.isEmpty());
    m_out.appendTo(join.block);
    auto phiOf = [&](size_t index) {
        LValue phi = m_out.phi(Machine::Int64);
        for (auto& values : join.incoming)
            m_out.addIncomingToPhi(phi, values[index]);
        return phi;
    };
    State joined { .base = phiOf(0), .available = join.available, .bounds = boundsInFront };
    for (unsigned index = join.firstChangedBound; index < join.endOfChangedBounds; ++index)
        joined.bounds[index] = phiOf(index - join.firstChangedBound + 1);
    return joined;
}

void RegExpMatchEmitter::emitAlternatives(std::span<const Sequence> alternatives, bool mayBeLeftOut, unsigned minimumLengthAfter, const State& state, LBasicBlock fails, const Continuation& matches, const Continuation& isLeftOut)
{
    for (size_t index = 0; index < alternatives.size(); ++index) {
        bool isLast = index + 1 == alternatives.size() && !mayBeLeftOut;
        LBasicBlock next = isLast ? fails : newBlock();
        emitSequence(alternatives[index].span(), minimumLengthAfter, State(state), next, matches);
        if (isLast || !next->numberOfIncomingEdges())
            return;
        m_out.appendTo(next);
    }
    State leftOut = state;
    isLeftOut(leftOut, fails);
}

void RegExpMatchEmitter::emitGroup(const Node& node, std::span<const Node> rest, unsigned minimumLengthAfter, State&& state, LBasicBlock fails, const Continuation& next)
{
    const unsigned minimumLengthAfterGroup = minimumLengthOf(rest) + minimumLengthAfter;
    const bool mayBeLeftOut = !node.minimum;
    resetInnerGroups(node, state.bounds);
    const bool isKept = node.group && m_keepsGroups;
    LValue start = isKept ? addressOf(state) : nullptr;
    auto close = [&](State& inner) {
        if (!isKept)
            return;
        inner.bounds[2 * node.group] = start;
        inner.bounds[2 * node.group + 1] = addressOf(inner);
    };

    if (node.commits && (mayBeLeftOut || node.alternatives.size() > 1)) {
        Join join = createJoin(node.firstInnerGroup, node.lastInnerGroup);
        auto matches = [&](State& inner, LBasicBlock) {
            close(inner);
            addTo(join, inner);
        };
        auto isLeftOut = [&](State& inner, LBasicBlock) {
            addTo(join, inner);
        };
        emitAlternatives(node.alternatives.span(), mayBeLeftOut, minimumLengthAfterGroup, state, fails, matches, isLeftOut);
        emitSequence(rest, minimumLengthAfter, finish(join, state.bounds), fails, next);
        return;
    }

    auto matches = [&](State& inner, LBasicBlock innerFails) {
        close(inner);
        emitSequence(rest, minimumLengthAfter, WTF::move(inner), innerFails, next);
    };
    auto isLeftOut = [&](State& inner, LBasicBlock innerFails) {
        emitSequence(rest, minimumLengthAfter, WTF::move(inner), innerFails, next);
    };
    emitAlternatives(node.alternatives.span(), mayBeLeftOut, minimumLengthAfterGroup, state, fails, matches, isLeftOut);
}

void RegExpMatchEmitter::emitGroupLoop(const Node& node, std::span<const Node> rest, unsigned minimumLengthAfter, State&& state, LBasicBlock fails, const Continuation& next)
{
    LBasicBlock loop = newBlock();
    LBasicBlock ends = newBlock();
    resetInnerGroups(node, state.bounds);
    LValue first = addressOf(state);
    Vector<unsigned, 8> changed;
    if (node.firstInnerGroup && m_keepsGroups) {
        for (unsigned index = 2 * node.firstInnerGroup; index < 2 * (node.lastInnerGroup + 1); ++index)
            changed.append(index);
    }
    ValueFromBlock begin = m_out.anchor(first);
    Vector<ValueFromBlock, 8> none;
    for (size_t index = 0; index < changed.size(); ++index)
        none.append(m_out.anchor(m_out.int64Zero));
    m_out.jump(loop);

    m_out.appendTo(loop);
    LValue position = m_out.phi(Machine::Int64, begin);
    State behind = WTF::move(state);
    behind.base = position;
    behind.offset = 0;
    behind.available = 0;
    behind.previous = std::nullopt;
    behind.isFirstCharacterTested = false;
    for (size_t index = 0; index < changed.size(); ++index)
        behind.bounds[changed[index]] = m_out.phi(Machine::Int64, none[index]);

    State iteration = behind;
    resetInnerGroups(node, iteration.bounds);
    auto matches = [&](State& inner, LBasicBlock) {
        LValue end = addressOf(inner);
        if (node.group && m_keepsGroups) {
            inner.bounds[2 * node.group] = position;
            inner.bounds[2 * node.group + 1] = end;
        }
        m_out.addIncomingToPhi(position, m_out.anchor(end));
        for (unsigned index : changed)
            m_out.addIncomingToPhi(behind.bounds[index], m_out.anchor(inner.bounds[index] ? inner.bounds[index] : m_out.int64Zero));
        m_out.jump(loop);
    };
    emitAlternatives(node.alternatives.span(), false, 0, iteration, ends, matches, matches);

    m_out.appendTo(ends);
    if (node.minimum)
        continueIf(m_out.notEqual(position, first), fails);
    emitSequence(rest, minimumLengthAfter, WTF::move(behind), fails, next);
}

void RegExpMatchEmitter::emitLookahead(const Node& node, State& state, LBasicBlock fails)
{
    LBasicBlock continuation = newBlock();
    const bool isNegative = node.kind == Node::Kind::NegativeLookahead;
    auto matches = [&](State&, LBasicBlock) {
        m_out.jump(isNegative ? fails : continuation);
    };
    emitAlternatives(node.alternatives.span(), false, 0, state, isNegative ? continuation : fails, matches, matches);
    m_out.appendTo(continuation);
}

void RegExpMatchEmitter::emitWordBoundary(const Node& node, std::span<const Node> rest, State& state, LBasicBlock fails)
{
    const bool isBoundary = node.kind == Node::Kind::WordBoundary;
    const bool mayBeAtStart = !state.previous && !state.offset;
    std::optional<bool> isPrecededByWordCharacter = isWordCharacter(state.previous);
    if (state.base == m_start && !state.offset)
        isPrecededByWordCharacter = false;
    std::optional<bool> isWordBehind;
    if (!rest.empty() && rest.front().kind == Node::Kind::Characters && rest.front().minimum)
        isWordBehind = isWordCharacter(rest.front().characters);

    LBasicBlock continuation = newBlock();
    auto branchOnPrecedingCharacter = [&](bool isExpected) {
        LBasicBlock isWord = isExpected ? continuation : fails;
        LBasicBlock isNoWord = isExpected ? fails : continuation;
        if (isWordBehind) {
            m_out.jump(*isWordBehind ? isWord : isNoWord);
            return;
        }
        if (!state.available) {
            LBasicBlock hasMore = newBlock();
            m_out.branch(m_out.equal(addressOf(state), m_end), normalEdge(isNoWord), normalEdge(hasMore));
            m_out.appendTo(hasMore);
        }
        branchOnCharacter(loadCharacter(state, 0), m_wordCharacters, isWord, isNoWord);
    };
    if (isPrecededByWordCharacter)
        branchOnPrecedingCharacter(*isPrecededByWordCharacter != isBoundary);
    else {
        LBasicBlock isWord = newBlock();
        LBasicBlock isNoWord = newBlock();
        if (mayBeAtStart) {
            LBasicBlock hasPrevious = newBlock();
            m_out.branch(m_out.equal(state.base, m_start), normalEdge(isNoWord), normalEdge(hasPrevious));
            m_out.appendTo(hasPrevious);
        }
        branchOnCharacter(loadCharacterAt(state.base, static_cast<ptrdiff_t>(state.offset) - 1), m_wordCharacters, isWord, isNoWord);
        m_out.appendTo(isWord);
        branchOnPrecedingCharacter(!isBoundary);
        m_out.appendTo(isNoWord);
        branchOnPrecedingCharacter(isBoundary);
    }
    m_out.appendTo(continuation);
}

void RegExpMatchEmitter::emitSequence(std::span<const Node> nodes, unsigned minimumLengthAfter, State&& state, LBasicBlock fails, const Continuation& next)
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
            if (state.base != m_start || state.offset)
                continueIf(m_out.equal(addressOf(state), m_start), fails);
            break;
        case Node::Kind::StartOfLine: {
            if (state.base == m_start && !state.offset)
                break;
            LBasicBlock isAtStart = newBlock();
            if (!state.previous && !state.offset) {
                LBasicBlock hasPrevious = newBlock();
                m_out.branch(m_out.equal(state.base, m_start), normalEdge(isAtStart), normalEdge(hasPrevious));
                m_out.appendTo(hasPrevious);
            }
            branchOnCharacter(loadCharacterAt(state.base, static_cast<ptrdiff_t>(state.offset) - 1), m_newlines, isAtStart, fails);
            m_out.appendTo(isAtStart);
            break;
        }
        case Node::Kind::EndOfSubject:
            continueIf(m_out.equal(addressOf(state), m_end), fails);
            break;
        case Node::Kind::EndOfLine: {
            LBasicBlock isAtEnd = newBlock();
            LBasicBlock hasMore = newBlock();
            m_out.branch(m_out.equal(addressOf(state), m_end), normalEdge(isAtEnd), normalEdge(hasMore));
            m_out.appendTo(hasMore);
            branchOnCharacter(loadCharacter(state, 0), m_newlines, isAtEnd, fails);
            m_out.appendTo(isAtEnd);
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
        state.isFirstCharacterTested = false;
    }
    next(state, fails);
}

bool RegExpMatchEmitter::isSearched(Begin begin) const
{
    return m_plan.isSearched && begin != Begin::AtPlace;
}

LValue RegExpMatchEmitter::addressAt(Word32 index)
{
    return m_out.add(m_start, zeroExtended(index).value());
}

RegExpMatchEmitter::Bounds RegExpMatchEmitter::match(LBasicBlock misses, Begin where, LValue first)
{
    if (where == Begin::AtStart)
        first = m_start;
    State state { .base = first };
    for (unsigned bound = 0; bound < 2 * ((m_keepsGroups ? m_plan.numberOfGroups : 0) + 1); ++bound)
        state.bounds.append(nullptr);

    LBasicBlock fails = misses;
    LBasicBlock loop { nullptr };
    LValue begin = first;
    LValue last { nullptr };
    if (isSearched(where)) {
        if (m_plan.minimumLength)
            continueIf(m_out.aboveOrEqual(m_out.sub(m_end, first), static_cast<int64_t>(m_plan.minimumLength)), misses);
        if (m_plan.maximumLengthToEnd) {
            int64_t length = *m_plan.maximumLengthToEnd;
            first = m_out.select(m_out.above(m_out.sub(m_end, first), length), m_out.sub(m_end, length), first);
        }
        last = m_plan.minimumLength ? m_out.sub(m_end, static_cast<int64_t>(m_plan.minimumLength)) : m_end;
        loop = newBlock();
        fails = newBlock();
        ValueFromBlock start = m_out.anchor(first);
        m_out.jump(loop);

        m_out.appendTo(loop);
        begin = m_out.phi(Machine::Int64, start);
        if (m_plan.firstCharacters) {
            LBasicBlock tries = newBlock();
            branchOnCharacter(loadCharacterAt(begin), *m_plan.firstCharacters, tries, fails);
            m_out.appendTo(tries);
            state.isFirstCharacterTested = true;
        }
        state.base = begin;
        state.available = m_plan.minimumLength;
    }

    Join found = createJoin(1, m_plan.numberOfGroups);
    auto matches = [&](State& inner, LBasicBlock) {
        addTo(found, inner);
    };
    emitAlternatives(m_plan.alternatives.span(), false, 0, state, fails, matches, matches);

    if (loop && fails->numberOfIncomingEdges()) {
        LBasicBlock advances = newBlock();
        m_out.appendTo(fails);
        m_out.branch(m_out.equal(begin, last), normalEdge(misses), normalEdge(advances));
        m_out.appendTo(advances);
        m_out.addIncomingToPhi(begin, m_out.anchor(m_out.add(begin, 1)));
        m_out.jump(loop);
    }
    State end = finish(found, state.bounds);
    end.bounds[0] = begin;
    end.bounds[1] = end.base;
    return WTF::move(end.bounds);
}

Word32 RegExpMatchEmitter::indexOf(LValue bound)
{
    return Word32(m_out.castToInt32(m_out.sub(bound, m_start)));
}

Word32 RegExpMatchEmitter::indexOfGroupBound(LValue boundOrNull)
{
    if (!boundOrNull)
        return word32(-1);
    if (boundOrNull->opcode() != Machine::Phi)
        return indexOf(boundOrNull);
    return Word32(m_out.select(m_out.isZero64(boundOrNull), m_out.constInt32(-1), indexOf(boundOrNull).value()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
