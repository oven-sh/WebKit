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
        // usedBytesOfLastChunk describes the last chunk, so the chunk that still has room stays last.
        if (chunks.size() > 1)
            chunks.last().swap(chunks[chunks.size() - 2]);
        else
            usedBytesOfLastChunk = roundedBytes;
        return memory;
    }

    size_t room = chunks.isEmpty() ? 0 : chunks.last().span().size() - usedBytesOfLastChunk;
    if (chunks.isEmpty() || roundedBytes > room) {
        // The verifier makes a table for each function, so a table that holds one index stays small. A chunk is as large as all in front of it together, up to bytesPerChunk.
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
        result += capacityInBytes(worker->edges) + capacityInBytes(worker->retreatingEdges) + capacityInBytes(worker->argumentLinks);
        result += capacityInBytes(worker->firstPredecessor) + capacityInBytes(worker->predecessors);
        result += capacityInBytes(worker->sizesOfSubtrees) + capacityInBytes(worker->nextNumbersBelow);
        result += capacityInBytes(worker->worklist) + capacityInBytes(worker->loops) + capacityInBytes(worker->releaseHints) + capacityInBytes(worker->mentionsOfIterations);
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
    // Null where the terminator itself is the test.
    BodyValue value;
};

} // anonymous namespace

// The index may still be under construction: it only has to know the place of the value that the Branch tests, which precedes the Branch in the stream.
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

// The rows with an operand of the kind IteratorState or with `held`, and ReleaseHint.
static constexpr OpcodeSet opcodesThatMayNameIterations {
    Opcode::IteratorResume, Opcode::IteratorClose, Opcode::Await, Opcode::Yield, Opcode::InitialYield, Opcode::StepAsyncGenerator, Opcode::AwaitBySavingFrame, Opcode::AwaitCallBySavingFrame,
    Opcode::YieldBySavingFrame, Opcode::AsyncGeneratorYield, Opcode::ReleaseHint
};

static bool takesBodyProofs(const BodyTerminator& terminator, uint32_t targetIndex)
{
    const TargetInfo* info = infoOfTarget(terminator.kind(), targetIndex, terminator.numberOfTargets());
    return info && info->takesProofs;
}

BodyIndexBuilder::BodyIndexBuilder(const Function& function, BodyIndexTable::Worker& worker)
    : m_function(function)
    , m_worker(worker)
{
    RELEASE_ASSERT(function.hasBody());
}

const BodyIndex& BodyIndexBuilder::run()
{
    allocate();
    indexWords();
    adoptMentionsOfIterations();
    findParametersReadByReleaseHintsAlone();
    computeDominators();
    numberDominatorTree();
    // The entry has the number zero and dominates every block that is reached.
    m_index->m_blocksInDominatorOrder = m_blocksInDominatorOrder.first(m_blocks[entryBlock.index()].lastDominatorNumberBelow + 1);
    findLoops();
    adoptLoops();
    return *m_index;
}

void BodyIndexBuilder::allocate()
{
    const Body& body = m_function.body;
    size_t numberOfBlocks = body.numberOfBlocks();
    size_t numberOfValues = body.numberOfValues();
    RELEASE_ASSERT(body.words().size() < BodyIndex::isParameterBit);

    std::span<uint8_t> memory = m_worker.allocate(bytesOfArrayInBodyIndex<BodyIndex>(1)
        + bytesOfArrayInBodyIndex<BlockPlace>(numberOfBlocks)
        + bytesOfArrayInBodyIndex<BlockID>(numberOfBlocks)
        + 2 * bytesOfArrayInBodyIndex<uint32_t>(numberOfValues)
        + bytesOfArrayInBodyIndex<uint8_t>(numberOfValues));

    m_index = new (NotNull, takeArrayOfBodyIndex<BodyIndex>(memory, 1).data()) BodyIndex;
    m_blocks = takeArrayOfBodyIndex<BlockPlace>(memory, numberOfBlocks);
    m_blocksInDominatorOrder = takeArrayOfBodyIndex<BlockID>(memory, numberOfBlocks);
    m_placesOfValues = takeArrayOfBodyIndex<uint32_t>(memory, numberOfValues);
    m_onlyUsersOfValues = takeArrayOfBodyIndex<uint32_t>(memory, numberOfValues);
    m_usesOfValues = takeArrayOfBodyIndex<uint8_t>(memory, numberOfValues);

    // A null id is not zero.
    std::uninitialized_default_construct(m_blocks.begin(), m_blocks.end());

    m_index->m_body = body;
    m_index->m_blocks = m_blocks;
    m_index->m_placesOfValues = m_placesOfValues;
    m_index->m_onlyUsersOfValues = m_onlyUsersOfValues;
    m_index->m_usesOfValues = m_usesOfValues;

    m_worker.edges.shrink(0);
    m_worker.retreatingEdges.shrink(0);
    m_worker.argumentLinks.shrink(0);
    m_worker.firstPredecessor.shrink(0);
    m_worker.predecessors.shrink(0);
    m_worker.sizesOfSubtrees.shrink(0);
    m_worker.nextNumbersBelow.shrink(0);
    m_worker.worklist.shrink(0);
    m_worker.loops.shrink(0);
    m_worker.releaseHints.shrink(0);
    m_worker.mentionsOfIterations.shrink(0);
}

