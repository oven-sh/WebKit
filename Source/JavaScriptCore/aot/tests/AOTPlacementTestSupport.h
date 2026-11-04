/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCallGraph.h"
#include "AOTPipeline.h"
#include "AOTPlacement.h"
#include "AOTTestCommand.h"
#include <memory>
#include <wtf/FastMalloc.h>

namespace JSC { namespace AOT { namespace Typed {

struct PlacedProgram {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PlacedProgram);

    const Program& program() const { return *output->program; }
    Program& program() { return *output->program; }
    const SlotPlan& slots() const { return *output->slots; }
    const Placement& placement() const { return *output->placement; }
    const SiteCaches& siteCaches() const { return *output->siteCaches; }
    PlacementInputs inputs() const;

    std::unique_ptr<PipelineOutput> output;
    CallGraph callGraph;
};

std::unique_ptr<PlacedProgram> runPipelineForPlacementTest(TestCommandContext&, PassID last);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
