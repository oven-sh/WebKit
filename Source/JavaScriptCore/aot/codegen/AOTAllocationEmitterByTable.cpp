/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAllocationEmitter.h"

#if ENABLE(AOT)

#include "AOTBranchCounterNumbers.h"
#include "AOTCFG.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPlacement.h"
#include "AOTResultRules.h"
#include "AOTSlotEmitter.h"
#include "AOTStubsAllocation.h"
#include "AOTThreshold.h"
#include <algorithm>
#include <limits>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// In the low half of a row: no case has this value.
static constexpr uint32_t noAllocation = std::numeric_limits<uint16_t>::max();

// The block allocates an empty object in a cell of its own, hands it on, and does nothing else.
ValueID AllocationEmitter::findOnlyAllocationIn(BlockID from, const Target& target)
{
    const Block& block = m_function.at(target.block);
    if (!target.arguments.isEmpty() || !block.parameters.isEmpty() || block.values.size() != 1 || block.terminator.kind != TerminatorKind::Jump)
        return { };
    bool isEnteredFromElsewhere = std::ranges::any_of(generator().analyses().cfg().predecessors(target.block), [&](BlockID predecessor) {
        return predecessor != from;
    });
    if (isEnteredFromElsewhere)
        return { };

    ValueID id = block.values[0];
    const Value& value = m_function.at(id);
    if (value.opcode != Opcode::NewObject || !value.allocation() || value.storage() != AllocationStorage::Heap || !value.fields().empty())
        return { };
    const Target& next = block.terminator.targets[0];
    if (next.arguments.size() != 1 || next.arguments[0] != id)
        return { };
    const PlacedAllocation& allocation = m_placement.allocation(value.allocation());
    LayoutID layout = allocation.layout;
    if (!isObjectLayout(m_program.at(layout).kind) || !m_placement.layout(layout).hasSlots() || m_placement.freeCellOffset(layout))
        return { };
    // The stub that is not told the size of the cell zeroes two chunks at a time from the end.
    if (allocation.size < 2 * copyChunkBytes)
        return { };
    return id;
}

// These cases of the switch differ in the row of the table alone, and all of them go on to one block.
std::optional<AllocationEmitter::AllocationsByCase> AllocationEmitter::findAllocationsByCase(const TerminatorContext& terminator)
{
    // A validating build tests each object against the one layout of its operation, and a counting build counts each edge.
    if (generator().validates() || inputs().branchCounters.isCounting())
        return std::nullopt;
    PayloadListView<int64_t> cases = terminator->cases();

    struct Candidate {
        BlockID block;
        Vector<WordOfEmptyObject, 8> words;
        uint32_t row { 0 };
    };
    Vector<Candidate, 16> candidates;
    Vector<BlockID, 16> seen;
    for (uint32_t index = 0; index < cases.size(); ++index) {
        const Target& target = terminator->targets[index];
        if (seen.contains(target.block))
            continue;
        seen.append(target.block);
        ValueID id = findOnlyAllocationIn(terminator.block(), target);
        if (!id)
            continue;
        const Value& value = m_function.at(id);
        if (value.allocation().index() >= noAllocation)
            continue;
        if (!candidates.isEmpty()) {
            const Block& firstBlock = m_function.at(candidates[0].block);
            const Value& first = m_function.at(firstBlock.values[0]);
            const Target& next = m_function.at(target.block).terminator.targets[0];
            const Target& nextOfFirst = firstBlock.terminator.targets[0];
            if (next.block != nextOfFirst.block || next.proofs != nextOfFirst.proofs || value.prototypeOperand() != first.prototypeOperand())
                continue;
            if (mayAllocateOtherLayout(m_program, value) != mayAllocateOtherLayout(m_program, first) || value.representation != first.representation)
                continue;
            if (m_function.at(target.block).shape != firstBlock.shape)
                continue;
        }
        candidates.append({ target.block, wordsOfEmptyObject(m_placement.allocation(value.allocation()).layout), value.allocation().index() });
    }
    if (candidates.size() < 2)
        return std::nullopt;

    AllocationsByCase result;
    WordsOfEmptyObjects& words = result.words;
    words.shared = candidates[0].words;
    for (const Candidate& candidate : candidates) {
        words.shared.removeAllMatching([&](const WordOfEmptyObject& word) {
            return !candidate.words.contains(word);
        });
    }
    for (Candidate& candidate : candidates) {
        candidate.words.removeAllMatching([&](const WordOfEmptyObject& word) {
            return words.shared.contains(word);
        });
    }

    // The own words are of the kind that the first of them is of, and one of the shared words has to be of that kind too.
    AOT_THRESHOLD(unsigned, maxOwnWordsInRow, 2);
    uint32_t placeStoredTwice = 0;
    auto withOwnWords = std::ranges::find_if(candidates, [](const Candidate& candidate) {
        return !candidate.words.isEmpty();
    });
    if (withOwnWords != candidates.end()) {
        words.representationOfOwnWords = withOwnWords->words[0].representation;
        auto sharedWord = std::ranges::find(words.shared, words.representationOfOwnWords, &WordOfEmptyObject::representation);
        if (sharedWord != words.shared.end())
            placeStoredTwice = sharedWord->offset / widthOf(words.representationOfOwnWords);
        uint32_t width = widthOf(words.representationOfOwnWords);
        candidates.removeAllMatching([&](const Candidate& candidate) {
            if (candidate.words.isEmpty())
                return false;
            return sharedWord == words.shared.end() || candidate.words.size() > maxOwnWordsInRow || std::ranges::any_of(candidate.words, [&](const WordOfEmptyObject& word) {
                return word.representation != words.representationOfOwnWords || word.offset % width || word.offset / width > std::numeric_limits<uint8_t>::max();
            });
        });
        if (candidates.size() < 2)
            return std::nullopt;
        for (const Candidate& candidate : candidates)
            words.maxOwnWords = std::max<unsigned>(words.maxOwnWords, candidate.words.size());
        for (Candidate& candidate : candidates) {
            for (unsigned position = 0; position < words.maxOwnWords; ++position) {
                uint32_t place = position < candidate.words.size() ? candidate.words[position].offset / width : placeStoredTwice;
                candidate.row |= place << (16 + 8 * position);
            }
        }
    }

    int64_t smallest = std::numeric_limits<int64_t>::max();
    int64_t largest = std::numeric_limits<int64_t>::min();
    size_t numberOfCases = 0;
    auto candidateOf = [&](uint32_t index) {
        return std::ranges::find(candidates, terminator->targets[index].block, &Candidate::block);
    };
    for (uint32_t index = 0; index < cases.size(); ++index) {
        if (candidateOf(index) == candidates.end())
            continue;
        smallest = std::min<int64_t>(smallest, cases[index]);
        largest = std::max<int64_t>(largest, cases[index]);
        ++numberOfCases;
    }
    // As dense as a jump table has to be.
    uint64_t length = static_cast<uint64_t>(largest) - static_cast<uint64_t>(smallest) + 1;
    if (length > 4 * static_cast<uint64_t>(numberOfCases))
        return std::nullopt;

    result.first = smallest;
    result.rows.fill(noAllocation, static_cast<size_t>(length));
    for (uint32_t index = 0; index < cases.size(); ++index) {
        if (auto candidate = candidateOf(index); candidate != candidates.end())
            result.rows[static_cast<size_t>(cases[index] - smallest)] = candidate->row;
    }
    for (const Candidate& candidate : candidates)
        result.blocks.append(candidate.block);
    return result;
}