ALWAYS_INLINE void BodyIndexBuilder::noteUse(ValueID used, BodyUseKind kind, uint32_t user)
{
    static_assert(static_cast<unsigned>(BodyUseKind::BlockSubject) < (1u << (8 - BodyIndex::bitsOfUseCount)));

    uint8_t& uses = m_usesOfValues[used.index()];
    uint32_t count = uses & BodyIndex::maxCountedUses;
    if (!count) {
        uses = static_cast<uint8_t>(static_cast<unsigned>(kind) << BodyIndex::bitsOfUseCount | 1);
        m_onlyUsersOfValues[used.index()] = user;
        return;
    }
    if (count < BodyIndex::maxCountedUses)
        ++uses;
}

// A ReleaseHint reads nothing, and an edge reads what it hands to a parameter for the sake of whoever reads the parameter.
// From the values with a mention of another kind, through the parameters among them, to what edges hand to those. Each link is followed once.
void BodyIndexBuilder::findParametersReadByReleaseHintsAlone()
{
    std::span<const BodyArgumentLink> links = m_worker.argumentLinks.span();
    if (m_worker.releaseHints.isEmpty() || links.empty() || !m_function.isCoroutine())
        return;

    // Nothing: the target has no such parameter, so nobody can say who reads the value.
    auto parameterFilledBy = [&](const BodyArgumentLink& link) -> std::optional<uint32_t> {
        if (link.indexOfParameter >= m_index->block(link.to).numberOfParameters())
            return std::nullopt;
        return m_blocks[link.to.index()].firstParameter + link.indexOfParameter;
    };

    uint32_t numberOfValues = m_function.body.numberOfValues();
    // By value.
    Vector<uint32_t> numberOfMentionsByHintsAndEdges;
    numberOfMentionsByHintsAndEdges.fill(0, numberOfValues);
    // By parameter, and one more. The count of each becomes the end of its list. Filling the lists from their ends turns each end into the start.
    Vector<uint32_t> firstArgument;
    firstArgument.fill(0, numberOfValues + 1);
    for (ValueID hint : m_worker.releaseHints) {
        if (ValueID released = m_index->value(hint).as<Opcode::ReleaseHint>().valueOperand())
            ++numberOfMentionsByHintsAndEdges[released.index()];
    }
    for (const BodyArgumentLink& link : links) {
        std::optional<uint32_t> parameter = parameterFilledBy(link);
        if (!parameter)
            continue;
        ++numberOfMentionsByHintsAndEdges[link.argument.index()];
        ++firstArgument[*parameter];
    }
    for (uint32_t index = 1; index <= numberOfValues; ++index)
        firstArgument[index] += firstArgument[index - 1];
    Vector<ValueID> arguments;
    arguments.grow(firstArgument.last());
    for (const BodyArgumentLink& link : links) {
        if (std::optional<uint32_t> parameter = parameterFilledBy(link))
            arguments[--firstArgument[*parameter]] = link.argument;
    }

    auto takesArguments = [&](uint32_t value) {
        return firstArgument[value] != firstArgument[value + 1];
    };
    Vector<bool> isRead;
    isRead.fill(false, numberOfValues);
    Vector<uint32_t, 32> parametersToFollow;
    auto noteRead = [&](uint32_t value) {
        if (!std::exchange(isRead[value], true) && takesArguments(value))
            parametersToFollow.append(value);
    };
    for (uint32_t value = 0; value < numberOfValues; ++value) {
        uint32_t numberOfMentions = m_index->useCount(ValueID(value));
        if (numberOfMentions == BodyIndex::maxCountedUses || numberOfMentions > numberOfMentionsByHintsAndEdges[value])
            noteRead(value);
    }
    while (!parametersToFollow.isEmpty()) {
        uint32_t parameter = parametersToFollow.takeLast();
        for (uint32_t place = firstArgument[parameter]; place < firstArgument[parameter + 1]; ++place)
            noteRead(arguments[place].index());
    }

    std::span<uint32_t> parametersThatNothingReads;
    for (uint32_t value = 0; value < numberOfValues; ++value) {
        if (isRead[value] || !takesArguments(value))
            continue;
        if (parametersThatNothingReads.empty()) {
            size_t numberOfWords = (numberOfValues + BodyIndex::bitsPerWord - 1) / BodyIndex::bitsPerWord;
            std::span<uint8_t> memory = m_worker.allocate(bytesOfArrayInBodyIndex<uint32_t>(numberOfWords));
            parametersThatNothingReads = takeArrayOfBodyIndex<uint32_t>(memory, numberOfWords);
            m_index->m_parametersReadByReleaseHintsAlone = parametersThatNothingReads;
        }
        parametersThatNothingReads[value / BodyIndex::bitsPerWord] |= 1u << (value % BodyIndex::bitsPerWord);
    }
}

