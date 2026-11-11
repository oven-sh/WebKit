/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSweep.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTHeldOperands.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <tuple>
#include <utility>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet interiorPointersInSweptCode = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (baseOf(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}();

static LayoutID layoutOfSweptEnvironmentHandedBack(const SweepInputs& inputs, TypeID type)
{
    if (!type || holdsOnlyPrimitives(type))
        return { };
    LayoutID environment = inputs.types.at(type).environment;
    return environment && inputs.plan.layout(environment).cellsAreHandedBack ? environment : LayoutID();
}

static bool namesOnlyEnvironmentsMadeBySweptCode(const Code& code, std::span<const uint32_t> firstIncomingEdge, std::span<const EdgeID> incomingEdges, ValueID named)
{
    Vector<ValueID, 8> parametersSeen;
    Vector<ValueID, 8> worklist;
    worklist.append(named);
    while (!worklist.isEmpty()) {
        ValueID value = worklist.takeLast();
        if (!value || code.isConstant(value))
            return false;
        const Operation& operation = code.at(value);
        if (operation.opcode == Opcode::NewObject) {
            if (operation.storage() != AllocationStorage::Heap)
                return false;
            continue;
        }
        if (cellPreservingOpcodes.contains(operation.opcode)) {
            worklist.append(operation.operand(0));
            continue;
        }
        if (operation.opcode != Opcode::Parameter)
            return false;
        if (parametersSeen.contains(value))
            continue;
        parametersSeen.append(value);

        BlockID block = operation.block();
        if (code.at(block).isEntry || code.at(block).isHandlerBlock())
            return false;
        std::span<const ValueID> parameters = code.at(block).parameters();
        size_t position = std::ranges::find(parameters, value) - parameters.begin();
        for (uint32_t index = firstIncomingEdge[block.index()]; index < firstIncomingEdge[block.index() + 1]; ++index) {
            std::span<const ValueID> arguments = code.at(incomingEdges[index]).arguments();
            if (position >= arguments.size())
                return false;
            worklist.append(arguments[position]);
        }
    }
    return true;
}

static ValueID ownerOfSweptPointer(const Code& code, ValueID pointer)
{
    const Operation* producer = &code.at(pointer);
    if (producer->opcode == Opcode::ResultAt)
        producer = &code.at(producer->operand(OperandIndex::ResultAt::results));
    std::optional<uint8_t> operandIndex = baseOf(producer->opcode);
    return operandIndex ? producer->operand(*operandIndex) : ValueID();
}

static constexpr uint32_t noReadOfSweptValue = UINT32_MAX;

static ALWAYS_INLINE bool noteReadOfSweptValueInAnyOrder(Vector<uint32_t>& lastReads, Vector<ReadOfSweptValue>& reads, ValueID value, BlockID block, ValueID reader)
{
    uint32_t& last = lastReads.mutableSpan().data()[value.index()];
    if (last != noReadOfSweptValue && reads[last].block == block) {
        ValueID& noted = reads[last].reader;
        if (!noted || (reader && reader <= noted))
            return false;
        noted = reader;
        return true;
    }
    reads.append({ block, reader, last });
    last = static_cast<uint32_t>(reads.size() - 1);
    return true;
}

static bool isHeldBySweptSuspension(const Operation& suspension, ValueID value)
{
    std::span<const ValueID> held = heldOperandsOf(suspension);
    return std::ranges::find(held, value) != held.end();
}

namespace {

class SweepLiveRanges {
    WTF_MAKE_NONCOPYABLE(SweepLiveRanges);
public:
    SweepLiveRanges(Vector<uint32_t>& lastReads, Vector<ReadOfSweptValue>& reads)
        : m_lastRead(lastReads)
        , m_reads(reads)
    {
    }

    bool noteReadInAnyOrder(ValueID value, BlockID block, ValueID reader) { return noteReadOfSweptValueInAnyOrder(m_lastRead, m_reads, value, block, reader); }

private:
    Vector<uint32_t>& m_lastRead;
    Vector<ReadOfSweptValue>& m_reads;
};

class SweepLiveSets {
public:
    SweepLiveSets(const Code& code, const uint8_t* marksOfValues, const uint8_t* statesOfBlocks, std::span<const ValueID> releaseHints, const SweepBits& environmentsHandedBack, bool hasInteriorPointers)
        : m_code(code)
        , m_marksOfValues(marksOfValues)
        , m_statesOfBlocks(statesOfBlocks)
        , m_releaseHints(releaseHints)
        , m_environmentsHandedBack(environmentsHandedBack)
        , m_followsEveryValue(code.hasSuspensions())
        , m_hasInteriorPointers(hasInteriorPointers)
    {
    }

