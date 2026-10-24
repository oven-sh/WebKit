/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeCallEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTHeaps.h"
#include "AOTStubCatalog.h"
#include "AOTThreshold.h"

namespace JSC { namespace AOT { namespace Typed {

Effects RuntimeCallEmitter::effectsOfCall(const OperationContext& context, const Effects& effectsOfCallee) const
{
    Effects effects = context.effects().onAnyEdge().intersectedWith(effectsOfCallee);
    if (m_options.recorded.isSabotaged(Sabotage::NoCallCollects))
        effects.mayCollect = false;
    if (m_options.recorded.isSabotaged(Sabotage::NoCallWrites))
        effects.writes = { };
    return effects;
}

const StubSummary& RuntimeCallEmitter::summaryOf(StubKey key) const
{
    return inputs().stubCatalog.summaryOf(key);
}

bool RuntimeCallEmitter::mayConvertCells(const Effects& effects)
{
    return AOTHeaps::mayConvertCells(effects) && !build().recordedOptions().has(TestingSwitch::NoCallConvertsCells);
}

void RuntimeCallEmitter::applyEffects(B3::PatchpointValue* patchpoint, const Effects& effects, ArgumentBufferAccess access, bool accessesCollectorState, CellConversions conversions)
{
    RELEASE_ASSERT(!generator().isInitializingCell() || (!effects.mayCollect && !effects.mayThrow));

    bool isTerminal = patchpoint->effects.terminal;
    patchpoint->effects = B3::Effects::none();
    patchpoint->effects.terminal = isTerminal;
    patchpoint->effects.controlDependent = true;
    bool convertsCells = conversions == CellConversions::WhateverEffectsSay ? !build().recordedOptions().has(TestingSwitch::NoCallConvertsCells) : conversions == CellConversions::AsEffectsSay && mayConvertCells(effects);
    if (effects.isPure() && access == ArgumentBufferAccess::None && !accessesCollectorState && !convertsCells)
        return;

    bool exitsSideways = effects.mayExitSideways();
    B3HeapList reads;
    B3HeapList writes;
    m_heaps.appendHeaps(effects.reads, reads);
    m_heaps.appendHeaps(effects.writes, writes);
    if (convertsCells)
        m_heaps.appendHeapsOfConvertedCells(writes);
    if (effects.mayCollect)
        m_heaps.appendHeapsWrittenByCollection(writes);
    else if (accessesCollectorState)
        writes.append(&m_heaps.collectorState);
    if (effects.mayCollect)
        reads.append(&m_heaps.frameSlots);
    if (accessesCollectorState)
        reads.append(&m_heaps.collectorState);
    switch (access) {
    case ArgumentBufferAccess::ReadsAndWrites:
        writes.append(&m_heaps.argumentBuffer);
        [[fallthrough]];
    case ArgumentBufferAccess::Reads:
        reads.append(&m_heaps.argumentBuffer);
        break;
    case ArgumentBufferAccess::None:
        break;
    }
    // B3 takes the reads of a value that exits sideways as everything that is read after the exit.
    if (exitsSideways)
        patchpoint->effects.reads = B3::HeapRange::top();
    else if (!reads.isEmpty())
        m_heaps.decoratePatchpointRead(reads.span(), patchpoint);
    if (!writes.isEmpty())
        m_heaps.decoratePatchpointWrite(writes.span(), patchpoint);
    patchpoint->effects.exitsSideways = exitsSideways;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
