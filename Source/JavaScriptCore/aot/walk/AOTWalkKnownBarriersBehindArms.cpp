/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations: where arms meet that each store into one object, one RememberOwner behind them runs the barrier for all. The walk only proposes: the sweep marks the stores that it covers, and drops it where none waits.
// At a suspension, and at whatever else may collect: what the state knows of stores ends, so nothing is proposed behind it.
// At a throw: the state of a handler knows of no store, so a way out of a handler that meets the arms brings none and nothing is proposed.
// Withheld: no store is noted, and every store runs its own barrier.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTOpcodeSets.h"
#include "AOTOperandKinds.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

void buildBarrierBehindArms(Walker& walker, Reference owner)
{
    if (!walker.has(Mechanism::TypedOperations) || walker.isUnreachable() || !owner || !owner.isValue())
        return;
    // hasKind() passes by a word that may be unspecified, and build() does not.
    TypeID type = walker.typeOf(owner);
    if (!type || !hasKind(walker.types(), type, OperandKind::Object) || walker.types().at(type).mayBeUnspecified())
        return;

    const Code& code = walker.code();
    ValueID cell = rootOfCellInSweptCode(code, owner.id());
    // Two uses of one constant may make two cells.
    if (code.isConstant(cell))
        return;
    // No store into a cell of the frame runs a barrier, nor does one into a cell that cannot be marked.
    const Operation& made = code.at(cell);
    if (PayloadWord::storage(made.opcode) && made.storage() != AllocationStorage::Heap)
        return;
    KnownBarriers& barriers = walker.state().barriers();
    if (barriers.youngCell() == Reference::value(cell))
        return;

    Origin origin = walker.originOfOperation();
    origin.isTailCall = false;
    Draft barrier(Opcode::RememberOwner, origin);
    barrier.setObjectOperand(owner);
    walker.build(WTF::move(barrier));
    if (walker.hasFailed() || walker.isUnreachable())
        return;
    // Where this join is an arm of another, the barrier behind that one does for this one too.
    barriers.noteStore(owner, Reference::value(code.lastOperationOf(walker.currentBlock())));
}

void buildBarrierWhereStoresHaveMet(Walker& walker, const Draft* next)
{
    KnownBarriers& barriers = walker.state().barriers();
    Reference owner = barriers.owner();
    if (next) {
        // A Narrowed stands at the head of its block, among conversions and in front of all else.
        if (opcodesThatGiveSameCell.contains(next->opcode))
            return;
        // Its own barrier does for the stores that have met.
        std::optional<uint32_t> ownerOfNext = remembersOwnerOf(next->opcode) ? ownerOperandOfStoreOfCells(walker, *next) : std::nullopt;
        if (ownerOfNext && nameOfCellBehindForms(walker.code(), next->operand(*ownerOfNext)) == owner)
            return;
    }
    barriers.endStores();
    buildBarrierBehindArms(walker, owner);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
