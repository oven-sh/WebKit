/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestCommand.h"

#if ENABLE(AOT)

#include "AOTCodeGenerationDriver.h"
#include "AOTFunctionCodeGenerator.h"
#include "AOTIRBuilder.h"
#include "AOTPipeline.h"
#include "AOTRepresentation.h"
#include "AOTSchemaEnums.h"
#include "AOTSupportTestCommands.h"
#include "AOTTestRunner.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestPlan.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ConversionCase {
    Opcode opcode;
    TypeID type;
    Representation from;
    Representation to;
};

struct ReportedDiagnostics {
    Vector<UserDiagnostic> userDiagnostics;
    Vector<InternalError> internalErrors;
};

} // anonymous namespace

static constexpr ConversionCase conversionCases[] {
    { Opcode::Box, TypeTable::string, Representation::Void, Representation::JSValue },
    { Opcode::Box, TypeTable::string, Representation::Cell, Representation::Double },
    { Opcode::Unbox, TypeTable::string, Representation::Cell, Representation::Double },
    { Opcode::Decode, TypeTable::string, Representation::Cell32, Representation::Double },
    { Opcode::Compress, TypeTable::number, Representation::Double, Representation::Cell32 },
    { Opcode::Box, TypeTable::string, Representation::Cell, Representation::JSValue },
};

static std::optional<TypeID> parseTypeUnderTest(const Program& program, StringView text)
{
    if (!text.startsWith('t'))
        return std::nullopt;
    std::optional<uint32_t> index = parseTestNumber<uint32_t>(text.substring(1));
    if (!index || *index >= program.variables.size())
        return std::nullopt;
    return program.at(VariableID(*index)).declared.type;
}

static bool printRequestedPlan(TestCommandContext& context, std::span<const String> words)
{
    const Program& program = *context.program;
    std::optional<TypeID> from = parseTypeUnderTest(program, words[0]);
    std::optional<Representation> representation = parseEnum<Representation>(words[1]);
    std::optional<TypeID> to = parseTypeUnderTest(program, words[2]);
    if (!from || !representation || !to) {
        printBadArgument(context.out, !from ? words[0] : !representation ? words[1] : words[2]);
        return false;
    }
    context.out.print(words[0], " ", words[1], " ", words[2], ": ", computeTypeTestPlan(program, *from, *representation, *to));
    return true;
}

static TypeID testedTypeOf(const Value& value)
{
    if (value.opcode == Opcode::TypeTest)
        return value.testedType();
    if (value.opcode == Opcode::Claim)
        return value.castType();
    return { };
}

static void printPlansOfFunction(PrintStream& out, const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    for (const Block& block : function.blocks) {
        for (ValueID test : block.values) {
            TypeID to = testedTypeOf(function.at(test));
            if (!to)
                continue;
            const Value& tested = function.at(function.at(test).valueOperand());
            bool isChosen = tested.representation != Representation::Unchosen;
            Representation representation = isChosen ? tested.representation : defaultRepresentationOf(program.at(tested.type));
            out.print("f", id.index(), " v", test.index(), ": ", computeTypeTestPlan(program, tested.type, representation, to));
        }
    }
}

static void runTypeTestPlan(TestCommandContext& context)
{
    constexpr size_t numberOfWordsInPlan = 3;

    std::span<const String> words = context.arguments;
    if (words.empty()) {
        for (size_t index = 0; index < context.program->functions.size(); ++index)
            printPlansOfFunction(context.out, *context.program, FunctionID(index));
        return;
    }
    if (words.size() % numberOfWordsInPlan) {
        printMissingArgument(context.out);
        return;
    }
    for (; !words.empty(); words = words.subspan(numberOfWordsInPlan)) {
        if (!printRequestedPlan(context, words.first(numberOfWordsInPlan)))
            return;
    }
}

static std::optional<FunctionID> findFunctionWithBody(const Program& program)
{
    for (size_t index = 0; index < program.functions.size(); ++index) {
        if (program.functions[index].hasBody())
            return FunctionID(index);
    }
    return std::nullopt;
}

static ReportedDiagnostics takeReportedDiagnostics(BuildContext& build)
{
    Diagnostics reported = build.takeDiagnostics();
    reported.finish(nullptr);
    return { reported.userDiagnostics(), reported.internalErrors() };
}

