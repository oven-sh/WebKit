/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionTestCommands.h"

#if ENABLE(AOT)

#include "AOTChooseCallingConventions.h"
#include "AOTConversions.h"
#include "AOTPlacementTestSupport.h"
#include "AOTRepresentationRules.h"
#include "AOTSupportTestCommands.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class Report : uint8_t { Rules, Errors };

} // anonymous namespace

static constexpr OpcodeSet rowsWithDeclaredOperands {
    Opcode::NewClosure,
    Opcode::PutModuleVariable,
    Opcode::PutEnvironmentField,
    Opcode::NewObject,
    Opcode::CallDirect,
    Opcode::CallGroup,
    Opcode::Results,
    Opcode::PutSlot,
    Opcode::PutSlotOrAdd,
    Opcode::AwaitCallBySavingFrame,
    Opcode::CloneAs,
    Opcode::PutSlots,
};

static ASCIILiteral nameOfResult(Representation representation)
{
    return representation == Representation::Unchosen ? "own"_s : nameOf(representation);
}

static ASCIILiteral nameOfWanted(Representation representation)
{
    return representation == Representation::Unchosen ? "-"_s : nameOf(representation);
}

template<typename User>
static void printWantedRepresentations(PrintStream& out, const RepresentationInputs& inputs, const User& user)
{
    uint32_t numberOfOperands = user.numberOfOperands();
    if (numberOfOperands)
        out.print(" <-"_s);
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex)
        out.print(' ', nameOfWanted(wantedRepresentationOf(inputs, user, operandIndex)));
    out.println();
}

static void printValue(PrintStream& out, const RepresentationInputs& inputs, ValueID id)
{
    const Value& value = inputs.function.at(id);
    out.print('v', id.index(), ' ', nameOf(value.opcode), ": "_s, nameOfResult(resultRepresentationOf(inputs, value)));
    printWantedRepresentations(out, inputs, value);
}

static void printEdgeResults(PrintStream& out, const RepresentationInputs& inputs, BlockID from)
{
    const Function& function = inputs.function;
    for (uint32_t targetIndex = 0; targetIndex < function.at(from).terminator.targets.size(); ++targetIndex) {
        uint32_t numberOfResults = numberOfEdgeResults(function, from, targetIndex);
        for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
            Representation passed = edgeResultRepresentationOf(inputs, EdgeResultPosition { from, targetIndex, resultIndex });
            out.println('b', from.index(), '.', targetIndex, '.', resultIndex, ": "_s, nameOfWanted(passed));
        }
    }
}

static void printRules(PrintStream& out, const RepresentationInputs& inputs)
{
    const Function& function = inputs.function;
    for (size_t index = 0; index < function.blocks.size(); ++index) {
        BlockID id(index);
        const Block& block = function.at(id);
        out.println('b', index);
        for (ValueID parameter : block.parameters)
            printValue(out, inputs, parameter);
        for (ValueID value : block.values)
            printValue(out, inputs, value);
        out.print(nameOf(block.terminator.kind));
        printWantedRepresentations(out, inputs, block.terminator);
        printEdgeResults(out, inputs, id);
    }
}

static void printError(PrintStream& out, const InternalError& error)
{
    out.print("error "_s, nameOf(error.kind));
    if (error.block)
        out.print(" b"_s, error.block.index());
    if (error.value)
        out.print(" v"_s, error.value.index());
    out.println();
}

static void printErrors(PrintStream& out, const RepresentationInputs& inputs)
{
    Vector<InternalError> errors;
    checkRepresentations(inputs, errors);
    for (const InternalError& error : errors)
        printError(out, error);
    if (errors.isEmpty())
        out.println("valid"_s);
}

static void printRowsWithDeclaredOperands(PrintStream& out)
{
    for (Opcode opcode : allMembersOf<Opcode>) {
        uint32_t numberOfOperandInfos = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
        for (uint32_t operandIndex = 0; operandIndex < numberOfOperandInfos; ++operandIndex) {
            const OperandInfo& info = operandInfoOf(opcode, operandIndex);
            if (info.kind != OperandKind::Declared)
                continue;
            out.println(nameOf(opcode), '.', info.name, rowsWithDeclaredOperands.contains(opcode) ? ""_s : " missing"_s);
        }
    }
}

