/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestCommand.h"

#if ENABLE(AOT)

#include "AOTB3Dump.h"
#include "AOTCodeGenerationDriver.h"
#include "AOTCodeShapes.h"
#include "AOTCompileFunctions.h"
#include "AOTEffectSummaries.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionCodeGenerator.h"
#include "AOTHeat.h"
#include "AOTInterningPhases.h"
#include "AOTNativeTest.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTPipelineInternal.h"
#include "AOTPlacement.h"
#include "AOTRuntimeTable.h"
#include "AOTSavedFrameMap.h"
#include "AOTSchemaEnums.h"
#include "AOTScratchRegisterChecker.h"
#include "AOTSort.h"
#include "AOTStatistics.h"
#include "AOTStubDescriptor.h"
#include "AOTSupportTestCommands.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include "B3Generate.h"
#include <wtf/DataLog.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class ChosenRepresentations : uint8_t { InNoFunction, InSomeFunctions, InEveryFunction };

struct PrintedParts {
    Vector<StatisticID> statistics;
    bool calls { false };
    bool units { false };
    bool removesGuardOperands { false };
};

struct B3DumpRequest {
    enum class Point : uint8_t { Lowered, AfterStep, Optimized, Air };

    Point point { Point::Lowered };
    String step;
    std::optional<unsigned> runOfStep;
    B3DumpScope scope { B3DumpScope::WholeProcedure };
    Vector<FunctionID> functions;
    bool printsOnlyEntryModules { false };
};

} // anonymous namespace

static std::optional<PrintedParts> parsePrintedParts(TestCommandContext& context)
{
    PrintedParts parts;
    for (const String& word : context.arguments) {
        if (word == "calls"_s) {
            parts.calls = true;
            continue;
        }
        if (word == "units"_s) {
            parts.units = true;
            continue;
        }
        if (word == "remove-guard-operands"_s) {
            parts.removesGuardOperands = true;
            continue;
        }
        std::optional<StatisticID> statistic = parseEnum<StatisticID>(word);
        if (!statistic) {
            printBadArgument(context.out, word);
            return std::nullopt;
        }
        parts.statistics.append(*statistic);
    }
    return parts;
}

static void skipEarlierProducers(Pipeline& pipeline)
{
    IRForm form = pipeline.program().form();
    for (PassID id : allMembersOf<PassID>) {
        if (id == PassID::None)
            continue;
        const PassDescriptor& descriptor = descriptorOf(id);
        if (descriptor.lastForm < form)
            pipeline.runner().didCompute(descriptor.outputs);
    }
}

// Selection writes a shape into every block, and text has no word for one.
static void chooseShapesOfBlocks(BuildContext& build, Pipeline& pipeline)
{
    Program& program = pipeline.program();
    OptimizationInputs inputs = pipeline.createInputs({ ProgramResult::Flow, ProgramResult::CallGraph, ProgramResult::EffectSummaries, ProgramResult::Heat });
    TaskArenas arenas(program);
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        Function& function = program.functions[index];
        if (!function.hasBody())
            continue;
        FunctionID id(index);
        FunctionAnalyses analyses(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, function);
        FunctionPassContext pass { program, inputs, id, function, analyses, arenas.types, arenas.strings, build };
        chooseCodeShapes(pass, ChosenShapes::KeepChosen);
    }
}

static ChosenRepresentations findChosenRepresentations(const Program& program)
{
    bool hasChosen = false;
    bool hasUnchosen = false;
    for (const Function& function : program.functions) {
        if (!function.hasBody())
            continue;
        if (function.representationsAreChosen)
            hasChosen = true;
        else
            hasUnchosen = true;
    }
    if (!hasChosen)
        return ChosenRepresentations::InNoFunction;
    return hasUnchosen ? ChosenRepresentations::InSomeFunctions : ChosenRepresentations::InEveryFunction;
}

