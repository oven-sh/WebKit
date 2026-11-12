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

static bool runCheckBeforeGeneration(Procedure& procedure, bool (*check)(Procedure&))
{
    bool foundNothing = check(procedure);
    RELEASE_ASSERT(foundNothing == !procedure.failure());
    return foundNothing;
}

void prepareForGeneration(Procedure& procedure)
{
    if (procedure.failure())
        return;
    RELEASE_ASSERT(!procedure.m_order);

    if (procedure.entrypoints().empty()) {
        procedure.fail(FailureKind::MalformedProcedure);
        return;
    }

    if (procedure.keepsStoresToLookAtAgain()) [[unlikely]] {
        procedure.stopAtStoresThatWayLeadsToAfterAll();
        if (procedure.failure())
            return;
    }

    bool runsChecks = procedure.traits().checksAllocation;
    if (runsChecks && !runCheckBeforeGeneration(procedure, validate))
        return;

    if (procedure.holdsVectors() && !checkVectorsAcrossCalls(procedure)) [[unlikely]]
        return;

    if (procedure.m_roomsThatBuildingLeaves) [[unlikely]]
        procedure.letGoOfWhatBuildingLeaves();

    procedure.separateShiftsOfSums();
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
    runsChecks = procedure.checksAllocation();

    procedure.m_liveness = scanBackward(procedure);
    if (procedure.failure())
        return;

    procedure.m_allocation = allocateRegisters(procedure);
    if (procedure.failure())
        return;
    if (runsChecks && !runCheckBeforeGeneration(procedure, checkAllocation))
        return;
    if (!runsChecks && procedure.allocation().leavesOutSavesAtHeads() && !runCheckBeforeGeneration(procedure, checkSavesAtHeads))
        return;
    if (procedure.allocation().recordsWhatCheckersRead() && !runCheckBeforeGeneration(procedure, checkReferences))
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
    RELEASE_ASSERT(jit.policy().features == procedure.traits().features);
    RELEASE_ASSERT(!procedure.rareCodeStart().isSet());

    Encoder encoder(procedure, jit, relocations);
    encoder.run();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
