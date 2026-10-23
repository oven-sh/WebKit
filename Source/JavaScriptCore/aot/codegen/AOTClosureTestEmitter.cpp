/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClosureTestEmitter.h"

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTDerivedFunctions.h"
#include "AOTEntryEmitter.h"
#include "AOTFlowResults.h"
#include "AOTFunctionTables.h"
#include "AOTGuardEmitter.h"
#include "AOTInstance.h"
#include "AOTPlacement.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSlimClosure.h"
#include "AOTSort.h"
#include "AOTStubsClosures.h"
#include "AOTVector.h"
#include "JSCell.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(JSObject::butterflyOffset() == offsetOfSlimClosureWord);
static_assert(minCell32 > 1);

ClosureTestEmitter::ClosureTestEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

bool ClosureTestEmitter::closureWordNames(FunctionID function) const
{
    return inputs().flow.programInstances() == ProgramInstances::AtMostOne && m_placement.functionKind(function).isSlim
        && inputs().derivedFunctions.find(derivedFunctionKey(FunctionRole::GenericEntrypoint, function));
}

// A closure that has got a property, another prototype or another level of integrity has a butterfly where its word was, and the comparison refuses it.
bool ClosureTestEmitter::isDecidedByClosureWord(FunctionID function, const Type& tested) const
{
    if (!closureWordNames(function))
        return false;
    return (tested.anyFunction || tested.anyObject) && !m_options.recorded.validates;
}

Word32 ClosureTestEmitter::upperHalfOfWordAtSlimClosureWord(CellPointer cell)
{
    return Word32(m_out.load32(m_out.address(cell.value(), m_heaps.AOTSlimClosure_word, sizeof(uint32_t))));
}

Condition ClosureTestEmitter::isClosureOf(CellPointer cell, FunctionID function, const Type& tested, CodeShape shape)
{
    if (isDecidedByClosureWord(function, tested))
        return hasClosureWordOf(cell, true, inputs().derivedFunctions.find(derivedFunctionKey(FunctionRole::GenericEntrypoint, function)));
    return asBoolean([&](BranchTargets targets) {
        branchOnIsClosureOf(cell, function, tested, targets, shape);
    });
}

bool ClosureTestEmitter::asksStubWhetherClosureOf(FunctionID function, CodeShape shape) const
{
    return shape != CodeShape::Fast
        && inputs().derivedFunctions.find(derivedFunctionKey(FunctionRole::GenericEntrypoint, function))
        && m_placement.closureIdentityOf(function) != ClosureIdentity::TableCell
        && !closureWordNames(function);
}

void ClosureTestEmitter::branchOnIsClosureOfByStub(LValue cellOrOtherValue, FunctionID function, BranchTargets targets)
{
    FunctionID entrypoint = inputs().derivedFunctions.find(derivedFunctionKey(FunctionRole::GenericEntrypoint, function));
    LValue operands[] = { cellOrOtherValue, m_out.constInt32(static_cast<int32_t>(upperHalfOfClosureWordOf(entrypoint))) };
    emitter<RuntimeCallEmitter>().callStubWithoutSiteAndBranch(valueIsClosureOf(), operands, targets);
}

