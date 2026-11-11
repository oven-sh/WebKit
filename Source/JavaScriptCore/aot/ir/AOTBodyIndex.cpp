/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBodyIndex.h"

#if ENABLE(AOT)

#include "AOTBodyIndexBuilder.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTOperandKinds.h"
#include "AOTSort.h"
#include "AOTTypeNarrowing.h"
#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t alignmentOfBodyIndexMemory = 8;
static constexpr size_t bytesOfFirstChunkOfBodyIndexMemory = 4 * KB;

std::span<uint8_t> BodyIndexTable::Worker::allocate(size_t numberOfBytes)
{
    size_t roundedBytes = WTF::roundUpToMultipleOf<alignmentOfBodyIndexMemory>(numberOfBytes);
    if (roundedBytes > bytesPerChunk) {
        chunks.append(MallocSpan<uint8_t>::zeroedMalloc(roundedBytes));
        allocatedBytes += roundedBytes;
        std::span<uint8_t> memory = chunks.last().mutableSpan().first(numberOfBytes);
        if (chunks.size() > 1)
            chunks.last().swap(chunks[chunks.size() - 2]);
        else
            usedBytesOfLastChunk = roundedBytes;
        return memory;
    }

    size_t room = chunks.isEmpty() ? 0 : chunks.last().span().size() - usedBytesOfLastChunk;
    if (chunks.isEmpty() || roundedBytes > room) {
        size_t bytesOfChunk = std::max(roundedBytes, std::min(bytesPerChunk, std::max(bytesOfFirstChunkOfBodyIndexMemory, allocatedBytes)));
        chunks.append(MallocSpan<uint8_t>::zeroedMalloc(bytesOfChunk));
        allocatedBytes += bytesOfChunk;
        usedBytesOfLastChunk = 0;
    }
    std::span<uint8_t> memory = chunks.last().mutableSpan().subspan(usedBytesOfLastChunk, numberOfBytes);
    usedBytesOfLastChunk += roundedBytes;
    return memory;
}

BodyIndexTable::BodyIndexTable(uint32_t numberOfFunctions, unsigned numberOfWorkers)
    : m_indexes(FillWith { }, numberOfFunctions, nullptr)
    , m_workers(numberOfWorkers, [](size_t) { return makeUniqueWithoutFastMallocCheck<Worker>(); })
{
}

BodyIndexTable::~BodyIndexTable()
{
    for (const BodyIndex* index : m_indexes) {
        if (index)
            delete index->m_liveness.load(std::memory_order_relaxed);
        if (index)
            delete index->m_keptByWalk.load(std::memory_order_relaxed);
    }
}

const BodyIndex& BodyIndexTable::build(FunctionID id, const Function& function, unsigned worker)
{
    ASSERT(!has(id));
    BodyIndexBuilder builder(function, *m_workers[worker]);
    const BodyIndex& index = builder.run();
    m_indexes[id.index()] = &index;
    return index;
}

void BodyIndexTable::adopt(FunctionID id, const BodyIndex& index)
{
    ASSERT(!has(id));
    m_indexes[id.index()] = &index;
}

Vector<ConstructionStartInModule> BodyIndexTable::takeConstructionStarts()
{
    size_t size = 0;
    for (const std::unique_ptr<Worker>& worker : m_workers)
        size += worker->constructionStarts.size();

    Vector<ConstructionStartInModule> result;
    result.reserveInitialCapacity(size);
    for (const std::unique_ptr<Worker>& worker : m_workers)
        result.appendVector(std::exchange(worker->constructionStarts, { }));
    return result;
}

uint64_t BodyIndexTable::valuesGoneOver() const
{
    uint64_t result = 0;
    for (const std::unique_ptr<Worker>& worker : m_workers)
        result += worker->valuesGoneOver;
    return result;
}

