/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowAnalysisInternal.h"
#include "AOTFlowNames.h"
#include "AOTFlowPropertyAccessInternal.h"
#include "AOTFlowPropertyWalkInternal.h"
#include "AOTIRPrinter.h"
#include "AOTSupportTestCommands.h"
#include <memory>
#include <optional>
#include <wtf/CommaPrinter.h>
#include <wtf/StringPrintStream.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowWalkCommandOptions {
    Vector<FunctionID> functions;
    bool printsPrototypes { false };
    bool printsSymbols { false };
};

struct FlowWalkOutput {
    PrintStream& out;
    const FlowSolution& solution;
    FlowValueTable& values;
    FlowWalkInputs inputs;
};

static ASCIILiteral nameOf(FlowWalkOperation operation)
{
    switch (operation) {
    case FlowWalkOperation::Get:
        return "Get"_s;
    case FlowWalkOperation::Set:
        return "Set"_s;
    case FlowWalkOperation::DefineOwnProperty:
        return "DefineOwnProperty"_s;
    case FlowWalkOperation::CopyDataProperties:
        return "CopyDataProperties"_s;
    case FlowWalkOperation::HasProperty:
        return "HasProperty"_s;
    case FlowWalkOperation::Delete:
        return "Delete"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(FlowPropertySourceKind kind)
{
    switch (kind) {
    case FlowPropertySourceKind::Unknown:
        return "Unknown"_s;
    case FlowPropertySourceKind::Absent:
        return "Absent"_s;
    case FlowPropertySourceKind::Value:
        return "Value"_s;
    case FlowPropertySourceKind::Getter:
        return "Getter"_s;
    case FlowPropertySourceKind::Setter:
        return "Setter"_s;
    case FlowPropertySourceKind::BuiltinMember:
        return "BuiltinMember"_s;
    case FlowPropertySourceKind::BuiltinGetter:
        return "BuiltinGetter"_s;
    case FlowPropertySourceKind::BuiltinSetter:
        return "BuiltinSetter"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral wordOf(const FlowPrimitiveWrapper& wrapper)
{
    if (wrapper.primitives & Primitive::AnyString)
        return "string"_s;
    if (wrapper.primitives & Primitive::Number)
        return "number"_s;
    if (wrapper.primitives & Primitive::Boolean)
        return "boolean"_s;
    if (wrapper.primitives & Primitive::AnySymbol)
        return "symbol"_s;
    RELEASE_ASSERT(wrapper.primitives & Primitive::BigInt);
    return "bigint"_s;
}

static std::optional<FunctionID> parseFunctionWord(const Program& program, StringView word)
{
    if (word.length() < 2 || word[0] != 'f')
        return std::nullopt;
    std::optional<uint32_t> index = parseTestNumber<uint32_t>(word.substring(1));
    if (!index || *index >= program.functions.size())
        return std::nullopt;
    return FunctionID(*index);
}

static std::optional<FlowWalkCommandOptions> parseOptions(TestCommandContext& context)
{
    FlowWalkCommandOptions options;
    for (const String& word : context.arguments) {
        if (word == "prototypes"_s) {
            options.printsPrototypes = true;
            continue;
        }
        if (word == "symbols"_s) {
            options.printsSymbols = true;
            continue;
        }
        std::optional<FunctionID> function = parseFunctionWord(*context.program, word);
        if (!function) {
            printBadArgument(context.out, word);
            return std::nullopt;
        }
        options.functions.append(*function);
    }
    return options;
}

static String labelOf(const FlowObjectTable& objects, FlowObjectID object)
{
    StringPrintStream label;
    objects.dump(label, object);
    return label.toString();
}

static void printSource(const FlowWalkOutput& output, StringView receiver, const FlowPropertySource& source)
{
    PrintStream& out = output.out;
    out.print("  "_s, receiver, ": "_s, nameOf(source.kind));
    if (source.isOwn)
        out.print(" own"_s);
    out.print(" "_s, nameOf(source.certainty));
    if (source.kind == FlowPropertySourceKind::Absent) {
        out.println();
        return;
    }

    out.print(" "_s);
    if (source.holder)
        output.solution.objects.dump(out, source.holder);
    else
        out.print("-"_s);

    out.print(" "_s);
    if (source.key)
        printPropertyKey(out, output.solution.program, source.key);
    else
        out.print("*"_s);

    if (source.builtin)
        out.print(" "_s, output.solution.builtins().tokenOf(source.builtin));
    out.println();
}

static void printHead(const FlowWalkOutput& output, FlowInstanceID instance, ValueID origin, FlowWalkOperation operation, const FlowKeySet& keys)
{
    PrintStream& out = output.out;
    out.print("f"_s, output.solution.graph.at(instance).function.index(), "@i"_s, instance.index(), " "_s);
    if (origin)
        out.print("v"_s, origin.index());
    else
        out.print("-"_s);
    out.print(" "_s, nameOf(operation), " "_s);
    keys.dump(out, output.solution.program);
    out.println();
}

template<typename Statement>
static void printAccess(const FlowWalkOutput& output, FlowInstanceID instance, const Statement& statement, const FlowKeySet& keys)
{
    const FlowSolution& solution = output.solution;
    FlowPropertyWalk walk { walkOperationOf(statement.kind), statement.flags.contains(FlowAccessFlag::StartsAtPrototype), { }, statement.flags.contains(FlowAccessFlag::IsOwnOnly) };
    printHead(output, instance, statement.origin, walk.operation, keys);

    const FlowValue& receivers = solution.graph.valueOf(solution.graph.nodeOf(instance, statement.receiver));
    receivers.objects.forEach([&](FlowObjectID receiver) {
        String label = labelOf(solution.objects, receiver);
        forEachPropertySource(solution, receiver, keys, walk, [&](const FlowPropertySource& source) {
            printSource(output, label, source);
        });
    });

    for (const FlowPrimitiveWrapper& wrapper : primitiveWrappers()) {
        uint16_t primitives = static_cast<uint16_t>(receivers.primitives & wrapper.primitives);
        if (!primitives)
            continue;
        auto print = [&](const FlowPropertySource& source) {
            printSource(output, wordOf(wrapper), source);
        };
        FlowPropertySourceFunctor functor = print;
        FlowPropertyWalker(output.inputs, keys, walk, functor).walkFrom(FlowPrimitiveReceiver { primitives });
    }
}

static void printAccessesOf(const FlowWalkOutput& output, FlowInstanceID instance)
{
    const FlowGraph& graph = output.solution.graph;
    const FlowStatements& statements = graph.summaryOf(instance).statements;
    for (const FlowPropertyAccess& statement : statements.propertyAccesses)
        printAccess(output, instance, statement, keySetOf(output.solution.program, statement.key));
    for (const FlowElementAccess& statement : statements.elementAccesses)
        printAccess(output, instance, statement, output.values.keysOf(graph.valueOf(graph.nodeOf(instance, statement.key))));
}

static void printPrototypeLinks(const FlowWalkOutput& output)
{
    PrintStream& out = output.out;
    const FlowObjectTable& objects = output.solution.objects;
    const FlowGraph& graph = output.solution.graph;
    for (uint32_t index = 0; index < objects.size(); ++index) {
        FlowObjectID object(index);
        FlowNodeID link = graph.find(object, FlowOpenPart::PrototypeLink);
        if (!link)
            continue;

        const FlowValue& prototypes = graph.valueOf(link);
        objects.dump(out, object);
        out.print(" -> "_s);
        CommaPrinter comma;
        prototypes.objects.forEach([&](FlowObjectID prototype) {
            if (prototype == unknownFlowObject)
                return;
            out.print(comma);
            objects.dump(out, prototype);
        });
        if (prototypes.primitives & Primitive::Null)
            out.print(comma, "null"_s);
        if (prototypes.hasUnknownObject())
            out.print(comma, "unknown"_s);
        out.println();
    }
}

static void printSymbolCreations(const FlowWalkOutput& output)
{
    PrintStream& out = output.out;
    for (const FlowSymbolCreation& creation : symbolCreationsOf(output.solution)) {
        printPropertyKey(out, output.solution.program, creation.key);
        out.print(" f"_s, creation.function.index(), " v"_s, creation.call.index());
        if (creation.isInCycle)
            out.print(" cycle"_s);
        out.println();
    }
}

void runFlowPropertyWalkTestCommand(TestCommandContext& context)
{
    std::optional<FlowWalkCommandOptions> options = parseOptions(context);
    if (!options)
        return;

    std::unique_ptr<FlowSolvedProgram> solved = solveFlow(*context.program, context.program->types, context.build);
    FlowSolution solution = solved->solution();
    FlowWalkOutput output { context.out, solution, *solved->values, FlowWalkInputs::of(solution) };

    const FlowGraph& graph = solution.graph;
    for (uint32_t index = 0; index < graph.numberOfInstances(); ++index) {
        FlowInstanceID instance(index);
        const FlowInstance& record = graph.at(instance);
        if (graph.isHost(record.function) || record.state != FlowInstanceState::Initialized)
            continue;
        if (!options->functions.isEmpty() && !options->functions.contains(record.function))
            continue;
        printAccessesOf(output, instance);
    }

    if (options->printsPrototypes)
        printPrototypeLinks(output);
    if (options->printsSymbols)
        printSymbolCreations(output);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