void ClosureTestEmitter::branchOnIsClosureOf(CellPointer cell, FunctionID function, const Type& tested, BranchTargets targets, CodeShape shape)
{
    if (asksStubWhetherClosureOf(function, shape)) {
        branchOnIsClosureOfByStub(cell.value(), function, targets);
        return;
    }
    FunctionID entrypoint = inputs().derivedFunctions.find(derivedFunctionKey(FunctionRole::GenericEntrypoint, function));
    if (!entrypoint) {
        m_out.jump(targets.notTaken.block());
        return;
    }
    if (isDecidedByClosureWord(function, tested)) {
        branch(hasClosureWordOf(cell, true, entrypoint), targets);
        return;
    }
    if (m_placement.closureIdentityOf(function) == ClosureIdentity::TableCell) {
        branch(isTableCell(cell, entrypoint), targets);
        return;
    }
    bool isSlim = m_placement.functionKind(function).isSlim;

    auto continueIf = [&](Condition condition) {
        LBasicBlock continuation = newBlock();
        m_out.branch(condition.value(), normalEdge(continuation), targets.notTaken);
        m_out.appendTo(continuation);
    };

    LBasicBlock hasOtherStructure = newBlock();
    if (closureWordNames(function)) {
        Word32 upperHalf = upperHalfOfWordAtSlimClosureWord(cell);
        LBasicBlock hasOtherWord = newBlock();
        m_out.branch(equal(upperHalf, word32(upperHalfOfClosureWordOf(entrypoint))).value(), targets.taken, normalEdge(hasOtherWord));

        // Another slim closure that is as it was created, or a compiled object.
        m_out.appendTo(hasOtherWord);
        m_out.branch(hasBits(upperHalf, closureWordMarker | compiledObjectShapeBit).value(), targets.notTaken, unlikelyEdge(hasOtherStructure));

        m_out.appendTo(hasOtherStructure);
        if (shape != CodeShape::Fast) {
            branchOnIsClosureOfByStub(cell.value(), function, targets);
            return;
        }
    } else {
        LBasicBlock hasInitialStructure = newBlock();
        m_out.branch(hasClosureStructure(cell, function).value(), normalEdge(hasInitialStructure), unlikelyEdge(hasOtherStructure));

        m_out.appendTo(hasInitialStructure);
        branch(hasClosureWordOf(cell, isSlim, entrypoint), targets);

        m_out.appendTo(hasOtherStructure);
    }
    continueIf(isSlim ? hasMutableCellType(cell, SlimClosureWithPropertiesType) : hasCellType(cell, CompiledClosureType));
    continueIf(emitter<GuardEmitter>().belongsToInstance(cell));
    if (!isSlim) {
        branch(hasClosureWordOf(cell, false, entrypoint), targets);
        return;
    }
    Pointer butterfly = loadField<Pointer>(cell, m_heaps.JSObject_butterfly);
    Word32 reifiedFunction(m_out.load32(m_out.address(m_heaps.properties.atAnyNumber(), butterfly.value(), ReifiedSlimClosure::offsetOfFunctionIndexInButterfly())));
    branch(equal(reifiedFunction, word32(entrypoint.index())), targets);
}

Condition ClosureTestEmitter::isClosureOfAny(CellPointer cell, std::span<const FunctionID> functions)
{
    return asBoolean([&](BranchTargets targets) {
        branchOnIsClosureOfAny(cell, functions, targets);
    });
}

void ClosureTestEmitter::branchOnIndexInRuns(Word32 index, std::span<const EntrypointRun> runs, BranchTargets targets)
{
    static constexpr size_t maxRunsTestedInOrder = 3;
    if (runs.size() > maxRunsTestedInOrder) {
        size_t middle = runs.size() / 2;
        LBasicBlock lower = newBlock();
        LBasicBlock upper = newBlock();
        m_out.branch(below(index, word32(runs[middle].first)).value(), normalEdge(lower), normalEdge(upper));
        m_out.appendTo(lower);
        branchOnIndexInRuns(index, runs.first(middle), targets);
        m_out.appendTo(upper);
        branchOnIndexInRuns(index, runs.subspan(middle), targets);
        return;
    }
    for (size_t position = 0; position < runs.size(); ++position) {
        const EntrypointRun& run = runs[position];
        Condition isInRun = run.first == run.last ? equal(index, word32(run.first)) : belowOrEqual(sub(index, word32(run.first)), word32(run.last - run.first));
        if (position + 1 == runs.size()) {
            branch(isInRun, targets);
            return;
        }
        LBasicBlock next = newBlock();
        m_out.branch(isInRun.value(), targets.taken, normalEdge(next));
        m_out.appendTo(next);
    }
}