size_t BodyIndexTable::sizeInBytes() const
{
    auto capacityInBytes = [](const auto& vector) {
        return vector.capacity() * sizeof(typename std::remove_cvref_t<decltype(vector)>::ValueType);
    };

    size_t result = capacityInBytes(m_indexes) + capacityInBytes(m_workers);
    for (const BodyIndex* index : m_indexes) {
        if (const BodyLivenessData* liveness = index ? index->m_liveness.load(std::memory_order_acquire) : nullptr)
            result += sizeof(BodyLivenessData) + capacityInBytes(liveness->lastUses) + capacityInBytes(liveness->liveAtHeads) + capacityInBytes(liveness->branches) + capacityInBytes(liveness->words);
    }
    for (const std::unique_ptr<Worker>& worker : m_workers) {
        result += sizeof(Worker) + worker->allocatedBytes;
        result += capacityInBytes(worker->chunks) + capacityInBytes(worker->constructionStarts);
        result += capacityInBytes(worker->edges) + capacityInBytes(worker->retreatingEdges);
        result += capacityInBytes(worker->firstPredecessor) + capacityInBytes(worker->predecessors);
        result += capacityInBytes(worker->sizesOfSubtrees) + capacityInBytes(worker->nextNumbersBelow);
        result += capacityInBytes(worker->worklist) + capacityInBytes(worker->loops) + capacityInBytes(worker->mentionsOfIterations) + capacityInBytes(worker->blocks);
    }
    return result;
}

template<typename T>
static size_t bytesOfArrayInBodyIndex(size_t size)
{
    return WTF::roundUpToMultipleOf<alignmentOfBodyIndexMemory>(size * sizeof(T));
}

template<typename T>
static std::span<T> takeArrayOfBodyIndex(std::span<uint8_t>& memory, size_t size)
{
    static_assert(alignof(T) <= alignmentOfBodyIndexMemory && std::is_trivially_destructible_v<T>);
    T* first = reinterpret_cast<T*>(memory.data());
    memory = memory.subspan(bytesOfArrayInBodyIndex<T>(size));
    return unsafeMakeSpan(first, size);
}

namespace {

struct TestBehindBodyProofs {
    Proof proof { Proof::None };
    BodyValue value;
};

} // anonymous namespace

static TestBehindBodyProofs findTestBehindBodyProofs(const BodyIndex& index, const BodyTerminator& terminator)
{
    if (!terminator.isConditionalBranch())
        return { provesOf(terminator.kind()), { } };
    BodyValue test = index.value(terminator.as<TerminatorKind::Branch>().conditionOperand());
    return { provesOf(test.opcode()), test };
}

static ValueID subjectOfBodyProof(const TestBehindBodyProofs& test, const BodyTerminator& terminator, uint32_t operandIndex)
{
    if (test.proof == Proof::None)
        return { };
    return test.value ? test.value.operand(operandIndex) : terminator.operand(operandIndex);
}

static bool takesBodyProofs(const BodyTerminator& terminator, uint32_t targetIndex)
{
    TerminatorKind kind = terminator.kind();
    if (std::optional<uint32_t> numberOfFixedTargets = numberOfFixedTargetsOf(kind))
        return targetIndex < terminator.numberOfTargets() && targetIndex < *numberOfFixedTargets && targetsTakingProofsOf(kind) >> targetIndex & 1;
    const TargetInfo* info = infoOfTarget(kind, targetIndex, terminator.numberOfTargets());
    return info && info->takesProofs;
}

BodyIndexBuilder::BodyIndexBuilder(const Function& function, BodyIndexTable::Worker& worker)
    : m_function(function)
    , m_worker(worker)
{
    RELEASE_ASSERT(function.hasBody());
}

BodyIndexBuilder::BodyIndexBuilder(const Function& function, BodyIndexTable& table, unsigned worker)
    : BodyIndexBuilder(function, *table.m_workers[worker])
{
}

const BodyIndex& BodyIndexBuilder::run()
{
    begin();
    indexWords();
    return finish();
}

const BodyIndex& BodyIndexBuilder::finish()
{
    m_index->m_opcodes = m_opcodes;
    adoptMentionsOfIterations();
    computeDominators();
    numberDominatorTree();
    m_index->m_numberOfReachableBlocks = m_blocks[entryBlock.index()].lastDominatorNumberBelow + 1;
    findLoops();
    adoptLoops();
    adoptBlocks();
    return *m_index;
}

