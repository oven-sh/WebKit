/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachine.h"

#if ENABLE(AOT)

#include "AOTMachineAllocation.h"
#include "AOTMachineChecker.h"
#include "AOTMachineEncoder.h"
#include "AOTMachineFrame.h"
#include "AOTMachineLiveness.h"
#include "AOTMachineOrder.h"
#include "AOTMachineSlots.h"

namespace JSC { namespace AOT { namespace Machine {

// A check that refused a procedure without reporting a failure would leave it half prepared, and generate() would take it for a finished one.
static bool runCheckBeforeGeneration(Procedure& procedure, bool (*check)(Procedure&))
{
    bool foundNothing = check(procedure);
    RELEASE_ASSERT(foundNothing == !procedure.failure());
    return foundNothing;
}

// Each step reads the products of the earlier steps from the procedure, and so does whoever prints a procedure that failed.
// So a product is stored as soon as it exists, before anyone asks whether its step failed.
void prepareForGeneration(Procedure& procedure)
{
    if (procedure.failure())
        return;
    RELEASE_ASSERT(!procedure.m_order);

    // There would be nothing to encode, and whoever links the code asks for the label of the primary entry.
    if (procedure.entrypoints().empty()) {
        procedure.fail(FailureKind::MalformedProcedure);
        return;
    }

    const bool runsChecks = procedure.traits().checksAllocation;
    if (runsChecks && !runCheckBeforeGeneration(procedure, validate))
        return;

    threadJumps(procedure);
    if (procedure.failure())
        return;

    mergeIdenticalBlocks(procedure);
    if (procedure.failure())
        return;
    if (runsChecks && !runCheckBeforeGeneration(procedure, validateBehindBlockPasses))
        return;

    procedure.m_order = orderBlocks(procedure);
    if (procedure.failure())
        return;

    procedure.m_liveness = scanBackward(procedure);
    if (procedure.failure())
        return;

    procedure.m_allocation = allocateRegisters(procedure);
    if (procedure.failure())
        return;
    if (runsChecks && !runCheckBeforeGeneration(procedure, checkAllocation))
        return;
    if (runsChecks && !runCheckBeforeGeneration(procedure, checkReferences))
        return;

    procedure.m_slots = assignSlots(procedure);
    if (procedure.failure())
        return;

    procedure.m_frame = planFrame(procedure);
    if (procedure.failure())
        return;
    if (runsChecks)
        runCheckBeforeGeneration(procedure, checkFrame);
}

void generate(Procedure& procedure, CCallHelpers& jit, RelocationRecorder& relocations)
{
    RELEASE_ASSERT(!procedure.failure());
    // The scan and the allocator went by the features in the traits. The assembler picks its instructions by its own.
    RELEASE_ASSERT(jit.policy().features == procedure.traits().features);
    // The encoder appends to the records of the procedure.
    RELEASE_ASSERT(!procedure.rareCodeStart().isSet());

    Encoder encoder(procedure, jit, relocations);
    encoder.run();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
