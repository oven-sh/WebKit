/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompile.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeGenerationDriver.h"
#include "AOTCodeIndex.h"
#include "AOTCodeTemplates.h"
#include "AOTCompileOptions.h"
#include "AOTFFIStubs.h"
#include "AOTIRProgram.h"
#include "AOTImageBuilder.h"
#include "AOTImageFormat.h"
#include "AOTImageSymbols.h"
#include "AOTPhaseTimes.h"
#include "AOTPipeline.h"
#include "AOTProgramTableEncoder.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTRegExpCompiler.h"
#include "AOTTextEncoder.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AOTWasmModules.h"
#include "JSLock.h"
#include "VM.h"
#include <cstdlib>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <wtf/CheckedArithmetic.h>
#include <wtf/Condition.h>
#include <wtf/DataLog.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ScopedLambda.h>
#include <wtf/StackAllocation.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>

namespace JSC { namespace AOT {

using PipelineRunner = ScopedLambda<std::unique_ptr<Typed::PipelineOutput>(const Typed::ReaderOfFinishedBodies&)>;
using LevelCounts = decltype(FunctionReport::accesses);

static_assert(std::is_same_v<LevelCounts, decltype(FunctionReport::calls)>);
static_assert(std::tuple_size_v<LevelCounts> == numberOfEnumerators<Wire::KnowledgeLevel>);
static_assert(std::tuple_size_v<decltype(FunctionReport::accessCosts)> == numberOfEnumerators<Wire::AccessCost>);

namespace {

struct CodeAndTables {
    GeneratedCode code;
    Typed::EncodedProgramTables tables;
    EncodedText text;
    CompiledRegExps regExps;
};

struct GeneratedProgram {
    CodeGenerationPlan plan;
    CodeAndTables generated;
};

struct PreparedProgram {
    std::optional<CodeGenerationPlan> plan;
    Typed::FlowReportBodyRecords bodyRecords;
};

struct ImageParts {
    WebAssemblyImageInputs webAssembly;
    FFIStubs ffiStubs;
};

struct SiteCounts {
    friend bool operator==(const SiteCounts&, const SiteCounts&) = default;

    LevelCounts accesses { };
    decltype(FunctionReport::accessCosts) accessCosts { };
    LevelCounts calls { };
};

enum class WhatTheBuildLeaves : uint8_t { DiesBeforeTheCallReturns, DiesBesideTheCaller };

class CallerOfBuildThread final : public ThreadSafeRefCounted<CallerOfBuildThread> {
public:
    static Ref<CallerOfBuildThread> create() { return adoptRef(*new CallerOfBuildThread); }

    void release()
    {
        Locker locker { m_lock };
        m_isReleased = true;
        m_condition.notifyOne();
    }

    void wait()
    {
        Locker locker { m_lock };
        m_condition.wait(m_lock, [this] {
            assertIsHeld(m_lock);
            return m_isReleased;
        });
    }

private:
    CallerOfBuildThread() = default;

