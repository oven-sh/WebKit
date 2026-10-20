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
#include "AOTValueFlowBodyCopies.h"
#include "AOTValueFlowTestCommands.h"
#include "AOTValueSets.h"
#include <wtf/CommaPrinter.h>
#include <wtf/MonotonicTime.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using Body = ValueFlowBody;

ASCIILiteral nameOf(ValueFlowBodyNodeKind kind)
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

class BodyPrinter {
public:
    BodyPrinter(PrintStream& out, const Program& program, const Body& body)
        : m_out(out)
        , m_printer(out, program)
        , m_body(body)
    {
    }

    void print()
    {
        m_out.print("body "_s);
        if (m_body.function)
            m_printer.print(m_body.function);
        else
            m_out.print("host"_s);
        m_out.println(": nodes="_s, m_body.nodes.size(), " sites="_s, m_body.sites.size(), " edges="_s, m_body.edges.size(), " edgesOnceReached="_s, m_body.edgesOnceReached.size(),
            " seeds="_s, m_body.seedsOnceReached.size(), " statements="_s, m_body.statements.size());

        for (uint32_t index = 0; index < m_body.nodes.size(); ++index)
            printNode(index);
        for (uint32_t index = 0; index < m_body.sites.size(); ++index)
            printSite(index);
        for (const Body::Edge& edge : m_body.edges)
            printEdge("edge"_s, edge);
        for (const Body::Edge& edge : m_body.edgesOnceReached)
            printEdge("reached: edge"_s, edge);
        for (const Body::Seed& seed : m_body.seedsOnceReached) {
            m_out.print("  reached: seed "_s);
            print(seed.node);
            m_out.print(" <- "_s);
            print(Body::Operand { { }, seed.constant, seed.mask });
            m_out.println();
        }
        for (FunctionID function : m_body.functionsEnteredOnceReached) {
            m_out.print("  reached: enters "_s);
            m_printer.print(function);
            m_out.println();
        }
        for (uint32_t index = 0; index < m_body.statements.size(); ++index)
            printStatement(index);
        for (const Body::ObservedValue& observed : m_body.observedValues) {
            m_out.print("  observed "_s);
            if (observed.test)
                m_printer.print(observed.test);
            else
                m_out.print("switch"_s);
            m_out.print(": "_s);
            print(observed.operand);
            m_out.println();
        }
    }

private:
    void printNode(uint32_t index)
    {
        const Body::Node& node = m_body.nodes[index];
        m_out.print("  n"_s, index, " "_s, nameOf(node.kind));
        if (node.value) {
            m_out.print(" "_s);
            m_printer.print(node.value);
        }
        printMask(node.mask);
        m_out.println();
    }

    void printSite(uint32_t index)
    {
        const ValueFlowSite& site = m_body.sites[index];
        m_out.print("  s"_s, index, " "_s);
        if (site.function) {
            m_printer.print(site.function);
            m_out.print(" "_s);
        }
        m_printer.print(site.value);
        switch (site.origin) {
        case ValueFlowSiteOrigin::Operation:
            break;
        case ValueFlowSiteOrigin::DataNode:
            m_out.print(" ordinal="_s, site.ordinal);
            break;
        case ValueFlowSiteOrigin::BuiltinResult:
            m_out.print(site.ordinal ? " contents of the result of a built-in"_s : " result of a built-in"_s);
            break;
        case ValueFlowSiteOrigin::ParsedData:
            m_out.print(" parsed data"_s);
            break;
        }
        if (site.layout) {
            m_out.print(" layout="_s);
            m_printer.print(site.layout);
        }
        if (site.builtinClass) {
            m_out.print(" class="_s);
            m_printer.print(site.builtinClass);
        }
        m_out.println();
    }

    void printEdge(ASCIILiteral keyword, const Body::Edge& edge)
    {
        m_out.print("  "_s, keyword, " "_s);
        print(edge.from);
        m_out.print(" -> "_s);
        print(edge.to);
        m_out.println();
    }

