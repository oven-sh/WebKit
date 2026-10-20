/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompile.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
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
#include "AOTRegExpCompiler.h"
#include "AOTTextEncoder.h"
#include "AOTThreads.h"
#include "AOTWasmModules.h"
#include "JSLock.h"
#include "VM.h"
#include <optional>
#include <tuple>
#include <type_traits>
#include <wtf/CheckedArithmetic.h>
#include <wtf/DataLog.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

using PipelineRunner = ScopedLambda<std::unique_ptr<Typed::PipelineOutput>()>;
using LevelCounts = decltype(FunctionReport::accesses);

static_assert(std::is_same_v<LevelCounts, decltype(FunctionReport::calls)>);
static_assert(std::tuple_size_v<LevelCounts> == numberOfEnumerators<Wire::KnowledgeLevel>);

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

struct ImageParts {
    WebAssemblyImageInputs webAssembly;
    FFIStubs ffiStubs;
};

struct SiteCounts {
    friend bool operator==(const SiteCounts&, const SiteCounts&) = default;

    LevelCounts accesses { };
    LevelCounts calls { };
};

} // anonymous namespace

static CodeAndTables generateCodeAndTables(const Typed::PipelineOutput& pipeline, const CodeGenerationPlan& plan, BuildContext& context)
{
    Typed::CodeGenerationInputs inputs = createCodeGenerationInputs(pipeline, plan);

    CodeAndTables result;
    // Most of encoding the text runs on one thread, so it runs beside code generation, which is longer.
    auto encodeTablesAndText = [&] {
        result.tables = Typed::encodeProgramTables({ pipeline, plan.instanceSizes, plan.stringConstants }, context);
        if (!context.diagnostics().hasErrors())
            result.text = encodeText({ *pipeline.program, plan.uses.strings, context.identifierNumbering(), plan.stringConstants, result.tables.namedStrings }, context);
    };
    auto generateCode = [&] {
        result.code = generateAllCode(inputs, pipeline.program->types, context);
    };
    auto compileRegExps = [&] {
        result.regExps = AOT::compileRegExps(*pipeline.program, *pipeline.regExpRequests, context);
    };
    runTasksInParallel(context.parallelLoops(), { encodeTablesAndText, generateCode, compileRegExps });
    return result;
}

static GeneratedProgram generateProgram(const Typed::PipelineOutput& pipeline, BuildContext& context)
{
    PhaseTimes times(context, Stage::GenerateCode);
    CodeGenerationPlan plan = prepareCodeGeneration(pipeline, context);
    times.didFinish("prepare code generation"_s);
    if (context.diagnostics().hasErrors())
        return { WTF::move(plan), { } };

    CodeAndTables generated = generateCodeAndTables(pipeline, plan, context);
    times.didFinish("generate code, encode program tables and text, compile regular expressions"_s);
    return { WTF::move(plan), WTF::move(generated) };
}

static WebAssemblyImageInputs compileWebAssembly(const Typed::Program& program, BuildContext& context)
{
    if (program.webAssemblyModules.isEmpty())
        return { };

    std::expected<CodeTemplates, LinkFailure> templates = CodeTemplates::create(context.assemblerPolicy(), &context);
    if (!templates) {
        reportLinkFailure(templates.error(), { }, context);
        return { };
    }

    RefPtr<VM> vm = VM::create(HeapType::Small);
    JSLockHolder locker(*vm);
    WebAssemblyImageInputs modules = compileWebAssemblyModules(*vm, program, *templates, context);
    // The lock holder is left with the last reference, so the VM dies under its lock.
    vm = nullptr;
    return modules;
}

static std::optional<ImageParts> createImageParts(const Typed::PipelineOutput& pipeline, BuildContext& context)
{
    PhaseTimes times(context, Stage::BuildImage);
    Diagnostics& diagnostics = context.diagnostics();

    ImageParts parts;
    parts.webAssembly = compileWebAssembly(*pipeline.program, context);
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
    std::optional<ImageParts> parts = createImageParts(pipeline, context);
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
        if (LevelCounts* counters = countersOf(counts[function.index()], access.kind))
            ++(*counters)[static_cast<size_t>(access.level)];
    }
    return counts;
}

static Vector<std::pair<Typed::Opcode, uint32_t>> countOperations(const Typed::Function& function)
{
    if (!function.hasBody())
        return { };

    std::array<uint32_t, numberOfEnumerators<Typed::Opcode>> counts { };
    Typed::CFG cfg(function);
    for (Typed::BlockID block : cfg.reversePostOrder()) {
        for (Typed::ValueID value : function.at(block).values)
            ++counts[static_cast<size_t>(function.at(value).opcode)];
    }

    Vector<std::pair<Typed::Opcode, uint32_t>> operations;
    for (Typed::Opcode opcode : allMembersOf<Typed::Opcode>) {
        if (uint32_t count = counts[static_cast<size_t>(opcode)])
            operations.append({ opcode, count });
    }
    return operations;
}