void BodyIndexBuilder::adoptBlocks()
{
    if (!m_hasNarrowNumbers) {
        std::span<uint8_t> memory = m_worker.allocate(bytesOfArrayInBodyIndex<BlockPlace>(m_blocks.size()));
        std::span<BlockPlace> blocks = takeArrayOfBodyIndex<BlockPlace>(memory, m_blocks.size());
        std::ranges::copy(m_blocks, blocks.begin());
        m_index->m_wideBlocks = blocks.data();
        return;
    }
    using NarrowBlockPlace = BodyIndex::NarrowBlockPlace;
    std::span<uint8_t> memory = m_worker.allocate(bytesOfArrayInBodyIndex<NarrowBlockPlace>(m_blocks.size()));
    std::span<NarrowBlockPlace> blocks = takeArrayOfBodyIndex<NarrowBlockPlace>(memory, m_blocks.size());
    for (size_t block = 0; block < m_blocks.size(); ++block)
        new (NotNull, &blocks[block]) NarrowBlockPlace(m_blocks[block]);
    m_index->m_narrowBlocks = blocks.data();
    m_index->m_hasNarrowBlocks = true;
}

BodyIndex::RareParts& BodyIndexBuilder::ensureRareParts()
{
    if (!m_rareParts) {
        std::span<uint8_t> memory = m_worker.allocate(bytesOfArrayInBodyIndex<BodyIndex::RareParts>(1));
        m_rareParts = new (NotNull, takeArrayOfBodyIndex<BodyIndex::RareParts>(memory, 1).data()) BodyIndex::RareParts;
        m_index->m_rareParts = m_rareParts;
    }
    return *m_rareParts;
}

void BodyIndexBuilder::begin()
{
    const Body& body = m_function.body;
    size_t numberOfBlocks = body.numberOfBlocks();
    size_t numberOfValues = body.numberOfValues();
    RELEASE_ASSERT(body.words().size() < BodyIndex::isParameterBit);

    m_hasNarrowNumbers = BodyIndex::fitsNarrowNumbers(body);
    std::span<uint8_t> memory = m_worker.allocate(bytesOfArrayInBodyIndex<BodyIndex>(1)
        + bytesOfArrayInBodyIndex<BlockID>(numberOfBlocks)
        + bytesOfArrayInBodyIndex<uint32_t>(numberOfValues)
        + (m_hasNarrowNumbers ? bytesOfArrayInBodyIndex<uint16_t>(numberOfValues) : bytesOfArrayInBodyIndex<uint32_t>(numberOfValues))
        + bytesOfArrayInBodyIndex<uint8_t>(numberOfValues));

    m_index = new (NotNull, takeArrayOfBodyIndex<BodyIndex>(memory, 1).data()) BodyIndex;
    m_blocksInDominatorOrder = takeArrayOfBodyIndex<BlockID>(memory, numberOfBlocks);
    m_placesOfValues = takeArrayOfBodyIndex<uint32_t>(memory, numberOfValues);
    if (m_hasNarrowNumbers)
        m_narrowOnlyUsersOfValues = takeArrayOfBodyIndex<uint16_t>(memory, numberOfValues);
    else
        m_onlyUsersOfValues = takeArrayOfBodyIndex<uint32_t>(memory, numberOfValues);
    m_usesOfValues = takeArrayOfBodyIndex<uint8_t>(memory, numberOfValues);

    m_worker.blocks.shrink(0);
    m_worker.blocks.grow(numberOfBlocks);
    m_blocks = m_worker.blocks.mutableSpan();

    m_index->m_body = body;
    m_index->m_wideBlocks = m_blocks.data();
    m_index->m_blocksInDominatorOrder = m_blocksInDominatorOrder.data();
    m_index->m_placesOfValues = m_placesOfValues.data();
    if (m_hasNarrowNumbers)
        m_index->m_narrowOnlyUsersOfValues = m_narrowOnlyUsersOfValues.data();
    else
        m_index->m_onlyUsersOfValues = m_onlyUsersOfValues.data();
    m_index->m_hasNarrowUsers = m_hasNarrowNumbers;
    m_index->m_usesOfValues = m_usesOfValues.data();

    m_worker.edges.shrink(0);
    m_worker.retreatingEdges.shrink(0);
    m_worker.firstPredecessor.shrink(0);
    m_worker.predecessors.shrink(0);
    m_worker.sizesOfSubtrees.shrink(0);
    m_worker.nextNumbersBelow.shrink(0);
    m_worker.worklist.shrink(0);
    m_worker.loops.shrink(0);
    m_worker.mentionsOfIterations.shrink(0);
}