static void insertConversion(const AnalysisContext& context, Function& function, TypeArena& types, const ConversionCase& conversion)
{
    IRBuilder builder(context, function, types);
    ProgramPoint head = ProgramPoint::atHeadOf(entryBlock);

    Value source = IRBuilder::make(Opcode::Unspecified);
    source.setValueType(conversion.type);
    source.representation = conversion.from;
    ValueID sourceID = builder.insertBefore(head, WTF::move(source));

    Value converted = IRBuilder::make(conversion.opcode, { sourceID });
    converted.representation = conversion.to;
    builder.insertBefore(head, WTF::move(converted));
    builder.applyInsertions();
}

static bool generateWithConversion(const CodeGenerationWorker& worker, const CodeGenerationInputs& inputs, FunctionID id, Function& function, const ConversionCase& conversion)
{
    Function original = function;
    insertConversion({ worker.build, inputs.program, &inputs.flow, &inputs.effects, id }, function, worker.types, conversion);
    CodeGenerationTotals totals;
    bool isGenerated = !!FunctionCodeGenerator::create(worker, inputs, id, totals);
    function = WTF::move(original);
    return isGenerated;
}

// A generator reports only its first failure and the runner removes repeated diagnostics, so each conversion gets a new generator and prints its own diagnostics.
static void printConversionResults(TestCommandContext& context, const CodeGenerationInputs& inputs, FunctionID id, Function& function)
{
    BuildContext& build = context.build;
    ScratchTypeArena types(inputs.program.types);
    CodeGenerationWorker worker { build, types.arena(), build.backEndScratch(0) };

    ReportedDiagnostics reported = takeReportedDiagnostics(build);
    for (const ConversionCase& conversion : conversionCases) {
        bool isGenerated = generateWithConversion(worker, inputs, id, function, conversion);
        context.out.print(nameOf(conversion.opcode), " ", nameOf(conversion.from), " to ", nameOf(conversion.to), ": ", isGenerated ? "generated" : "failed");

        ReportedDiagnostics reportedByConversion = takeReportedDiagnostics(build);
        for (const UserDiagnostic& diagnostic : reportedByConversion.userDiagnostics)
            context.out.print(" ", nameOf(diagnostic.kind));
        for (const InternalError& error : reportedByConversion.internalErrors)
            context.out.print(" ", nameOf(error.kind));
        context.out.println();

        reported.userDiagnostics.appendVector(WTF::move(reportedByConversion.userDiagnostics));
        reported.internalErrors.appendVector(WTF::move(reportedByConversion.internalErrors));
    }
    build.diagnostics().append(WTF::move(reported.userDiagnostics));
    build.diagnostics().append(WTF::move(reported.internalErrors));
}

static void runUnsupportedConversions(TestCommandContext& context)
{
    if (!context.arguments.empty()) {
        printBadArgument(context.out, context.arguments.front());
        return;
    }
    if (context.form != IRForm::AsRead) {
        context.out.println("error: unsupported-conversions starts from AsRead");
        return;
    }

    BuildContext& build = context.build;
    std::unique_ptr<PipelineOutput> pipeline = runPipeline(WTF::move(context.program), build);
    if (!pipeline) {
        reopenDiagnostics(build);
        return;
    }
    CodeGenerationPlan plan = prepareCodeGeneration(*pipeline, build);

    std::optional<FunctionID> function = findFunctionWithBody(*pipeline->program);
    if (!function)
        context.out.println("error: no function has a body");
    else if (!build.diagnostics().hasErrors())
        printConversionResults(context, createCodeGenerationInputs(*pipeline, plan), *function, pipeline->program->at(*function));

    context.program = WTF::move(pipeline->program);
    context.form = context.program->form();
}

static void runBranchCounters(TestCommandContext& context)
{
    if (!context.arguments.empty()) {
        printBadArgument(context.out, context.arguments.front());
        return;
    }
    numberBranchCounters(*context.program, context.build).dump(context.out, *context.program);
}

std::span<const TestCommand> codeGeneratorTestCommands()
{
    static constexpr TestCommand commands[] {
        { "branch-counters"_s, IRForm::Selected, runBranchCounters },
        { "type-test-plan"_s, IRForm::Selected, runTypeTestPlan },
        { "unsupported-conversions"_s, IRForm::AsRead, runUnsupportedConversions, RawWordCheck::Run, ProgramAsRead::WithLibrary },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
