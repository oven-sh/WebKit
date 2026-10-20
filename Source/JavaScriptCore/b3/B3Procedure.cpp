/*
 * Copyright (C) 2015-2023 Apple Inc. All rights reserved.
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
#include "B3Procedure.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirStackSlotKind.h"
#include "B3BackwardsCFG.h"
#include "B3BackwardsDominators.h"
#include "B3BasicBlockUtils.h"
#include "B3BlockRarity.h"
#include "B3CFG.h"
#include "B3Common.h"
#include "B3CompilationScratch.h"
#include "B3ConstPtrValue.h"
#include "B3DataSection.h"
#include "B3Dominators.h"
#include "B3EstimateFrequencies.h"
#include "B3MemoryValue.h"
#include "B3NaturalLoops.h"
#include "B3OriginDump.h"
#include "B3ProcedureInlines.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include "JITOpaqueByproducts.h"
#include <wtf/GraphOrdering.h>
#include <wtf/ListDump.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace B3 {

WTF_MAKE_SEQUESTERED_ARENA_ALLOCATED_IMPL(Procedure);

Procedure::Procedure(bool usesSIMD)
    : Procedure(Policy::fromProcessOptions(), nullptr, usesSIMD)
{
}

Procedure::Procedure(const Policy& policy, CompilationScratch* scratch, bool usesSIMD)
    : m_arena(scratch)
    , m_cfg(new CFG(*this))
    , m_lastPhaseName("initial")
    , m_byproducts(makeUnique<OpaqueByproducts>())
    , m_policy(policy)
    , m_scratch(scratch)
{
    m_policy.applyDisabledSwitches();
    for (Opcode opcode : { Nop, Identity, Jump, Oops, Phi })
        m_opcodesEverAdded.set(opcode);
    const AddressingPolicy& addressing = m_policy.addressing;
    RELEASE_ASSERT(!addressing.isPositionIndependent || (addressing.dataSectionSpace != runtimeFunctionSpace && addressing.jumpTableSpace != runtimeFunctionSpace && addressing.dataSectionSpace != addressing.jumpTableSpace));
    if (usesSIMD)
        setUsesSIMD();
    // Initialize all our fields before constructing Air::Code since
    // it looks into our fields.
    m_code = std::unique_ptr<Air::Code>(new Air::Code(*this));
    m_code->setNumEntrypoints(m_numEntrypoints);
    setOptLevel(m_policy.upstream.optLevel);
    m_policy.air.registers.pinned.forEach([&](Reg reg) {
        pinRegister(reg);
    });
}

Procedure::~Procedure()
{
    if (m_scratch && m_heaps)
        m_scratch->giveBackHeaps(WTF::move(m_heaps));
}

void Procedure::createHeaps()
{
    m_heaps = m_scratch ? m_scratch->takeHeaps() : makeUnique<AbstractHeapRepository>();
    m_heaps->attachProcedure(*this);
}

void Procedure::fail(FailureKind kind, Origin origin)
{
    if (!m_report.failure)
        m_report.failure = Failure { kind, origin };
}

void Procedure::addRemark(const Remark& remark, uint32_t count)
{
    ASSERT((remark.reason == GaveUpReason::LimitReached) == remark.limit.has_value());
    if (std::optional<Counter> counter = counterOf(remark.kind))
        m_report.counters[static_cast<size_t>(*counter)] += count;
    if (m_policy.collectsRemarks)
        m_report.remarks.append(remark);
}

void Procedure::printOrigin(PrintStream& out, Origin origin) const
{
    if (m_originPrinter)
        m_originPrinter->run(out, origin);
    else
        out.print(origin);
}

BasicBlock* Procedure::addBlock(double frequency)
{
    m_blocksInPreOrder.shrink(0);
    m_blocksInPostOrder.shrink(0);
    m_blockRarity = nullptr;
    m_areFrequenciesCurrent = false;
    std::unique_ptr<BasicBlock> block(new (arenaOrNull()) BasicBlock(m_blocks.size(), frequency));
    block->m_isInArena = !!arenaOrNull();
    BasicBlock* result = block.get();
    m_blocks.append(WTF::move(block));
    return result;
}

Air::StackSlot* Procedure::addStackSlot(uint64_t byteSize)
{
    return m_code->addStackSlot(byteSize, Air::StackSlotKind::Locked);
}

Variable* Procedure::addVariable(Type type)
{
    return m_variables.addNew(type); 
}

Type Procedure::addTuple(Vector<Type>&& types)
{
    Type result = Type::tupleFromIndex(m_tuples.size());
    m_tuples.append(WTF::move(types));
    ASSERT(result.isTuple());
    return result;
}

bool Procedure::isValidTuple(Type tuple) const
{
    return tuple.tupleIndex() < m_tuples.size();
}

const Vector<Type>& Procedure::tupleForType(Type tuple) const
{
    return m_tuples[tuple.tupleIndex()];
}

Value* Procedure::clone(Value* value)
{
    SparseCollection<Value>::Pointer clone(value->cloneImpl(arenaOrNull()));
    m_opcodesEverAdded.set(value->opcode());
    clone->m_index = UINT_MAX;
    clone->owner = nullptr;
    Value* result = m_values.add(WTF::move(clone));
    m_knownProperties.copy(value, result);
    return result;
}

Value* Procedure::addIntConstant(Origin origin, Type type, int64_t value)
{
    switch (type.kind()) {
    case Int32:
        return add<Const32Value>(origin, static_cast<int32_t>(value));
    case Int64:
        return add<Const64Value>(origin, value);
    case Double:
        return add<ConstDoubleValue>(origin, static_cast<double>(value));
    case Float:
        return add<ConstFloatValue>(origin, static_cast<float>(value));
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return nullptr;
    }
}

Value* Procedure::addIntConstant(Value* likeValue, int64_t value)
{
    return addIntConstant(likeValue->origin(), likeValue->type(), value);
}

Value* Procedure::addConstant(Origin origin, Type type, uint64_t bits)
{
    switch (type.kind()) {
    case Int32:
        return add<Const32Value>(origin, static_cast<int32_t>(bits));
    case Int64:
        return add<Const64Value>(origin, bits);
    case Float:
        return add<ConstFloatValue>(origin, std::bit_cast<float>(static_cast<int32_t>(bits)));
    case Double:
        return add<ConstDoubleValue>(origin, std::bit_cast<double>(bits));
    case V128:
        RELEASE_ASSERT(!bits);
        return addConstant(origin, type, v128_t { });
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return nullptr;
    }
}

Value* Procedure::addConstant(Origin origin, Type type, v128_t bits)
{
    switch (type.kind()) {
    case V128:
        return add<Const128Value>(origin, bits);
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return nullptr;
    }
}

Value* Procedure::addBottom(Origin origin, Type type)
{
    if (type.isTuple())
        return add<BottomTupleValue>(origin, type);
    return addConstant(origin, type, 0);
}

Value* Procedure::addBottom(Value* value)
{
    return addBottom(value->origin(), value->type());
}

Value* Procedure::addBoolConstant(Origin origin, TriState triState)
{
    int32_t value = 0;
    switch (triState) {
    case TriState::False:
        value = 0;
        break;
    case TriState::True:
        value = 1;
        break;
    case TriState::Indeterminate:
        return nullptr;
    }

    return addIntConstant(origin, Int32, value);
}

void Procedure::resetValueOwners()
{
    for (BasicBlock* block : *this) {
        for (Value* value : *block)
            value->owner = block;
    }
}

void Procedure::resetReachability()
{
    m_blocksInPreOrder.shrink(0);
    m_blocksInPostOrder.shrink(0);
    recomputePredecessors(m_blocks);
    
    // The common case is that this does not find any dead blocks.
    bool foundDead = false;
    for (auto& block : m_blocks) {
        if (isBlockDead(block.get())) {
            foundDead = true;
            break;
        }
    }
    if (!foundDead)
        return;
    
    IndexSet<Value*> deadPhis;
    for (auto& block : m_blocks) {
        if (!isBlockDead(block.get()))
            continue;
        for (Value* value : *block) {
            if (value->opcode() == Phi)
                deadPhis.add(value);
        }
    }
    if (!deadPhis.isEmpty()) {
        for (Value* value : values()) {
            if (UpsilonValue* upsilon = value->as<UpsilonValue>()) {
                if (deadPhis.contains(upsilon->phi()))
                    upsilon->replaceWithNop();
            }
        }
    }
    
    for (auto& block : m_blocks) {
        if (isBlockDead(block.get())) {
            for (Value* value : *block)
                deleteValue(value);
            block = nullptr;
        }
    }
}

void Procedure::invalidateCFG()
{
    m_blocksInPreOrder.shrink(0);
    m_blocksInPostOrder.shrink(0);
    m_dominators = nullptr;
    m_naturalLoops = nullptr;
    m_backwardsCFG = nullptr;
    m_backwardsDominators = nullptr;
    m_blockRarity = nullptr;
    m_areFrequenciesCurrent = false;
}

static void dumpKnownProperties(const Procedure& proc, PrintStream& out)
{
    bool didPrintHeading = false;
    auto printHeadingOnce = [&] {
        if (!std::exchange(didPrintHeading, true))
            out.print(tierName, "Known properties:\n");
    };
    for (Value* value : proc.values()) {
        std::optional<IntRange> range = proc.knownRange(value);
        uint32_t dereferenceableBytes = proc.knownDereferenceableBytes(value);
        if (!range && !dereferenceableBytes)
            continue;
        printHeadingOnce();
        out.print(tierName, "    ", *value, ":");
        if (range)
            out.print(" range = ", *range);
        if (dereferenceableBytes)
            out.print(" dereferenceableBytes = ", dereferenceableBytes);
        out.print("\n");
    }
    proc.knownInductions().forEach([&](const InductionRecord& record) {
        printHeadingOnce();
        out.print(tierName, "    ", *record.phi, ": induction, next = ", *record.next, ", step = ", record.step);
        if (record.test)
            out.print(", test = ", *record.test);
        if (record.doesNotWrap)
            out.print(", doesNotWrap");
        if (record.relationHoldsOnEntry)
            out.print(", relationHoldsOnEntry");
        out.print("\n");
    });
}

static void dumpReport(const Procedure& proc, PrintStream& out)
{
    auto printOrigin = [&](Origin origin) {
        if (origin)
            out.print(" at ", OriginDump(&proc, origin));
        out.print("\n");
    };

    const CompilationReport& report = proc.report();
    if (report.failure) {
        out.print(tierName, "Failure: ", report.failure->kind);
        printOrigin(report.failure->origin);
    }
    if (report.remarks.isEmpty())
        return;
    out.print(tierName, "Remarks:\n");
    for (const Remark& remark : report.remarks) {
        out.print(tierName, "    ", remark.kind);
        if (remark.reason != GaveUpReason::None)
            out.print(", ", remark.reason);
        if (remark.limit)
            out.print(", ", *remark.limit);
        printOrigin(remark.origin);
    }
}

void Procedure::dump(PrintStream& out) const
{
    out.print("Opt Level: ", optLevel(), "\n");
    IndexSet<Value*> valuesInBlocks;
    for (BasicBlock* block : *this) {
        out.print(deepDump(*this, block));
        valuesInBlocks.addAll(*block);
    }
    bool didPrint = false;
    for (Value* value : values()) {
        if (valuesInBlocks.contains(value))
            continue;

        if (!didPrint) {
            dataLog(tierName, "Orphaned values:\n");
            didPrint = true;
        }
        dataLog(tierName, "    ", deepDump(*this, value), "\n");
    }
    if (hasQuirks())
        out.print(tierName, "Has Quirks: True\n");
    if (variables().size()) {
        out.print(tierName, "Variables:\n");
        for (Variable* variable : variables())
            out.print(tierName, "    ", deepDump(variable), "\n");
    }
    if (stackSlots().size()) {
        out.print(tierName, "Stack slots:\n");
        for (Air::StackSlot* slot : stackSlots())
            out.print(tierName, "    ", pointerDump(slot), ": ", deepDump(slot), "\n");
    }
    if (auto policyDump = toUTF8CString(m_policy); policyDump.length())
        out.print(tierName, "Policy: ", policyDump, "\n");
    dumpKnownProperties(*this, out);
    dumpReport(*this, out);
    if (m_byproducts->count())
        out.print(*m_byproducts);
}

template<GraphOrder order>
static void appendBlocksInOrder(Procedure& proc, Vector<BasicBlock*>& output)
{
    constexpr uintptr_t isSecondVisit = 1;
    static_assert(alignof(BasicBlock) > isSecondVisit);
    output.reserveCapacity(proc.size());
    Vector<uint8_t, 512> visited;
    visited.fill(0, proc.size());
    uint8_t* isVisited = visited.mutableSpan().data();
    Vector<uintptr_t, 128> stack;
    stack.append(std::bit_cast<uintptr_t>(proc[0]));
    while (!stack.isEmpty()) {
        uintptr_t entry = stack.takeLast();
        if (entry & isSecondVisit) {
            output.unsafeAppendWithoutCapacityCheck(std::bit_cast<BasicBlock*>(entry - isSecondVisit));
            continue;
        }
        BasicBlock* block = std::bit_cast<BasicBlock*>(entry);
        if (isVisited[block->index()])
            continue;
        isVisited[block->index()] = 1;
        if (order == GraphOrder::PreOrder)
            output.unsafeAppendWithoutCapacityCheck(block);
        else
            stack.append(entry | isSecondVisit);
        for (const FrequentedBlock& successor : block->successors()) {
            if (!isVisited[successor.block()->index()])
                stack.append(std::bit_cast<uintptr_t>(successor.block()));
        }
    }
}

void Procedure::didReorderSuccessors()
{
    m_blocksInPreOrder.shrink(0);
    m_blocksInPostOrder.shrink(0);
}

const Vector<BasicBlock*>& Procedure::cachedBlocksInPreOrder()
{
    if (m_blocksInPreOrder.isEmpty())
        appendBlocksInOrder<GraphOrder::PreOrder>(*this, m_blocksInPreOrder);
    else if (shouldValidateIRAtEachPhase(*this))
        RELEASE_ASSERT(m_blocksInPreOrder == blocksInPreOrder());
    return m_blocksInPreOrder;
}

const Vector<BasicBlock*>& Procedure::cachedBlocksInPostOrder()
{
    if (m_blocksInPostOrder.isEmpty())
        appendBlocksInOrder<GraphOrder::PostOrder>(*this, m_blocksInPostOrder);
    else if (shouldValidateIRAtEachPhase(*this))
        RELEASE_ASSERT(m_blocksInPostOrder == blocksInPostOrder());
    return m_blocksInPostOrder;
}

Vector<BasicBlock*> Procedure::blocksInPreOrder()
{
    Vector<BasicBlock*> result;
    result.reserveInitialCapacity(size());
    appendNodesInOrder(cfg(), GraphOrder::PreOrder, result);
    return result;
}

Vector<BasicBlock*> Procedure::blocksInPostOrder()
{
    Vector<BasicBlock*> result;
    result.reserveInitialCapacity(size());
    appendNodesInOrder(cfg(), GraphOrder::PostOrder, result);
    return result;
}

void Procedure::deleteVariable(Variable* variable)
{
    m_variables.remove(variable);
}

void Procedure::deleteAllVariables()
{
    m_variables.clearAll();
}

void Procedure::deleteValue(Value* value)
{
    m_knownProperties.remove(value);
    m_knownInductions.willDelete(value, m_values);
    m_values.remove(value);
}

void Procedure::setKnownRange(Value* value, IntRange range)
{
    ASSERT(value->type().isInt());
    ASSERT(range.isSubsetOf(IntRange::top(value->type())));
    if (m_policy.knownProperties.trustsKnownRanges)
        m_knownProperties.setRange(value, range);
}

void Procedure::setKnownDereferenceableBytes(Value* value, uint32_t bytes)
{
    ASSERT(value->type() == pointerType());
    if (m_policy.knownProperties.trustsKnownDereferenceableBytes)
        m_knownProperties.setDereferenceableBytes(value, bytes);
}

bool Procedure::isKnownDereferenceable(const MemoryValue* memory) const
{
    return isKnownDereferenceable(memory->lastChild(), memory->offset(), memory->accessByteSize());
}

bool Procedure::isKnownDereferenceable(const Value* address, int64_t offset, size_t byteSize) const
{
    if (m_knownProperties.isEmpty())
        return false;
    uint32_t bytes = m_knownProperties.dereferenceableBytes(address->foldIdentity());
    return offset >= 0 && offset + static_cast<int64_t>(byteSize) <= static_cast<int64_t>(bytes);
}

Effects Procedure::effectsOf(const Value* value) const
{
    Effects effects = value->effects();
    if (!effects.controlDependent)
        return effects;
    const MemoryValue* memory = value->as<MemoryValue>();
    if (memory && memory->isLoad() && !memory->traps() && isKnownDereferenceable(memory))
        effects.controlDependent = false;
    return effects;
}

void Procedure::setKnownInduction(const InductionRecord& record)
{
    ASSERT(record.phi->opcode() == Phi && record.phi->type().isInt());
    ASSERT(record.next && record.next->type() == record.phi->type());
    ASSERT(record.step);
    if (m_policy.knownProperties.trustsKnownInductions)
        m_knownInductions.add(record);
}

void Procedure::setKnownInduction(const DemotedInduction& demoted)
{
    ASSERT(demoted.variable->type().isInt());
    ASSERT(demoted.header && demoted.next && demoted.next->type() == demoted.variable->type());
    ASSERT(demoted.step);
    if (m_policy.knownProperties.trustsKnownInductions)
        m_knownInductions.addDemoted(demoted);
}

void Procedure::deleteOrphans()
{
    size_t numberOfValuesInBlocks = 0;
    for (BasicBlock* block : *this)
        numberOfValuesInBlocks += block->size();
    if (numberOfValuesInBlocks == m_values.numberOfEntries())
        return;
    IndexSet<Value*> valuesInBlocks;
    for (BasicBlock* block : *this)
        valuesInBlocks.addAll(*block);

    // Since this method is not on any hot path, we do it conservatively: first a pass to
    // identify the values to be removed, and then a second pass to remove them. This avoids any
    // risk of the value iteration being broken by removals.
    Vector<Value*, 16> toRemove;
    for (Value* value : values()) {
        if (!valuesInBlocks.contains(value))
            toRemove.append(value);
        else if (UpsilonValue* upsilon = value->as<UpsilonValue>()) {
            if (!valuesInBlocks.contains(upsilon->phi()))
                upsilon->replaceWithNop();
        }
    }

    for (Value* value : toRemove)
        deleteValue(value);
}

Dominators& Procedure::dominators()
{
    if (!m_dominators)
        m_dominators = makeUnique<Dominators>(*this);
    return *m_dominators;
}

NaturalLoops& Procedure::naturalLoops()
{
    if (!m_naturalLoops)
        m_naturalLoops = makeUnique<NaturalLoops>(*this);
    return *m_naturalLoops;
}

BackwardsCFG& Procedure::backwardsCFG()
{
    if (!m_backwardsCFG)
        m_backwardsCFG = makeUnique<BackwardsCFG>(*this);
    return *m_backwardsCFG;
}

BackwardsDominators& Procedure::backwardsDominators()
{
    if (!m_backwardsDominators)
        m_backwardsDominators = makeUnique<BackwardsDominators>(*this);
    return *m_backwardsDominators;
}

BlockRarity& Procedure::blockRarity()
{
    if (!m_blockRarity)
        m_blockRarity = makeUnique<BlockRarity>(*this);
    return *m_blockRarity;
}

void Procedure::updateFrequencies()
{
    if (m_areFrequenciesCurrent || !m_policy.optimization.estimatesFrequencies)
        return;
    estimateFrequencies(*this);
    m_blockRarity = nullptr;
    m_areFrequenciesCurrent = true;
}

void Procedure::addFastConstant(const ValueKey& constant)
{
    RELEASE_ASSERT(constant.isConstant());
    m_fastConstants.add(constant);
}

bool Procedure::isFastConstant(const ValueKey& constant)
{
    if (!constant)
        return false;
    return m_fastConstants.contains(constant);
}

void* Procedure::addDataSection(size_t size)
{
    if (!size)
        return nullptr;
    std::unique_ptr<DataSection> dataSection = makeUnique<DataSection>(size);
    void* result = dataSection->data();
    m_byproducts->add(WTF::move(dataSection));
    if (m_policy.addressing.isPositionIndependent)
        m_dataSections.append(DataSectionRecord { result, size });
    return result;
}

Value* Procedure::addDataSectionAddress(Origin origin, const void* dataSection)
{
    if (!m_policy.addressing.isPositionIndependent)
        return add<ConstPtrValue>(origin, dataSection);
    size_t index = m_dataSections.findIf([&](const DataSectionRecord& record) {
        return record.data == dataSection;
    });
    RELEASE_ASSERT(index != notFound);
    return add<RelocatableValue>(origin, RelocationSymbol { m_policy.addressing.dataSectionSpace, static_cast<uint32_t>(index) });
}

unsigned Procedure::addJumpTable(unsigned numberOfEntries)
{
    RELEASE_ASSERT(m_policy.addressing.isPositionIndependent);
    auto record = makeUnique<JumpTableRecord>();
    record->targetOffsets.fill(0, numberOfEntries);
    m_jumpTables.append(WTF::move(record));
    return m_jumpTables.size() - 1;
}

Value* Procedure::addJumpTableAddress(Origin origin, unsigned jumpTable)
{
    RELEASE_ASSERT(jumpTable < m_jumpTables.size());
    return add<RelocatableValue>(origin, RelocationSymbol { m_policy.addressing.jumpTableSpace, jumpTable });
}

unsigned Procedure::callArgAreaSizeInBytes() const
{
    return code().callArgAreaSizeInBytes();
}

void Procedure::requestCallArgAreaSizeInBytes(unsigned size)
{
    code().requestCallArgAreaSizeInBytes(size);
}

void Procedure::pinRegister(Reg reg)
{
    code().pinRegister(reg);
}

void Procedure::setOptLevel(unsigned optLevel)
{
    m_optLevel = optLevel;
    code().setOptLevel(optLevel);
}

unsigned Procedure::frameSize() const
{
    return code().frameSize();
}

RegisterAtOffsetList Procedure::calleeSaveRegisterAtOffsetList() const
{
    return code().calleeSaveRegisterAtOffsetList();
}

Value* Procedure::addValueImpl(Value* value)
{
    m_opcodesEverAdded.set(value->opcode());
    return m_values.add(SparseCollection<Value>::Pointer(value));
}

void Procedure::setBlockOrderImpl(Vector<BasicBlock*>& blocks)
{
    IndexSet<BasicBlock*> blocksSet;
    blocksSet.addAll(blocks);

    for (BasicBlock* block : *this) {
        if (!blocksSet.contains(block))
            blocks.append(block);
    }

    // Place blocks into this's block list by first leaking all of the blocks and then readopting
    // them.
    for (auto& entry : m_blocks)
        entry.release();

    m_blocks.resize(blocks.size());
    for (unsigned i = 0; i < blocks.size(); ++i) {
        BasicBlock* block = blocks[i];
        block->m_index = i;
        m_blocks[i] = std::unique_ptr<BasicBlock>(block);
    }
}

void Procedure::setWasmBoundsCheckGenerator(RefPtr<WasmBoundsCheckGenerator> generator)
{
    code().setWasmBoundsCheckGenerator(generator);
}

RegisterSet Procedure::mutableGPRs()
{
    return code().mutableGPRs();
}

void Procedure::setNumEntrypoints(unsigned numEntrypoints)
{
    m_numEntrypoints = numEntrypoints;
    m_code->setNumEntrypoints(numEntrypoints);
}

void Procedure::freeUnneededB3ValuesAfterLowering()
{
    m_blocksInPreOrder.clear();
    m_blocksInPostOrder.clear();
    m_knownInductions.clear();
    m_blockRarity = nullptr;
    // We cannot clear m_stackSlots() or m_tuples here, as they are unfortunately modified and read respectively by Air.
    m_variables.clearAll();
    m_blocks.clear();
    m_cfg = nullptr;
    m_dominators = nullptr;
    m_naturalLoops = nullptr;
    m_backwardsCFG = nullptr;
    m_backwardsDominators = nullptr;
    m_fastConstants.clear();

    if (m_code->shouldPreserveB3Origins())
        return;
    if (m_policy.allocation.keepsValuesAfterLowering)
        return;

    m_knownProperties.clear();
    BitVector valuesToPreserve;
    valuesToPreserve.ensureSize(m_values.size());
    for (Value* value : m_values) {
        switch (value->opcode()) {
        // Ideally we would also be able to get rid of all of those.
        // But Air currently relies on these origins being preserved, see https://bugs.webkit.org/show_bug.cgi?id=194040
        case WasmBoundsCheck:
            valuesToPreserve.quickSet(value->index());
            break;
        case CCall:
        case Patchpoint:
        case CheckAdd:
        case CheckSub:
        case CheckMul:
        case Check:
            valuesToPreserve.quickSet(value->index());
            for (Value* child : value->children())
                valuesToPreserve.quickSet(child->index());
            break;
        default:
            break;
        }
    }
    for (Value* value : m_values) {
        if (!valuesToPreserve.quickGet(value->index()))
            m_values.remove(value);
    }
    m_values.packIndices();
}

void Procedure::setShouldDumpIR()
{
    m_shouldDumpIR = true;
    m_code->forcePreservationOfB3Origins();
}

void Procedure::setNeedsPCToOriginMap()
{ 
    m_needsPCToOriginMap = true;
    m_code->forcePreservationOfB3Origins();
}

void Procedure::setIonGraphPasses(Ref<JSON::Array>&& array)
{
    m_ionGraphPasses = array.get();
    m_code->setIonGraphPasses(WTF::move(array));
}

void Procedure::appendIonGraphPass(ASCIILiteral passName)
{
    auto pass = JSON::Object::create();
    pass->setString("name"_s, makeString("B3: "_s, passName));
    {
        auto ionGraph = JSON::Object::create();
        auto ionBlocks = JSON::Array::create();
        ionGraph->setArray("blocks"_s, ionBlocks);

        for (auto* block : *this) {
            if (!block)
                continue;

            auto ionBlock = JSON::Object::create();
            auto attributes = JSON::Array::create();
            auto predecessors = JSON::Array::create();
            auto successors = JSON::Array::create();
            auto instructions = JSON::Array::create();

            for (auto* value : *block) {
                auto instruction = JSON::Object::create();
                auto inputs = JSON::Array::create();

                for (auto* child : value->children())
                    inputs->pushInteger(child->index());

                String type;
                {
                    StringPrintStream stream;
                    if (value->type().isTuple())
                        stream.print(listDump(tupleForType(value->type())));
                    else
                        stream.print(value->type());
                    type = stream.toString();
                }

                StringPrintStream stream;
                stream.print(value->kind(), "("_s);
                CommaPrinter comma;
                for (auto* child : value->children())
                    stream.print(comma, child->kind(), "#"_s, child->index());
                value->dumpMeta(comma, stream);
                auto effects = value->effects();
                {
                    auto string = toUTF8CString(effects);
                    if (string.length())
                        stream.print(comma, string);
                }
                if (auto origin = value->origin())
                    stream.print(comma, OriginDump(this, origin));
                stream.print(")"_s);

                if (effects.terminal) {
                    stream.print(" -> "_s);
                    value->dumpSuccessors(block, stream);
                }

                instruction->setInteger("ptr"_s, value->index() + 1);
                instruction->setInteger("id"_s, value->index());
                instruction->setString("opcode"_s, stream.toString());
                instruction->setArray("attributes"_s, JSON::Array::create());
                instruction->setArray("inputs"_s, WTF::move(inputs));
                instruction->setArray("uses"_s, JSON::Array::create());
                instruction->setArray("memInputs"_s, JSON::Array::create());
                instruction->setString("type"_s, WTF::move(type));

                instructions->pushObject(WTF::move(instruction));
            }

            for (auto predecessor : block->predecessors())
                predecessors->pushInteger(predecessor->index());

            for (auto successor : block->successors())
                successors->pushInteger(successor.block()->index());

            ionBlock->setInteger("ptr"_s, block->index() + 1);
            ionBlock->setInteger("id"_s, block->index());
            ionBlock->setInteger("loopDepth"_s, 0);
            ionBlock->setArray("attributes"_s, JSON::Array::create());
            ionBlock->setArray("predecessors"_s, WTF::move(predecessors));
            ionBlock->setArray("successors"_s, WTF::move(successors));
            ionBlock->setArray("instructions"_s, WTF::move(instructions));
            ionBlocks->pushObject(ionBlock);
        }

        pass->setObject("mir"_s, WTF::move(ionGraph)); // MIR stands for SpiderMonkey's middle-level IR.
    }
    {
        auto ionGraph = JSON::Object::create();
        ionGraph->setArray("blocks"_s, JSON::Array::create());
        pass->setObject("lir"_s, WTF::move(ionGraph)); // LIR stands for SpiderMonkey's low-level IR.
    }
    m_ionGraphPasses->pushObject(pass);
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