static void runStagesAfterPlacement(TestCommandContext& context, Pipeline& pipeline, ASCIILiteral command)
{
    switch (findChosenRepresentations(pipeline.program())) {
    case ChosenRepresentations::InNoFunction:
        pipeline.runRepresentationStage();
        break;
    case ChosenRepresentations::InSomeFunctions:
        context.out.println("error: ", command, " takes representations in every function or in none");
        return;
    case ChosenRepresentations::InEveryFunction:
        pipeline.verify(IRForm::Placed);
        break;
    }
    if (pipeline.runner().isStopped())
        return;
    PipelineOutput& output = pipeline.output();
    output.effects = pipeline.analyses().takeEffects(pipeline.flow());
    output.heat = pipeline.analyses().takeHeat(pipeline.flow());
    output.isComplete = true;
}

static std::unique_ptr<PipelineOutput> runRemainingStages(TestCommandContext& context, ASCIILiteral command)
{
    context.build.resolveFunctionToDump(*context.program);
    PlacedTableSizes tableSizes;
    PassRunner runner(context.build);
    Pipeline pipeline(*context.program, runner);
    skipEarlierProducers(pipeline);

    switch (context.form) {
    case IRForm::AsRead:
        pipeline.runAllStages();
        break;
    case IRForm::Prepared:
        context.out.println("error: ", command, " starts from AsRead, Selected or Placed");
        return nullptr;
    case IRForm::Selected:
        chooseShapesOfBlocks(context.build, pipeline);
        pipeline.runProgramPass(PassID::NumberLiteralStrings);
        pipeline.runProgramPass(PassID::PlanSlots);
        // The passes that rewrite a program in form Selected do not run, so code is generated for the operations of the test.
        pipeline.runProgramPass(PassID::ElideStackChecks);
        pipeline.runProgramPass(PassID::CollectRegExpLiterals);
        pipeline.runPlacementStage();
        runStagesAfterPlacement(context, pipeline, command);
        break;
    case IRForm::Placed:
        chooseShapesOfBlocks(context.build, pipeline);
        pipeline.runProgramPass(PassID::NumberLiteralStrings);
        tableSizes = pipeline.output().placement->tableSizes();
        runner.setVerifierInputs({ .flow = &pipeline.flow(), .placed = &tableSizes });
        runStagesAfterPlacement(context, pipeline, command);
        break;
    }
    if (!pipeline.output().isComplete)
        return nullptr;

    std::unique_ptr<PipelineOutput> output = pipeline.takeOutput();
    output->program = WTF::move(context.program);
    return output;
}

static Vector<FunctionID> collectFunctionsWithBody(const Program& program)
{
    Vector<FunctionID> ids;
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        if (program.functions[index].hasBody())
            ids.append(FunctionID(index));
    }
    return ids;
}

static Vector<CompiledCode> compileAllFunctions(const PipelineOutput& output, const CodeGenerationPlan& plan, BuildContext& build)
{
    const Program& program = *output.program;
    CodeGenerationInputs inputs = createCodeGenerationInputs(output, plan);
    Vector<FunctionID> ids = collectFunctionsWithBody(program);
    Vector<CompiledCode> code(ids.size());
    CodeGenerationTotals totals;
    ScratchTypeArena types(program.types);
    CodeGenerationWorker worker { build, types.arena(), build.backEndScratch(0) };
    compileFunctions(worker, inputs, ids.span(), code.mutableSpan(), totals);
    return code;
}

static void printFrame(PrintStream& out, const FrameDescription& frame)
{
    if (!frame.hasFrame()) {
        out.print("none");
        return;
    }
    out.print(frame.frameSizeInBytes);
    ASCIILiteral separator = "@"_s;
    for (const CodeRange& range : frame.frameRanges) {
        out.print(separator, range.begin, "-", range.end);
        separator = ","_s;
    }
}