    void noteReadsAtEndOf(BlockID block, SweepLiveRanges& ranges) const
    {
        forEachValueReadAtEndOf(block, [&](ValueID read) ALWAYS_INLINE_LAMBDA {
            readWithOwners(read, [&](ValueID value, uint32_t definingBlock) ALWAYS_INLINE_LAMBDA {
                if (definingBlock == block.index())
                    return false;
                return !ranges.noteReadInAnyOrder(value, block, ValueID());
            });
        });
    }

private:
    bool isFollowed(ValueID id) const { return m_followsEveryValue || m_releaseHints[id.index()] || m_environmentsHandedBack.get(id.index()); }
    bool isNeeded(ValueID id) const { return m_marksOfValues[id.index()] & neededMarkOfSweep; }

    template<typename Functor>
    ALWAYS_INLINE void forEachValueReadAtEndOf(BlockID id, NOESCAPE const Functor& functor) const
    {
        const Block& block = m_code.at(id);
        auto readArgumentsOn = [&](const Edge& edge) ALWAYS_INLINE_LAMBDA {
            ASSERT(!edge.isOpen());
            std::span<const ValueID> arguments = edge.arguments();
            if (arguments.empty())
                return;
            std::span<const ValueID> parameters = m_code.at(edge.to).parameters();
            for (size_t index = 0; index < arguments.size(); ++index) {
                if (index >= parameters.size() || isNeeded(parameters[index]))
                    functor(arguments[index]);
            }
        };
        for (uint32_t index = 0; index < block.terminator.numberOfEdges; ++index)
            readArgumentsOn(m_code.at(block.terminator.edge(index)));
        if (EdgeID handler = handlerOfSweptBlock(block, m_statesOfBlocks[id.index()]))
            readArgumentsOn(m_code.at(handler));
        block.terminator.forEachChild(functor);
        if (block.subject)
            functor(block.subject);
    }

    ValueID ownerOf(ValueID pointer) const { return ownerOfSweptPointer(m_code, pointer); }

    template<typename Functor>
    ALWAYS_INLINE void readWithOwners(ValueID id, NOESCAPE const Functor& wasRead) const
    {
        if (!m_hasInteriorPointers) {
            if (!id || !isFollowed(id))
                return;
            uint32_t definingBlock = m_code.at(id).blockIndex;
            if (definingBlock != Operation::noBlock && isNeeded(id))
                wasRead(id, definingBlock);
            return;
        }
        while (id) {
            uint32_t definingBlock = m_code.at(id).blockIndex;
            if (definingBlock == Operation::noBlock || !isNeeded(id))
                return;
            if (isFollowed(id) && wasRead(id, definingBlock))
                return;
            id = ownerOf(id);
        }
    }

    const Code& m_code;
    const uint8_t* m_marksOfValues;
    const uint8_t* m_statesOfBlocks;
    std::span<const ValueID> m_releaseHints;
    const SweepBits& m_environmentsHandedBack;
    bool m_followsEveryValue;
    bool m_hasInteriorPointers;
};

} // anonymous namespace

