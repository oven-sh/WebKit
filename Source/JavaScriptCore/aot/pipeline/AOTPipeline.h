/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowReport.h"
#include "AOTPass.h"
#include <memory>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/Nonmovable.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT {

class RegExpRequests;

namespace Typed {

class CallGroups;
class ConstantCollections;
class EnvironmentReuse;
class FrameAllocation;
class LiteralStringTable;
class PassRunner;
class SiteCaches;
class StackChecks;

struct PipelineOutput {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PipelineOutput);
    WTF_MAKE_NONCOPYABLE(PipelineOutput);
    WTF_MAKE_NONMOVABLE(PipelineOutput);

    PipelineOutput();
    ~PipelineOutput();

    std::unique_ptr<Program> program;
    std::unique_ptr<DerivedFunctions> derivedFunctions;
    std::unique_ptr<FlowResults> flow;
    std::unique_ptr<EffectSummaries> effects;
    std::unique_ptr<Heat> heat;
    std::unique_ptr<CallGroups> callGroups;
    std::unique_ptr<CallingConventions> callingConventions;
    std::unique_ptr<FrameAllocation> frameAllocation;
    std::unique_ptr<EnvironmentReuse> environmentReuse;
    std::unique_ptr<LiteralStringTable> literalStrings;
    std::unique_ptr<ConstantCollections> constantCollections;
    std::unique_ptr<SlotPlan> slots;
    std::unique_ptr<RegExpRequests> regExpRequests;
    std::unique_ptr<StackChecks> stackChecks;
    std::unique_ptr<Placement> placement;
    std::unique_ptr<SiteCaches> siteCaches;
    Vector<FlowSiteLevel> accessSiteLevels;
    bool isComplete { false };
};

std::unique_ptr<PipelineOutput> runPipeline(std::span<const uint8_t> programBytes, BuildContext&);
std::unique_ptr<PipelineOutput> runPipeline(std::unique_ptr<Program>, BuildContext&);
std::unique_ptr<PipelineOutput> runPipelineUntil(PassID last, std::unique_ptr<Program>, BuildContext&);
std::unique_ptr<PipelineOutput> runPasses(std::unique_ptr<Program>, PassRunner&);

bool canRunPassForTesting(PassID, IRForm);
PassOutcome runPassForTesting(PassID, Program&, PrintStream& results, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
