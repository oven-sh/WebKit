/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestCommand.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRParser.h"
#include "AOTIRPrinter.h"
#include "AOTInt32Proof.h"
#include "AOTResultRuleMonotonicity.h"
#include "AOTResultRules.h"
#include "AOTSupportTestCommands.h"
#include "AOTTypeArena.h"
#include "AOTTypePropagation.h"
#include "AOTVector.h"
#include "AOTVerifier.h"
#include <algorithm>
#include <climits>
#include <wtf/StringPrintStream.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static std::optional<FunctionID> parseFunctionWord(const Program& program, StringView word)
{
    if (word.length() < 2 || word[0] != 'f')
        return std::nullopt;
    std::optional<uint32_t> index = parseTestNumber<uint32_t>(word.substring(1));
    if (!index || *index >= program.functions.size())
        return std::nullopt;
    return FunctionID(*index);
}

static std::optional<Vector<FunctionID>> parseFunctionWords(TestCommandContext& context, std::span<const String> words)
{
    const Program& program = *context.program;
    Vector<FunctionID> functions;
    if (words.empty()) {
        for (size_t index = 0; index < program.functions.size(); ++index)
            functions.append(FunctionID(index));
        return functions;
    }
    for (const String& word : words) {
        std::optional<FunctionID> function = parseFunctionWord(program, word);
        if (!function) {
            printBadArgument(context.out, word);
            return std::nullopt;
        }
        functions.append(*function);
    }
    return functions;
}

static Vector<StringView> linesOf(StringView text)
{
    Vector<StringView> lines;
    for (StringView line : text.splitAllowingEmptyEntries('\n'))
        lines.append(line);
    return lines;
}

static StringView lineAt(const Vector<StringView>& lines, size_t index)
{
    return index < lines.size() ? lines[index] : StringView();
}

static void printRoundTripComparison(PrintStream& out, const String& firstText, const String& secondText)
{
    if (firstText == secondText) {
        out.println("round trip: same"_s);
        return;
    }
    Vector<StringView> first = linesOf(firstText);
    Vector<StringView> second = linesOf(secondText);
    size_t numberOfSharedLines = std::min(first.size(), second.size());
    size_t index = 0;
    while (index < numberOfSharedLines && first[index] == second[index])
        ++index;
    out.println("round trip: differs at line "_s, index + 1);
    out.println("first: "_s, lineAt(first, index));
    out.println("second: "_s, lineAt(second, index));
}

static void runRoundTrip(TestCommandContext& context)
{
    if (!context.arguments.empty()) {
        printBadArgument(context.out, context.arguments.front());
        return;
    }

    StringPrintStream first;
    printProgram(first, *context.program);
    String firstText = first.toString();
    context.out.print(firstText);

    std::unique_ptr<Program> again = parseProgram(firstText, context.form, context.build, VerifierInputs { .isOptimizing = context.isOptimizing });
    if (!again) {
        context.out.println("round trip: does not parse"_s);
        return;
    }
    StringPrintStream second;
    printProgram(second, *again);
    printRoundTripComparison(context.out, firstText, second.toString());
}

static void runPrint(TestCommandContext& context)
{
    std::span<const String> words = context.arguments;
    if (words.empty()) {
        printProgram(context.out, *context.program);
        return;
    }
    std::optional<Vector<FunctionID>> functions = parseFunctionWords(context, words);
    if (!functions)
        return;
    for (FunctionID id : *functions)
        printFunction(context.out, *context.program, id, context.program->at(id));
}

static void typeFunction(const Program& program, FunctionID id, Function& function, TypeArena& types, BuildContext& build)
{
    FunctionAnalyses analyses(AnalysisContext { build, program, nullptr, nullptr, id }, function);
    typeValuesFromNever({ program, id, function, analyses, types, build });
}

static void typeProgramAsRead(Program& program, BuildContext& build)
{
    TypeArena types(program.types);
    for (size_t index = 0; index < program.functions.size(); ++index) {
        Function& function = program.functions[index];
        if (function.hasBody())
            typeFunction(program, FunctionID(index), function, types, build);
    }
}

static void runVerify(TestCommandContext& context)
{
    PlacedTableSizes unboundedSizes { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX };
    VerifierInputs inputs { .placed = &unboundedSizes };
    for (const String& word : context.arguments) {
        if (word != "optimizing"_s) {
            printBadArgument(context.out, word);
            return;
        }
        inputs.isOptimizing = true;
    }

    if (context.form == IRForm::AsRead)
        typeProgramAsRead(*context.program, context.build);
    if (verifyProgram(*context.program, context.form, context.build, inputs))
        context.out.println("valid"_s);
}

static void printTypeAndExpectedType(PrintStream& out, const TypingContext& typing, ValueID id)
{
    const Value& value = typing.function.at(id);
    out.print("v"_s, id.index(), ": "_s);
    printType(out, typing.program, value.type);
    if (TypeID expected = value.expected().predicted()) {
        out.print(" ~ "_s);
        printType(out, typing.program, expected);
    }
    out.print("\n"_s);
}