static uint32_t codeBytesOf(const BuiltImage& built, const CodeIndex& index, FunctionID function)
{
    std::optional<uint32_t> position = index.indexOf(function);
    if (!position)
        return 0;
    return safeCast<uint32_t>(built.functions[*position].body.bytes.size() + built.outlinedCode.ownBodiesSize(*position));
}

static Vector<FunctionReport> collectFunctionReports(const Typed::Program& program, const BuiltImage& built, std::span<const Wire::AccessRecord> accesses, BuildContext& context)
{
    Vector<SiteCounts> sitesByFunctionAsRead = countSitesByFunctionAsRead(program, accesses);
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
            .calls = sites.calls,
            .operations = countOperations(function),
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

static void writeReports(const Typed::PipelineOutput& pipeline, const BuiltImage& built, BuildContext& context, CompileOutput& output)
{
    PhaseTimes times(context, Stage::BuildImage);
    const CompileOptions& options = context.options();
    const Typed::Program& program = *pipeline.program;

    if (options.obfuscates()) {
        output.obfuscatedSites = collectObfuscatedSites(built.functions, program, context);
        times.didFinish("collect obfuscated sites"_s);
    }
    if (hasFlag(options.reports, ReportSet::Functions)) {
        output.functionReports = collectFunctionReports(program, built, output.flowReport.accesses.span(), context);
        times.didFinish("collect function reports"_s);
    }
    writeSymbols(pipeline, built, context, output);
    times.didFinish("write symbols"_s);
}

static void generateImage(const Typed::PipelineOutput& pipeline, BuildContext& context, CompileOutput& output)
{
    RELEASE_ASSERT(pipeline.isComplete);
    GeneratedProgram generated = generateProgram(pipeline, context);
    BuiltImage built = createImage(pipeline, generated, context, output);
    if (!context.diagnostics().hasErrors())
        writeReports(pipeline, built, context, output);
    {
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

static void run(NOESCAPE const PipelineRunner& runPipeline, BuildContext& context, CompileOutput& output)
{
    Diagnostics& diagnostics = context.diagnostics();
    if (diagnostics.hasErrors()) {
        diagnostics.finish(nullptr);
        return;
    }

    std::unique_ptr<Typed::PipelineOutput> pipeline = runPipeline();
    if (!pipeline)
        return;

    const CompileOptions& options = context.options();
    const Typed::Program& program = *pipeline->program;
    output.entryModuleIDs = program.entries.map([](const Typed::Entry& entry) {
        return entry.module.index() + 1;
    });
    {
        PhaseTimes times(context, Stage::AnalyzeFlow);
        output.flowReport = Typed::buildFlowReport(program, *pipeline->flow, pipeline->accessSiteLevels.span(), context);
        times.didFinish("build flow report"_s);
    }

    if (!options.isVerifyOnly())
        generateImage(*pipeline, context, output);
    if (!hasFlag(options.reports, ReportSet::Flow))
        output.flowReport.accesses.clear();

    diagnostics.finish(&program);
    output.losses = context.takeLosses(&program);
    output.declines = context.takeDeclines(&program);
    output.allocationRemarks = context.takeAllocationRemarks();
    destroyFunctions(*pipeline->program, context);
    context.destroyLater(WTF::move(pipeline));
}

static void takeResults(BuildContext& context, CompileOutput& output)
{
    {
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

std::unique_ptr<CompileOutput> compileProgram(std::span<const uint8_t> program, const Wire::CompileOptions& options, const HostThreadPool* hostPool)
{
    auto output = makeUnique<CompileOutput>();
    runOnBuildThread([&] {
        BuildContext context(options, hostPool);
        run([&] {
            return Typed::runPipeline(program, context);
        }, context, *output);
        takeResults(context, *output);
    });
    return output;
}

std::unique_ptr<CompileOutput> compileParsedProgram(std::unique_ptr<Typed::Program> program, BuildContext& context)
{
    auto output = makeUnique<CompileOutput>();
    runOnBuildThread([&] {
        run([&] {
            return Typed::runPipeline(WTF::move(program), context);
        }, context, *output);
        takeResults(context, *output);
    });
    return output;
}

size_t imageAlignment() { return imagePageSize; }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