void BodyIndexBuilder::noteEdge(BlockID from, BodyTarget target, uint32_t targetIndex, bool isHandlerEdge)
{
    BodyUseKind kind = isHandlerEdge ? BodyUseKind::HandlerArgument : BodyUseKind::EdgeArgument;
    BlockID to = target.block();
    bool keepsLinks = m_function.isCoroutine();
    uint32_t indexOfParameter = 0;
    for (ValueID argument : target.arguments()) {
        if (argument) {
            noteUse(argument, kind, from.index());
            if (keepsLinks)
                m_worker.argumentLinks.append(BodyArgumentLink { argument, from, to, indexOfParameter });
        }
        ++indexOfParameter;
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

// It counts a mention exactly where BodyIndex::edgeResult() answers.
void BodyIndexBuilder::noteProofs(const BodyBlock& from, const BodyTerminator& terminator)
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
                    noteUse(subject, BodyUseKind::EdgeProof, from.id().index());
            }
        }
        ++targetIndex;
    }
}

void BodyIndexBuilder::indexWords()
{
    static_assert(sizeof(Wire::ParameterHead) == sizeof(uint32_t));

    const Body& body = m_function.body;
    const UnitIDRemap* unit = &body.unit();
    const uint32_t* firstWord = body.words().data();
    uint32_t numberOfBlocks = body.numberOfBlocks();
    uint32_t numberOfValues = body.numberOfValues();
    OpcodeSet opcodes;

    auto offsetOf = [&](const uint32_t* word) {
        return static_cast<uint32_t>(word - firstWord);
    };
    // The place of a value that the loop has not come to is zero, where no operation begins.
    auto isIteratorOpen = [&](ValueID named) {
        uint32_t place = m_placesOfValues[named.index()];
        return place && !(place & BodyIndex::isParameterBit) && body.operationAt(place, named).opcode() == Opcode::IteratorOpen;
    };

    BodyBlock block = body.entry();
    for (uint32_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        BlockID id(blockIndex);
        uint32_t firstParameter = block.firstParameter().index();
        RELEASE_ASSERT(static_cast<uint64_t>(firstParameter) + block.numberOfParameters() + block.numberOfValues() <= numberOfValues);
        m_worker.valuesGoneOver += block.numberOfParameters() + block.numberOfValues();

        BlockPlace& place = m_blocks[blockIndex];
        place.wordOffset = offsetOf(block.words());
        place.firstParameter = firstParameter;

        uint32_t placeOfParameter = BodyIndex::isParameterBit | (place.wordOffset + BodyBlock::numberOfHeadWords);
        uint32_t firstValue = block.firstValue().index();
        for (uint32_t parameter = firstParameter; parameter < firstValue; ++parameter)
            m_placesOfValues[parameter] = placeOfParameter++;

        if (ValueID subject = block.subject())
            noteUse(subject, BodyUseKind::BlockSubject, blockIndex);

        BodyValues::Iterator cursor = block.values().begin();
        for (uint32_t remaining = block.numberOfValues(); remaining--; ++cursor) {
            BodyValue value = *cursor;
            uint32_t user = value.id().index();
            m_placesOfValues[user] = offsetOf(value.words());
            opcodes.add(value.opcode());
            value.forEachOperand([&](ValueID operand, uint32_t) {
                noteUse(operand, BodyUseKind::Operand, user);
            });
            if (value.opcode() == Opcode::ReleaseHint) [[unlikely]]
                m_worker.releaseHints.append(value.id());
            if (opcodesThatMayNameIterations.contains(value.opcode())) [[unlikely]] {
                value.forEachOperand([&](ValueID operand, uint32_t) {
                    if (isIteratorOpen(operand))
                        m_worker.mentionsOfIterations.append({ operand, value.id(), id });
                });
            }
            if (value.words()[0] & Wire::WrittenPositionFollows) [[unlikely]] {
                std::optional<ConstructionStart> start = value.constructionStart();
                // Program::findConstructionReportedAt() looks up starts by module, so a start without a module could never be found.
                if (start && m_function.module)
                    m_worker.constructionStarts.append(ConstructionStartInModule { m_function.module, *start });
            }
        }

        BodyTerminator terminator(cursor.words(), unit, id);
        place.wordOffsetOfTerminator = offsetOf(terminator.words());
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            noteUse(operand, BodyUseKind::TerminatorOperand, blockIndex);
        });
        uint32_t targetIndex = 0;
        for (BodyTarget target : terminator.targets())
            noteEdge(id, target, targetIndex++, false);
        if (block.hasHandler())
            noteEdge(id, block.handler(), 0, true);
        noteProofs(block, terminator);

        block = BodyBlock(terminator.end(), nullptr, unit, BlockID(blockIndex + 1), block.endOfValues());
    }

    RELEASE_ASSERT(block.words() == firstWord + body.words().size());
    RELEASE_ASSERT(block.firstParameter().index() == numberOfValues);
    m_index->m_opcodes = opcodes;
}

