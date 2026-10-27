/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"
#include "testb3_bun_forked_passes.h"

#if ENABLE(B3_JIT)

void addBunTests(const TestConfig* config, Deque<RefPtr<SharedTask<void()>>>& tasks)
{
    addBunHeapSetTests(config, tasks);
    addBunKnownPropertyTests(config, tasks);
    addBunReadOnlyDataTests(config, tasks);
    addBunLoopTests(config, tasks);
    addBunLoopUnrollingTests(config, tasks);
    addBunCountedLoopTests(config, tasks);
    addBunComparisonFoldingTests(config, tasks);
    addBunLoopPhaseTests(config, tasks);
    addBunLoopVersioningTests(config, tasks);
    addBunLoopPlanTests(config, tasks);
    addBunCountedLoopVersioningTests(config, tasks);
    addBunStridedRangeCheckTests(config, tasks);
    addBunPlacementTests(config, tasks);
    addBunHoistingTests(config, tasks);
    addBunNumberingTests(config, tasks);
    addBunAnalysisTests(config, tasks);
    addBunRegionClonerTests(config, tasks);
    addBunReductionTests(config, tasks);
    addBunRangeTests(config, tasks);
    addBunStoreMergingTests(config, tasks);
    addBunJumpThreadingTests(config, tasks);
    addBunControlFlowTests(config, tasks);
    addBunForkedPassTests(config, tasks);
    addBunSelectTests(config, tasks);
    addBunSelectionTests(config, tasks);
    addBunPolicyTests(config, tasks);
    addBunSharedStepTests(config, tasks);
    addBunOptionTests(config, tasks);
    addBunRandomTests(config, tasks);
}

#endif // ENABLE(B3_JIT)