static void printFunction(PrintStream& out, const RepresentationInputs& inputs, Report report)
{
    out.println("function f"_s, inputs.id.index());
    switch (report) {
    case Report::Rules:
        return printRules(out, inputs);
    case Report::Errors:
        return printErrors(out, inputs);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void forEachFunctionWithBody(const Program& program, NOESCAPE const Invocable<void(FunctionID)> auto& functor)
{
    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        if (program.at(id).hasBody())
            functor(id);
    }
}

static void printPlacedProgram(TestCommandContext& context, Report report)
{
    forEachFunctionWithBody(*context.program, [&](FunctionID id) {
        SelectionTestSession session(context, id);
        const FunctionPassContext& pass = session.pass();
        RELEASE_ASSERT(pass.inputs.placement && pass.inputs.callingConventions);
        printFunction(context.out, { pass.program, pass.id, pass.function, *pass.inputs.placement, *pass.inputs.callingConventions }, report);
    });
}

static void placeAndPrintProgram(TestCommandContext& context, Report report)
{
    PassID last = report == Report::Errors ? PassID::ChooseRepresentations : PassID::CheckPlacement;
    std::unique_ptr<PlacedProgram> placed = runPipelineForPlacementTest(context, last);
    if (!placed)
        return;

    const Program& program = placed->program();
    forEachFunctionWithBody(program, [&](FunctionID id) {
        printFunction(context.out, { program, id, program.at(id), placed->placement(), *placed->output->callingConventions }, report);
    });
    context.program = WTF::move(placed->output->program);
    context.form = context.program->form();
}

void runRepresentationRulesTestCommand(TestCommandContext& context)
{
    PrintStream& out = context.out;
    if (context.arguments.size() > 1)
        return printBadArgument(out, context.arguments[1]);
    if (isOnlyWord(context.arguments, "declared-rows"_s))
        return printRowsWithDeclaredOperands(out);

    bool shouldCheck = isOnlyWord(context.arguments, "check"_s);
    if (!shouldCheck && !context.arguments.empty())
        return printBadArgument(out, context.arguments.front());
    Report report = shouldCheck ? Report::Errors : Report::Rules;

    switch (context.form) {
    case IRForm::AsRead:
        return placeAndPrintProgram(context, report);
    case IRForm::Placed:
        return printPlacedProgram(context, report);
    case IRForm::Prepared:
    case IRForm::Selected:
        return out.println("error: representation-rules reads AsRead or Placed"_s);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<TypeID> parseTypeUnderTest(const Program& program, StringView word)
{
    if (program.functions.isEmpty() || !word.startsWith('t'))
        return std::nullopt;
    const Vector<DeclaredType>& parameters = program.functions[0].parameters;
    std::optional<uint32_t> index = parseTestNumber<uint32_t>(word.substring(1));
    if (!index || *index >= parameters.size())
        return std::nullopt;
    return parameters[*index].type;
}

static std::optional<Representation> parseChosenRepresentation(StringView word)
{
    std::optional<Representation> representation = parseEnum<Representation>(word);
    if (representation == Representation::Unchosen)
        return std::nullopt;
    return representation;
}

static void printConversion(PrintStream& out, const Type& type, Representation from, Representation to)
{
    out.print(nameOf(from), " -> "_s, nameOf(to), ':');
    std::optional<ConversionPath> path = findConversion(type, from, to);
    if (!path)
        return out.println(" none"_s);
    if (path->isEmpty())
        return out.println(" nothing"_s);
    for (const ConversionStep& step : *path)
        out.print(' ', nameOf(step.opcode), '(', nameOf(step.to), ')');
    out.println();
}

void runConversionsTestCommand(TestCommandContext& context)
{
    constexpr size_t wordsPerConversion = 3;
    PrintStream& out = context.out;
    std::span<const String> words = context.arguments;
    if (words.empty() || words.size() % wordsPerConversion)
        return printMissingArgument(out);

    const Program& program = *context.program;
    for (; !words.empty(); words = words.subspan(wordsPerConversion)) {
        std::optional<TypeID> type = parseTypeUnderTest(program, words[0]);
        if (!type)
            return printBadArgument(out, words[0]);
        std::optional<Representation> from = parseChosenRepresentation(words[1]);
        if (!from)
            return printBadArgument(out, words[1]);
        std::optional<Representation> to = parseChosenRepresentation(words[2]);
        if (!to || (*to == Representation::Void && *from != Representation::Void))
            return printBadArgument(out, words[2]);
        printConversion(out, program.at(*type), *from, *to);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
