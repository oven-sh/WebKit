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
#include "AOTLeavePoints.h"
#include "AOTMechanisms.h"
#include "AOTPlan.h"
#include "AOTStubCatalog.h"
#include <memory>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

// Who runs checkPlan(). Every build runs it, and no image is made from a plan that fails it.
enum class CheckOfPlan : bool {
    // prepareProgram() runs it right after the plan. A plan that fails never reaches the walk.
    InFrontOfCompile,
    // The caller of prepareProgram() runs checkPlanOfPreparedProgram() in a lane beside Compile and joins it before Image.
    LeftToCaller,
};

// What the stages in front of Compile leave, for AOTDriver.cpp and for the tests that stop between two stages (`testaot facts | plan | walk | compile`).
struct PreparedProgram {
    WTF_MAKE_NONCOPYABLE(PreparedProgram);
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PreparedProgram);

    PreparedProgram() = default;

    std::unique_ptr<Typed::Program> program;
    std::unique_ptr<Typed::Facts> facts;
    std::unique_ptr<Typed::Plan> plan;
    std::unique_ptr<const StubCatalog> stubCatalog;
    // CodeGenerationInputs::heaps refers to it.
    std::unique_ptr<const AOTHeaps> heaps;
    // WalkInputs::arrivals refers to it: the one of the stage.
    std::unique_ptr<Typed::Arrivals> arrivals;
    std::unique_ptr<const Typed::CompileInputs> compileInputs;

    // Set by the caller, like `program`.
    CheckOfPlan checkOfPlan { CheckOfPlan::InFrontOfCompile };
    // For a test of the check: called with the plan before anything reads it.
    void (*changePlan)(Typed::Plan&) { nullptr };
    // Only tests and testaot withhold any. They go into the request of computeFacts(), where Plan finds them: Facts::mechanisms().
    // The walk has those that are neither withheld here nor were withheld from the facts.
    Typed::Mechanisms mechanisms { Typed::allMechanisms };
};

// verifyRecords() and verifyBodies(), then Facts, Plan, the stub catalog and the inputs of Compile, each under its Stage, and checkPlan() unless it is left to the caller.
// False once a stage has reported an error. What the stages in front of it made is there all the same, for the diagnostics.
// `program` is set by the caller. Facts that the caller has set already are left as they are: a test puts there what it has made itself. TypeTable::freeze() comes behind planProgram().
JS_EXPORT_PRIVATE bool prepareProgram(PreparedProgram&, BuildContext&);

// checkPlan(), after prepareProgram() has returned true with CheckOfPlan::LeftToCaller.
// Like compileQueuedUnits() it reads the program, Facts and the plan and writes none of them, so the two may run at once.
JS_EXPORT_PRIVATE void checkPlanOfPreparedProgram(const PreparedProgram&, BuildContext&);

// The inputs of Compile as prepareProgram() makes them, with other facts in place of PreparedProgram::facts and arrivals made for those facts. The plan, the stub catalog, the heaps and the mechanisms are those of `prepared`.
// `prepared`, the facts and the arrivals outlive the answer.
inline std::unique_ptr<const Typed::CompileInputs> createCompileInputs(const PreparedProgram& prepared, const Typed::Facts& facts, Typed::Arrivals& arrivals, BuildContext& build)
{
    RELEASE_ASSERT(prepared.plan && prepared.stubCatalog && prepared.heaps);
    RELEASE_ASSERT(&facts.program() == prepared.program.get());

    const Typed::Program& program = *prepared.program;
    const Typed::Plan& plan = *prepared.plan;
    Typed::WalkOptions options = Typed::walkOptionsOf(build);
    options.mechanisms = Typed::withoutMechanismsMissingDependencies(prepared.mechanisms & facts.mechanisms());
    build.leavePoints().prepare(program.functions.size());
    return makeUniqueWithoutFastMallocCheck<const Typed::CompileInputs>(Typed::CompileInputs {
        .walk = { .program = program, .facts = facts, .plan = plan, .options = options, .arrivals = arrivals, .regExpSummaries = &build.regExpSummaries(), .formatPatternPlans = &build.formatPatternPlans(), .leavePoints = &build.leavePoints() },
        .generation = { .program = program, .facts = facts, .plan = plan, .stubCatalog = *prepared.stubCatalog, .heaps = *prepared.heaps },
    });
}

// AOTDriver.cpp
// compileProgram() and compileParsedProgram() with PreparedProgram::mechanisms set. For testaot and the tests alone: no build of Bun's withholds a mechanism, and AOTDriver.h has no word of them.
JS_EXPORT_PRIVATE std::unique_ptr<CompileOutput> compileProgramWithMechanisms(std::span<const uint8_t> program, const Wire::CompileOptions&, Typed::Mechanisms);
JS_EXPORT_PRIVATE std::unique_ptr<CompileOutput> compileParsedProgramWithMechanisms(std::unique_ptr<Typed::Program>, BuildContext&, Typed::Mechanisms);
// For tests of checkPlan() in a whole build: PreparedProgram::changePlan.
JS_EXPORT_PRIVATE std::unique_ptr<CompileOutput> compileParsedProgramWithChangedPlan(std::unique_ptr<Typed::Program>, BuildContext&, void (*changePlan)(Typed::Plan&), Typed::Mechanisms);

// AOTResultSections.cpp
// UserDiagnostics InternalErrors. The diagnostics are finished.
void writeDiagnosticSections(const Diagnostics&, CompileOutput&);
// Statistics
void writeStatisticSection(const Statistics&, CompileOutput&);
// EntryModuleIds UnseenCode BuiltinsUsed LocalesUsed WorkerEntries WorkerStartsWithoutName URLsThatStartWorkers LibraryFunctionsLacked
void writeFactSections(const Typed::Program&, const Typed::Facts&, CompileOutput&);
// FeaturesUsed: what Facts found, and behind an image also the features of the run-time entries, regular expressions and WebAssembly modules that the image names. Ascending by feature.
void writeFeatureSection(const Typed::Facts&, const BuiltImage*, CompileOutput&);
// ObfuscatedSites
void writeObfuscatedSiteSection(const Typed::Program&, const BuiltImage&, BuildContext&, CompileOutput&);

// AOTExplainRecords.cpp
// Accesses OtherOrigins TypeTests AccessCounts AllocationRemarks FunctionNames LayoutNames.
// The notes of one place (function, operation, access) become one record: the worst outcome and cost among the copies, the greatest loop depth, and how many copies there are.
// An AllocationRemarkRecord counts only the copies that have the outcome it reports: a place in the frame in one copy and on the heap in another is Heap with one copy.
// Accesses, TypeTests and AllocationRemarks ascend by module, position, kind and function, which tells all of them apart, so the threads' order is gone.
// AccessCounts ascends by module, OtherOrigins by content.
// FunctionNames ascends by function and LayoutNames by layout, each id once: Bun's src/aot/report/names.rs finds a record by binary search.
void writeExplainSections(const Typed::Program&, const Typed::Facts&, std::span<const Typed::WalkNotes>, BuildContext&, CompileOutput&);

// AOTCodeReports.cpp
// BytesByModule always. BytesByForm and Symbols where the options ask for them.
// The map of the image (LoggingOptions::mapFilePath) is no section. AOTDriver.cpp writes it.
void writeCodeReportSections(const Typed::Program&, const BuiltImage&, BuildContext&, CompileOutput&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
