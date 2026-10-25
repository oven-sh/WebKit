/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3LowerRelocatableValues.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3InsertionSetInlines.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3RelocationLowering.h"
#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

bool lowerRelocatableValues(Procedure& proc)
{
    PhaseScope phaseScope(proc, "lowerRelocatableValues"_s);

    const AddressingPolicy& addressing = proc.policy().addressing;
    bool hasRelocatables = proc.hasEverAdded(Relocatable);
    bool checksCallees = addressing.isPositionIndependent && proc.hasEverAdded(CCall);
    if (!hasRelocatables && !checksCallees)
        return false;

    bool changed = false;
    InsertionSet insertionSet(proc);
    for (BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            Value* value = block->at(index);

            if (const RelocatableValue* relocatable = value->as<RelocatableValue>()) {
                Value* lowered = addressing.relocationLowering
                    ? addressing.relocationLowering->lower(proc, insertionSet, index, *relocatable)
                    : nullptr;
                if (!lowered) {
                    proc.fail(FailureKind::RelocationNotLowered, value->origin());
                    continue;
                }
                value->replaceWithIdentity(lowered);
                changed = true;
                continue;
            }

            if (checksCallees && value->opcode() == CCall && value->child(0)->foldIdentity()->isConstant())
                proc.fail(FailureKind::AbsoluteAddress, value->origin());
        }
        insertionSet.execute(block);
    }

    if (changed) {
        for (Value* value : proc.values())
            value->performSubstitution();
    }
    return changed;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