static void printCode(PrintStream& out, const CompiledCode& code)
{
    out.print("f", code.function.index(), ": ");
    if (code.body.bytes.isEmpty()) {
        out.println("no code");
        return;
    }
    const SiteDescription& sites = code.sites;
    size_t numberOfSites = sites.positions.isEmpty() ? 0 : sites.positions.size() - 1;
    out.print("size=", code.body.bytes.size(), " frame=");
    printFrame(out, code.frame);
    out.print(" entries=", code.body.entries.size(), " sites=", numberOfSites, " handlers=", sites.handlers.size());
    out.print(" faults=", sites.faultingAccesses.size(), " polls=", sites.pollSites.size());
    out.print(" constants=", code.body.constants.size(), " jumpTables=", code.body.jumpTables.size());
    out.print(" prologueChecks=", code.frame.prologueChecks.size());
    size_t numberOfTailCallSites = std::ranges::count_if(sites.flaggedSites, [](const FlaggedSite& site) {
        return isTailCallOf(site.kind);
    });
    if (numberOfTailCallSites)
        out.print(" tailCallSites=", numberOfTailCallSites);
    if (!sites.constructedCallees.isEmpty()) {
        out.print(" constructedCallees=");
        CommaPrinter comma(","_s);
        for (const ConstructedCallee& row : sites.constructedCallees)
            out.print(comma, "f", code.body.callees[row.callee].index());
    }
    if (code.frame.isEnteredWithoutStackCheck)
        out.print(" enteredWithoutStackCheck");
    if (code.frame.isProvenToFitOnStack)
        out.print(" provenToFitOnStack");
    if (!code.frame.hasFrameRecord)
        out.print(" noFrameRecord");
    if (!sites.frameMapWords.isEmpty()) {
        // For each map: the references that it lists, and the other live words, if it lists every live word.
        out.print(" deadAtSuspensions=", code.frame.bytesDeadAtSuspensions, " liveWordsInMaps=");
        CommaPrinter comma(","_s);
        for (uint32_t offset = 0; offset < sites.frameMapWords.size();) {
            SavedFrameMap map = *SavedFrameMap::tryCreate(sites.frameMapWords.span(), offset);
            offset += map.numberOfWords();
            unsigned numberOfReferences = 0;
            unsigned numberOfOtherLiveWords = 0;
            auto count = [](unsigned& number) {
                return [&number](uint32_t) {
                    ++number;
                    return true;
                };
            };
            map.forEachValue(count(numberOfReferences));
            map.forEachNarrowReference(count(numberOfReferences));
            map.forEachOtherLiveWord(count(numberOfOtherLiveWords));
            out.print(comma, numberOfReferences);
            if (map.listsEveryLiveWord())
                out.print("+", numberOfOtherLiveWords);
        }
    }
    out.println();
}

static String describeCalledStub(StubKey key)
{
    if (isOperationCallHead(key))
        return makeString("operation "_s, nameOf(entryAtOffset(*key.immediate())));
    StringPrintStream name;
    dump(name, key);
    return makeString("stub "_s, name.toString());
}