void Sweep::beginFollowingLiveness()
{
    Code& code = *m_code;
    code.m_suspensions.shrink(0);
    code.m_liveValues.shrink(0);
    m_mayHandBackEnvironments = code.opcodesOfValues().contains(Opcode::ReleaseEnvironment);
    m_followsLiveness = code.hasSuspensions() || m_mayHandBackEnvironments;
    if (!m_followsLiveness)
        return;

    m_liveness = makeUniqueWithoutFastMallocCheck<SweepLivenessStorage>();
    m_hasInteriorPointers = code.opcodesOfValues().intersects(interiorPointersInSweptCode);
    uint32_t numberOfValues = code.numberOfValues();
    uint32_t numberOfBlocks = code.numberOfBlocks();
    m_environmentsFollowed.clearAll();
    m_environmentsHandedBack.shrink(0);
    m_namesOfCellsThatMayHoldEnvironments.shrink(0);
    m_blocksThatSuspend.clearAll();
    m_blocksThatSuspend.ensureSize(numberOfBlocks);
    m_blocksWithHints.clearAll();
    m_blocksWithHints.ensureSize(numberOfBlocks);
    m_lastReads.fill(noReadOfSweptValue, numberOfValues);
    m_reads.shrink(0);
    m_lastReadersInOwnBlocks.fill(ValueID(), numberOfValues);
    m_liveness->blockOfValue.fill(Operation::noBlock, numberOfValues);
    m_liveness->operationsThatSuspend.clearAll();
    m_liveness->operationsThatSuspend.ensureSize(numberOfValues);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Sweep::followIfEnvironmentIsHandedBack(ValueID id)
{
    const Code& code = *m_code;
    LayoutID layout = layoutOfSweptEnvironmentHandedBack(*m_inputs, code.at(id).type);
    if (!layout)
        return;
    m_environmentsFollowed.set(id.index());
    m_environmentsHandedBack.append({ layout, id });
    if (code.at(id).opcode == Opcode::ResultAt) {
        ValueID producer = code.at(id).operand(OperandIndex::ResultAt::results);
        m_environmentsFollowed.set(producer.index());
        m_environmentsHandedBack.append({ layout, producer });
    }
}

static bool holdsEnvironmentsWithoutMarkingThemInSweptCode(const Operation& holder)
{
    bool mayLieInFrame = holder.opcode == Opcode::NewClosure || (holder.opcode == Opcode::NewObject && holder.storage() != AllocationStorage::Heap);
    return mayLieInFrame || holder.opcode == Opcode::Results;
}

ALWAYS_INLINE_INTO_OTHER_FILES void Sweep::noteIfMayHoldEnvironmentInFrame(ValueID id)
{
    const Code& code = *m_code;
    if (holdsEnvironmentsWithoutMarkingThemInSweptCode(code.at(underlyingCellOf(code, id))))
        m_namesOfCellsThatMayHoldEnvironments.append(id);
}

void Sweep::followCellsThatHoldEnvironmentsInFrames()
{
    struct LayoutsHeld {
        uint32_t first { 0 };
        uint32_t count { 0 };
    };
    const Code& code = *m_code;
    Vector<ValueID>& names = m_namesOfCellsThatMayHoldEnvironments;
    AOT::sort(names);
    Vector<LayoutsHeld> heldByNames(names.size());
    Vector<LayoutID> layouts;
    for (size_t index = 0; index < names.size(); ++index) {
        ValueID id = names[index];
        auto settledFor = [&](ValueID cell) {
            size_t position = std::ranges::lower_bound(names, cell) - names.begin();
            RELEASE_ASSERT(position < index && names[position] == cell);
            return heldByNames[position];
        };
        ValueID cell = underlyingCellOf(code, id);
        if (cell != id)
            heldByNames[index] = settledFor(cell);
        else {
            uint32_t first = static_cast<uint32_t>(layouts.size());
            auto add = [&](LayoutID layout) {
                if (!std::ranges::contains(layouts.span().subspan(first), layout))
                    layouts.append(layout);
            };
            for (ValueID held : code.at(cell).operands()) {
                if (!held || code.isConstant(held))
                    continue;
                if (LayoutID layout = layoutOfSweptEnvironmentHandedBack(*m_inputs, code.at(held).type))
                    add(layout);
                ValueID captured = underlyingCellOf(code, held);
                if (!holdsEnvironmentsWithoutMarkingThemInSweptCode(code.at(captured)))
                    continue;
                LayoutsHeld below = settledFor(captured);
                for (uint32_t offset = 0; offset < below.count; ++offset)
                    add(layouts[below.first + offset]);
            }
            heldByNames[index] = { first, static_cast<uint32_t>(layouts.size()) - first };
        }
        LayoutsHeld held = heldByNames[index];
        if (held.count)
            m_environmentsFollowed.set(id.index());
        for (uint32_t offset = 0; offset < held.count; ++offset)
            m_environmentsHandedBack.append({ layouts[held.first + offset], id });
    }
    names.shrink(0);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Sweep::noteReadForLiveness(ValueID read, BlockID block, ValueID reader)
{
    const Code& code = *m_code;
    for (ValueID value = read; value; value = ownerOfSweptPointer(code, value)) {
        uint32_t definingBlock = code.at(value).blockIndex;
        if (definingBlock == Operation::noBlock)
            return;
        if (definingBlock == block.index()) {
            ValueID& last = m_lastReadersInOwnBlocks.mutableSpan().data()[value.index()];
            if (last && reader <= last)
                return;
            last = reader;
        } else if (!noteReadOfSweptValueInAnyOrder(m_lastReads, m_reads, value, block, reader))
            return;
        if (!m_hasInteriorPointers)
            return;
    }
}

void Sweep::noteNeededOperationForLiveness(ValueID id, const Operation& operation)
{
    BlockID block = operation.block();
    m_liveness->blockOfValue.mutableSpan().data()[id.index()] = block.index();
    if (m_mayHandBackEnvironments) {
        followIfEnvironmentIsHandedBack(id);
        noteIfMayHoldEnvironmentInFrame(id);
    }
    if (operation.traits().maySuspend()) {
        m_blocksThatSuspend.quickSet(block.index());
        m_liveness->operationsThatSuspend.quickSet(id.index());
    }
    if (operation.opcode == Opcode::ReleaseEnvironment)
        return;
    if (operation.opcode == Opcode::Narrowed) {
        noteReadForLiveness(operation.operand(OperandIndex::Narrowed::value), block, id);
        return;
    }
    operation.forEachChild([&](ValueID read) {
        noteReadForLiveness(read, block, id);
    });
}

void Sweep::finishLiveness()
{
    Code& code = *m_code;
    if (!m_followsLiveness)
        return;
    RELEASE_ASSERT(!m_reachedInPostorder.isEmpty());

    if (m_mayHandBackEnvironments) {
        for (BlockID block : m_reachedInPostorder) {
            for (ValueID parameter : code.at(block).parameters()) {
                if (code.isNeeded(parameter))
                    followIfEnvironmentIsHandedBack(parameter);
            }
        }
        followCellsThatHoldEnvironmentsInFrames();
    }

    ASSERT(m_worklist.isEmpty());
    Vector<ValueID>& releaseHints = m_worklist;
    auto releaseStorage = makeScopeExit([&] {
        m_worklist.shrink(0);
    });
    releaseHints.fill(ValueID(), m_mayHandBackEnvironments ? code.numberOfValues() : 0);

    std::span<const OperationKeptForOperand> operationsKeptForOperands = m_operationsKeptForOperands.span();
    for (const OperationKeptForOperand& noted : operationsKeptForOperands) {
        ValueID id = noted.operation;
        if (noted.opcode != Opcode::ReleaseEnvironment || !code.isNeeded(id))
            continue;
        releaseHints[id.index()] = std::exchange(releaseHints[noted.operand.index()], id);
        m_blocksWithHints.quickSet(noted.block.index());
        if (LayoutID layout = layoutOfSweptEnvironmentHandedBack(*m_inputs, code.at(noted.operand).type))
            m_environmentsHandedBack.append({ layout, id });
    }
    RELEASE_ASSERT(code.hasSuspensions() || m_blocksThatSuspend.isEmpty());
    if (m_blocksThatSuspend.isEmpty() && m_blocksWithHints.isEmpty())
        return;
    AOT::sort(m_environmentsHandedBack, { }, [](const EnvironmentHandedBack& environment) {
        return std::tuple(environment.layout, environment.value);
    });

    SweepLiveSets sets(code, m_arrays.marks, m_states, releaseHints.span(), m_environmentsFollowed, m_hasInteriorPointers);
    SweepLiveRanges ranges(m_lastReads, m_reads);
    for (BlockID block : m_reachedInPostorder)
        sets.noteReadsAtEndOf(block, ranges);
    SweepBits& valuesToFollow = m_liveness->valuesToFollow;
    valuesToFollow.clearAll();
    if (!code.hasSuspensions())
        valuesToFollow.merge(m_environmentsFollowed);
    for (const OperationKeptForOperand& noted : operationsKeptForOperands) {
        if (noted.opcode != Opcode::ReleaseEnvironment || !code.isNeeded(noted.operation))
            continue;
        if (!code.hasSuspensions()) {
            valuesToFollow.set(noted.operand.index());
            valuesToFollow.set(noted.operation.index());
        }
        if (code.blockOf(noted.operand) != noted.block)
            ranges.noteReadInAnyOrder(noted.operation, noted.block, noted.operation);
    }

    findLiveSets(releaseHints.span(), code.hasSuspensions() ? nullptr : &valuesToFollow);
    markReleasesBehindLastUsesBySets();
    if (m_blocksThatSuspend.isEmpty())
        return;
    listLiveValuesAtSuspensionsBySets();
    AOT::sort(code.m_suspensions, { }, &LiveAtSuspension::suspension);
}

static void forEachValueReadAtEndOfSweptBlock(const Code& code, const Block& block, EdgeID handler, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    auto readArgumentsOn = [&](const Edge& edge) {
        std::span<const ValueID> arguments = edge.arguments();
        if (arguments.empty())
            return;
        std::span<const ValueID> parameters = code.at(edge.to).parameters();
        for (size_t index = 0; index < arguments.size(); ++index) {
            if (index >= parameters.size() || code.isNeeded(parameters[index]))
                functor(arguments[index]);
        }
    };
    for (uint32_t index = 0; index < block.terminator.numberOfEdges; ++index)
        readArgumentsOn(code.at(block.terminator.edge(index)));
    if (handler)
        readArgumentsOn(code.at(handler));
    block.terminator.forEachChild(functor);
    if (block.subject)
        functor(block.subject);
}

void Sweep::findLiveSets(std::span<const ValueID> releaseHints, const SweepBits* valuesToFollow)
{
    const Code& code = *m_code;
    SweepLivenessStorage& storage = *m_liveness;
    SweepValueSets& sets = storage.sets;
    uint32_t* const blockOfValue = storage.blockOfValue.mutableSpan().data();
    SweepBits& blocksAskedAbout = storage.blocksAskedAbout;
    SweepBits& valuesReadInOtherBlocks = storage.valuesReadInOtherBlocks;
    uint32_t numberOfValues = code.numberOfValues();
    uint32_t numberOfBlocks = code.numberOfBlocks();
    const bool followsEveryValue = code.hasSuspensions();
    const bool hasInteriorPointers = m_hasInteriorPointers;
    const uint8_t* const marksOfValues = m_arrays.marks;
    const uint8_t* const states = m_states;
    std::span<const OperationKeptForOperand> keptForOperands = m_operationsKeptForOperands.span();

    blocksAskedAbout.clearAll();
    blocksAskedAbout.ensureSize(numberOfBlocks);
    blocksAskedAbout.merge(m_blocksThatSuspend);
    blocksAskedAbout.merge(m_blocksWithHints);
    valuesReadInOtherBlocks.clearAll();
    valuesReadInOtherBlocks.ensureSize(numberOfValues);
    storage.hintsThatStay.clearAll();
    storage.hintsThatStay.ensureSize(numberOfValues);

    SweepFlow& flow = storage.flow;
    flow.compute(code, m_statesOfBlocks.span(), blocksAskedAbout);
    RELEASE_ASSERT(flow.numberOfBlocks() == m_reachedInPostorder.size());
    if (flow.hasLoops()) {
        ensureEdgesThatAreTakenAreListed();
        flow.findLoops(code, m_firstIncomingEdge.span(), m_incomingEdges.span());
    }

    SweepBits& blocksAwaited = storage.blocksAwaited;
    blocksAwaited.clearAll();
    blocksAwaited.merge(blocksAskedAbout);
    if (flow.hasLoops()) {
        blocksAskedAbout.forEachSetBit([&](size_t index) {
            uint32_t loop = flow.loopAround(BlockID(static_cast<uint32_t>(index)));
            if (loop == SweepFlow::none)
                return;
            while (flow.loopAroundLoop(loop) != SweepFlow::none)
                loop = flow.loopAroundLoop(loop);
            blocksAwaited.quickSet(flow.headOf(loop).index());
        });
    }
    uint32_t numberOfBlocksAwaited = 0;
    blocksAwaited.forEachSetBit([&](size_t) {
        ++numberOfBlocksAwaited;
    });

    auto blockOf = [&](ValueID id) ALWAYS_INLINE_LAMBDA {
        uint32_t& block = blockOfValue[id.index()];
        if (block == Operation::noBlock)
            block = code.at(id).blockIndex;
        return block;
    };
    storage.firstKeptForOperandInBlock.fill(SweepFlow::none, keptForOperands.empty() ? 0 : numberOfBlocks);
    for (size_t index = keptForOperands.size(); index--;) {
        const OperationKeptForOperand& kept = keptForOperands[index];
        storage.firstKeptForOperandInBlock[kept.block.index()] = static_cast<uint32_t>(index);
        if (kept.opcode != Opcode::ReleaseEnvironment || !code.isNeeded(kept.operation))
            continue;
        storage.hintsThatStay.quickSet(kept.operation.index());
        uint32_t blockOfOperand = blockOf(kept.operand);
        RELEASE_ASSERT(blockOfOperand != Operation::noBlock);
        blockOfValue[kept.operation.index()] = blockOfOperand;
        if (blockOfOperand != kept.block.index())
            valuesReadInOtherBlocks.quickSet(kept.operand.index());
    }

    storage.reads.shrink(0);
    storage.firstReadOfBlock.fill(0, numberOfBlocks + 1);
    uint32_t* const firstReadOfBlock = storage.firstReadOfBlock.mutableSpan().data();
    auto listRead = [&](BlockID block, ValueID value, ValueID reader) ALWAYS_INLINE_LAMBDA {
        storage.reads.append({ block, { value, reader } });
        ++firstReadOfBlock[block.index()];
    };
    const uint32_t* const lastReads = m_lastReads.span().data();
    const ValueID* const lastReadersInOwnBlocks = m_lastReadersInOwnBlocks.span().data();
    const ReadOfSweptValue* const notedReads = m_reads.span().data();
    for (uint32_t index = 0; index < numberOfValues; ++index) {
        uint32_t read = lastReads[index];
        ValueID lastReaderInOwnBlock = lastReadersInOwnBlocks[index];
        if ((read == noReadOfSweptValue && !lastReaderInOwnBlock) || (valuesToFollow && !valuesToFollow->get(index)))
            continue;
        uint32_t definingBlock = blockOf(ValueID(index));
        RELEASE_ASSERT(definingBlock != Operation::noBlock);
        if (!flow.mayLeadToBlockAskedAbout(BlockID(definingBlock)))
            continue;
        bool isDefinedWhereAskedAbout = blocksAskedAbout.quickGet(definingBlock);
        if (lastReaderInOwnBlock && isDefinedWhereAskedAbout)
            listRead(BlockID(definingBlock), ValueID(index), lastReaderInOwnBlock);
        for (; read != noReadOfSweptValue; read = notedReads[read].previous) {
            const ReadOfSweptValue& noted = notedReads[read];
            if (noted.block.index() != definingBlock)
                valuesReadInOtherBlocks.quickSet(index);
            else if (!isDefinedWhereAskedAbout)
                continue;
            listRead(noted.block, ValueID(index), noted.reader);
        }
    }
    uint32_t numberOfReads = 0;
    for (uint32_t index = 0; index <= numberOfBlocks; ++index)
        numberOfReads += std::exchange(firstReadOfBlock[index], numberOfReads);
    storage.readsByBlock.resize(numberOfReads);
    SweepLivenessStorage::ReadOfValue* const readsByBlock = storage.readsByBlock.mutableSpan().data();
    for (const SweepLivenessStorage::ReadInBlock& read : storage.reads)
        readsByBlock[firstReadOfBlock[read.block.index()]++] = read.read;
    for (uint32_t index = numberOfBlocks; index; --index)
        firstReadOfBlock[index] = firstReadOfBlock[index - 1];
    firstReadOfBlock[0] = 0;

    sets.clear();
    storage.liveAtHeads.fill(SweepValueSets::empty, numberOfBlocks);
    SweepValueSets::Set* const liveAtHeads = storage.liveAtHeads.mutableSpan().data();
    storage.hints.shrink(0);
    storage.suspensions.shrink(0);

    SweepValueSets::Set live = SweepValueSets::empty;
    auto define = [&](ValueID id) ALWAYS_INLINE_LAMBDA {
        live = sets.without(live, id.index());
        if (releaseHints.empty())
            return;
        for (ValueID hint = releaseHints[id.index()]; hint; hint = releaseHints[hint.index()])
            live = sets.without(live, hint.index());
    };
    auto isFollowed = [&](ValueID id) ALWAYS_INLINE_LAMBDA {
        return followsEveryValue || releaseHints[id.index()] || m_environmentsFollowed.get(id.index());
    };
    auto readAtEndOfBlock = [&](ValueID read) {
        for (ValueID id = read; id && (marksOfValues[id.index()] & neededMarkOfSweep) && blockOf(id) != Operation::noBlock; id = ownerOfSweptPointer(code, id)) {
            if (isFollowed(id))
                live = sets.with(live, id.index());
            if (!hasInteriorPointers)
                return;
        }
    };

    auto readsIn = [&](BlockID block) ALWAYS_INLINE_LAMBDA {
        uint32_t first = firstReadOfBlock[block.index()];
        return std::span<SweepLivenessStorage::ReadOfValue> { readsByBlock + first, firstReadOfBlock[block.index() + 1] - first };
    };
    auto forEachEdgeOf = [&](const Block& contents, EdgeID handler, NOESCAPE const Invocable<void(const Edge&)> auto& functor) ALWAYS_INLINE_LAMBDA {
        for (uint32_t index = 0; index < contents.terminator.numberOfEdges; ++index)
            functor(code.at(contents.terminator.edge(index)));
        if (handler)
            functor(code.at(handler));
    };
    constexpr SweepValueSets::Set readsAreWhatIsLive = UINT32_MAX;

    for (BlockID block : flow.blocksBehindTheirSuccessors()) {
        const Block& contents = code.at(block);
        EdgeID handler = handlerOfSweptBlock(contents, states[block.index()]);
        bool isAskedAbout = blocksAskedAbout.quickGet(block.index());
        if (!isAskedAbout && !contents.terminator.numberOfEdges && !handler) {
            liveAtHeads[block.index()] = readsAreWhatIsLive;
            continue;
        }

        sets.keepSetsMadeSoFar();
        live = SweepValueSets::empty;
        bool leadsToListOfReads = false;
        forEachEdgeOf(contents, handler, [&](const Edge& edge) ALWAYS_INLINE_LAMBDA {
            SweepValueSets::Set liveAtHead = liveAtHeads[edge.to.index()];
            if (liveAtHead == readsAreWhatIsLive) {
                leadsToListOfReads = true;
                return;
            }
            live = sets.unite(live, liveAtHead);
            if (flow.isReducible())
                return;
            if (BlockID head = flow.headOfOutermostLoopEntered(block, edge.to))
                live = sets.unite(live, liveAtHeads[head.index()]);
        });
        if (leadsToListOfReads) {
            forEachEdgeOf(contents, handler, [&](const Edge& edge) ALWAYS_INLINE_LAMBDA {
                if (liveAtHeads[edge.to.index()] != readsAreWhatIsLive)
                    return;
                for (const SweepLivenessStorage::ReadOfValue& read : readsIn(edge.to))
                    live = sets.with(live, read.value.index());
            });
        }

        std::span<SweepLivenessStorage::ReadOfValue> reads = readsIn(block);
        std::span<const ValueID> order = code.m_order.span().subspan(contents.m_firstInOrder, contents.m_numberInOrder);
        if (!isAskedAbout) {
            for (const SweepLivenessStorage::ReadOfValue& read : reads)
                live = sets.with(live, read.value.index());
            for (ValueID id : order) {
                if (live == SweepValueSets::empty)
                    break;
                if (valuesReadInOtherBlocks.quickGet(id.index()))
                    define(id);
            }
        } else {
            forEachValueReadAtEndOfSweptBlock(code, contents, handler, readAtEndOfBlock);
            std::ranges::sort(reads, std::ranges::greater { }, &SweepLivenessStorage::ReadOfValue::reader);
            size_t nextRead = 0;
            for (; nextRead < reads.size() && !reads[nextRead].reader; ++nextRead)
                live = sets.with(live, reads[nextRead].value.index());

            size_t nextKeptForOperand = keptForOperands.empty() ? 0 : storage.firstKeptForOperandInBlock[block.index()];
            for (size_t index = order.size(); index--;) {
                ValueID id = order[index];
                if (storage.operationsThatSuspend.quickGet(id.index())) {
                    storage.suspensions.append({ id.index(), block, live });
                    sets.keepSetsMadeSoFar();
                }
                if (storage.hintsThatStay.quickGet(id.index())) {
                    for (;; ++nextKeptForOperand) {
                        RELEASE_ASSERT(nextKeptForOperand < keptForOperands.size());
                        if (keptForOperands[nextKeptForOperand].operation == id)
                            break;
                    }
                    storage.hints.append({ static_cast<uint32_t>(nextKeptForOperand), block, live });
                    sets.keepSetsMadeSoFar();
                    live = sets.with(live, id.index());
                    for (; nextRead < reads.size() && reads[nextRead].reader == id; ++nextRead) { }
                    continue;
                }
                define(id);
                for (; nextRead < reads.size() && reads[nextRead].reader == id; ++nextRead)
                    live = sets.with(live, reads[nextRead].value.index());
            }
            RELEASE_ASSERT(nextRead == reads.size());
        }
        for (ValueID parameter : contents.parameters()) {
            if (live == SweepValueSets::empty)
                break;
            if (isAskedAbout || valuesReadInOtherBlocks.quickGet(parameter.index()))
                define(parameter);
        }
        liveAtHeads[block.index()] = live;
        if (blocksAwaited.quickGet(block.index()) && !--numberOfBlocksAwaited)
            break;
    }
    sets.keepSetsMadeSoFar();

    storage.liveThroughLoops.fill(SweepValueSets::empty, flow.hasLoops() ? flow.numberOfBlocks() : 0);
    for (uint32_t loop = 0; loop < storage.liveThroughLoops.size(); ++loop) {
        if (!flow.isHeadOfLoop(loop))
            continue;
        uint32_t around = flow.loopAroundLoop(loop);
        storage.liveThroughLoops[loop] = sets.unite(storage.liveAtHeads[flow.headOf(loop).index()], around == SweepFlow::none ? SweepValueSets::empty : storage.liveThroughLoops[around]);
    }
}

SweepValueSets::Set Sweep::liveThroughLoopsAround(BlockID block) const
{
    uint32_t loop = m_liveness->flow.loopAround(block);
    return loop == SweepFlow::none ? SweepValueSets::empty : m_liveness->liveThroughLoops[loop];
}

void Sweep::markReleasesBehindLastUsesBySets()
{
    Code& code = *m_code;
    const SweepValueSets& sets = m_liveness->sets;
    for (const SweepLivenessStorage::PointAskedAbout& point : m_liveness->hints) {
        const OperationKeptForOperand& hint = m_operationsKeptForOperands[point.index];
        SweepValueSets::Set liveThroughLoops = liveThroughLoopsAround(point.block);
        auto isLive = [&](ValueID value) {
            return sets.contains(point.liveBehind, value.index()) || sets.contains(liveThroughLoops, value.index());
        };

        bool releasesOperand = false;
        if (LayoutID layout = layoutOfSweptEnvironmentHandedBack(*m_inputs, code.at(hint.operand).type)) {
            ensureEdgesThatAreTakenAreListed();
            auto ofSameLayout = std::ranges::equal_range(m_environmentsHandedBack, layout, { }, &EnvironmentHandedBack::layout);
            releasesOperand = std::ranges::none_of(ofSameLayout, [&](const EnvironmentHandedBack& other) {
                return isLive(other.value);
            }) && namesOnlyEnvironmentsMadeBySweptCode(code, m_firstIncomingEdge.span(), m_incomingEdges.span(), hint.operand);
        }
        OptionSet<SweepMark> marks = code.marksOf(hint.operation);
        marks.set(SweepMark::ReleasesOperand, releasesOperand);
        code.m_marks[hint.operation.index()] = marks.toRaw();
    }
}

void Sweep::listLiveValuesAtSuspensionsBySets()
{
    Code& code = *m_code;
    SweepValueSets& sets = m_liveness->sets;
    const bool mayPointIntoStorage = code.opcodesOfValues().contains(Opcode::GetTypedArrayVector) || code.opcodesOfValues().contains(Opcode::GetArrayStorage);
    sets.keepSetsMadeSoFar();
    for (const SweepLivenessStorage::PointAskedAbout& point : m_liveness->suspensions) {
        ValueID suspension(point.index);
        uint32_t numberOfNodes = sets.numberOfNodes();
        SweepValueSets::Set live = sets.unite(point.liveBehind, liveThroughLoopsAround(point.block));
        uint32_t first = static_cast<uint32_t>(code.m_liveValues.size());
        sets.forEachMember(live, [&](uint32_t index) {
            ValueID id(index);
            if (id == suspension)
                return;
            if (!m_liveness->hintsThatStay.quickGet(index)) {
                if (mayPointIntoStorage) {
                    Opcode opcode = code.at(id).opcode;
                    RELEASE_ASSERT(opcode != Opcode::GetTypedArrayVector && opcode != Opcode::GetArrayStorage);
                }
                code.m_liveValues.append(id);
                return;
            }
            // A ReleaseEnvironment would hand back a cell that the collector may have freed in the meantime.
            m_totals.operationsSwept += 2;
            ValueID released = code.at(id).operand(OperandIndex::ReleaseEnvironment::environment);
            if (released == suspension || sets.contains(live, released.index()) || isHeldBySweptSuspension(code.at(suspension), released))
                return;
            OptionSet<SweepMark> marks = code.marksOf(id);
            marks.remove(SweepMark::ReleasesOperand);
            code.m_marks[id.index()] = marks.toRaw();
        });
        code.m_suspensions.append(LiveAtSuspension { suspension, first, static_cast<uint32_t>(code.m_liveValues.size()) - first });
        sets.forgetSetsMadeSince(numberOfNodes);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
