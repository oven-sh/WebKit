/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrivalSiteNumbers.h"
#include "AOTDiagnostics.h"
#include "AOTFlowReport.h"
#include "AOTID.h"
#include "AOTLosses.h"
#include "AOTObfuscatedSites.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaEnums.h"
#include "AOTStatistics.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "JSExportMacros.h"
#include <array>
#include <memory>
#include <span>
#include <utility>
#include <wtf/BitSet.h>
#include <wtf/FastMalloc.h>

namespace JSC { namespace AOT {

class BuildContext;
struct ExternalFunctionBodies;
struct HostThreadPool;

namespace Typed {
class Program;
}

struct ImageContents {
    uint32_t numberOfRegExpsWithCode { 0 };
    uint32_t numberOfRegExpsWithoutCode { 0 };
    uint32_t numberOfRegExpsWithUnicodePropertyLookup { 0 };
    uint32_t numberOfWebAssemblyModules { 0 };
};

struct FunctionReport {
    Typed::FunctionID source;
    Typed::FunctionRole role { Typed::FunctionRole::Source };
    uint32_t codeBytes { 0 };
    std::array<uint32_t, 3> accesses { };
    std::array<uint32_t, 4> accessCosts { };
    std::array<uint32_t, 3> calls { };
    Vector<std::pair<Typed::Opcode, uint32_t>> operations;
};

struct CompileOutput {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CompileOutput);

    Vector<uint8_t> image;
    Vector<uint32_t> entryModuleIDs;
    Diagnostics diagnostics;
    Statistics statistics;
    Vector<uint64_t> limitValues;
    Typed::FlowReport flowReport;
    Vector<Wire::LossRecord> losses;
    Vector<Wire::DeclineRecord> declines;
    Vector<AllocationRemark> allocationRemarks;
    Vector<FunctionReport> functionReports;
    Vector<uint8_t> symbols;
    ObfuscatedSites obfuscatedSites;
    UTF8CString arrivalSites;
    WTF::BitSet<numberOfEntries> entriesUsed;
    ImageContents contents;
};

// The lists of the flow report that say what a program uses of the caller's own code.
struct FlowFindings {
    const Wire::UnseenCodeRecord* unseenCode { nullptr };
    size_t unseenCodeCount { 0 };
    const Wire::BuiltinUseRecord* builtinsUsed { nullptr };
    size_t builtinsUsedCount { 0 };
    const Wire::LackedFunctionRecord* libraryFunctionsLacked { nullptr };
    size_t libraryFunctionsLackedCount { 0 };
};

// The compiler calls release(owner) at most once, on any thread, when it has read the program bytes.
// It calls goesOnBehindFlowAnalysis(owner, ..) at most once, on any thread, when the flow analysis has ended. False: the caller hands over
// another program in place of this one, so the build ends there, with the flow report and without an image.
struct ProgramOwner {
    void (*release)(void* owner) { nullptr };
    void* owner { nullptr };
    const ExternalFunctionBodies* externalBodies { nullptr };
    bool (*goesOnBehindFlowAnalysis)(void* owner, const FlowFindings*) { nullptr };
};

JS_EXPORT_PRIVATE std::unique_ptr<CompileOutput> compileProgram(std::span<const uint8_t> program, const Wire::CompileOptions&, const HostThreadPool* = nullptr, ProgramOwner = { });
JS_EXPORT_PRIVATE std::unique_ptr<CompileOutput> compileParsedProgram(std::unique_ptr<Typed::Program>, BuildContext&);
JS_EXPORT_PRIVATE size_t imageAlignment();

// For a program that holds expected results and nothing else: the plan of each, in their order, each behind its length in four bytes, the low one first. Empty where the bytes are no program.
JS_EXPORT_PRIVATE Vector<uint8_t> writeValidationPlans(std::span<const uint8_t> program, const Wire::CompileOptions&);

// For whoever reports that the process stops, or is told to: a signal handler may call them. They write text of whole lines and return its length, nothing where no build is at work.
// Where the calling thread's part of a build stands, and what gets a build around it.
JS_EXPORT_PRIVATE size_t describeWhereThisThreadStopped(std::span<char>);
// A line for each thread that is at work for a build.
JS_EXPORT_PRIVATE size_t describeWhatThreadsAreCompiling(std::span<char>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
