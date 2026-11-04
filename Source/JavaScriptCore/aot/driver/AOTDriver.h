/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTHostFunctionTexts.h"
#include "AOTSchemaEnums.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "JSExportMacros.h"
#include <array>
#include <memory>
#include <span>
#include <type_traits>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

class BuildContext;
struct ExternalFunctionBodies;
struct HostThreadPool;

namespace Typed {
class Program;
}

// The engine behind Bun__AOT__compile. This header is all that Bun's bindings and the shell of the tests include to compile a program.
//
//   stage      booked under          what runs                                                          parallel by
//   Read       Stage::ReadAndVerify  readProgramWithExternalBodies, the verifiers                       table
//   Facts      Stage::AnalyzeFlow    computeFacts: the scan, going up, then Link                        function, then table
//   Plan       Stage::PlanSlots      planProgram, the stub catalog                                      layout
//   Compile    Stage::GenerateCode   compileQueuedUnits: walk > sweep > emit > allocate > encode         unit, start to finish, callers first
//   Image      Stage::BuildImage     buildImage                                                         table and function
//
// checkPlan() runs in every build. Where the build makes an image and has a second thread it is a lane beside Compile, and Image waits for it. Else it ends Plan.
// Each stage runs once. A stage that has reported an error ends the build behind it: the output then has diagnostics and statistics and no image.
// Nothing in the output asks for another compile.
// What the stages have left is destroyed on the host's pool, and the call does not wait for that: BuildContext::backgroundJobs().

inline constexpr unsigned numberOfResultSections = numberOfEnumerators<Wire::ResultSection>;
inline constexpr size_t alignmentOfResultSections = 8;

// What the engine hands back: the sections of ir/schema/compile-call.def, each already in the bytes that Rust reads.
// A section holds records of one type, or plain bytes. Every Wire::TextRange of every record leads into ResultSection::Text.
//
//   always                              Image Text UserDiagnostics InternalErrors EntryModuleIds Statistics BytesByModule
//                                       UnseenCode BuiltinsUsed FeaturesUsed LocalesUsed WorkerEntries WorkerStartsWithoutName URLsThatStartWorkers
//                                       LibraryFunctionsLacked LibraryModulesLacked LibraryModulesWithoutRecord
//   OutputOptions::Obfuscate            ObfuscatedSites
//   ReportSet::Flow                     Accesses OtherOrigins TypeTests LayoutNames, and with an image BytesByForm
//   ReportSet::AccessCounts             AccessCounts
//   ReportSet::Allocations              AllocationRemarks
//   ReportSet::Flow or Allocations      FunctionNames
//   ReportSet::Symbols                  Symbols
//   never                               RemainingChecks Declines Losses Functions OperationCounts PassTimes LimitValues
//                                       ArrivalSites ArrivalSiteLayouts ArrivalLayouts ArrivalLayoutFields
//                                       ExportsThatSpiesReplace BuiltinsThatTestsReplace NamesThatTestsAdd
//
// CompileAction::CheckOnly gives everything but Image, Symbols, ObfuscatedSites, BytesByForm and BytesByModule. It stops after Compile and drops the code:
// the walk takes the notes that the reports are made of.
// What each record means to its reader: Bun's src/aot/CONTRACT-report.md, section 2.
class CompileOutput {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CompileOutput);
    WTF_MAKE_NONCOPYABLE(CompileOutput);
public:
    CompileOutput() = default;

    std::span<const uint8_t> section(Wire::ResultSection section) const { return m_sections[static_cast<unsigned>(section)].span(); }
    bool hasImage() const { return !section(Wire::ResultSection::Image).empty(); }

    template<typename Record>
    std::span<const Record> records(Wire::ResultSection section) const
    {
        static_assert(std::is_trivially_copyable_v<Record> && alignof(Record) <= alignmentOfResultSections);
        return spanReinterpretCast<const Record>(this->section(section));
    }

    void setSection(Wire::ResultSection section, Vector<uint8_t>&& bytes) { m_sections[static_cast<unsigned>(section)] = WTF::move(bytes); }

    template<typename Record>
    void setRecords(Wire::ResultSection section, std::span<const Record> records)
    {
        static_assert(std::is_trivially_copyable_v<Record> && alignof(Record) <= alignmentOfResultSections);
        setSection(section, Vector<uint8_t>(asBytes(records)));
    }

    // Into ResultSection::Text. An empty range where the text would pass four gigabytes.
    JS_EXPORT_PRIVATE Wire::TextRange appendText(std::span<const uint8_t> utf8);
    JS_EXPORT_PRIVATE Wire::TextRange appendText(const String&);

private:
    std::array<Vector<uint8_t>, numberOfResultSections> m_sections;
};

// The engine reads the program's bytes and the external bodies in place until it calls release(owner): at most once, on any thread, behind Compile.
// Where the bodies are external, the records that it has read are all that it wanted of the bytes: it calls releaseBytes(owner) behind the read, at most once, in front of release().
// The owner itself outlives the call of the engine: the texts of functions are asked of it in Image.
struct ProgramOwner {
    void (*release)(void* owner) { nullptr };
    void* owner { nullptr };
    // Null: the bodies lie in the program's bytes.
    const ExternalFunctionBodies* externalBodies { nullptr };
    // Of a module whose text the program does not hold.
    HostFunctionTexts functionTexts { };
    void (*releaseBytes)(void* owner) { nullptr };
};

// Never null.
JS_EXPORT_PRIVATE std::unique_ptr<CompileOutput> compileProgram(std::span<const uint8_t> program, const Wire::CompileOptions&, const HostThreadPool* = nullptr, ProgramOwner = { });
// For tests, whose programs come from text. The bodies lie in the program.
JS_EXPORT_PRIVATE std::unique_ptr<CompileOutput> compileParsedProgram(std::unique_ptr<Typed::Program>, BuildContext&);
// The same builds with mechanisms withheld are for testaot alone: AOTStages.h.

JS_EXPORT_PRIVATE size_t imageAlignment();
// The most memory that the process has had resident. The log of phases resets the system's own mark at every line, and getrusage() reads that mark.
JS_EXPORT_PRIVATE size_t peakResidentMegabytesOfProcess();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
