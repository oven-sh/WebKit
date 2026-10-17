/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3ProcedureFingerprint.h"

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "AirCode.h"
#include "B3ArgumentRegValue.h"
#include "B3BasicBlockInlines.h"
#include "B3CCallValue.h"
#include "B3Const32Value.h"
#include "B3Const64Value.h"
#include "B3ConstDoubleValue.h"
#include "B3ConstFloatValue.h"
#include "B3ExtractValue.h"
#include "B3FenceValue.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3SwitchValue.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include "B3VariableValue.h"

namespace JSC { namespace B3 {

static_assert(std::has_unique_object_representations_v<RegisterSet>);
static_assert(!(sizeof(RegisterSet) % sizeof(uint32_t)));

void ProcedureFingerprint::append(const HeapRange& range)
{
    append64(range.begin());
    append64(range.end());
}

void ProcedureFingerprint::append(const Effects& effects)
{
    append(static_cast<uint32_t>(effects.terminal) | effects.exitsSideways << 1 | effects.controlDependent << 2 | effects.writesLocalState << 3 | effects.readsLocalState << 4 | effects.readsPinned << 5 | effects.writesPinned << 6 | effects.fence << 7 | effects.isInvalid() << 8);
    append(effects.writes);
    append(effects.reads);
}

void ProcedureFingerprint::append(const RegisterSet& set)
{
    std::array<uint32_t, sizeof(RegisterSet) / sizeof(uint32_t)> words = std::bit_cast<decltype(words)>(set);
    for (uint32_t word : words)
        append(word);
}

void ProcedureFingerprint::append(const ValueRep& rep)
{
    append(rep.kind());
    switch (rep.kind()) {
    case ValueRep::Register:
    case ValueRep::LateRegister:
        append(rep.reg().index());
        break;
    case ValueRep::Stack:
        append(rep.offsetFromFP());
        break;
    case ValueRep::StackArgument:
        append(rep.offsetFromSP());
        break;
    case ValueRep::Constant:
        append64(rep.value());
        break;
    default:
        break;
    }
}

void ProcedureFingerprint::appendStackmap(StackmapValue* stackmap, std::span<StackmapGenerator* const> distinguishedGenerators)
{
    RefPtr<StackmapGenerator> generator = stackmap->generator();
    size_t distinguished = std::ranges::find(distinguishedGenerators, generator.get()) - distinguishedGenerators.begin();
    size_t first = findGenerator(generator.get());
    append(generator ? distinguished : std::numeric_limits<uint32_t>::max());
    append(first == notFound ? m_generators.size() : first);
    m_generators.append(WTF::move(generator));
    reserve(maxWordsInValueWithoutLists + maxWordsInRep * stackmap->reps().size());
    append(stackmap->reps().size());
    for (auto& rep : stackmap->reps())
        append(rep);
    append(stackmap->earlyClobbered());
    append(stackmap->lateClobbered());
    append(stackmap->m_usedRegisters);
}

bool ProcedureFingerprint::appendValue(Value* value, std::span<StackmapGenerator* const> distinguishedGenerators)
{
    Kind kind = value->kind();
    reserve(maxWordsInValueWithoutLists + value->numChildren());
    append(value->index());
    append(static_cast<uint32_t>(kind.opcode()) | kind.isChill() << 16 | kind.traps() << 17 | kind.isCloningForbidden() << 18);
    append(value->type().hash());
    append(value->numChildren());
    for (Value* child : value->children())
        append(child->index());
    switch (kind.opcode()) {
    case Const32:
        append(value->asInt32());
        return true;
    case Const64:
        append64(value->asInt64());
        return true;
    case ConstDouble:
        append64(std::bit_cast<uint64_t>(value->asDouble()));
        return true;
    case ConstFloat:
        append(std::bit_cast<uint32_t>(value->asFloat()));
        return true;
    case ArgumentReg:
        append(value->as<ArgumentRegValue>()->argumentReg().index());
        return true;
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
    case Store8:
    case Store16:
    case Store: {
        auto* memory = value->as<MemoryValue>();
        append(memory->offset());
        append(memory->range());
        append(memory->fenceRange());
        append(static_cast<uint32_t>(memory->readsMutability()) | memory->controlDependent() << 1);
        return true;
    }
    case Get:
    case Set:
        append(value->as<VariableValue>()->variable()->index());
        return true;
    case Upsilon:
        append(value->as<UpsilonValue>()->phi()->index());
        return true;
    case Switch:
        reserve(2 * value->as<SwitchValue>()->numCaseValues());
        for (int64_t caseValue : value->as<SwitchValue>()->caseValues())
            append64(caseValue);
        return true;
    case Extract:
        append(value->as<ExtractValue>()->index());
        return true;
    case Fence:
        append(value->as<FenceValue>()->read);
        append(value->as<FenceValue>()->write);
        return true;
    case CCall:
        append(value->as<CCallValue>()->effects);
        return true;
    case CheckAdd:
    case CheckSub:
    case CheckMul:
    case Check:
        appendStackmap(value->as<StackmapValue>(), distinguishedGenerators);
        return true;
    case Patchpoint: {
        auto* patchpoint = value->as<PatchpointValue>();
        appendStackmap(patchpoint, distinguishedGenerators);
        append(patchpoint->effects);
        append64(patchpoint->pureFunctionKey);
        append(patchpoint->pureFunctionOfChildrenKey);
        reserve(2 + maxWordsInRep * patchpoint->resultConstraints.size());
        append(patchpoint->resultConstraints.size());
        for (auto& rep : patchpoint->resultConstraints)
            append(rep);
        append(patchpoint->numGPScratchRegisters | patchpoint->numFPScratchRegisters << 8);
        return true;
    }
    default:
        return value->m_adjacencyListOffset == sizeof(Value);
    }
}

bool ProcedureFingerprint::compute(Procedure& proc, std::span<StackmapGenerator* const> distinguishedGenerators, uint32_t clientBits)
{
    m_words.grow(m_words.capacity());
    m_size = 0;
    m_generators.shrink(0);
    Air::Code& code = proc.code();
    if (!proc.m_fastConstants.isEmpty() || proc.m_numEntrypoints != 1 || proc.m_needsPCToOriginMap || proc.m_shouldDumpIR || proc.m_isWasm || proc.m_usesSIMD)
        return false;
    if (code.size() || !code.stackSlots().isEmpty() || !code.specials().isEmpty() || code.numTmps(GP) || code.numTmps(FP) || code.m_wasmBoundsCheckGenerator || code.m_disassembler)
        return false;

    static_assert(5 * sizeof(RegisterSet) / sizeof(uint32_t) + 32 <= maxWordsInValueWithoutLists);
    reserve(256);
    append(clientBits);
    append(proc.m_optLevel);
    append(static_cast<uint32_t>(proc.m_positionIndependent) | proc.m_keepsDataInCode << 1 | proc.m_materializesConstantsByFrequency << 2 | proc.m_rotatesLoops << 3 | proc.m_namesPinnedArgumentRegistersDirectly << 4 | proc.m_reachabilityIsFresh << 5
        | proc.m_keepsDominatorsThroughReset << 6 | proc.m_needsUsedRegisters << 7 | proc.m_hasQuirks << 8 | proc.m_usesColdCCall << 9 | proc.m_usesShuffle << 10 | proc.m_usesEntrySwitch << 11 | !!proc.m_loopPollGenerator << 12 | !!proc.m_lastPhase << 13);
    for (auto word : proc.m_opcodesEverAdded.storage())
        append64(word);

    append(code.m_optLevel);
    append(static_cast<uint32_t>(code.m_chargesForCalleeSaves) | code.m_placesRareBlocksInGaps << 1 | code.m_ordersBlocksByEdgeFrequency << 2 | code.m_mayPlaceSpillsAboveFrameRecord << 3 | code.m_mayPlaceCalleeSavesAtFrameBottom << 4 | code.m_preserveB3Origins << 5 | code.m_recordsRareCodeStart << 6 | !!code.m_maskInMemory << 7 | code.m_placesRareBlocksBehindGuards << 8);
    if (code.m_maskInMemory) {
        append64(code.m_maskInMemory->mask);
        append(code.m_maskInMemory->base);
        append(code.m_maskInMemory->offset);
    }
    append(code.m_frameSize);
    append(code.m_callArgAreaSize);
    for (Bank bank : { GP, FP }) {
        append(code.regsInPriorityOrder(bank).size());
        for (Reg reg : code.regsInPriorityOrder(bank))
            append(reg.index());
    }
    append(code.mutableRegs());
    append(code.pinnedRegisters().toRegisterSet());
    append(code.m_unsavedCalleeSaves);
    append(code.m_calleeSavesClobberedAtSuspensions);

    append(proc.m_tuples.size());
    for (auto& tuple : proc.m_tuples) {
        reserve(1 + tuple.size());
        append(tuple.size());
        for (Type type : tuple)
            append(type.hash());
    }
    reserve(4 + 3 * proc.m_variables.size() + proc.m_values.indexFreeList().size());
    append(proc.m_variables.size());
    for (Variable* variable : proc.m_variables) {
        append(variable->index());
        append(variable->type().hash());
    }
    for (size_t index : proc.m_variables.indexFreeList())
        append(index);
    append(proc.m_values.size());
    for (size_t index : proc.m_values.indexFreeList())
        append(index);

    unsigned numberOfValuesInBlocks = 0;
    append(proc.size());
    for (unsigned blockIndex = 0; blockIndex < proc.size(); ++blockIndex) {
        BasicBlock* block = proc[blockIndex];
        reserve(8 + (block ? block->numPredecessors() + 2 * block->numSuccessors() : 0));
        if (!block) {
            append(std::numeric_limits<uint32_t>::max());
            continue;
        }
        append(block->size());
        append64(std::bit_cast<uint64_t>(block->frequency()));
        append(block->numPredecessors());
        for (BasicBlock* predecessor : block->predecessors())
            append(predecessor->index());
        append(block->numSuccessors());
        for (FrequentedBlock successor : block->successors()) {
            append(successor.block()->index());
            append(static_cast<uint32_t>(successor.frequency()));
        }
        numberOfValuesInBlocks += block->size();
        for (Value* value : *block) {
            if (!appendValue(value, distinguishedGenerators))
                return false;
        }
    }
    if (numberOfValuesInBlocks != proc.m_values.numberOfEntries())
        return false;

    reserve(1);
    if (m_size % 2)
        append(0);
    m_words.shrink(m_size);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < m_size; i += 2)
        hash = (hash ^ (m_words[i] | static_cast<uint64_t>(m_words[i + 1]) << 32)) * 0x100000001b3ull;
    m_hash = hash;
    return true;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