// A table gives the number of the allocation, and the code of the first case runs for all of them. So there is one allocation in the place of one for each layout, and no jump through a table.
Vector<BlockID, 16> AllocationEmitter::allocateByTable(const TerminatorContext& terminator, LValue subject)
{
    std::optional<AllocationsByCase> allocations = findAllocationsByCase(terminator);
    if (!allocations)
        return { };

    bool isWord32 = subject->type() == B3::Int32;
    LValue position = m_out.sub(subject, isWord32 ? m_out.constInt32(static_cast<int32_t>(allocations->first)) : m_out.constInt64(allocations->first));
    LValue length = isWord32 ? m_out.constInt32(static_cast<int32_t>(allocations->rows.size())) : m_out.constInt64(static_cast<int64_t>(allocations->rows.size()));
    LBasicBlock isInTable = newBlock();
    LBasicBlock isListed = newBlock();
    LBasicBlock isNotListed = newBlock();
    branch(Condition(m_out.below(position, length)), { normalEdge(isInTable), normalEdge(isNotListed) });

    m_out.appendTo(isInTable);
    Pointer table(m_out.relocatable(RelocationSpace::Constants, m_out.addConstantData(asBytes(allocations->rows.span()))));
    Integer index(isWord32 ? m_out.zeroExtPtr(position) : position);
    Word32 row(m_out.load32(m_out.address(m_heaps.AOTImage_tables, offsetBy(table, index, 2).value())));
    if (allocations->rows.contains(noAllocation))
        branch(notEqual(row, word32(noAllocation)), { normalEdge(isListed), normalEdge(isNotListed) });
    else
        m_out.jump(isListed);

    m_out.appendTo(isListed);
    m_allocationChosenByTable = { m_function.at(allocations->blocks[0]).values[0], row.value(), WTF::move(allocations->words) };
    generator().generateInPlaceOfBlocks(allocations->blocks.span());
    RELEASE_ASSERT(generator().hasFailed() || !m_allocationChosenByTable.operation);

    m_out.appendTo(isNotListed);
    return WTF::move(allocations->blocks);
}

void AllocationEmitter::generateNewObjectChosenByTable(const ValueContext& value, const AllocationChosenByTable& chosen)
{
    Word32 row(chosen.row);
    Word32 number = bitAnd(row, word32(noAllocation));
    CellPointer cell = emitter<CellAllocationEmitter>().allocateZeroedByNumber(value, number);
    {
        CodeGenerator::InitializationScope scope(generator());
        // Each layout has heaps of its own for its fields.
        const B3::AbstractHeap& anyField = m_heaps.entire(AbstractHeap::ObjectFields);
        SlotEmitter& slots = emitter<SlotEmitter>();
        for (const WordOfEmptyObject& word : chosen.words.shared)
            slots.storeAbsent(m_out.address(anyField, cell.value(), word.offset), word.representation);
        unsigned logWidth = WTF::fastLog2(widthOf(chosen.words.representationOfOwnWords));
        for (unsigned position = 0; position < chosen.words.maxOwnWords; ++position) {
            Word32 place = bitAnd(shiftRightUnsigned(row, word32(16 + 8 * position)), word32(std::numeric_limits<uint8_t>::max()));
            slots.storeAbsent(m_out.address(anyField, offsetBy(asPointer(cell), zeroExtended(place), logWidth).value()), chosen.words.representationOfOwnWords);
        }
    }
    emitter<BarrierEmitter>().fenceAfterInitialization();
    if (ValueID prototype = value->prototypeOperand())
        storePrototype(value, cell, prototype, number);
    else
        define(value, cell);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
