/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrivals.h"
#include "AOTCompileQueue.h"
#include "AOTCompileUnit.h"
#include "AOTDriver.h"
#include "AOTFacts.h"
#include "AOTHeaps.h"
#include "AOTImageBuilder.h"
#include "AOTMechanisms.h"
#include "AOTPlan.h"
#include "AOTStubCatalog.h"
#include <memory>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

enum class CheckOfPlan : bool {
    InFrontOfCompile,
    LeftToCaller,
};

struct PreparedProgram {
    WTF_MAKE_NONCOPYABLE(PreparedProgram);
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PreparedProgram);

    PreparedProgram() = default;

    std::unique_ptr<Typed::Program> program;
    std::unique_ptr<Typed::Facts> facts;
    std::unique_ptr<Typed::Plan> plan;
    std::unique_ptr<const StubCatalog> stubCatalog;
    std::unique_ptr<const AOTHeaps> heaps;
    std::unique_ptr<Typed::Arrivals> arrivals;
    std::unique_ptr<const Typed::CompileInputs> compileInputs;

    CheckOfPlan checkOfPlan { CheckOfPlan::InFrontOfCompile };
    void (*changePlan)(Typed::Plan&) { nullptr };
    Typed::Mechanisms mechanisms { Typed::allMechanisms };
    bool isVerifiedByCaller { false };
};

inline bool willVerifyCode(const PreparedProgram& prepared, const BuildContext& build)
{
    return prepared.isVerifiedByCaller || Typed::walkOptionsOf(build).verifiesCode;
}

JS_EXPORT_PRIVATE bool prepareProgram(PreparedProgram&, BuildContext&);

JS_EXPORT_PRIVATE void checkPlanOfPreparedProgram(const PreparedProgram&, BuildContext&);

// `prepared`, the facts and the arrivals outlive the answer.
inline std::unique_ptr<const Typed::CompileInputs> createCompileInputs(const PreparedProgram& prepared, const Typed::Facts& facts, Typed::Arrivals& arrivals, BuildContext& build)
{
    RELEASE_ASSERT(prepared.plan && prepared.stubCatalog && prepared.heaps);
    RELEASE_ASSERT(&facts.program() == prepared.program.get());

    const Typed::Program& program = *prepared.program;
    const Typed::Plan& plan = *prepared.plan;
    Typed::WalkOptions options = Typed::walkOptionsOf(build);
    options.mechanisms = Typed::withoutMechanismsMissingDependencies(prepared.mechanisms & facts.mechanisms());
    return makeUniqueWithoutFastMallocCheck<const Typed::CompileInputs>(Typed::CompileInputs {
        .walk = { .program = program, .facts = facts, .plan = plan, .options = options, .arrivals = arrivals, .regExpSummaries = &build.regExpSummaries(), .formatPatternPlans = &build.formatPatternPlans() },
        .generation = { .program = program, .facts = facts, .plan = plan, .stubCatalog = *prepared.stubCatalog, .heaps = *prepared.heaps },
    });
}

template<typename Switches>
Typed::Mechanisms withoutMechanismsThatOptionsWithhold(Typed::Mechanisms mechanisms, Switches testing)
{
    if (AOT::hasFlag(testing, Wire::TestingSwitch::DisableFrameCells))
        mechanisms.remove(Typed::Mechanism::CellsInFrames);
    return mechanisms;
}

JS_EXPORT_PRIVATE std::unique_ptr<CompileOutput> compileProgramWithMechanisms(std::span<const uint8_t> program, const Wire::CompileOptions&, Typed::Mechanisms);
JS_EXPORT_PRIVATE std::unique_ptr<CompileOutput> compileParsedProgramWithMechanisms(std::unique_ptr<Typed::Program>, BuildContext&, Typed::Mechanisms);
JS_EXPORT_PRIVATE std::unique_ptr<CompileOutput> compileParsedProgramWithChangedPlan(std::unique_ptr<Typed::Program>, BuildContext&, void (*changePlan)(Typed::Plan&), Typed::Mechanisms);

void writeDiagnosticSections(const Diagnostics&, CompileOutput&);
void writeStatisticSection(const Statistics&, CompileOutput&);
void writeFactSections(const Typed::Program&, const Typed::Facts&, CompileOutput&);
void writeFeatureSection(const Typed::Facts&, const BuiltImage*, CompileOutput&);
void writeObfuscatedSiteSection(const Typed::Program&, const BuiltImage&, BuildContext&, CompileOutput&);

void writeExplainSections(const Typed::Program&, std::span<const Typed::WalkNotes>, BuildContext&, CompileOutput&);

void writeCodeReportSections(const Typed::Program&, const BuiltImage&, BuildContext&, CompileOutput&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