void BodyIndexBuilder::fileEdge(BlockID from, BodyTarget target, uint32_t targetIndex, bool isHandlerEdge)
{
    BodyUseKind kind = isHandlerEdge ? BodyUseKind::HandlerArgument : BodyUseKind::EdgeArgument;
    BlockID to = target.block();
    for (ValueID argument : target.arguments()) {
        if (argument)
            fileUse(argument, kind, from.index());
    }

    m_worker.edges.append(BodyFlowEdge { from, to });
    if (to <= from)
        m_worker.retreatingEdges.append(BodyFlowEdge { from, to });

    BlockPlace& place = m_blocks[to.index()];
    if (!place.numberOfIncomingEdges) {
        place.indexOfFirstIncomingTarget = targetIndex;
        place.wordOffsetOfFirstIncomingTarget = static_cast<uint32_t>(target.end() - target.numberOfIncomingValues() - BodyTarget::numberOfHeadWords - m_function.body.words().data());
    }
    if (place.numberOfIncomingEdges < BodyIndex::maxCountedIncomingEdges)
        ++place.numberOfIncomingEdges;
    if (isHandlerEdge)
        place.flags |= BodyIndex::IsEnteredByHandlerEdge;
}

void BodyIndexBuilder::fileProofs(BlockID from, const BodyTerminator& terminator)
{
    TestBehindBodyProofs test = findTestBehindBodyProofs(*m_index, terminator);
    if (test.proof == Proof::None)
        return;

    uint32_t targetIndex = 0;
    for (BodyTarget target : terminator.targets()) {
        std::span<const uint32_t> proofs = target.proofs();
        if (!proofs.empty() && takesBodyProofs(terminator, targetIndex)) {
            for (uint32_t operandIndex : proofs) {
                if (ValueID subject = subjectOfBodyProof(test, terminator, operandIndex))
                    fileUse(subject, BodyUseKind::EdgeProof, from.index());
            }
        }
        ++targetIndex;
    }
}

void BodyIndexBuilder::fileWhatFewOperationsBring(const BodyValue& value, BlockID block)
{
    const Body& body = m_function.body;
    auto isIteratorOpen = [&](ValueID named) {
        uint32_t place = m_placesOfValues[named.index()];
        return place && !(place & BodyIndex::isParameterBit) && body.operationAt(place, named).opcode() == Opcode::IteratorOpen;
    };

    if (opcodesThatMayNameIterationsInBodyIndex.contains(value.opcode())) {
        value.forEachOperand([&](ValueID operand, uint32_t) {
            if (isIteratorOpen(operand))
                m_worker.mentionsOfIterations.append({ operand, value.id(), block });
        });
    }
    if (value.words()[0] & Wire::WrittenPositionFollows) {
        std::optional<ConstructionStart> start = value.constructionStart();
        if (start && m_function.module)
            m_worker.constructionStarts.append(ConstructionStartInModule { m_function.module, *start });
    }
}