void BodyIndexBuilder::adoptMentionsOfIterations()
{
    std::span<BodyMentionOfIteration> found = m_worker.mentionsOfIterations.mutableSpan();
    if (found.empty())
        return;

    std::ranges::stable_sort(found, { }, [](const BodyMentionOfIteration& mention) {
        return mention.open.index();
    });
    std::span<uint8_t> memory = m_worker.allocate(bytesOfArrayInBodyIndex<BodyMentionOfIteration>(found.size()));
    std::span<BodyMentionOfIteration> mentions = takeArrayOfBodyIndex<BodyMentionOfIteration>(memory, found.size());
    std::ranges::copy(found, mentions.begin());
    m_index->m_mentionsOfIterations = mentions;
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
    // Where the members of each loop stand in a row among those of the loop around it, the list of a loop in no other serves all inside it: a block is listed once, however many loops hold it.
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
            continue;
        }
        outermostAround[place] = place;
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
    // A parent has a smaller id.
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
    m_index->m_loops = loops;
    m_index->m_membersOfLoops = members;
}

ValueID BodyIndex::valueBehindCasts(ValueID id) const
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

std::optional<BodyEdgeResult> BodyIndex::edgeResult(const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target, uint32_t proofIndex) const
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
    result.parameter = ValueID(m_blocks[target.block().index()].firstParameter + static_cast<uint32_t>(target.arguments().size()) + proofIndex);
    return result;
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
