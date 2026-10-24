/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3MemoryNumbering.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3Common.h"
#include "B3Dominators.h"
#include "B3NaturalLoops.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"
#include "B3ValueKeyInlines.h"
#include <algorithm>
#include <limits>
#include <wtf/DataLog.h>
#include <wtf/HashFunctions.h>

namespace JSC { namespace B3 {

static ALWAYS_INLINE ValueKey keyOf(Value* value)
{
    switch (value->opcode()) {
    case Add:
    case Sub:
    case Mul:
    case BitAnd:
    case BitOr:
    case BitXor:
    case Shl:
    case SShr:
    case ZShr:
    case Equal:
    case NotEqual:
    case LessThan:
    case LessEqual:
    case GreaterThan:
    case GreaterEqual:
    case Above:
    case Below:
    case AboveEqual:
    case BelowEqual:
        return ValueKey(value->kind(), value->type(), value->child(0), value->child(1));
    case Opaque:
    case SExt32:
    case ZExt32:
    case Trunc:
    case IToD:
    case BitwiseCast:
        return ValueKey(value->kind(), value->type(), value->child(0));
    case Load:
    case Load8Z:
    case Load16Z:
    case Load8S:
    case Load16S:
    case Store:
    case Store8:
    case Store16:
    case Branch:
    case Patchpoint:
        return ValueKey();
    default:
        return value->key();
    }
}

MemoryNumbering::MemoryNumbering(Procedure& proc)
    : m_proc(proc)
    , m_validates(shouldValidateIRAtEachPhase(proc))
{
}

void MemoryNumbering::beginWalk(Dominators& dominators)
{
    m_dominators = &dominators;
    m_values.clear();
    m_pureCalls.clear();
    m_heapEpochs.clear();
    m_replacedLoads.shrink(0);
    m_loadsToCheck.shrink(0);
    m_lastMemoryStateID = noID;
    m_memoryStateAtTail.fill(MemoryState { }, m_proc.size());
    recordLoopWrites();
}

void MemoryNumbering::enterBlock(BasicBlock* block)
{
    m_block = block;
    m_values.startLayer();
    m_pureCalls.startLayer();
    m_heapEpochs.startLayer();

    MemoryState& state = m_memoryState;
    const MemoryState unvisitedBlockState { };
    bool isFirst = true;
    for (BasicBlock* predecessor : block->predecessors()) {
        const MemoryState& atTail = predecessor->index() < m_memoryStateAtTail.size() ? m_memoryStateAtTail[predecessor->index()] : unvisitedBlockState;
        if (isFirst) {
            if (state.id != atTail.id || state.id == noID)
                state = atTail;
            isFirst = false;
            continue;
        }
        if (state.id != atTail.id)
            state = { };
    }
    if (isFirst)
        state = { };
    if ((state.id == noID || !m_dominators->dominates(state.owner, block)) && !mergeSameEpochStates(block, state) && !inheritStateFromBeforeLoop(block, state)) {
        state.id = ++m_lastMemoryStateID;
        state.owner = block;
        state.beginEpoch(block);
    }
    ASSERT(state.id != noID && state.epoch != noID);
}

HeapSet MemoryNumbering::writesOf(Value* value)
{
    const Effects& constantEffects = constantEffectsTable[value->opcode()];
    if (constantEffects.isValid())
        return constantEffects.fence ? HeapSet(HeapRange::top()) : constantEffects.writes;
    if (isLoad(value->opcode()) && !value->as<MemoryValue>()->hasFence())
        return HeapSet();
    Effects effects = value->effects();
    return effects.fence ? HeapSet(HeapRange::top()) : effects.writes;
}

MemoryNumbering::Result MemoryNumbering::numberValue(Value* current, Opcode opcodeBeforeReduction, const HeapSet& writesBeforeReduction)
{
    if (current->opcode() != opcodeBeforeReduction && (current->opcode() == Nop || current->opcode() == Identity) && writesBeforeReduction)
        didWriteMemory(writesBeforeReduction, current->origin());
    return replaceIfRedundant(current);
}

void MemoryNumbering::didFinishBlock(BasicBlock* block)
{
    if (block->index() < m_memoryStateAtTail.size())
        m_memoryStateAtTail[block->index()] = m_memoryState;
}

void MemoryNumbering::leaveDominatorSubtree()
{
    m_values.dropLastLayer();
    m_pureCalls.dropLastLayer();
    m_heapEpochs.dropLastLayer();
}

void MemoryNumbering::endWalk()
{
    if (m_validates)
        validateReplacedLoads();
    checkReplacedLoadsAtRunTime();
}

static PatchpointValue* asPureCall(Value* value)
{
    auto* patchpoint = value->as<PatchpointValue>();
    return patchpoint && patchpoint->pureFunction ? patchpoint : nullptr;
}

Value* MemoryNumbering::representativeOf(Value* value)
{
    if (PatchpointValue* pureCall = asPureCall(value))
        return m_pureCalls.find(keyForPureCall(pureCall)).value_or(value);
    ValueKey key = keyOf(value);
    if (!key || value->isConstant())
        return value;
    return m_values.find(key).value_or(value);
}

Value* MemoryNumbering::ensureRepresentative(Value* value)
{
    if (PatchpointValue* pureCall = asPureCall(value))
        return m_pureCalls.findOrAdd(keyForPureCall(pureCall), value).value_or(value);
    ValueKey key = keyOf(value);
    if (!key || value->isConstant())
        return value;
    return m_values.findOrAdd(key, value).value_or(value);
}

bool MemoryNumbering::canBeRecorded(const HeapRange& range)
{
    return range.end() <= std::numeric_limits<unsigned>::max();
}

bool MemoryNumbering::record(RecordedWrites& writes, RecordedWrite write, Origin origin)
{
    for (RecordedWrite& recorded : writes) {
        if (recorded.hasSameHeapsAs(write)) {
            recorded.id = write.id;
            return true;
        }
    }
    if (writes.size() == m_proc.limit(Limit::MaxRecordedWritesPerMemoryState)) {
        m_proc.didReachLimit(Limit::MaxRecordedWritesPerMemoryState, origin);
        return false;
    }
    writes.append(write);
    return true;
}

bool MemoryNumbering::hasRecordedOverlay(const RecordedWrite& write)
{
    ASSERT(write.id != noID);
    return m_heapEpochs.find(write.begin).value_or(noID) == write.id;
}

void MemoryNumbering::recordLoopWrites()
{
    m_loops = nullptr;
    m_loopWrites.shrink(0);
    if (!m_proc.hasCycle())
        return;
    auto& loops = m_proc.naturalLoops();
    if (!loops.numLoops())
        return;
    m_loops = &loops;
    m_loopWrites.grow(loops.numLoops());
    for (BasicBlock* block : m_proc) {
        auto* innerMost = loops.innerMostLoopOf(block);
        if (!innerMost)
            continue;
        for (Value* value : *block) {
            HeapSet writes = writesOf(value);
            for (const HeapRange& range : writes.ranges()) {
                RecordedWrite write { static_cast<unsigned>(range.begin()), static_cast<unsigned>(range.end()), noID };
                for (auto* loop = innerMost; loop; loop = loops.innerMostOuterLoop(*loop)) {
                    LoopWrites& loopWrites = m_loopWrites[loop->index()];
                    if (loopWrites.writesEverything)
                        continue;
                    if (!canBeRecorded(range) || !record(loopWrites.writes, write, value->origin()))
                        loopWrites.writesEverything = true;
                }
            }
        }
    }
}

bool MemoryNumbering::inheritStateFromBeforeLoop(BasicBlock* header, MemoryState& state)
{
    if (!m_loops)
        return false;
    auto* loop = m_loops->headerOf(header);
    if (!loop || m_loopWrites[loop->index()].writesEverything)
        return false;
    const MemoryState* entryState = nullptr;
    for (BasicBlock* predecessor : header->predecessors()) {
        if (predecessor->index() >= m_memoryStateAtTail.size())
            return false;
        if (m_loops->belongsTo(predecessor, *loop))
            continue;
        const MemoryState& atTail = m_memoryStateAtTail[predecessor->index()];
        if (atTail.id == noID || (entryState && entryState->id != atTail.id))
            return false;
        entryState = &atTail;
    }
    if (!entryState || !entryState->epochOwner || !m_dominators->dominates(entryState->epochOwner, header))
        return false;
    MemoryState stateBeforeLoop = *entryState;
    unsigned id = m_lastMemoryStateID + 1;
    for (RecordedWrite& write : stateBeforeLoop.recordedWrites) {
        if (write.hasSingleHeap() && !hasRecordedOverlay(write))
            write.id = id;
    }
    for (RecordedWrite write : m_loopWrites[loop->index()].writes) {
        write.id = id;
        if (!record(stateBeforeLoop.recordedWrites, write, header->at(0)->origin()))
            return false;
    }
    m_lastMemoryStateID = id;
    stateBeforeLoop.id = id;
    stateBeforeLoop.owner = header;
    for (const RecordedWrite& write : stateBeforeLoop.recordedWrites) {
        if (write.id == id && write.hasSingleHeap())
            m_heapEpochs.overlay(write.begin, id);
    }
    state = WTF::move(stateBeforeLoop);
    return true;
}

bool MemoryNumbering::mergeSameEpochStates(BasicBlock* block, MemoryState& merged)
{
    if (block->numPredecessors() < 2)
        return false;
    unsigned numberOfPredecessors = 0;
    Vector<unsigned, 8> agreeingStateCount;
    auto findInMerged = [&](const RecordedWrite& write, size_t likelyIndex) {
        if (likelyIndex < merged.recordedWrites.size() && merged.recordedWrites[likelyIndex].hasSameHeapsAs(write))
            return likelyIndex;
        return merged.recordedWrites.findIf([&](const RecordedWrite& other) {
            return other.hasSameHeapsAs(write);
        });
    };
    for (BasicBlock* predecessor : block->predecessors()) {
        if (predecessor->index() >= m_memoryStateAtTail.size())
            return false;
        const MemoryState& atTail = m_memoryStateAtTail[predecessor->index()];
        if (atTail.id == noID)
            return false;
        if (!numberOfPredecessors++) {
            merged = atTail;
            agreeingStateCount.fill(1, merged.recordedWrites.size());
            continue;
        }
        if (merged.epoch != atTail.epoch)
            return false;
        for (size_t i = 0; i < atTail.recordedWrites.size(); ++i) {
            const RecordedWrite& write = atTail.recordedWrites[i];
            size_t index = findInMerged(write, i);
            if (index != notFound) {
                agreeingStateCount[index] += merged.recordedWrites[index].id == write.id;
                continue;
            }
            if (!record(merged.recordedWrites, write, block->at(0)->origin()))
                return false;
            agreeingStateCount.append(0);
        }
    }
    if (merged.epoch == noID || !merged.epochOwner || !m_dominators->dominates(merged.epochOwner, block))
        return false;
    merged.id = ++m_lastMemoryStateID;
    merged.owner = block;
    for (unsigned i = 0; i < merged.recordedWrites.size(); ++i) {
        RecordedWrite& write = merged.recordedWrites[i];
        if (agreeingStateCount[i] == numberOfPredecessors && (!write.hasSingleHeap() || hasRecordedOverlay(write)))
            continue;
        write.id = merged.id;
        if (write.hasSingleHeap())
            m_heapEpochs.overlay(write.begin, merged.id);
    }
    return true;
}

void MemoryNumbering::didWriteMemory(const HeapSet& writes, Origin origin)
{
    m_memoryState.id = ++m_lastMemoryStateID;
    m_memoryState.owner = m_block;
    for (const HeapRange& range : writes.ranges()) {
        RecordedWrite write { static_cast<unsigned>(range.begin()), static_cast<unsigned>(range.end()), m_memoryState.id };
        if (!canBeRecorded(range) || !record(m_memoryState.recordedWrites, write, origin)) {
            m_memoryState.beginEpoch(m_block);
            return;
        }
        if (write.hasSingleHeap())
            m_heapEpochs.overlay(write.begin, m_memoryState.id);
    }
}

unsigned MemoryNumbering::heapEpoch(unsigned heap)
{
    unsigned epoch = std::max(m_memoryState.epoch, m_heapEpochs.find(heap).value_or(noID));
    for (const RecordedWrite& write : m_memoryState.recordedWrites) {
        if (!write.hasSingleHeap() && write.begin <= heap && heap < write.end)
            epoch = std::max(epoch, write.id);
    }
    return epoch;
}

ValueKey MemoryNumbering::keyForLoad(Kind kind, Type type, MemoryValue* memory)
{
    v128_t bits;
    bits.u32x4[0] = memory->lastChild()->index();
    bits.u32x4[1] = static_cast<uint32_t>(memory->offset());
    if (memory->isLoad() && memory->readsMutability() == Mutability::Immutable)
        bits.u32x4[2] = noID;
    else if (memory->range().distance() == 1)
        bits.u32x4[2] = heapEpoch(static_cast<unsigned>(memory->range().begin()));
    else
        bits.u32x4[2] = m_memoryState.id;
    bits.u32x4[3] = 0;
    return ValueKey(kind, type, bits);
}

unsigned MemoryNumbering::PureCallKeyHash::hash(const PureCallKey& key)
{
    unsigned result = pairIntHash(intHash(key.id), key.type.hash());
    for (unsigned child : key.children)
        result = pairIntHash(result, child);
    return result;
}

MemoryNumbering::PureCallKey MemoryNumbering::keyForPureCall(const PatchpointValue* patchpoint)
{
    PureCallKey key { patchpoint->pureFunction->id, patchpoint->type(), { } };
    if (patchpoint->pureFunction->dependsOnChildren) {
        for (Value* child : patchpoint->children())
            key.children.append(child->index());
    }
    return key;
}

bool MemoryNumbering::isEqualLoad(const MemoryValue* load, const Value* other)
{
    const MemoryValue* otherMemory = other->as<MemoryValue>();
    return otherMemory
        && otherMemory->kind() == load->kind()
        && otherMemory->type() == load->type()
        && otherMemory->lastChild() == load->lastChild()
        && otherMemory->offset() == load->offset();
}

MemoryNumbering::Result MemoryNumbering::replaceIfRedundant(Value* current)
{
    Value* value = current->opcode() == Identity ? current->child(0) : current;
    if (value->opcode() == Identity || value->isConstant())
        return Result::Unchanged;
    if (PatchpointValue* pureCall = asPureCall(value))
        return replacePureCallIfRedundant(current, pureCall);

    MemoryValue* load = nullptr;
    ValueKey key = keyOf(value);
    if (!key) {
        if (value != current)
            return Result::Unchanged;
        MemoryValue* memory = current->as<MemoryValue>();
        if (!memory || memory->hasFence() || memory->isExotic()) {
            if (HeapSet writes = writesOf(current))
                didWriteMemory(writes, current->origin());
            return Result::Unchanged;
        }
        if (memory->isStore()) {
            didWriteMemory(memory->range(), memory->origin());
            if (memory->opcode() == Store)
                m_values.add(keyForLoad(Load, memory->child(0)->type(), memory), memory->child(0));
            return Result::Unchanged;
        }
        key = keyForLoad(memory->kind(), memory->type(), memory);
        load = memory;
    }

    std::optional<Value*> match = m_values.findOrAdd(key, value);
    if (!match || *match == value)
        return Result::Unchanged;
    if (value != current) {
        current->child(0) = *match;
        return Result::Replaced;
    }

    if (load && m_validates) {
        m_replacedLoads.append(ReplacedLoad {
            .load = current,
            .block = m_block,
            .replacement = *match,
            .address = load->lastChild(),
            .offset = load->offset(),
            .range = load->range(),
            .isImmutable = load->readsMutability() == Mutability::Immutable,
            .isForwardedStore = !isEqualLoad(load, *match),
        });
    }
    if (load) {
        if (std::optional<MovedLoad> moved = MovedLoad::of(m_proc, load))
            m_loadsToCheck.append({ *moved, current, m_block, load->lastChild(), *match });
    }
    current->replaceWithIdentity(*match);
    return Result::Replaced;
}

void MemoryNumbering::checkReplacedLoadsAtRunTime()
{
    for (const LoadToCheck& replaced : m_loadsToCheck) {
        size_t index = replaced.block->values().find(replaced.load);
        if (index == notFound)
            continue;
        replaced.block->values().insert(index + 1, createMovedLoadCheck(m_proc, replaced.moved, replaced.address, replaced.replacement));
    }
    m_loadsToCheck.shrink(0);
}

MemoryNumbering::Result MemoryNumbering::replacePureCallIfRedundant(Value* current, PatchpointValue* patchpoint)
{
    std::optional<Value*> match = m_pureCalls.findOrAdd(keyForPureCall(patchpoint), patchpoint);
    if (!match) {
        if (patchpoint->effects.writes)
            didWriteMemory(patchpoint->effects.writes, patchpoint->origin());
        return Result::Unchanged;
    }
    if (*match == patchpoint)
        return Result::Unchanged;
    if (current != patchpoint)
        current->child(0) = *match;
    else
        current->replaceWithIdentity(*match);
    return Result::Replaced;
}

void MemoryNumbering::validateReplacedLoads()
{
    IndexSet<BasicBlock*> reachable;
    reachable.addAll(m_proc.blocksInPreOrder());
    for (const ReplacedLoad& replaced : m_replacedLoads) {
        if (!reachable.contains(replaced.block))
            continue;
        Value* clobber = nullptr;
        if (isUnchangedSince(replaced, reachable, clobber))
            continue;
        dataLogLn("B3 VALIDATION FAILURE");
        dataLogLn("    Memory numbering replaced a load whose location may be written after the replacement.");
        dataLogLn("    Load: ", deepDump(m_proc, replaced.load), " in ", *replaced.block);
        dataLogLn("    Replacement: ", deepDump(m_proc, replaced.replacement));
        if (clobber)
            dataLogLn("    Clobbered by: ", deepDump(m_proc, clobber));
        else
            dataLogLn("    A path from the root reaches the load without its replacement.");
        dataLogLn("At time of failure:");
        dataLog(m_proc);
        CRASH();
    }
}

bool MemoryNumbering::isUnchangedSince(const ReplacedLoad& replaced, const IndexSet<BasicBlock*>& reachable, Value*& clobber)
{
    Value* replacement = replaced.replacement->foldIdentity();
    Value* address = replaced.address->foldIdentity();
    auto isSource = [&](Value* value) {
        if (!replaced.isForwardedStore && value == replaced.replacement)
            return true;
        if (value->opcode() != Store)
            return false;
        MemoryValue* store = value->as<MemoryValue>();
        return !store->hasFence()
            && store->child(0)->foldIdentity() == replacement
            && store->lastChild()->foldIdentity() == address
            && store->offset() == replaced.offset;
    };

    struct Position {
        BasicBlock* block;
        size_t index;
    };
    size_t loadIndex = replaced.block->values().find(replaced.load);
    RELEASE_ASSERT(loadIndex != notFound);

    IndexSet<BasicBlock*> entered;
    Vector<Position, 16> worklist;
    worklist.append(Position { replaced.block, loadIndex });
    while (!worklist.isEmpty()) {
        Position position = worklist.takeLast();
        bool didReachSource = false;
        for (size_t index = position.index; index--;) {
            Value* value = position.block->at(index);
            if (isSource(value)) {
                didReachSource = true;
                break;
            }
            if (replaced.isImmutable)
                continue;
            Effects effects = value->effects();
            if (effects.fence || effects.writes.overlaps(replaced.range)) {
                clobber = value;
                return false;
            }
        }
        if (didReachSource)
            continue;
        if (position.block == m_proc.at(0))
            return false;
        for (BasicBlock* predecessor : position.block->predecessors()) {
            bool isLiveEdge = predecessor->successors().containsIf([&](const FrequentedBlock& successor) {
                return successor.block() == position.block;
            });
            if (isLiveEdge && reachable.contains(predecessor) && entered.add(predecessor))
                worklist.append(Position { predecessor, predecessor->size() });
        }
    }
    return true;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