static std::optional<String> describeCallee(const CodeBody& body, const Relocation& relocation)
{
    if (!transfersControl(relocation.kind))
        return std::nullopt;
    switch (relocation.space) {
    case SymbolSpace::Function:
        return makeString("function f"_s, body.callees[relocation.symbol].index());
    case SymbolSpace::Stub:
        return describeCalledStub(body.stubKeys[relocation.symbol]);
    case SymbolSpace::ArgumentThunk:
        return describeCalledStub(StubKey::fromBits(body.argumentMoves[relocation.symbol][0]));
    case SymbolSpace::OutlinedBody:
    case SymbolSpace::Constant:
    case SymbolSpace::JumpTable:
    case SymbolSpace::SiteCache:
    case SymbolSpace::RegExp:
    case SymbolSpace::WebAssembly:
    case SymbolSpace::FFI:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// One line per callee, sorted by its text: where a call lies in the code is no contract.
static void printCalls(PrintStream& out, const CodeBody& body)
{
    Vector<String> callees;
    for (const Relocation& relocation : body.relocations) {
        if (std::optional<String> callee = describeCallee(body, relocation))
            callees.append(WTF::move(*callee));
    }
    AOT::sort(callees, codePointCompareLessThan);
    for (size_t begin = 0; begin < callees.size();) {
        size_t end = begin + 1;
        while (end < callees.size() && callees[end] == callees[begin])
            ++end;
        out.println("  ", callees[begin], " x", end - begin);
        begin = end;
    }
}

static void printUnits(PrintStream& out, const CodeBody& body)
{
    size_t plainBytes = 0;
    size_t callBytes = 0;
    size_t pinnedBytes = 0;
    size_t endBytes = 0;
    size_t bytesThatMayRun = 0;
    for (const CodeUnit& unit : body.units) {
        if (unit.mayRun)
            bytesThatMayRun += unit.length;
        switch (unit.kind) {
        case CodeUnit::Kind::Plain:
        case CodeUnit::Kind::PlainOffStackPointer:
            plainBytes += unit.length;
            break;
        case CodeUnit::Kind::Call:
            callBytes += unit.length;
            break;
        case CodeUnit::Kind::Pinned:
            pinnedBytes += unit.length;
            break;
        case CodeUnit::Kind::End:
            endBytes += unit.length;
            break;
        }
    }
    out.println("  units plain=", plainBytes, " call=", callBytes, " pinned=", pinnedBytes, " end=", endBytes, " mayRun=", bytesThatMayRun);
}

static void printStatistics(PrintStream& out, const Statistics& statistics, std::span<const StatisticID> named)
{
    Vector<uint64_t> values = statistics.values();
    for (StatisticID statistic : named)
        out.println("statistic ", nameOf(statistic), " ", values[static_cast<size_t>(statistic)]);
}

#if ENABLE(AOT_SCRATCH_REGISTER_CHECKER)
static void checkScratchRegisters(const CompiledCode& code)
{
    std::span<const uint8_t> bytes = code.body.bytes.span();
    Vector<uint32_t> entryOffsets = code.body.entries.map([](const CodeEntry& entry) {
        return entry.offset;
    });
    Vector<ScratchRegisterConflict> conflicts = findScratchRegisterConflicts(bytes, entryOffsets);
    for (const ScratchRegisterConflict& conflict : conflicts) {
        bool isReplaced = conflict.kind == ScratchRegisterConflict::Kind::ReplacedUnread;
        dataLogLn("f", code.function.index(), ": x", static_cast<unsigned>(conflict.reg), isReplaced ? " is written twice and not read in between:" : " is two operands of one instruction:");
        printInstructions(WTF::dataFile(), bytes, isReplaced ? conflict.offsetOfWrite : conflict.offset, conflict.offset);
    }
    AOT_TEST_CHECK(conflicts.isEmpty());
}
#endif

static void removeGuardOperands(Program& program)
{
    for (Function& function : program.functions) {
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                if (OperandPosition::guard(function.at(id).opcode) && function.at(id).guardOperand())
                    function.edit(id).setGuardOperand({ });
            }
        }
    }
}

static void runGenerateCode(TestCommandContext& context)
{
    std::optional<PrintedParts> parts = parsePrintedParts(context);
    if (!parts)
        return;

    std::unique_ptr<PipelineOutput> output = runRemainingStages(context, "generate-code"_s);
    if (!output) {
        context.form = context.program->form();
        return;
    }
    if (parts->removesGuardOperands)
        removeGuardOperands(*output->program);

    BuildContext& build = context.build;
    CodeGenerationPlan plan = prepareCodeGeneration(*output, build, context.form == IRForm::AsRead ? ProgramTablesUse::Written : ProgramTablesUse::NeverWritten);
    if (!build.diagnostics().hasErrors()) {
        for (const CompiledCode& code : compileAllFunctions(*output, plan, build)) {
            printCode(context.out, code);
#if ENABLE(AOT_SCRATCH_REGISTER_CHECKER)
            checkScratchRegisters(code);
#endif
            if (parts->units)
                printUnits(context.out, code.body);
            if (parts->calls)
                printCalls(context.out, code.body);
        }
    }
    printStatistics(context.out, build.statistics(), parts->statistics.span());

    context.program = WTF::move(output->program);
    context.form = context.program->form();
}