void BodyIndexBuilder::indexWords()
{
    const Body& body = m_function.body;
    const UnitIDRemap* unit = &body.unit();
    const uint32_t* firstWord = body.words().data();
    uint32_t numberOfBlocks = body.numberOfBlocks();
    uint32_t numberOfValues = body.numberOfValues();

    auto offsetOf = [&](const uint32_t* word) {
        return static_cast<uint32_t>(word - firstWord);
    };

    BodyBlock block = body.entry();
    for (uint32_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        BlockID id(blockIndex);
        uint32_t firstParameter = block.firstParameter().index();
        RELEASE_ASSERT(static_cast<uint64_t>(firstParameter) + block.numberOfParameters() + block.numberOfValues() <= numberOfValues);
        fileBlock(id, offsetOf(block.words()), firstParameter, block.numberOfParameters(), block.numberOfValues());
        if (ValueID subject = block.subject())
            fileUse(subject, BodyUseKind::BlockSubject, blockIndex);

        BodyValues::Iterator cursor = block.values().begin();
        for (uint32_t remaining = block.numberOfValues(); remaining--; ++cursor) {
            BodyValue value = *cursor;
            uint32_t user = value.id().index();
            fileOperation(value.id(), offsetOf(value.words()), value.opcode());
            value.forEachOperand([&](ValueID operand, uint32_t) {
                fileUse(operand, BodyUseKind::Operand, user);
            });
            if (opcodesThatMayNameIterationsInBodyIndex.contains(value.opcode()) || value.words()[0] & Wire::WrittenPositionFollows) [[unlikely]]
                fileWhatFewOperationsBring(value, id);
        }

        BodyTerminator terminator(cursor.words(), unit, id);
        fileTerminator(id, offsetOf(terminator.words()));
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            fileUse(operand, BodyUseKind::TerminatorOperand, blockIndex);
        });
        uint32_t targetIndex = 0;
        bool someTargetBringsProofs = false;
        for (BodyTarget target : terminator.targets()) {
            fileEdge(id, target, targetIndex++, false);
            someTargetBringsProofs |= !target.proofs().empty();
        }
        if (block.hasHandler())
            fileEdge(id, block.handler(), 0, true);
        if (someTargetBringsProofs)
            fileProofs(id, terminator);

        block = BodyBlock(terminator.end(), nullptr, unit, BlockID(blockIndex + 1), block.endOfValues());
    }

    RELEASE_ASSERT(block.words() == firstWord + body.words().size());
    RELEASE_ASSERT(block.firstParameter().index() == numberOfValues);
}

void BodyIndexBuilder::adoptMentionsOfIterations()
{
    std::span<BodyMentionOfIteration> found = m_worker.mentionsOfIterations.mutableSpan();
    if (found.empty())
        return;

    AOT::stableSort(found, { }, [](const BodyMentionOfIteration& mention) {
        return mention.open.index();
    });
    std::span<uint8_t> memory = m_worker.allocate(bytesOfArrayInBodyIndex<BodyMentionOfIteration>(found.size()));
    std::span<BodyMentionOfIteration> mentions = takeArrayOfBodyIndex<BodyMentionOfIteration>(memory, found.size());
    std::ranges::copy(found, mentions.begin());
    ensureRareParts().mentionsOfIterations = mentions;
}

