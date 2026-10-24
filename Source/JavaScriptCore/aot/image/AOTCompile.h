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

// The compiler calls release(owner) at most once, on any thread, when it has read the program bytes.
struct ProgramOwner {
    void (*release)(void* owner) { nullptr };
    void* owner { nullptr };
    const ExternalFunctionBodies* externalBodies { nullptr };
};

JS_EXPORT_PRIVATE std::unique_ptr<CompileOutput> compileProgram(std::span<const uint8_t> program, const Wire::CompileOptions&, const HostThreadPool* = nullptr, ProgramOwner = { });
JS_EXPORT_PRIVATE std::unique_ptr<CompileOutput> compileParsedProgram(std::unique_ptr<Typed::Program>, BuildContext&);
JS_EXPORT_PRIVATE size_t imageAlignment();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
