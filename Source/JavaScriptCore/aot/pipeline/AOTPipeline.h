/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEntrypointsNothingEnters.h"
#include "AOTFlowReport.h"
#include "AOTPass.h"
#include "AOTVector.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/Nonmovable.h>
#include <wtf/PrintStream.h>
#include <wtf/RefPtr.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class BackgroundTask;
class RegExpRequests;
struct CompiledRegExps;
struct ExternalFunctionBodies;

namespace Typed {

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

    void releaseCodeGenerationInputs(BuildContext&);
    void startCompilingRegExps(const Program&, BuildContext&);
    CompiledRegExps takeCompiledRegExps(BuildContext&);
    void waitForRegExps();

    std::unique_ptr<Program> program;
    std::unique_ptr<DerivedFunctions> derivedFunctions;
    std::unique_ptr<FlowResults> flow;
    std::unique_ptr<EffectSummaries> effects;
    std::unique_ptr<Heat> heat;
    std::unique_ptr<CallingConventions> callingConventions;
    std::unique_ptr<FrameAllocation> frameAllocation;
    std::unique_ptr<EnvironmentReuse> environmentReuse;
    std::unique_ptr<LiteralStringTable> literalStrings;
    std::unique_ptr<ConstantCollections> constantCollections;
    std::unique_ptr<SlotPlan> slots;
    std::unique_ptr<RegExpRequests> regExpRequests;
    std::unique_ptr<CompiledRegExps> compiledRegExps;
    RefPtr<BackgroundTask> compilationOfRegExps;
    std::unique_ptr<StackChecks> stackChecks;
    std::unique_ptr<Placement> placement;
    std::unique_ptr<SiteCaches> siteCaches;
    AccessSiteObservations accessSiteObservations;
    Vector<Wire::RemainingCheckRecord> remainingChecks;
    CalleesOfSites calleesOfSites;
    // By function: what forEachCalledFunction() names. Empty where the bodies were not read for it while they were in the cache.
    Vector<Vector<FunctionID>> calledFunctions;
    bool isComplete { false };
};

using ReaderOfFinishedBodies = ScopedLambda<void(const PipelineOutput&)>;

std::unique_ptr<PipelineOutput> runPipeline(std::span<const uint8_t> programBytes, const ExternalFunctionBodies*, BuildContext&, NOESCAPE const ScopedLambda<void()>& didReadProgram, NOESCAPE const ReaderOfFinishedBodies& readBesideLastEffectSummaries);
// Where a last pass is named, the run stops behind it.
std::unique_ptr<PipelineOutput> runPipeline(std::unique_ptr<Program>, BuildContext&, NOESCAPE const ReaderOfFinishedBodies& readBesideLastEffectSummaries = [](const PipelineOutput&) { }, std::optional<PassID> last = std::nullopt);
std::unique_ptr<PipelineOutput> runPipelineUntil(PassID last, std::unique_ptr<Program>, BuildContext&);

bool canRunPassForTesting(PassID, IRForm);
PassOutcome runPassForTesting(PassID, Program&, PrintStream& results, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