static void printResultTypes(PrintStream& out, const TypingContext& typing, ValueID id)
{
    const Value& value = typing.function.at(id);
    std::optional<uint32_t> numberOfResults = numberOfResultsOf(typing.program, typing.function, value);
    if (!numberOfResults) {
        if (resultRuleOf(value.opcode) != ResultRule::None)
            printTypeAndExpectedType(out, typing, id);
        return;
    }
    for (uint32_t resultIndex = 0; resultIndex < *numberOfResults; ++resultIndex) {
        out.print("v"_s, id.index(), "."_s, resultIndex, ": "_s);
        printType(out, typing.program, computeResultType(typing, value, resultIndex));
        out.print("\n"_s);
    }
}

static void printResultTypesOfFunction(TestCommandContext& context, FunctionID id, TypeArena& types)
{
    const Program& program = *context.program;
    Function function = program.at(id);
    typeFunction(program, id, function, types, context.build);

    context.out.println("function f"_s, id.index());
    TypingContext typing { program, function, types };
    for (const Block& block : function.blocks) {
        for (ValueID parameter : block.parameters)
            printTypeAndExpectedType(context.out, typing, parameter);
        for (ValueID value : block.values)
            printResultTypes(context.out, typing, value);
    }
}

static void runResultTypes(TestCommandContext& context)
{
    std::optional<Vector<FunctionID>> functions = parseFunctionWords(context, context.arguments);
    if (!functions)
        return;

    TypeArena types(context.program->types);
    for (FunctionID id : *functions) {
        if (context.program->at(id).hasBody())
            printResultTypesOfFunction(context, id, types);
    }
}

static void printBounds(PrintStream& out, const NumberBounds& bounds)
{
    switch (bounds.kind) {
    case BoundsKind::Unknown:
        out.print("unknown "_s, nameOf(bounds.failure));
        break;
    case BoundsKind::None:
        out.print("no number"_s);
        break;
    case BoundsKind::Integers:
        out.print("integers ["_s, bounds.range.minimum, ", "_s, bounds.range.maximum, "]"_s);
        break;
    case BoundsKind::Reals:
        out.print("reals ["_s, bounds.range.minimum, ", "_s, bounds.range.maximum, "]"_s);
        break;
    }
    out.println(bounds.mayBeNullish ? " or nullish"_s : ""_s);
}

static void printInt32BoundsOfFunction(TestCommandContext& context, FunctionID id, TypeArena& types)
{
    const Program& program = *context.program;
    Function function = program.at(id);
    FunctionAnalyses analyses(AnalysisContext { context.build, program, nullptr, nullptr, id }, function);
    typeValuesFromNever({ program, id, function, analyses, types, context.build });

    context.out.println("function f"_s, id.index());
    Int32Prover prover({ program, function, analyses, types, context.build });
    for (const Block& block : function.blocks) {
        for (ValueID value : block.values) {
            const Value& request = function.at(value);
            if (request.opcode != Opcode::Convert || !request.declared().has(PositionFlags::IsInt32Promise))
                continue;
            context.out.print("v"_s, value.index(), ": "_s);
            printBounds(context.out, prover.boundsOf(request.valueOperand()));
        }
    }
}

static void runInt32Bounds(TestCommandContext& context)
{
    std::optional<Vector<FunctionID>> functions = parseFunctionWords(context, context.arguments);
    if (!functions)
        return;

    TypeArena types(context.program->types);
    for (FunctionID id : *functions) {
        if (context.program->at(id).hasBody())
            printInt32BoundsOfFunction(context, id, types);
    }
}

void BrokenResultRule::dump(PrintStream& out, const Program& program, const Function& function) const
{
    out.print("v", value.index(), " ", nameOf(function.at(value).opcode), " operand ", operandIndex, ": ");
    printType(out, program, narrowOperand);
    out.print(" gives ");
    printType(out, program, resultOfNarrowOperand);
    switch (law) {
    case Law::NarrowerOperandGivesNarrowerResult:
        out.print(", ");
        printType(out, program, wideOperand);
        out.print(" gives ");
        break;
    case Law::BothPathsAgree:
        out.print(" by type ids, by the rule ");
        break;
    }
    printType(out, program, resultOfWideOperand);
}

static void runLawsOfResultRules(TestCommandContext& context)
{
    std::optional<Vector<FunctionID>> functions = parseFunctionWords(context, context.arguments);
    if (!functions)
        return;

    const Program& program = *context.program;
    TypeArena types(context.program->types);
    ShapesWithMonotoneResult checkedShapes;
    for (FunctionID id : *functions) {
        if (!program.at(id).hasBody())
            continue;
        Function function = program.at(id);
        typeFunction(program, id, function, types, context.build);
        std::optional<BrokenResultRule> found = findBrokenResultRule(program, function, types, checkedShapes);
        context.out.print("function f"_s, id.index(), ": "_s);
        if (found)
            found->dump(context.out, program, function);
        else
            context.out.print("every rule keeps both laws"_s);
        context.out.println();
    }
}

std::span<const TestCommand> irTestCommands()
{
    static constexpr TestCommand commands[] = {
        { "round-trip"_s, IRForm::AsRead, runRoundTrip },
        { "print"_s, IRForm::AsRead, runPrint },
        { "verify"_s, IRForm::AsRead, runVerify },
        { "result-types"_s, IRForm::AsRead, runResultTypes },
        { "laws-of-result-rules"_s, IRForm::AsRead, runLawsOfResultRules },
        { "int32-bounds"_s, IRForm::AsRead, runInt32Bounds },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