static bool parseStep(StringView word, B3DumpRequest& request)
{
    size_t colon = word.find(':');
    request.point = B3DumpRequest::Point::AfterStep;
    request.step = word.left(colon).toString();
    if (colon == notFound)
        return !request.step.isEmpty();
    request.runOfStep = parseTestNumber<unsigned>(word.substring(colon + 1));
    return !request.step.isEmpty() && request.runOfStep && *request.runOfStep;
}

static std::optional<B3DumpRequest> parseB3DumpRequest(TestCommandContext& context, B3DumpRequest::Point defaultPoint)
{
    constexpr ASCIILiteral afterPrefix = "after="_s;

    B3DumpRequest request;
    request.point = defaultPoint;
    bool takesPoint = defaultPoint != B3DumpRequest::Point::Air;
    for (const String& word : context.arguments) {
        if (word == "loops"_s) {
            request.scope = request.scope == B3DumpScope::SizeOnly ? B3DumpScope::SizeOfFunctionAndLoops : B3DumpScope::LoopsOnly;
            continue;
        }
        if (word == "entry-modules"_s) {
            request.printsOnlyEntryModules = true;
            continue;
        }
        if (!takesPoint && word == "size"_s) {
            request.scope = request.scope == B3DumpScope::LoopsOnly ? B3DumpScope::SizeOfFunctionAndLoops : B3DumpScope::SizeOnly;
            continue;
        }
        if (takesPoint && word == "optimized"_s) {
            request.point = B3DumpRequest::Point::Optimized;
            continue;
        }
        if (takesPoint && word.startsWith(afterPrefix) && parseStep(StringView(word).substring(afterPrefix.length()), request))
            continue;
        std::optional<uint32_t> function = word.startsWith('f') ? parseTestNumber<uint32_t>(StringView(word).substring(1)) : std::nullopt;
        if (!function || *function >= context.program->functions.size()) {
            printBadArgument(context.out, word);
            return std::nullopt;
        }
        request.functions.append(FunctionID(*function));
    }
    return request;
}

static bool isRequested(const B3DumpRequest& request, const BuildContext& build, const Program& program, FunctionID id)
{
    if (request.printsOnlyEntryModules) {
        FunctionID functionAsRead = findFunctionAsRead(&program, id);
        return functionAsRead && program.entries.containsIf([&](const Entry& entry) {
            return entry.module == program.at(functionAsRead).module;
        });
    }
    if (request.functions.isEmpty())
        return program.at(id).role != FunctionRole::Library && build.shouldDump(id);
    return request.functions.contains(id) || request.functions.contains(findFunctionAsRead(&program, id));
}

static void printFunctionHeading(PrintStream& out, const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    out.print("function f", id.index());
    if (function.name)
        out.print(" name=\"", String::fromUTF8(program.string(function.name)), "\"");
    out.println(" role=", nameOf(function.role));
}