void ClosureTestEmitter::branchOnIsClosureOfAny(CellPointer cell, std::span<const FunctionID> functions, BranchTargets targets)
{
    struct InitialStructure {
        bool isSlim;
        ptrdiff_t offsetOfHeader;
    };
    Vector<uint32_t, 8> entrypoints;
    Vector<InitialStructure, 4> structures;
    bool hasSlimMember = false;
    bool hasFullMember = false;
    for (FunctionID function : functions) {
        FunctionID entrypoint = inputs().derivedFunctions.find(derivedFunctionKey(FunctionRole::GenericEntrypoint, function));
        if (!entrypoint)
            continue;
        entrypoints.append(entrypoint.index());
        FunctionKindData kind = m_placement.functionKind(function);
        (kind.isSlim ? hasSlimMember : hasFullMember) = true;
        ptrdiff_t offsetOfHeader = offsetOfClosureHeader(kind);
        bool isListed = structures.containsIf([&](const InitialStructure& listed) {
            return listed.offsetOfHeader == offsetOfHeader;
        });
        if (!isListed)
            structures.append({ kind.isSlim, offsetOfHeader });
    }
    if (entrypoints.isEmpty()) {
        m_out.jump(targets.notTaken.block());
        return;
    }

    AOT::sort(entrypoints);
    Vector<EntrypointRun, 4> runs;
    for (uint32_t entrypoint : entrypoints) {
        if (!runs.isEmpty() && runs.last().last + 1 >= entrypoint)
            runs.last().last = entrypoint;
        else
            runs.append({ entrypoint, entrypoint });
    }

    auto functionIndexIn = [&](const B3::AbstractHeap& word) {
        Word32 upperHalf(m_out.load32(m_out.address(cell.value(), word, sizeof(uint32_t))));
        return bitAnd(upperHalf, word32(ClosureWord::functionMask));
    };
    auto continueIf = [&](Condition condition, B3::FrequentedBlock otherwise) {
        LBasicBlock continuation = newBlock();
        m_out.branch(condition.value(), normalEdge(continuation), otherwise);
        m_out.appendTo(continuation);
    };

    Join<Word32> functionIndex(m_out);
    if (hasSlimMember && inputs().flow.programInstances() == ProgramInstances::AtMostOne) {
        Word32 upperHalf = upperHalfOfWordAtSlimClosureWord(cell);
        LBasicBlock isNotAsCreated = newBlock();
        continueIf(equal(bitAnd(upperHalf, word32(closureWordMarker | compiledObjectShapeBit)), word32(closureWordMarker)), normalEdge(isNotAsCreated));
        functionIndex.add(bitAnd(upperHalf, word32(ClosureWord::functionMask)));
        m_out.appendTo(isNotAsCreated);
        structures.removeAllMatching([](const InitialStructure& structure) {
            return structure.isSlim;
        });
    }
    Word32 structureID = loadStructureID(cell);
    for (const InitialStructure& structure : structures) {
        LBasicBlock hasOtherStructure = newBlock();
        continueIf(equal(structureID, loadImmutable32(structure.offsetOfHeader)), normalEdge(hasOtherStructure));
        functionIndex.add(functionIndexIn(structure.isSlim ? m_heaps.AOTSlimClosure_word : m_heaps.AOTClosure_entry));
        m_out.appendTo(hasOtherStructure);
    }
    if (hasFullMember) {
        LBasicBlock isNoFullClosure = newBlock();
        continueIf(hasCellType(cell, CompiledClosureType), normalEdge(isNoFullClosure));
        continueIf(emitter<GuardEmitter>().belongsToInstance(cell), targets.notTaken);
        functionIndex.add(functionIndexIn(m_heaps.AOTClosure_entry));
        m_out.appendTo(isNoFullClosure);
    }
    if (hasSlimMember) {
        continueIf(hasMutableCellType(cell, SlimClosureWithPropertiesType), targets.notTaken);
        continueIf(emitter<GuardEmitter>().belongsToInstance(cell), targets.notTaken);
        Pointer butterfly = loadField<Pointer>(cell, m_heaps.JSObject_butterfly);
        functionIndex.add(Word32(m_out.load32(m_out.address(m_heaps.properties.atAnyNumber(), butterfly.value(), ReifiedSlimClosure::offsetOfFunctionIndexInButterfly()))));
    } else
        m_out.jump(targets.notTaken.block());

    branchOnIndexInRuns(functionIndex.finish(), runs.span(), targets);
}

