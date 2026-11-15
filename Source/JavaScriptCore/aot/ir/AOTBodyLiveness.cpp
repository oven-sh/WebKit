/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBodyIndex.h"
#include "AOTEffectsDeclared.h"

#if ENABLE(AOT)

#include <algorithm>
#include <bit>
#include <functional>
#include <numeric>
#include <span>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class BodyLivenessSolver {
    WTF_MAKE_NONCOPYABLE(BodyLivenessSolver);
public:
    BodyLivenessSolver(const BodyIndex& index, bool goesAsSuspensionsHold, std::span<const ValueID> readByEverySuspension, BodyLivenessData& data, BodyLivenessScratch& scratch)
        : m_index(index)
        , m_goesAsSuspensionsHold(goesAsSuspensionsHold)
        , m_readByEverySuspension(readByEverySuspension)
        , m_data(data)
        , m_lastUses(data.lastUses)
        , m_liveAtHeads(data.liveAtHeads)
        , m_scratch(scratch)
    {
    }

    void run();

private:
    using Branches = BodyLivenessData::Branches;
    static constexpr uint32_t numberOfBranches = BodyLivenessData::numberOfBranches;

    static constexpr uint64_t spanOf(uint32_t level) { return uint64_t(1) << BodyLivenessData::shiftOf(level); }

    uint32_t addWord(uint64_t);
    uint32_t addBranches(const Branches&);
    uint32_t unite(uint32_t level, uint32_t, uint32_t);
    uint32_t below(uint32_t level, uint32_t set, uint64_t limit);
    uint32_t withValues(uint32_t level, uint32_t set, uint64_t base, std::span<const ValueID> ascending);
    uint32_t splice(uint32_t level, uint32_t low, uint32_t high, uint64_t place);
    uint32_t uniteAndRaiseLastUses(uint32_t level, uint32_t raised, uint32_t set, uint64_t base, uint32_t position);
    BodyLivenessScratch::Union& placeOfUnion(uint32_t first, uint32_t second) { return m_scratch.unions[(first * 0x9E3779B1u ^ second * 0x85EBCA77u) >> m_shiftOfUnions]; }

    void collectFromLastBlockToFirst();
    uint32_t collectLiveAtHead(const BodyBlock&);
    bool arrangeFromFirstBlockToLast();
    void raiseLastUsesToEndsOfLoops();
    void raiseLastUsesAlongRetreatingEdges();
    void raiseLastUsesOfWhatIsLiveAtHeads();

    void raiseLastUse(ValueID value, uint32_t position)
    {
        uint32_t& lastUse = m_lastUses[value.index()];
        lastUse = std::max(lastUse, position);
    }

    void read(ValueID value, uint32_t position)
    {
        raiseLastUse(value, position);
        if (value.index() < m_firstIDOfBlock)
            m_scratch.reads.append(value);
        if (m_goesAsSuspensionsHold) [[unlikely]]
            readWhatValueHolds(value, position);
    }

    void readWhatValueHolds(ValueID value, uint32_t position)
    {
        ValueID definition = m_index.stripCasts(value);
        if (m_index.isParameter(definition))
            return;
        BodyValue made = m_index.value(definition);
        if (made.opcode() == Opcode::IteratorOpen)
            read(made.as<Opcode::IteratorOpen>().iterableOperand(), position);
        else if (made.opcode() == Opcode::NewClosure) {
            for (ValueID capture : made.as<Opcode::NewClosure>().capturesOperands())
                read(capture, position);
        }
    }

    void readArguments(BodyTarget target, uint32_t position)
    {
        for (ValueID argument : target.arguments()) {
            if (argument)
                read(argument, position);
        }
    }

    const BodyIndex& m_index;
    bool m_goesAsSuspensionsHold;
    std::span<const ValueID> m_readByEverySuspension;
    BodyLivenessData& m_data;
    Vector<uint32_t>& m_lastUses;
    Vector<uint32_t>& m_liveAtHeads;
    BodyLivenessScratch& m_scratch;
    uint32_t m_firstIDOfBlock { 0 };
    uint32_t m_shiftOfUnions { 0 };
};

uint32_t BodyLivenessSolver::addWord(uint64_t word)
{
    if (!word)
        return 0;
    m_data.words.append(word);
    return m_data.words.size() - 1;
}

uint32_t BodyLivenessSolver::addBranches(const Branches& branches)
{
    m_data.branches.append(branches);
    return m_data.branches.size() - 1;
}