    static ASCIILiteral nameOfStatementMadeByBodies(ValueFlowStatementKind kind)
    {
        switch (kind) {
        case ValueFlowStatementKind::ModuleEvaluation:
            return "ModuleEvaluation"_s;
        case ValueFlowStatementKind::Coroutine:
            return "Coroutine"_s;
        case ValueFlowStatementKind::HostRoots:
            return "HostRoots"_s;
        case ValueFlowStatementKind::UsedBuiltins:
            return "UsedBuiltins"_s;
        case ValueFlowStatementKind::UnseenCode:
            return "UnseenCode"_s;
        case ValueFlowStatementKind::ModulesLoadedByName:
            return "ModulesLoadedByName"_s;
        case ValueFlowStatementKind::Behavior:
            return "Behavior"_s;
        case ValueFlowStatementKind::Operation:
        case ValueFlowStatementKind::Lookup:
        case ValueFlowStatementKind::CallGroup:
        case ValueFlowStatementKind::Escape:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void printStatement(uint32_t index)
    {
        const Body::Statement& statement = m_body.statements[index];
        if (statement.kind != ValueFlowStatementKind::Operation) {
            m_out.print("  "_s, nameOfStatementMadeByBodies(statement.kind));
            if (statement.result) {
                m_out.print(" -> "_s);
                print(statement.result);
            }
            m_out.println();
            return;
        }
        m_out.print("  "_s, index < m_body.firstStatementLeavingBody ? "inside: "_s : ""_s, Typed::nameOf(statement.opcode), " "_s);
        if (statement.function) {
            m_printer.print(statement.function);
            m_out.print(" "_s);
        }
        m_printer.print(statement.site);
        m_out.print(" ("_s);
        CommaPrinter comma;
        for (const Body::Operand& operand : m_body.operandsOf(statement)) {
            m_out.print(comma);
            print(operand);
        }
        m_out.print(")"_s);
        if (statement.result) {
            m_out.print(" -> "_s);
            print(statement.result);
        }
        if (statement.thrown) {
            m_out.print(" throws "_s);
            print(statement.thrown);
        }
        if (statement.copiedCallee) {
            m_out.print(" has a copy of "_s);
            m_printer.print(statement.copiedCallee);
        }
        m_out.println();
    }

    void print(const ValueFlowNodeName& name)
    {
        using Kind = ValueFlowNodeName::Kind;
        switch (name.kind) {
        case Kind::None:
            m_out.print("-"_s);
            return;
        case Kind::OfBody:
            m_out.print("n"_s, name.owner);
            return;
        case Kind::Escaped:
            m_out.print("escaped"_s);
            return;
        case Kind::Variable:
            m_printer.print(VariableID(name.owner));
            return;
        case Kind::Field:
            m_printer.print(LayoutID(name.owner));
            m_out.print("."_s, name.index);
            return;
        case Kind::EntryParameter:
            m_printer.print(FunctionID(name.owner));
            m_out.print(".parameter"_s, name.index);
            return;
        case Kind::Result:
            m_printer.print(FunctionID(name.owner));
            m_out.print(".result"_s, name.index);
            return;
        case Kind::Thrown:
            m_printer.print(FunctionID(name.owner));
            m_out.print(".thrown"_s);
            return;
        case Kind::OfHost:
            m_out.print("host"_s, name.owner);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void print(const ValueFlowObjectName& object)
    {
        switch (object.kind) {
        case ValueFlowObjectKind::Primitive:
            m_out.print("primitive"_s, object.index);
            return;
        case ValueFlowObjectKind::UnknownObject:
            m_out.print("unknown"_s);
            return;
        case ValueFlowObjectKind::Builtin:
            m_printer.print(BuiltinID(object.index));
            return;
        case ValueFlowObjectKind::ModuleBuiltin:
            m_printer.print(BuiltinID(object.index));
            m_out.print("@"_s);
            m_printer.print(object.module);
            return;
        case ValueFlowObjectKind::Namespace:
            m_out.print("namespace "_s);
            m_printer.print(ModuleID(object.index));
            return;
        case ValueFlowObjectKind::Closure:
            m_out.print("closure "_s);
            m_printer.print(FunctionID(object.index));
            return;
        case ValueFlowObjectKind::CoroutineResult:
            m_out.print("coroutine "_s);
            m_printer.print(FunctionID(object.index));
            return;
        case ValueFlowObjectKind::RestArray:
            m_out.print("rest "_s);
            m_printer.print(FunctionID(object.index));
            return;
        case ValueFlowObjectKind::Site:
            m_out.print("s"_s, object.index);
            return;
        case ValueFlowObjectKind::Instance:
            m_out.print("instance "_s);
            m_printer.print(LayoutID(object.index));
            return;
        case ValueFlowObjectKind::Prototype:
            m_out.print("prototype "_s);
            m_printer.print(LayoutID(object.index));
            return;
        case ValueFlowObjectKind::ResultOfBuiltin:
            m_out.print(object.index % 2 ? "contents of the result of "_s : "result of "_s);
            m_printer.print(BuiltinID(object.index / 2));
            return;
        case ValueFlowObjectKind::Key:
            m_printer.print(PropertyKeyID(object.index));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void print(const Body::Operand& operand)
    {
        const Body::Constant& constant = m_body.constants[operand.constant];
        if (!operand.node && constant.isEmpty()) {
            m_out.print("-"_s);
            return;
        }
        CommaPrinter bar(" | "_s);
        if (operand.node) {
            m_out.print(bar);
            print(operand.node);
        }
        if (constant.primitives) {
            m_out.print(bar);
            m_printer.printType(Type::fromPrimitives(constant.primitives));
        }
        for (const ValueFlowObjectName& object : constant.objects) {
            m_out.print(bar);
            print(object);
        }
        printMask(operand.mask);
    }

    void printMask(uint32_t index)
    {
        const Body::Mask& mask = m_body.masks[index];
        for (TypeID type : mask.inside) {
            m_out.print(" & "_s);
            m_printer.print(type);
        }
        for (TypeID type : mask.outside) {
            m_out.print(" - "_s);
            m_printer.print(type);
        }
    }

    PrintStream& m_out;
    IRPrinter m_printer;
    const Body& m_body;
};

} // namespace

// valueflow-body [copies] [fN | host ..]: what is left of a function; with `copies` after the bodies of small functions have been copied into call sites.
void runValueFlowBodyTestCommand(TestCommandContext& context)
{
    const Program& program = *context.program;
    ValueFlowBuiltinTables tables(program.builtins());
    auto print = [&](const Body& body) {
        BodyPrinter(context.out, program, body).print();
    };

    if (!context.arguments.empty() && context.arguments[0] == "copies"_s) {
        unsigned numberOfFunctions = program.functions.size();
        ParallelArray<Body> bodies(context.build.parallelLoops(), numberOfFunctions + 1, [&](unsigned index) {
            return index == numberOfFunctions ? makeHostBody(program, tables) : reduceBody(program, tables, FunctionID(index));
        });
        copyBodiesIntoCallSites(program, context.build, bodies);
        auto parseFunction = functionParser(program);
        for (const String& word : context.arguments.subspan(1)) {
            std::optional<FunctionID> function = parseFunction(word);
            if (!function)
                return printBadArgument(context.out, word);
            print(bodies[function->index()]);
        }
        return;
    }

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

// valueflow-bodies [time] [operations]: how much is left of the whole program.
void runValueFlowBodiesTestCommand(TestCommandContext& context)
{
    const Program& program = *context.program;
    PrintStream& out = context.out;
    MonotonicTime start = MonotonicTime::now();
    ValueFlowBodies bodies = reduceBodies(program, context.build);
    Seconds elapsed = MonotonicTime::now() - start;

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
        if (word == "time"_s)
            out.println("milliseconds "_s, static_cast<uint64_t>(elapsed.milliseconds()), ": reduce "_s, static_cast<uint64_t>(bodies.times().reduce.milliseconds()), ", number "_s, static_cast<uint64_t>(bodies.times().number.milliseconds()),
                ", masks "_s, static_cast<uint64_t>(bodies.times().makeMasks.milliseconds()), ", link "_s, static_cast<uint64_t>(bodies.times().link.milliseconds()));
        else if (word == "operations"_s) {
            Vector<uint32_t> counts;
            counts.fill(0, numberOfEnumerators<Opcode>);
            for (const ValueFlowStatement& statement : bodies.statements())
                ++counts[static_cast<unsigned>(statement.opcode)];
            for (unsigned index = 0; index < counts.size(); ++index) {
                if (counts[index])
                    out.println("  "_s, Typed::nameOf(static_cast<Opcode>(index)), " "_s, counts[index]);
            }
        } else
            return printBadArgument(out, word);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