    Lock m_lock;
    WTF::Condition m_condition;
    bool m_isReleased WTF_GUARDED_BY_LOCK(m_lock) { false };
};

} // anonymous namespace

#if ASAN_ENABLED
// What still waits for its destruction when the process ends is held by memory that LeakSanitizer does not read.
static void waitAtExitFor(Ref<Thread>&& thread)
{
    static Lock lock;
    static LazyNeverDestroyed<WTF::Vector<Ref<Thread>>> threads;
    static std::once_flag once;
    std::call_once(once, [] {
        threads.construct();
        std::atexit([] {
            Locker locker { lock };
            for (auto& thread : threads.get())
                thread->waitForCompletion();
        });
    });
    Locker locker { lock };
    threads->append(WTF::move(thread));
}
#endif

static CodeAndTables generateCodeAndTables(Typed::PipelineOutput& pipeline, const CodeGenerationPlan& plan, NOESCAPE const ScopedLambda<void()>& buildReport, BuildContext& context)
{
    Typed::CodeGenerationInputs inputs = createCodeGenerationInputs(pipeline, plan);
    CodeGenerationJob codeGeneration(inputs, *pipeline.program, context, &plan.bodiesWithoutCode);

    CodeAndTables result;
    // Most of encoding the text runs on one thread, so it runs beside code generation, which is longer.
    auto encodeTablesAndText = [&] {
        result.tables = Typed::encodeProgramTables({ pipeline, plan.instanceSizes, plan.stringConstants, plan.bodyFactsForTables }, context);
        if (!context.diagnostics().hasErrors())
            result.text = encodeText({ *pipeline.program, plan.uses.strings, context.identifierNumbering(), plan.stringConstants, result.tables.namedStrings, &result.tables.stringsOnlyShown }, context);
    };
    auto generateCode = [&] {
        result.code = codeGeneration.run();
    };
    auto compileRegExps = [&] {
        result.regExps = pipeline.takeCompiledRegExps(context);
    };
    // So does most of the flow report, which reads what flow has left and no body.
    auto buildFlowReport = [&] {
        buildReport();
    };
    runTasksInParallel(context.parallelLoops(), { encodeTablesAndText, generateCode, compileRegExps, buildFlowReport });
    return result;
}

static void prepareProgram(const Typed::PipelineOutput& pipeline, BuildContext& context, PreparedProgram& prepared)
{
    runTasksInParallel(context.parallelLoops(), {
        [&] { prepared.plan.emplace(prepareCodeGeneration(pipeline, context)); },
        [&] { prepared.bodyRecords = Typed::collectFlowReportBodyRecords(*pipeline.program, context); },
    });
}

static GeneratedProgram generateProgram(Typed::PipelineOutput& pipeline, PreparedProgram& prepared, NOESCAPE const ScopedLambda<void()>& buildReport, BuildContext& context, CompileOutput& output)
{
    PhaseTimes times(context, Stage::GenerateCode);
    if (!prepared.plan) {
        prepareProgram(pipeline, context, prepared);
        times.didFinish("prepare code generation, collect type tests"_s);
    }
    CodeGenerationPlan plan = WTF::move(*prepared.plan);
    if (context.diagnostics().hasErrors()) {
        buildReport();
        return { WTF::move(plan), { } };
    }

    if (hasFlag(context.options().reports, ReportSet::Arrivals) && context.recordedOptions().countsArrivals) {
        output.arrivalSites = Typed::describeArrivalSites({ *pipeline.program, pipeline.flow.get() }, plan.arrivalSites, context);
        times.didFinish("describe arrival sites"_s);
    }

    CodeAndTables generated = generateCodeAndTables(pipeline, plan, buildReport, context);
    times.didFinish("generate code, encode program tables and text, compile regular expressions"_s);
    return { WTF::move(plan), WTF::move(generated) };
}

static WebAssemblyImageInputs compileWebAssembly(const Typed::Program& program, const BitVector& stringsInImage, BuildContext& context)
{
    if (!mayHaveWebAssemblyModules(program))
        return { };

    std::expected<CodeTemplates, LinkFailure> templates = CodeTemplates::create(context.assemblerPolicy(), &context);
    if (!templates) {
        reportLinkFailure(templates.error(), { }, context);
        return { };
    }

    RefPtr<VM> vm = VM::create(HeapType::Small);
    JSLockHolder locker(*vm);
    WebAssemblyImageInputs modules = compileWebAssemblyModules(*vm, program, *templates, context, &stringsInImage);
    // The lock holder is left with the last reference, so the VM dies under its lock.
    vm = nullptr;
    return modules;
}

static std::optional<ImageParts> createImageParts(const Typed::PipelineOutput& pipeline, const CodeGenerationPlan& plan, BuildContext& context)
{
    PhaseTimes times(context, Stage::BuildImage);
    Diagnostics& diagnostics = context.diagnostics();

    ImageParts parts;
    parts.webAssembly = compileWebAssembly(*pipeline.program, plan.uses.strings.isNamed, context);
    times.didFinish("compile WebAssembly"_s);
    if (diagnostics.hasErrors())
        return std::nullopt;

    if (mayUseFFI(pipeline))
        parts.ffiStubs = generateFFIStubs(context);
    times.didFinish("generate FFI stubs"_s);
    if (diagnostics.hasErrors())
        return std::nullopt;
    return WTF::move(parts);
}

static BuiltImage createImage(const Typed::PipelineOutput& pipeline, GeneratedProgram& generatedProgram, BuildContext& context, CompileOutput& output)
{
    if (context.diagnostics().hasErrors())
        return { };

    const CodeGenerationPlan& plan = generatedProgram.plan;
    CodeAndTables& generated = generatedProgram.generated;
    std::optional<ImageParts> parts = createImageParts(pipeline, plan, context);
    if (!parts)
        return { };

    uint32_t numberOfRegExpsWithCode = generated.regExps.numberWithForm(RegExpRecordForm::Code);
    output.contents = {
        .numberOfRegExpsWithCode = numberOfRegExpsWithCode,
        .numberOfRegExpsWithoutCode = safeCast<uint32_t>(generated.regExps.results.size()) - numberOfRegExpsWithCode,
        .numberOfRegExpsWithUnicodePropertyLookup = generated.regExps.numberWithUnicodePropertyLookup,
        .numberOfWebAssemblyModules = parts->webAssembly.numberOfModules,
    };

    BuiltImage built = buildImage(ImageInputs {
        .pipeline = pipeline,
        .stubCatalog = *plan.stubCatalog,
        .instanceSizes = plan.instanceSizes,
        .functions = WTF::move(generated.code.functions),
        .entriesUsedByFunctions = generated.code.entriesUsed,
        .programTables = WTF::move(generated.tables.bytes),
        .text = WTF::move(generated.text),
        .regExpRequests = *pipeline.regExpRequests,
        .regExps = WTF::move(generated.regExps),
        .webAssembly = WTF::move(parts->webAssembly),
        .ffiStubs = WTF::move(parts->ffiStubs),
    }, context);
    output.entriesUsed = built.entriesUsed;
    output.image = WTF::move(built.image);
    RELEASE_ASSERT(!(output.image.size() % imagePageSize));
    return built;
}

static LevelCounts* countersOf(SiteCounts& counts, Wire::AccessKind kind)
{
    switch (kind) {
    case Wire::AccessKind::Unknown:
        return nullptr;
    case Wire::AccessKind::Read:
    case Wire::AccessKind::Store:
    case Wire::AccessKind::Define:
    case Wire::AccessKind::Has:
    case Wire::AccessKind::Delete:
        return &counts.accesses;
    case Wire::AccessKind::Call:
    case Wire::AccessKind::Construct:
        return &counts.calls;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<SiteCounts> countSitesByFunctionAsRead(const Typed::Program& program, std::span<const Wire::AccessRecord> accesses)
{
    Vector<SiteCounts> counts(program.numberOfFunctionsAsRead());
    for (const Wire::AccessRecord& access : accesses) {
        FunctionID function = findFunctionAsRead(&program, FunctionID(access.function));
        if (!function)
            continue;
        SiteCounts& sites = counts[function.index()];
        LevelCounts* counters = countersOf(sites, access.kind);
        if (!counters)
            continue;
        ++(*counters)[static_cast<size_t>(access.level)];
        if (counters == &sites.accesses)
            ++sites.accessCosts[static_cast<size_t>(access.cost)];
    }
    return counts;
}

static uint32_t codeBytesOf(const BuiltImage& built, const CodeIndex& index, FunctionID function)
{
    std::optional<uint32_t> position = index.indexOf(function);
    if (!position)
        return 0;
    return safeCast<uint32_t>(built.functions[*position].body.bytes.size() + built.outlinedCode.ownBodiesSize(*position));
}

static Vector<FunctionReport> collectFunctionReports(const Typed::Program& program, const BuiltImage& built, std::span<OperationCounts> operationCountsByFunction, std::span<const SiteCounts> sitesByFunctionAsRead, BuildContext& context)
{
    CodeIndex index(built.functions.span());
    return collectInParallel<FunctionReport>(context.parallelLoops(), program.functions.size(), [&](unsigned functionIndex, Vector<FunctionReport>& reports) {
        const Typed::Function& function = program.functions[functionIndex];
        SiteCounts sites = functionIndex < sitesByFunctionAsRead.size() ? sitesByFunctionAsRead[functionIndex] : SiteCounts { };
        if (!function.hasBody() && sites == SiteCounts { })
            return;

        FunctionID id(functionIndex);
        reports.append(FunctionReport {
            .source = findFunctionAsRead(&program, id),
            .role = function.role,
            .codeBytes = codeBytesOf(built, index, id),
            .accesses = sites.accesses,
            .accessCosts = sites.accessCosts,
            .calls = sites.calls,
            .operations = function.bodyIsReleased ? WTF::move(operationCountsByFunction[functionIndex]) : countOperations(function),
        });
    });
}

static void writeSymbols(const Typed::PipelineOutput& pipeline, const BuiltImage& built, BuildContext& context, CompileOutput& output)
{
    const CompileOptions& options = context.options();
    bool encodesSymbols = hasFlag(options.reports, ReportSet::Symbols);
    if (!encodesSymbols && !options.writesCodeSizeMap())
        return;

    ImageSymbolInputs inputs { built, *pipeline.program, *pipeline.heat, *pipeline.regExpRequests };
    Vector<ImageSymbol> symbols = collectSymbols(inputs);
    if (encodesSymbols)
        output.symbols = encodeSymbols(symbols.span(), output.image.span());
    if (options.writesCodeSizeMap())
        writeMapFile(options.logging.mapFilePath, symbols.span(), inputs, output.image.span(), context);
}

static void writeReports(const Typed::PipelineOutput& pipeline, const BuiltImage& built, std::span<OperationCounts> operationCountsByFunction, std::span<const SiteCounts> sitesByFunctionAsRead, BuildContext& context, CompileOutput& output)
{
    PhaseTimes times(context, Stage::BuildImage);
    const CompileOptions& options = context.options();
    const Typed::Program& program = *pipeline.program;

    if (options.obfuscates()) {
        output.obfuscatedSites = collectObfuscatedSites(built.functions, program, context);
        times.didFinish("collect obfuscated sites"_s);
    }
    if (hasFlag(options.reports, ReportSet::Functions)) {
        output.functionReports = collectFunctionReports(program, built, operationCountsByFunction, sitesByFunctionAsRead, context);
        times.didFinish("collect function reports"_s);
    }
    writeSymbols(pipeline, built, context, output);
    times.didFinish("write symbols"_s);
}

static Vector<SiteCounts> buildFlowReport(Typed::PipelineOutput& pipeline, Typed::FlowReportBodyRecords&& bodyRecords, BuildContext& context, CompileOutput& output)
{
    const CompileOptions& options = context.options();
    const Typed::Program& program = *pipeline.program;
    Vector<Typed::FlowSiteLevel> accessSiteLevels = Typed::gradeAccessSites(*pipeline.flow, WTF::move(pipeline.accessSiteObservations), context);
    output.flowReport = Typed::buildFlowReport(program, *pipeline.flow, accessSiteLevels.span(), pipeline.remainingChecks.span(), WTF::move(bodyRecords), context);
    Vector<SiteCounts> sitesByFunctionAsRead;
    if (hasFlag(options.reports, ReportSet::Functions))
        sitesByFunctionAsRead = countSitesByFunctionAsRead(program, output.flowReport.accesses.span());
    if (!hasFlag(options.reports, ReportSet::Flow))
        output.flowReport.accesses.clear();
    return sitesByFunctionAsRead;
}

static void generateImage(Typed::PipelineOutput& pipeline, PreparedProgram& prepared, BuildContext& context, CompileOutput& output, WhatTheBuildLeaves whatTheBuildLeaves)
{
    RELEASE_ASSERT(pipeline.isComplete);
    Vector<SiteCounts> sitesByFunctionAsRead;
    GeneratedProgram generated = generateProgram(pipeline, prepared, [&] {
        sitesByFunctionAsRead = buildFlowReport(pipeline, WTF::move(prepared.bodyRecords), context, output);
    }, context, output);
    pipeline.releaseCodeGenerationInputs(context);
    BuiltImage built = createImage(pipeline, generated, context, output);
    if (!context.diagnostics().hasErrors())
        writeReports(pipeline, built, generated.generated.code.operationCountsByFunction.mutableSpan(), sitesByFunctionAsRead.span(), context, output);
    if (whatTheBuildLeaves == WhatTheBuildLeaves::DiesBeforeTheCallReturns) {
        PhaseTimes times(context, Stage::BuildImage);
        built.functions.clearOnAllThreads(context);
        times.didFinish("destroy code"_s);
    }
    context.destroyLater(WTF::move(built));
    context.destroyLater(WTF::move(generated));
}

static void destroyFunctions(Typed::Program& program, BuildContext& context)
{
    PhaseTimes times(context, Stage::BuildImage);
    forEachIndexInParallel(context.parallelLoops(), program.functions.size(), [&](unsigned index) {
        Typed::Function function = WTF::move(program.functions[index]);
    });
    times.didFinish("destroy functions"_s);
}

static void makeReportOrImage(Typed::PipelineOutput& pipeline, PreparedProgram& prepared, BuildContext& context, CompileOutput& output, WhatTheBuildLeaves whatTheBuildLeaves)
{
    if (context.options().isVerifyOnly()) {
        PhaseTimes times(context, Stage::AnalyzeFlow);
        buildFlowReport(pipeline, Typed::collectFlowReportBodyRecords(*pipeline.program, context), context, output);
        times.didFinish("collect type tests, build flow report"_s);
    } else
        generateImage(pipeline, prepared, context, output, whatTheBuildLeaves);
}

static void run(NOESCAPE const PipelineRunner& runPipeline, BuildContext& context, CompileOutput& output, WhatTheBuildLeaves whatTheBuildLeaves)
{
    Diagnostics& diagnostics = context.diagnostics();
    if (diagnostics.hasErrors()) {
        diagnostics.finish(nullptr);
        return;
    }

    PreparedProgram prepared;
    std::unique_ptr<Typed::PipelineOutput> pipeline = runPipeline([&](const Typed::PipelineOutput& finishedPipeline) {
        PhaseTimes times(context, PassID::GenerateCode);
        prepareProgram(finishedPipeline, context, prepared);
        times.didFinish("prepare code generation, collect type tests"_s);
    });
    if (!pipeline)
        return;

    const Typed::Program& program = *pipeline->program;
    output.entryModuleIDs = program.entries.map([](const Typed::Entry& entry) {
        return entry.module.index() + 1;
    });
    if (!context.options().isCheckOnly())
        makeReportOrImage(*pipeline, prepared, context, output, whatTheBuildLeaves);

    diagnostics.finish(&program);
    output.losses = context.takeLosses(&program);
    output.declines = context.takeDeclines(&program);
    output.allocationRemarks = context.takeAllocationRemarks();
    if (whatTheBuildLeaves == WhatTheBuildLeaves::DiesBeforeTheCallReturns)
        destroyFunctions(*pipeline->program, context);
    context.destroyLater(WTF::move(pipeline));
}

static void takeResults(BuildContext& context, CompileOutput& output, WhatTheBuildLeaves whatTheBuildLeaves)
{
    if (whatTheBuildLeaves == WhatTheBuildLeaves::DiesBeforeTheCallReturns) {
        PhaseTimes times(context, Stage::BuildImage);
        context.destroyBackEndScratches();
        context.finishDeferredDestruction();
        times.didFinish("destroy results"_s);
    }
    output.diagnostics = context.takeDiagnostics();
    if (context.options().logging.logsPhaseTimes) [[unlikely]]
        context.statistics().dumpPassTimes(WTF::dataFile());
    output.statistics = context.takeStatistics();
    output.limitValues = Vector<uint64_t>(context.limits().span());
    if (output.diagnostics.hasErrors()) {
        output.image.clear();
        output.symbols.clear();
        output.obfuscatedSites = { };
    }
}

std::unique_ptr<CompileOutput> compileProgram(std::span<const uint8_t> program, const Wire::CompileOptions& options, const HostThreadPool* hostPool, ProgramOwner owner)
{
    auto output = makeUnique<CompileOutput>();
    Ref<CallerOfBuildThread> caller = CallerOfBuildThread::create();
    Ref<Thread> buildThread = Thread::create("AOT build"_s, [&, caller] {
        BuildContext context(options, hostPool);
        run([&](const Typed::ReaderOfFinishedBodies& readBesideLastEffectSummaries) {
            return Typed::runPipeline(program, owner.externalBodies, context, [&] {
                if (owner.release)
                    owner.release(owner.owner);
            }, readBesideLastEffectSummaries);
        }, context, *output, WhatTheBuildLeaves::DiesBesideTheCaller);
        takeResults(context, *output, WhatTheBuildLeaves::DiesBesideTheCaller);
        context.parallelLoops().sendRestingThreadsBack();
        caller->release();
    }, ThreadType::Compiler, Thread::defaultQOS, Thread::defaultSchedulingPolicy, StackAllocationSpecification::RequestSize(ParallelLoops::workerStackSize));
#if ASAN_ENABLED
    waitAtExitFor(WTF::move(buildThread));
#else
    buildThread->detach();
#endif
    caller->wait();
    return output;
}

std::unique_ptr<CompileOutput> compileParsedProgram(std::unique_ptr<Typed::Program> program, BuildContext& context)
{
    auto output = makeUnique<CompileOutput>();
    runOnBuildThread([&] {
        run([&](const Typed::ReaderOfFinishedBodies& readBesideLastEffectSummaries) {
            return Typed::runPipeline(WTF::move(program), context, readBesideLastEffectSummaries);
        }, context, *output, WhatTheBuildLeaves::DiesBeforeTheCallReturns);
        takeResults(context, *output, WhatTheBuildLeaves::DiesBeforeTheCallReturns);
    });
    return output;
}

size_t imageAlignment() { return imagePageSize; }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