uint32_t BodyLivenessSolver::unite(uint32_t level, uint32_t first, uint32_t second)
{
    if (first == second || !second)
        return first;
    if (!first)
        return second;
    if (!level) {
        uint64_t word = m_data.words[first] | m_data.words[second];
        if (word == m_data.words[first])
            return first;
        return word == m_data.words[second] ? second : addWord(word);
    }

    if (first > second)
        std::swap(first, second);
    if (const BodyLivenessScratch::Union& remembered = placeOfUnion(first, second); remembered.first == first && remembered.second == second)
        return remembered.result;

    Branches branchesOfFirst = m_data.branches[first];
    Branches branchesOfSecond = m_data.branches[second];
    Branches united;
    bool isFirst = true;
    bool isSecond = true;
    for (uint32_t branch = 0; branch < numberOfBranches; ++branch) {
        uint32_t ofFirst = branchesOfFirst[branch];
        uint32_t ofSecond = branchesOfSecond[branch];
        united[branch] = ofFirst == ofSecond || !ofSecond ? ofFirst : !ofFirst ? ofSecond : unite(level - 1, ofFirst, ofSecond);
        isFirst &= united[branch] == ofFirst;
        isSecond &= united[branch] == ofSecond;
    }
    uint32_t result = isFirst ? first : isSecond ? second : addBranches(united);
    placeOfUnion(first, second) = { first, second, result };
    return result;
}

uint32_t BodyLivenessSolver::uniteAndRaiseLastUses(uint32_t level, uint32_t raised, uint32_t set, uint64_t base, uint32_t position)
{
    if (raised == set || !set)
        return raised;
    if (!level) {
        uint64_t former = m_data.words[raised];
        for (uint64_t word = m_data.words[set] & ~former; word; word &= word - 1)
            raiseLastUse(ValueID(static_cast<uint32_t>(base + std::countr_zero(word))), position);
        uint64_t word = former | m_data.words[set];
        if (word == former)
            return raised;
        return word == m_data.words[set] ? set : addWord(word);
    }

    if (const BodyLivenessScratch::Union& remembered = placeOfUnion(raised, set); remembered.first == raised && remembered.second == set)
        return remembered.result;

    Branches united;
    bool isRaised = true;
    bool isSet = true;
    for (uint32_t branch = 0; branch < numberOfBranches; ++branch) {
        uint32_t ofRaised = m_data.branches[raised][branch];
        uint32_t ofSet = m_data.branches[set][branch];
        united[branch] = uniteAndRaiseLastUses(level - 1, ofRaised, ofSet, base + branch * spanOf(level - 1), position);
        isRaised &= united[branch] == ofRaised;
        isSet &= united[branch] == ofSet;
    }
    uint32_t result = isRaised ? raised : isSet ? set : addBranches(united);
    if (raised)
        placeOfUnion(raised, set) = { raised, set, result };
    return result;
}

uint32_t BodyLivenessSolver::below(uint32_t level, uint32_t set, uint64_t limit)
{
    if (!set || limit >= spanOf(level))
        return set;
    if (!limit)
        return 0;
    if (!level) {
        uint64_t word = m_data.words[set] & ((uint64_t(1) << limit) - 1);
        return word == m_data.words[set] ? set : addWord(word);
    }

    uint64_t spanOfBranch = spanOf(level - 1);
    uint32_t cut = static_cast<uint32_t>(limit >> BodyLivenessData::shiftOf(level - 1));
    Branches former = m_data.branches[set];
    Branches kept { };
    for (uint32_t branch = 0; branch < cut; ++branch)
        kept[branch] = former[branch];
    kept[cut] = below(level - 1, former[cut], limit & (spanOfBranch - 1));
    return kept == former ? set : kept == Branches { } ? 0 : addBranches(kept);
}

uint32_t BodyLivenessSolver::withValues(uint32_t level, uint32_t set, uint64_t base, std::span<const ValueID> ascending)
{
    if (ascending.empty())
        return set;
    if (!level) {
        uint64_t word = m_data.words[set];
        for (ValueID value : ascending)
            word |= uint64_t(1) << (value.index() - base);
        return word == m_data.words[set] ? set : addWord(word);
    }

    uint64_t spanOfBranch = spanOf(level - 1);
    Branches grown = m_data.branches[set];
    bool isSame = true;
    while (!ascending.empty()) {
        uint32_t branch = static_cast<uint32_t>((ascending.front().index() - base) / spanOfBranch);
        uint64_t baseOfBranch = base + branch * spanOfBranch;
        size_t size = std::ranges::lower_bound(ascending, baseOfBranch + spanOfBranch, { }, [](ValueID value) { return static_cast<uint64_t>(value.index()); }) - ascending.begin();
        uint32_t former = grown[branch];
        grown[branch] = withValues(level - 1, former, baseOfBranch, ascending.first(size));
        isSame &= grown[branch] == former;
        ascending = ascending.subspan(size);
    }
    return isSame ? set : addBranches(grown);
}

