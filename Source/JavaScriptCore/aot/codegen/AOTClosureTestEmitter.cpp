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
#include "AOTFunctionTables.h"
#include "AOTGuardEmitter.h"
#include "AOTInstance.h"
#include "AOTPlacement.h"
#include "AOTSlimClosure.h"
#include "JSCell.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(JSObject::butterflyOffset() == offsetOfSlimClosureWord);
static_assert(minCell32 > 1);

ClosureTestEmitter::ClosureTestEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

Condition ClosureTestEmitter::isClosureOf(CellPointer cell, FunctionID function)
{
    return asBoolean([&](BranchTargets targets) {
        branchOnIsClosureOf(cell, function, targets);
    });
}

void ClosureTestEmitter::branchOnIsClosureOf(CellPointer cell, FunctionID function, BranchTargets targets)
{
    FunctionID entrypoint = inputs().derivedFunctions.find(derivedFunctionKey(FunctionRole::GenericEntrypoint, function));
    if (!entrypoint) {
        m_out.jump(targets.notTaken.block());
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

    LBasicBlock hasInitialStructure = newBlock();
    LBasicBlock hasOtherStructure = newBlock();
    m_out.branch(hasClosureStructure(cell, function).value(), normalEdge(hasInitialStructure), unlikelyEdge(hasOtherStructure));

    m_out.appendTo(hasInitialStructure);
    branch(hasClosureWordOf(cell, isSlim, entrypoint), targets);

    m_out.appendTo(hasOtherStructure);
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

Condition ClosureTestEmitter::hasClosureWordOf(CellPointer closure, bool isSlim, FunctionID genericEntrypoint)
{
    unsigned numberOfParameters = describeEntry(inputs(), genericEntrypoint, m_program.at(genericEntrypoint)).numberOfParameters;
    uint32_t expected = ClosureWord::encodeUpperHalf(numberOfParameters, genericEntrypoint.index(), closureWordFlags(m_placement.functionKind(genericEntrypoint)));
    const B3::AbstractHeap& word = isSlim ? m_heaps.AOTSlimClosure_word : m_heaps.AOTClosure_entry;
    return equal(Word32(m_out.load32(m_out.address(closure.value(), word, sizeof(uint32_t)))), word32(expected));
}

Condition ClosureTestEmitter::hasFunctionCellType(CellPointer cell)
{
    return logicalOr(hasCellTypeIn(cell, functionTypes), hasCellTypeIn(cell, slimClosureTypes));
}

Condition ClosureTestEmitter::isUnreifiedSlimClosure(CellPointer cell)
{
    return hasMutableCellType(cell, SlimClosureType);
}

Condition ClosureTestEmitter::isTableCell(CellPointer cell, FunctionID genericEntrypoint)
{
    if (m_options.recorded.useWideReferences())
        return equal(cell, closureIfCreated(genericEntrypoint));
    return equal(cell, decode(NarrowReference(add(closureTableEntry(genericEntrypoint), word32(1)).value())));
}

CellPointer ClosureTestEmitter::closureIfCreated(FunctionID genericEntrypoint)
{
    RELEASE_ASSERT(genericEntrypoint);
    if (m_options.recorded.useWideReferences()) {
        Pointer table = loadImmutablePointer(AOT_INSTANCE_OFFSET(closureCells));
        return CellPointer(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::RuntimeState), table.value(), static_cast<ptrdiff_t>(genericEntrypoint.index()) * sizeof(void*))));
    }
    Word32 entry = closureTableEntry(genericEntrypoint);
    return select(isZero(entry), CellPointer(m_out.constInt64(0)), decode(NarrowReference(add(entry, word32(1)).value())));
}

Word32 ClosureTestEmitter::closureTableEntry(FunctionID genericEntrypoint)
{
    RELEASE_ASSERT(genericEntrypoint);
    Pointer table = loadImmutablePointer(AOT_INSTANCE_OFFSET(closureCells));
    return Word32(m_out.load32(m_out.address(m_heaps.entire(AbstractHeap::RuntimeState), table.value(), static_cast<ptrdiff_t>(genericEntrypoint.index()) * sizeof(uint32_t))));
}

Condition ClosureTestEmitter::hasClosureStructure(CellPointer cell, FunctionID function)
{
    FunctionKindData kind = m_placement.functionKind(function);
    ptrdiff_t offsetOfHeader = kind.isSlim
        ? AOT_INSTANCE_OFFSET(slimClosureHeaders) + kind.slimClosureKind() * sizeof(uint64_t)
        : AOT_INSTANCE_OFFSET(closures.closureHeaders) + kind.closureStructure * sizeof(uint64_t);
    return equal(loadStructureID(cell), loadImmutable32(offsetOfHeader));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