uint32_t ClosureTestEmitter::upperHalfOfClosureWordOf(FunctionID genericEntrypoint)
{
    EntryDescription entry = describeEntry(inputs(), genericEntrypoint, m_program.at(genericEntrypoint));
    ClosureWord::Flags flags = closureWordFlags(m_placement.functionKind(genericEntrypoint));
    flags.borrowedArguments = entry.borrowedArguments;
    flags.endsItsTailCalls = entry.endsItsTailCalls;
    return ClosureWord::encodeUpperHalf(entry.numberOfParameters, genericEntrypoint.index(), flags);
}

Condition ClosureTestEmitter::hasClosureWordOf(CellPointer closure, bool isSlim, FunctionID genericEntrypoint)
{
    const B3::AbstractHeap& word = isSlim ? m_heaps.AOTSlimClosure_word : m_heaps.AOTClosure_entry;
    return equal(Word32(m_out.load32(m_out.address(closure.value(), word, sizeof(uint32_t)))), word32(upperHalfOfClosureWordOf(genericEntrypoint)));
}

Condition ClosureTestEmitter::isSlimClosureAsCreated(CellPointer cell)
{
    Word32 topByte(m_out.load8SignExt32(m_out.address(cell.value(), m_heaps.AOTSlimClosure_word, ClosureWord::topByteShift / 8)));
    return greaterThanOrEqual(topByte, word32(static_cast<uint32_t>(ClosureWord::minTopByteOfCalleeTaking(ClosureWord::maxNumberOfParameters))));
}

Condition ClosureTestEmitter::hasFunctionCellType(CellPointer cell)
{
    return logicalOr(hasCellTypeIn(cell, functionTypes), hasCellTypeIn(cell, slimClosureTypes));
}

Condition ClosureTestEmitter::isTableCell(CellPointer cell, FunctionID genericEntrypoint)
{
    if (m_options.recorded.useWideReferences())
        return equal(cell, closureIfCreated(genericEntrypoint));
    return equal(cell, decode(NarrowReference(add(closureTableEntry(genericEntrypoint), word32(1)).value())));
}

CellPointer ClosureTestEmitter::closureIfCreated(FunctionID genericEntrypoint)
{
    if (m_options.recorded.useWideReferences())
        return CellPointer(m_out.load64(closureCellOf(genericEntrypoint)));
    Word32 entry = closureTableEntry(genericEntrypoint);
    return select(isZero(entry), CellPointer(m_out.constInt64(0)), decode(NarrowReference(add(entry, word32(1)).value())));
}

Word32 ClosureTestEmitter::closureTableEntry(FunctionID genericEntrypoint)
{
    RELEASE_ASSERT(!m_options.recorded.useWideReferences());
    return Word32(m_out.load32(closureCellOf(genericEntrypoint)));
}

TypedPointer ClosureTestEmitter::closureCellOf(FunctionID genericEntrypoint)
{
    uint32_t number = m_placement.closureCellNumber(genericEntrypoint);
    RELEASE_ASSERT(number);
    size_t cellSize = m_options.recorded.useWideReferences() ? sizeof(JSCell*) : sizeof(uint32_t);
    Pointer table = loadImmutablePointer(AOT_INSTANCE_OFFSET(closureCells));
    return m_out.address(m_heaps.entire(AbstractHeap::RuntimeState), table.value(), static_cast<ptrdiff_t>(number * cellSize));
}

ptrdiff_t ClosureTestEmitter::offsetOfClosureHeader(FunctionKindData kind) const
{
    return kind.isSlim
        ? AOT_INSTANCE_OFFSET(slimClosureHeaders) + kind.slimClosureKind() * sizeof(uint64_t)
        : AOT_INSTANCE_OFFSET(closures.closureHeaders) + kind.closureStructure * sizeof(uint64_t);
}

Condition ClosureTestEmitter::hasClosureStructure(CellPointer cell, FunctionID function)
{
    return equal(loadStructureID(cell), loadImmutable32(offsetOfClosureHeader(m_placement.functionKind(function))));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
