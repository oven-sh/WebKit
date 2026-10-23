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
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {


} // anonymous namespace

static constexpr OpcodeSet rowsWithDeclaredOperands {
    Opcode::NewClosure,
    Opcode::PutModuleVariable,
    Opcode::PutEnvironmentField,
    Opcode::NewObject,
    Opcode::CallDirect,
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

static void printFunction(PrintStream& out, const RepresentationInputs& inputs)
{
    out.println("function f"_s, inputs.id.index());
    printRules(out, inputs);
}

static void forEachFunctionWithBody(const Program& program, NOESCAPE const Invocable<void(FunctionID)> auto& functor)
{
    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        if (program.at(id).hasBody())
            functor(id);
    }
}

static void printPlacedProgram(TestCommandContext& context)
{
    forEachFunctionWithBody(*context.program, [&](FunctionID id) {
        SelectionTestSession session(context, id);
        const FunctionPassContext& pass = session.pass();
        RELEASE_ASSERT(pass.inputs.placement && pass.inputs.callingConventions);
        printFunction(context.out, { pass.program, pass.id, pass.function, *pass.inputs.placement, *pass.inputs.callingConventions });
    });
}

static void placeAndPrintProgram(TestCommandContext& context)
{
    std::unique_ptr<PlacedProgram> placed = runPipelineForPlacementTest(context, PassID::PlaceSiteCaches);
    if (!placed)
        return;

    const Program& program = placed->program();
    forEachFunctionWithBody(program, [&](FunctionID id) {
        printFunction(context.out, { program, id, program.at(id), placed->placement(), *placed->output->callingConventions });
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

    if (!context.arguments.empty())
        return printBadArgument(out, context.arguments.front());

    switch (context.form) {
    case IRForm::AsRead:
        return placeAndPrintProgram(context);
    case IRForm::Placed:
        return printPlacedProgram(context);
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