static void lowerFunctionToB3(PrintStream& out, FunctionCodeGenerator& generator, const Function& function, const B3DumpRequest& request)
{
    constexpr ASCIILiteral lastStep = "lowerToAir"_s;

    B3::Procedure& procedure = generator.procedure();
    printOriginsAsOperations(procedure, function);
    if (request.point == B3DumpRequest::Point::Lowered) {
        if (request.scope == B3DumpScope::LoopsOnly)
            procedure.resetReachability();
        dumpB3(out, procedure, request.scope);
    }

    StringPrintStream dumpAtRequestedPoint;
    bool didReachRequestedPoint = false;
    unsigned numberOfRuns = 0;
    auto dumpAfterStep = [&](B3::Procedure& procedure, ASCIILiteral step) {
        switch (request.point) {
        case B3DumpRequest::Point::Lowered:
        case B3DumpRequest::Point::Air:
            return;
        case B3DumpRequest::Point::AfterStep:
            if (request.step != step || (request.runOfStep && ++numberOfRuns != *request.runOfStep))
                return;
            break;
        case B3DumpRequest::Point::Optimized:
            if (step == lastStep)
                return;
            break;
        }
        didReachRequestedPoint = true;
        dumpAtRequestedPoint.reset();
        dumpB3(dumpAtRequestedPoint, procedure, request.scope);
    };
    B3::StepObserver observer = dumpAfterStep;
    B3::Procedure* procedures[] = { &procedure };
    B3::prepareForGeneration(procedures, &observer);

    if (request.point == B3DumpRequest::Point::Lowered || procedure.report().failure)
        return;
    if (request.point == B3DumpRequest::Point::Air) {
        dumpAir(out, procedure.code(), request.scope);
        return;
    }
    if (!didReachRequestedPoint)
        out.println(request.step, " did not run");
    out.print(dumpAtRequestedPoint.toString());
}

static void lowerAllFunctionsToB3(PrintStream& out, const PipelineOutput& output, const CodeGenerationPlan& plan, BuildContext& build, const B3DumpRequest& request)
{
    const Program& program = *output.program;
    CodeGenerationInputs inputs = createCodeGenerationInputs(output, plan);
    CodeGenerationTotals totals;
    ScratchTypeArena types(program.types);
    CodeGenerationWorker worker { build, types.arena(), build.backEndScratch(0) };
    build.resolveFunctionToDump(program);
    Vector<FunctionID> ids = collectFunctionsWithBody(program);
    AOT::stableSort(ids, { }, [&](FunctionID id) {
        return findFunctionAsRead(&program, id).index();
    });
    for (FunctionID id : ids) {
        if (!isRequested(request, build, program, id))
            continue;
        printFunctionHeading(out, program, id);
        if (std::unique_ptr<FunctionCodeGenerator> generator = FunctionCodeGenerator::create(worker, inputs, id, totals)) {
            generator->procedure().setSubjectOfClient(findFunctionAsRead(&program, id).index() + 1);
            lowerFunctionToB3(out, *generator, program.at(id), request);
            generator->finish();
        }
        out.println("end of function f", id.index());
    }
}

static void runLoweringCommand(TestCommandContext& context, ASCIILiteral command, B3DumpRequest::Point defaultPoint)
{
    std::optional<B3DumpRequest> request = parseB3DumpRequest(context, defaultPoint);
    if (!request)
        return;

    std::unique_ptr<PipelineOutput> output = runRemainingStages(context, command);
    if (!output) {
        context.form = context.program->form();
        return;
    }

    BuildContext& build = context.build;
    CodeGenerationPlan plan = prepareCodeGeneration(*output, build, context.form == IRForm::AsRead ? ProgramTablesUse::Written : ProgramTablesUse::NeverWritten);
    if (!build.diagnostics().hasErrors())
        lowerAllFunctionsToB3(context.out, *output, plan, build, *request);

    context.program = WTF::move(output->program);
    context.form = context.program->form();
}

static void runLowerToB3(TestCommandContext& context)
{
    runLoweringCommand(context, "lower-to-b3"_s, B3DumpRequest::Point::Lowered);
}

static void runLowerToAir(TestCommandContext& context)
{
    runLoweringCommand(context, "lower-to-air"_s, B3DumpRequest::Point::Air);
}

std::span<const TestCommand> generatedCodeTestCommands()
{
    static constexpr TestCommand commands[] {
        { "generate-code"_s, IRForm::Selected, runGenerateCode, ProgramAsRead::WithLibrary },
        { "lower-to-air"_s, IRForm::Selected, runLowerToAir, ProgramAsRead::WithLibrary },
        { "lower-to-b3"_s, IRForm::Selected, runLowerToB3, ProgramAsRead::WithLibrary },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