void BodyIndexBuilder::adoptLoops()
{
    std::span<BodyLoop> found = m_worker.loops.mutableSpan();
    if (found.empty())
        return;

    auto forEachLoopAround = [&](const BlockPlace& place, const auto& functor) {
        for (LoopID holder = place.loop; holder; holder = found[holder.index()].parent)
            functor(found[holder.index()]);
    };
    for (const BlockPlace& place : m_blocks) {
        if (place.loop)
            ++found[place.loop.index()].numberOfMembers;
    }
    for (size_t place = found.size(); place--;) {
        if (LoopID parent = found[place].parent)
            found[parent.index()].numberOfMembers += found[place].numberOfMembers;
    }
    uint32_t numberOfBlocksInLoops = 0;
    Vector<uint32_t, 16> nextMember;
    nextMember.fill(0, found.size());
    Vector<uint32_t, 16> outermostAround;
    outermostAround.fill(0, found.size());
    for (size_t place = 0; place < found.size(); ++place) {
        if (LoopID parent = found[place].parent) {
            outermostAround[place] = outermostAround[parent.index()];
            found[place].outermost = LoopID(outermostAround[place]);
            continue;
        }
        outermostAround[place] = place;
        found[place].outermost = LoopID(place);
        nextMember[place] = numberOfBlocksInLoops;
        numberOfBlocksInLoops += found[place].numberOfMembers;
    }
    Vector<uint32_t, 64> placeInList;
    placeInList.fill(0, m_blocks.size());
    for (uint32_t block = 0; block < m_blocks.size(); ++block) {
        if (LoopID loop = m_blocks[block].loop)
            placeInList[block] = nextMember[outermostAround[loop.index()]]++;
    }
    bool membersStandInRows = std::ranges::all_of(found, [&](const BodyLoop& loop) {
        return placeInList[loop.last.index()] - placeInList[loop.head.index()] + 1 == loop.numberOfMembers;
    });

    uint32_t numberOfMembers = 0;
    if (membersStandInRows) {
        numberOfMembers = numberOfBlocksInLoops;
        for (BodyLoop& loop : found)
            loop.firstMember = placeInList[loop.head.index()];
    } else {
        for (BodyLoop& loop : found) {
            loop.firstMember = numberOfMembers;
            numberOfMembers += std::exchange(loop.numberOfMembers, 0);
        }
    }
    for (size_t place = found.size(); place--;) {
        if (LoopID parent = found[place].parent)
            found[parent.index()].numberOfLoopsWithin += found[place].numberOfLoopsWithin;
    }
    uint32_t nextPlaceOutsideLoops = 0;
    Vector<uint32_t, 16> nextPlaceInside;
    nextPlaceInside.fill(0, found.size());
    for (size_t place = 0; place < found.size(); ++place) {
        BodyLoop& loop = found[place];
        uint32_t& next = loop.parent ? nextPlaceInside[loop.parent.index()] : nextPlaceOutsideLoops;
        loop.placeAmongLoops = next;
        next += loop.numberOfLoopsWithin;
        nextPlaceInside[place] = loop.placeAmongLoops + 1;
    }

    std::span<uint8_t> memory = m_worker.allocate(bytesOfArrayInBodyIndex<BodyLoop>(found.size()) + bytesOfArrayInBodyIndex<BlockID>(numberOfMembers));
    std::span<BodyLoop> loops = takeArrayOfBodyIndex<BodyLoop>(memory, found.size());
    std::span<BlockID> members = takeArrayOfBodyIndex<BlockID>(memory, numberOfMembers);
    for (uint32_t block = 0; block < m_blocks.size(); ++block) {
        if (membersStandInRows) {
            if (m_blocks[block].loop)
                members[placeInList[block]] = BlockID(block);
            continue;
        }
        forEachLoopAround(m_blocks[block], [&](BodyLoop& loop) {
            members[loop.firstMember + loop.numberOfMembers++] = BlockID(block);
        });
    }
    std::ranges::copy(found, loops.begin());
    BodyIndex::RareParts& rareParts = ensureRareParts();
    rareParts.loops = loops;
    rareParts.membersOfLoops = members;
}

ValueID BodyIndex::stripCastsSlow(ValueID id) const
{
    while (!isParameter(id)) {
        BodyValue definition = value(id);
        if (!opcodesThatStateTypes.contains(definition.opcode()) || !definition.operand(0))
            break;
        id = definition.operand(0);
    }
    return id;
}

std::optional<BodyEdgeResult> BodyIndex::edgeResult(BlockID from, uint32_t targetIndex, uint32_t proofIndex) const
{
    BodyTerminator terminator = this->terminator(from);
    if (targetIndex >= terminator.numberOfTargets())
        return std::nullopt;
    return edgeResult(terminator, targetIndex, terminator.targets()[targetIndex], proofIndex);
}

std::optional<BodyEdgeResult> BodyIndex::edgeResultOfAnyTerminator(const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target, uint32_t proofIndex) const
{
    if (!takesBodyProofs(terminator, targetIndex))
        return std::nullopt;

    std::span<const uint32_t> proofs = target.proofs();
    if (proofIndex >= proofs.size())
        return std::nullopt;

    TestBehindBodyProofs test = findTestBehindBodyProofs(*this, terminator);
    uint32_t operandIndex = proofs[proofIndex];
    ValueID subject = subjectOfBodyProof(test, terminator, operandIndex);
    if (!subject)
        return std::nullopt;

    BodyEdgeResult result;
    result.proof = test.proof;
    result.subject = subject;
    result.operandIndex = operandIndex;
    if (test.value) {
        result.test = test.value.id();
        result.isOnTakenEdge = !targetIndex;
    } else
        result.caseIndex = targetIndex;
    result.parameter = ValueID(firstParameterOf(target.block()) + static_cast<uint32_t>(target.arguments().size()) + proofIndex);
    return result;
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
