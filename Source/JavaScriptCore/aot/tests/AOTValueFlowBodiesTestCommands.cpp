/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowBodies.h"

#if ENABLE(AOT)

#include "AOTAnalysisTestCommands.h"
#include "AOTIRPrinter.h"
#include "AOTSupportTestCommands.h"
#include "AOTValueFlowTestCommands.h"
#include "AOTValueSets.h"
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

static ASCIILiteral nameOf(ValueFlowBodyNodeKind kind)
{
    switch (kind) {
    case ValueFlowBodyNodeKind::EntryParameter:
        return "parameter"_s;
    case ValueFlowBodyNodeKind::Result:
        return "result"_s;
    case ValueFlowBodyNodeKind::Thrown:
        return "thrown"_s;
    case ValueFlowBodyNodeKind::Yielded:
        return "yielded"_s;
    case ValueFlowBodyNodeKind::Sent:
        return "sent"_s;
    case ValueFlowBodyNodeKind::Caught:
        return "caught"_s;
    case ValueFlowBodyNodeKind::OperationResult:
        return "operation"_s;
    case ValueFlowBodyNodeKind::Filter:
        return "filter"_s;
    case ValueFlowBodyNodeKind::Merge:
        return "merge"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printValueFlowBody(PrintStream& out, const Program& program, const ValueFlowBody& body)
{
    using Body = ValueFlowBody;
    IRPrinter printer(out, program);
    auto printAfter = [&](ASCIILiteral words, auto id) {
        out.print(words);
        printer.print(id);
    };
    auto printBefore = [&](auto id, auto... words) {
        printer.print(id);
        out.print(words...);
    };
    auto printMask = [&](uint32_t index) {
        const Body::Mask& mask = body.masks[index];
        for (TypeID type : mask.inside)
            printAfter(mask.kind == ValueFlowMaskKind::Checked ? " & "_s : " : "_s, type);
        for (TypeID type : mask.outside)
            printAfter(" - "_s, type);
        for (auto [builtin, module] : mask.outsideBuiltinsOfModules)
            out.print(" - builtin "_s, builtin.index(), " of m"_s, module.index());
    };
    auto printName = [&](const ValueFlowNodeName& name) {
        using Kind = ValueFlowNodeName::Kind;
        switch (name.kind) {
        case Kind::None:
            return out.print("-"_s);
        case Kind::OfBody:
            return out.print("n"_s, name.owner);
        case Kind::Escaped:
            return out.print("escaped"_s);
        case Kind::Variable:
            return printer.print(VariableID(name.owner));
        case Kind::Field:
            return printBefore(LayoutID(name.owner), "."_s, name.index);
        case Kind::EntryParameter:
            return printBefore(FunctionID(name.owner), ".parameter"_s, name.index);
        case Kind::Result:
            return printBefore(FunctionID(name.owner), ".result"_s, name.index);
        case Kind::Thrown:
            return printBefore(FunctionID(name.owner), ".thrown"_s);
        case Kind::OfHost:
            return out.print("host"_s, name.owner);
        case Kind::Hub:
            return out.print("host"_s, numberOfValueFlowHostNodes + name.owner);
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto printObject = [&](const ValueFlowObjectName& object) {
        switch (object.kind) {
        case ValueFlowObjectKind::Primitive:
            return out.print("primitive"_s, object.index);
        case ValueFlowObjectKind::UnknownObject:
            return out.print("unknown"_s);
        case ValueFlowObjectKind::Builtin:
            return printer.print(BuiltinID(object.index));
        case ValueFlowObjectKind::ModuleBuiltin:
            printBefore(BuiltinID(object.index), "@"_s);
            return printer.print(object.module);
        case ValueFlowObjectKind::Namespace:
            return printAfter("namespace "_s, ModuleID(object.index));
        case ValueFlowObjectKind::Closure:
            return printAfter("closure "_s, FunctionID(object.index));
        case ValueFlowObjectKind::CoroutineResult:
            return printAfter("coroutine "_s, FunctionID(object.index));
        case ValueFlowObjectKind::RestArray:
            return printAfter("rest "_s, FunctionID(object.index));
        case ValueFlowObjectKind::FunctionPrototype:
            return printAfter("prototype of "_s, FunctionID(object.index));
        case ValueFlowObjectKind::ConstructedObject:
            return printAfter("constructed by "_s, FunctionID(object.index));
        case ValueFlowObjectKind::Site:
            return out.print("s"_s, object.index);
        case ValueFlowObjectKind::Instance:
            return printAfter("instance "_s, LayoutID(object.index));
        case ValueFlowObjectKind::Prototype:
            return printAfter("prototype "_s, LayoutID(object.index));
        case ValueFlowObjectKind::ResultOfBuiltin:
            return printAfter(object.index % 2 ? "contents of the result of "_s : "result of "_s, BuiltinID(object.index / 2));
        case ValueFlowObjectKind::Key:
            return printer.print(PropertyKeyID(object.index));
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto printOperand = [&](const Body::Operand& operand) {
        const Body::Constant& constant = body.constants[operand.constant];
        if (!operand.node && constant.isEmpty()) {
            out.print("-"_s);
            return;
        }
        CommaPrinter bar(" | "_s);
        if (operand.node) {
            out.print(bar);
            printName(operand.node);
        }
        if (constant.primitives) {
            out.print(bar);
            printer.printType(Type::fromPrimitives(constant.primitives));
        }
        for (const ValueFlowObjectName& object : constant.objects) {
            out.print(bar);
            printObject(object);
        }
        printMask(operand.mask);
    };
    auto printNameAfter = [&](ASCIILiteral words, const ValueFlowNodeName& name) {
        out.print(words);
        printName(name);
    };
    auto printEdges = [&](ASCIILiteral keyword, const auto& edges) {
        for (const Body::Edge& edge : edges) {
            out.print("  "_s, keyword);
            printNameAfter(" "_s, edge.from);
            printNameAfter(" -> "_s, edge.to);
            out.println();
        }
    };

    out.print("body "_s);
    if (body.function)
        printer.print(body.function);
    else
        out.print("host"_s);
    out.println(": nodes="_s, body.nodes.size(), " sites="_s, body.sites.size(), " edges="_s, body.edges.size(), " edgesOnceReached="_s, body.edgesOnceReached.size(),
        " seeds="_s, body.seedsOnceReached.size(), " statements="_s, body.statements.size());

    for (uint32_t index = 0; index < body.nodes.size(); ++index) {
        const Body::Node& node = body.nodes[index];
        out.print("  n"_s, index, " "_s, nameOf(node.kind));
        if (node.value)
            printAfter(" "_s, node.value);
        printMask(node.mask);
        out.println();
    }
    for (uint32_t index = 0; index < body.sites.size(); ++index) {
        const ValueFlowSite& site = body.sites[index];
        out.print("  s"_s, index, " "_s);
        printer.print(site.value);
        switch (site.origin) {
        case ValueFlowSiteOrigin::Operation:
            break;
        case ValueFlowSiteOrigin::DataNode:
            out.print(" ordinal="_s, site.ordinal);
            break;
        case ValueFlowSiteOrigin::BuiltinResult:
            out.print(site.ordinal ? " contents of the result of a built-in"_s : " result of a built-in"_s);
            break;
        case ValueFlowSiteOrigin::TypedResult:
            out.print(" typed result ordinal="_s, site.ordinal);
            break;
        }
        if (site.layout)
            printAfter(" layout="_s, site.layout);
        if (site.builtinClass)
            printAfter(" class="_s, site.builtinClass);
        out.println();
    }
    printEdges("edge"_s, body.edges);
    printEdges("reached: edge"_s, body.edgesOnceReached);
    for (const Body::Seed& seed : body.seedsOnceReached) {
        printNameAfter("  reached: seed "_s, seed.node);
        out.print(" <- "_s);
        printOperand(Body::Operand { { }, seed.constant, seed.mask });
        out.println();
    }
    for (const Body::PassedParameter& passed : body.passedParameters) {
        out.print("  returns "_s);
        printOperand(Body::Operand { ValueFlowNodeName::ofBody(passed.entryParameter), 0, passed.mask });
        printNameAfter(" as it came, the rest in "_s, ValueFlowNodeName::ofBody(body.ownResult));
        out.println();
    }
    for (FunctionID function : body.functionsEnteredOnceReached) {
        printAfter("  reached: enters "_s, function);
        out.println();
    }
    for (uint32_t index = 0; index < body.statements.size(); ++index) {
        const Body::Statement& statement = body.statements[index];
        if (statement.kind != ValueFlowStatementKind::Operation) {
            RELEASE_ASSERT(statement.kind != ValueFlowStatementKind::Lookup && statement.kind != ValueFlowStatementKind::CallGroup && statement.kind != ValueFlowStatementKind::Escape);
            out.print("  "_s, nameOf(statement.kind));
        } else {
            out.print("  "_s, index < body.firstStatementLeavingBody ? "inside: "_s : ""_s, nameOf(statement.opcode), " "_s);
            printer.print(statement.site);
            out.print(" ("_s);
            CommaPrinter comma;
            for (const Body::Operand& operand : body.operandsOf(statement)) {
                out.print(comma);
                printOperand(operand);
            }
            out.print(")"_s);
        }
        if (statement.result)
            printNameAfter(" -> "_s, statement.result);
        if (statement.kind == ValueFlowStatementKind::Operation && statement.thrown)
            printNameAfter(" throws "_s, statement.thrown);
        out.println();
    }
    for (const Body::ObservedValue& observed : body.observedValues) {
        out.print("  observed "_s);
        if (observed.test)
            printer.print(observed.test);
        else
            out.print("switch"_s);
        out.print(": "_s);
        printOperand(observed.operand);
        out.println();
    }
}

void runValueFlowBodyTestCommand(TestCommandContext& context)
{
    const Program& program = *context.program;
    ValueFlowBuiltinTables tables(program.builtins());
    auto print = [&](const ValueFlowBody& body) {
        printValueFlowBody(context.out, program, body);
    };

    if (context.arguments.empty()) {
        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            if (program.functions[index].hasBody())
                print(reduceBody(program, tables, FunctionID(index)));
        }
        print(makeHostBody(program, tables));
        return;
    }

    auto parseFunction = functionParser(program);
    for (const String& word : context.arguments) {
        if (word == "host"_s) {
            print(makeHostBody(program, tables));
            continue;
        }
        std::optional<FunctionID> function = parseFunction(word);
        if (!function)
            return printBadArgument(context.out, word);
        print(reduceBody(program, tables, *function));
    }
}

void runValueFlowBodiesTestCommand(TestCommandContext& context)
{
    const Program& program = *context.program;
    PrintStream& out = context.out;
    ValueFlowBodies bodies = reduceBodies(program, context.build);

    uint32_t operandsWithNode = 0;
    uint32_t operandsWithConstantAlone = 0;
    uint32_t operandsWithMask = 0;
    for (const ValueFlowOperand& operand : bodies.operands()) {
        if (operand.node)
            ++operandsWithNode;
        else if (operand.constant != ValueSets::nothing)
            ++operandsWithConstantAlone;
        if (operand.mask != ValueSets::anything)
            ++operandsWithMask;
    }
    uint32_t firstNodeOfBodies = bodies.nodes().firstNodeOf(FunctionID(0u));
    out.println("bodies "_s, bodies.numberOfBodies(), " objects "_s, bodies.universe().numberOfObjects(), " sets "_s, bodies.sets().numberOfStoredSets());
    out.println("nodes "_s, bodies.nodes().numberOfNodes(), ": of bodies "_s, bodies.nodes().numberOfNodes() - firstNodeOfBodies, ", of the program "_s, firstNodeOfBodies, "; with a mask "_s, bodies.masks().size());
    out.println("edges "_s, bodies.edges().size(), ", once reached "_s, bodies.edgesOnceReached().size(), ", seeds "_s, bodies.seedsOnceReached().size(), ", functions entered "_s, bodies.functionsEnteredOnceReached().size());
    out.println("statements "_s, bodies.statements().size(), ", operands "_s, bodies.operands().size(), ": with a node "_s, operandsWithNode, ", a constant alone "_s, operandsWithConstantAlone, ", with a mask "_s, operandsWithMask,
        "; observed values "_s, bodies.observedValues().size());

    for (const String& word : context.arguments) {
        if (word != "operations"_s)
            return printBadArgument(out, word);
        Vector<uint32_t> counts;
        counts.fill(0, numberOfEnumerators<Opcode>);
        for (const ValueFlowStatement& statement : bodies.statements()) {
            if (statement.kind == ValueFlowStatementKind::Operation)
                ++counts[static_cast<unsigned>(statement.opcode)];
        }
        for (unsigned index = 0; index < counts.size(); ++index) {
            if (counts[index])
                out.println("  "_s, nameOf(static_cast<Opcode>(index)), " "_s, counts[index]);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