uint32_t BodyLivenessSolver::splice(uint32_t level, uint32_t low, uint32_t high, uint64_t place)
{
    if (low == high || place >= spanOf(level))
        return low;
    if (!place)
        return high;
    if (!level) {
        uint64_t mask = (uint64_t(1) << place) - 1;
        uint64_t word = (m_data.words[low] & mask) | (m_data.words[high] & ~mask);
        if (word == m_data.words[low])
            return low;
        return word == m_data.words[high] ? high : addWord(word);
    }

    uint64_t spanOfBranch = spanOf(level - 1);
    uint32_t cut = static_cast<uint32_t>(place / spanOfBranch);
    Branches spliced;
    bool isLow = true;
    bool isHigh = true;
    bool isEmpty = true;
    for (uint32_t branch = 0; branch < numberOfBranches; ++branch) {
        uint32_t ofLow = m_data.branches[low][branch];
        uint32_t ofHigh = m_data.branches[high][branch];
        spliced[branch] = branch < cut ? ofLow : branch > cut ? ofHigh : splice(level - 1, ofLow, ofHigh, place % spanOfBranch);
        isLow &= spliced[branch] == ofLow;
        isHigh &= spliced[branch] == ofHigh;
        isEmpty &= !spliced[branch];
    }
    return isLow ? low : isHigh ? high : isEmpty ? 0 : addBranches(spliced);
}

void BodyLivenessSolver::run()
{
    uint32_t numberOfValues = m_index.numberOfValues();
    m_lastUses.resize(numberOfValues);
    std::span<uint32_t> lastUses = m_lastUses.mutableSpan();
    std::iota(lastUses.begin(), lastUses.end(), 0u);
    m_liveAtHeads.fill(0, m_index.numberOfBlocks());
    m_scratch.collected.fill(0, m_index.numberOfBlocks());

    m_data.words.shrink(0);
    m_data.words.append(0);
    m_data.branches.shrink(0);
    m_data.branches.append(Branches { });
    m_data.levels = 0;
    while (spanOf(m_data.levels) < numberOfValues)
        ++m_data.levels;
    if (m_data.levels) {
        uint32_t bits = std::max(6u, WTF::fastLog2(numberOfValues / 64 + 1));
        m_shiftOfUnions = 32 - bits;
        m_scratch.unions.fill({ }, size_t(1) << bits);
    }

    if (!m_index.hasIrreducibleFlow()) {
        collectFromLastBlockToFirst();
        arrangeFromFirstBlockToLast();
        raiseLastUsesToEndsOfLoops();
        return;
    }

    do
        collectFromLastBlockToFirst();
    while (arrangeFromFirstBlockToLast());
    raiseLastUsesAlongRetreatingEdges();
}

void BodyLivenessSolver::collectFromLastBlockToFirst()
{
    for (uint32_t blockIndex = m_index.numberOfBlocks(); blockIndex--;) {
        BlockID id(blockIndex);
        m_scratch.collected[blockIndex] = m_index.isReachable(id) ? collectLiveAtHead(m_index.block(id)) : 0;
    }
}

uint32_t BodyLivenessSolver::collectLiveAtHead(const BodyBlock& block)
{
    m_firstIDOfBlock = block.firstParameter().index();
    m_scratch.reads.shrink(0);
    uint32_t end = block.endOfValues().index();
    uint32_t live = 0;

    block.forEachOutgoingEdge([&](BodyTarget target, bool) {
        BlockID successor = target.block();
        if (successor > block.id())
            live = unite(m_data.levels, live, m_scratch.collected[successor.index()]);
        else if (successor < block.id() && m_index.hasIrreducibleFlow()) {
            live = unite(m_data.levels, live, m_liveAtHeads[successor.index()]);
        }
        readArguments(target, end);
    });
    BodyTerminator terminator = block.terminator();
    if (terminator.isConditionalBranch()) {
        m_index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) {
            read(result.subject, end);
        });
    }
    terminator.forEachOperand([&](ValueID operand, uint32_t) {
        read(operand, end);
    });
    for (BodyValue value : block.values()) {
        value.forEachOperand([&](ValueID operand, uint32_t) {
            read(operand, value.id().index());
        });
        if (!m_readByEverySuspension.empty() && declaredBoundInTable(value.opcode()).maySuspend) [[unlikely]] {
            for (ValueID held : m_readByEverySuspension)
                read(held, value.id().index());
        }
    }
    if (ValueID subject = block.subject())
        read(subject, block.firstValue().index());

    std::ranges::sort(m_scratch.reads);
    return withValues(m_data.levels, below(m_data.levels, live, m_firstIDOfBlock), 0, m_scratch.reads.span());
}

