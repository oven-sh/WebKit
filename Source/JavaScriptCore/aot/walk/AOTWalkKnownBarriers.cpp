/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// EscapeAnalysis: a store into a cell that was made behind the last safepoint, and that nothing else names yet, runs no barrier: the collector cannot have marked the cell.
// At a suspension: whatever suspends runs other code, so no cell is young behind it.
// At a throw: the error is made on the way to the handler, so no cell is young at the head of a handler block: KnownBarriers::enterHead().
// Withheld: no cell is noted, and every store keeps its barrier.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTOpcodeSets.h"
#include "AOTOperandKinds.h"
#include "AOTPlan.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

// The run time allocates nothing behind the cell of any of them.
static constexpr OpcodeSet allocationsThatLeaveYoungCellInKnownBarriers = opcodesWhere([](Opcode opcode) {
    return allocatesAfterLastSafepointOf(opcode) && interpretedOf(opcode) == Interpretation::Allocation && effectRuleOf(opcode) == EffectRule::Declared;
}) | OpcodeSet { Opcode::NewHeapWords };

// Whatever the effects say: an operation that writes nothing may retain.
static constexpr OperandUses usesThatPublishInKnownBarriers { OperandUse::Retains, OperandUse::Calls, OperandUse::PassesToUserCode };
static constexpr OperandUses usesThatPublishOrGiveAnotherNameInKnownBarriers { OperandUse::Retains, OperandUse::Calls, OperandUse::PassesToUserCode, OperandUse::Returns };

static bool mayHoldCellInKnownBarriers(Walker& walker, Reference stored)
{
    TypeID type = walker.typeOf(stored);
    return !type || walker.types().at(type).hasCells();
}

Reference nameOfCellBehindForms(const Code& code, Reference name)
{
    if (!name || !name.isValue())
        return name;
    ValueID value = name.id();
    while (conversionsOfForm.contains(code.at(value).opcode))
        value = code.at(value).operand(0);
    return code.isConstant(value) ? Reference::constant(value) : Reference::value(value);
}

std::optional<uint32_t> ownerOperandOfStoreOfCells(Walker& walker, const Draft& store)
{
    // A store that makes room for its value lays the Extension in the owner, whatever the value is.
    bool laysCellInOwner = store.opcode == Opcode::PutSlotOrAdd;
    std::optional<uint32_t> owner;
    for (uint32_t operandIndex = 0; operandIndex < store.numberOfOperands(); ++operandIndex) {
        Reference stored = store.operand(operandIndex);
        if (!stored || retainedInOf(store.opcode, operandIndex) != RetainedIn::Operand)
            continue;
        std::optional<uint8_t> retaining = retainingOperandOf(store.opcode, operandIndex);
        if (!retaining || (owner && *owner != *retaining))
            return std::nullopt;
        owner = *retaining;
        laysCellInOwner |= mayHoldCellInKnownBarriers(walker, stored);
    }
    return laysCellInOwner ? owner : std::nullopt;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