bool BodyLivenessSolver::arrangeFromFirstBlockToLast()
{
    bool hasGrown = false;
    for (uint32_t blockIndex = 0; blockIndex < m_index.numberOfBlocks(); ++blockIndex) {
        BlockID id(blockIndex);
        uint32_t live = m_scratch.collected[blockIndex];

        LoopID loopAround = m_index.loopOf(id);
        if (loopAround && m_index.isLoopHead(id))
            loopAround = m_index.loop(loopAround).parent;
        if (loopAround) {
            BlockID head = m_index.loop(loopAround).head;
            ASSERT(head < id);
            if (!m_index.hasIrreducibleFlow())
                live = splice(m_data.levels, m_liveAtHeads[head.index()], live, m_index.block(head).firstParameter().index());
            else
                live = unite(m_data.levels, m_liveAtHeads[head.index()], live);
        }
        if (m_index.hasIrreducibleFlow()) {
            uint32_t former = m_liveAtHeads[blockIndex];
            live = unite(m_data.levels, former, live);
            hasGrown |= live != former;
        }
        m_liveAtHeads[blockIndex] = live;
    }
    return hasGrown;
}

void BodyLivenessSolver::raiseLastUsesToEndsOfLoops()
{
    m_scratch.raises.shrink(0);
    for (uint32_t loopIndex = 0; loopIndex < m_index.numberOfLoops(); ++loopIndex) {
        const BodyLoop& loop = m_index.loop(LoopID(loopIndex));
        m_scratch.raises.append({ m_index.block(loop.last).endOfValues().index(), loop.head.index() });
    }
    raiseLastUsesOfWhatIsLiveAtHeads();
}

void BodyLivenessSolver::raiseLastUsesAlongRetreatingEdges()
{
    m_scratch.raises.shrink(0);
    for (uint32_t blockIndex = 0; blockIndex < m_index.numberOfBlocks(); ++blockIndex) {
        BlockID id(blockIndex);
        if (!m_index.isReachable(id))
            continue;
        BodyBlock block = m_index.block(id);
        block.forEachOutgoingEdge([&](BodyTarget target, bool) {
            if (target.block() <= id)
                m_scratch.raises.append({ block.endOfValues().index(), target.block().index() });
        });
    }
    raiseLastUsesOfWhatIsLiveAtHeads();
}

void BodyLivenessSolver::raiseLastUsesOfWhatIsLiveAtHeads()
{
    std::ranges::sort(m_scratch.raises, std::ranges::greater());
    m_scratch.unions.fill({ }, m_scratch.unions.size());
    uint32_t raised = 0;
    for (const BodyLivenessScratch::Raise& raise : m_scratch.raises)
        raised = uniteAndRaiseLastUses(m_data.levels, raised, m_liveAtHeads[raise.head], 0, raise.position);
}

} // anonymous namespace

void BodyLiveness::countHeadsWhereLive(std::span<const BlockID> blocks, std::span<uint32_t> numbersByValue) const
{
    struct Place {
        uint32_t set;
        uint32_t base;
    };
    Vector<uint32_t> numbersOfBranches;
    Vector<uint32_t> numbersOfWords;
    numbersOfBranches.fill(0, m_data->branches.size());
    numbersOfWords.fill(0, m_data->words.size());
    Vector<Place> places;
    Vector<Place> placesBelow;
    auto add = [&](uint32_t level, uint32_t set, uint32_t base, uint32_t number, Vector<Place>& list) {
        if (!set)
            return;
        uint32_t& numberOfSet = level ? numbersOfBranches[set] : numbersOfWords[set];
        if (!numberOfSet)
            list.append({ set, base });
        numberOfSet += number;
    };
    for (BlockID block : blocks)
        add(m_data->levels, m_data->liveAtHeads[block.index()], 0, 1, places);
    for (uint32_t level = m_data->levels; level; --level) {
        placesBelow.shrink(0);
        for (Place place : places) {
            for (uint32_t branch = 0; branch < BodyLivenessData::numberOfBranches; ++branch)
                add(level - 1, m_data->branches[place.set][branch], place.base + (branch << BodyLivenessData::shiftOf(level - 1)), numbersOfBranches[place.set], placesBelow);
        }
        std::swap(places, placesBelow);
    }
    for (Place place : places) {
        for (uint64_t word = m_data->words[place.set]; word; word &= word - 1)
            numbersByValue[place.base + std::countr_zero(word)] += numbersOfWords[place.set];
    }
}

void BodyLiveness::compute(const BodyIndex& index)
{
    m_index = &index;
    m_data = index.m_liveness.load();
    if (m_data)
        return;

    BodyLivenessSolver(index, false, { }, m_ownData, m_scratch).run();
    auto* made = new BodyLivenessData { Vector<uint32_t>(m_ownData.lastUses.span()), Vector<uint32_t>(m_ownData.liveAtHeads.span()), Vector<BodyLivenessData::Branches>(m_ownData.branches.span()), Vector<uint64_t>(m_ownData.words.span()), m_ownData.levels };
    if (index.m_liveness.compare_exchange_strong(m_data, made))
        m_data = made;
    else
        delete made;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
