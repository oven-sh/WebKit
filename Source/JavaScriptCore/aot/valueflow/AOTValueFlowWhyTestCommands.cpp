/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTAddGenericVersion.h"
#include "AOTCFG.h"
#include "AOTSort.h"
#include "AOTSupportTestCommands.h"
#include "AOTTypeArena.h"
#include "AOTValueFlowAnalysis.h"
#include "AOTValueFlowLog.h"
#include "AOTValueFlowPositions.h"
#include "AOTValueFlowResultsInternal.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <wtf/BitVector.h>
#include <wtf/EnumSet.h>
#include <wtf/HashMap.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

static ASCIILiteral nameOf(ValueFlowStatementKind kind)
{
    switch (kind) {
    case ValueFlowStatementKind::Operation:
        return "Operation"_s;
    case ValueFlowStatementKind::Lookup:
        return "Lookup"_s;
    case ValueFlowStatementKind::CallGroup:
        return "CallGroup"_s;
    case ValueFlowStatementKind::Behavior:
        return "Behavior"_s;
    case ValueFlowStatementKind::Escape:
        return "Escape"_s;
    case ValueFlowStatementKind::ModuleEvaluation:
        return "ModuleEvaluation"_s;
    case ValueFlowStatementKind::EntryChecks:
        return "EntryChecks"_s;
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
    }
    return "?"_s;
}

constexpr size_t numberOfUnknownSources = static_cast<size_t>(ValueFlowUnknownSource::ObjectWithoutLayout) + 1;

static ASCIILiteral nameOf(ValueFlowUnknownSource source)
{
    switch (source) {
    case ValueFlowUnknownSource::UndescribedNativeResult:
        return "UndescribedNativeResult"_s;
    case ValueFlowUnknownSource::UnseenCode:
        return "UnseenCode"_s;
    case ValueFlowUnknownSource::UncheckedConversionOfUntracked:
        return "UncheckedConversionOfUntracked"_s;
    case ValueFlowUnknownSource::DataFromOutside:
        return "DataFromOutside"_s;
    case ValueFlowUnknownSource::ThrownByRunTime:
        return "ThrownByRunTime"_s;
    case ValueFlowUnknownSource::ParameterOfEscapedFunction:
        return "ParameterOfEscapedFunction"_s;
    case ValueFlowUnknownSource::ContentsOfEscapedObject:
        return "ContentsOfEscapedObject"_s;
    case ValueFlowUnknownSource::ResultOfUnknownCallee:
        return "ResultOfUnknownCallee"_s;
    case ValueFlowUnknownSource::ObjectWithoutLayout:
        return "ObjectWithoutLayout"_s;
    }
    return "?"_s;
}

static ASCIILiteral nameOf(ValueFlowMadeNodeKind kind)
{
    switch (kind) {
    case ValueFlowMadeNodeKind::Constant:
        return "constant"_s;
    case ValueFlowMadeNodeKind::Read:
        return "read"_s;
    case ValueFlowMadeNodeKind::Written:
        return "written"_s;
    case ValueFlowMadeNodeKind::Lookup:
        return "lookup"_s;
    case ValueFlowMadeNodeKind::CallGroup:
        return "group"_s;
    case ValueFlowMadeNodeKind::Hub:
        return "hub"_s;
    case ValueFlowMadeNodeKind::Masked:
        return "masked"_s;
    case ValueFlowMadeNodeKind::Marked:
        return "marked"_s;
    case ValueFlowMadeNodeKind::WrittenUnderKind:
        return "written-under-kind"_s;
    case ValueFlowMadeNodeKind::ReadUnderKind:
        return "read-under-kind"_s;
    case ValueFlowMadeNodeKind::Instance:
        return "instance"_s;
    }
    return "?"_s;
}

static void printKey(PrintStream& out, const ValueFlowSolver& solver, const ValueFlowNodeKey& key)
{
    out.print(nameOf(key.kind), " detail ", key.detail, " payload ", key.payload);
    bool hasName = key.kind == ValueFlowMadeNodeKind::Lookup || key.kind == ValueFlowMadeNodeKind::Read || key.kind == ValueFlowMadeNodeKind::Written;
    if (const Program* program = solver.input().program; program && hasName && key.name().kind == ValueFlowNameKind::Property) {
        if (StringID text = program->at(PropertyKeyID(key.payload)).text)
            out.print(" \"", String::fromUTF8(program->string(text)), "\"");
    }
    if (!key.set)
        return;
    out.print(" set of ", solver.sets().size(key.set));
    if (ValueSets::isComplement(key.set) || solver.sets().size(key.set) > 3)
        return;
    solver.sets().forEachMember(key.set, [&](ValueFlowObjectID member) {
        out.print(" #", member.index());
    });
}

static void printStatement(PrintStream& out, const ValueFlowSolver& solver, ValueFlowStatementID id)
{
    const ValueFlowStatement& statement = solver.statementAt(id);
    if (statement.kind == ValueFlowStatementKind::Operation) {
        out.print(statement.opcode, " f", statement.function.index(), " v", statement.site.index());
        return;
    }
    out.print(nameOf(statement.kind));
    if (statement.function)
        out.print(" f", statement.function.index());
    if (solver.isMadeByRule(id) && statement.kind != ValueFlowStatementKind::Escape) {
        out.print(" ");
        printKey(out, solver, solver.keyOf(id));
    }
}

static void printNode(PrintStream& out, ValueFlowSolution& solution, ValueFlowNodeID node)
{
    const ValueFlowSolver& solver = solution.solver();
    out.print("n", node.index(), " ");
    if (const ValueFlowNodeKey* key = solver.keyOfMadeNode(node))
        printKey(out, solver, *key);
    else if (node == ValueFlowProgramNodes::escaped())
        out.print("escaped");
    else if (node == ValueFlowProgramNodes::lost())
        out.print("lost");
    else if (FunctionID function = solution.bodies().nodes().functionOf(node))
        out.print("of f", function.index(), " +", node.index() - solution.bodies().nodes().firstNodeOf(function));
    else
        out.print("of the program");
}

// From the function back to the host: who reached it, who reached the function of that statement, and so on.
static void printWhyReached(PrintStream& out, ValueFlowSolution& solution, FunctionID function)
{
    const ValueFlowSolver& solver = solution.solver();
    const Program& program = solution.program();
    if (!solver.isReached(function)) {
        out.println("f", function.index(), " is not reached");
        return;
    }
    BitVector seen;
    while (function && !seen.set(function.index())) {
        ValueFlowReachWitness witness = solver.reachedBy(function);
        out.print("f", function.index(), " <- ");
        if (witness.statement) {
            printStatement(out, solver, witness.statement);
            out.println();
            function = solver.statementAt(witness.statement).function;
            continue;
        }
        if (witness.enteringBody >= program.functions.size()) {
            out.println("the host");
            return;
        }
        out.println("entered by f", witness.enteringBody);
        function = FunctionID(witness.enteringBody);
    }
}

static void printValue(PrintStream&, ValueFlowSolution&, ValueSetID);

// In the order of the rounds: the edges that rules have logged into the flags of the host, and the first edges into `escaped` and into `lost` from a node that in the end holds a built-in that lets in unseen code.
static void printTimeline(PrintStream& out, ValueFlowSolution& solution)
{
    constexpr unsigned numberOfEdgesPrinted = 60;
    constexpr ValueFlowHostNode flags[] {
        ValueFlowHostNode::UnseenCodeRuns, ValueFlowHostNode::UnknownCodeWrites, ValueFlowHostNode::HostChoosesOperands, ValueFlowHostNode::ModulesAreRequiredByName,
        ValueFlowHostNode::ModulesAreImportedByName, ValueFlowHostNode::LibraryModulesAreRequiredByName, ValueFlowHostNode::ObjectNobodyKnowsIsWritten,
        ValueFlowHostNode::UnseenCodeChangesRealm, ValueFlowHostNode::ValueNobodyKnowsIsCalled, ValueFlowHostNode::UnseenCodeHasLoader,
    };
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowProgramNodes& nodes = solution.bodies().nodes();
    const ValueFlowUniverse& universe = solution.bodies().universe();
    const BuiltinTable& builtins = solution.program().builtins();
    auto holdsEvaluator = [&](ValueFlowNodeID node) {
        bool holds = false;
        ValueSetID value = solver.valueOf(node);
        if (ValueSets::isComplement(value))
            return true;
        solution.sets().forEachMember(value, [&](ValueFlowObjectID member) {
            BuiltinID builtin = universe.builtinOf(member);
            holds |= builtin && builtins.letsInUnseenCode(builtins.canonicalID(builtin));
        });
        return holds;
    };
    unsigned printed = 0;
    for (const ValueFlowSolver::LoggedEdge& logged : solver.edgesInOrderLogged()) {
        ASCIILiteral target;
        unsigned flag = 0;
        if (logged.edge.to == ValueFlowProgramNodes::escaped())
            target = "escaped"_s;
        else if (logged.edge.to == ValueFlowProgramNodes::lost())
            target = "lost"_s;
        else {
            for (ValueFlowHostNode node : flags) {
                if (logged.edge.to == nodes.ofHost(node)) {
                    target = "flag h"_s;
                    flag = static_cast<unsigned>(node);
                }
            }
        }
        if (target.isNull())
            continue;
        if (!flag && (printed >= numberOfEdgesPrinted || !holdsEvaluator(logged.edge.from)))
            continue;
        printed += !flag;
        out.print("round ", logged.round, " ", target);
        if (flag)
            out.print(flag);
        out.print(" <- ");
        printNode(out, solution, logged.edge.from);
        out.print(" holding ");
        printValue(out, solution, solver.valueOf(logged.edge.from));
        out.print("      by ");
        printStatement(out, solver, logged.statement);
        out.println();
    }
}

// The used built-ins whose code the function of the library is, and those with a row that says native code calls it.
static void printUsedBuiltinsBehind(PrintStream& out, ValueFlowSolution& solution, FunctionID function)
{
    const Program& program = solution.program();
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowSolver& solver = solution.solver();
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (entry.function != function || !builtins.contains(entry.id))
            continue;
        for (BuiltinID builtin : builtins.at(entry.id).implements) {
            if (!solver.usesOf(builtin).isEmpty())
                out.print(" code-of:", builtins.tokenOf(builtin));
        }
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            BuiltinID builtin(index);
            if (builtins.canonicalID(builtin) != builtin || solver.usesOf(builtin).isEmpty())
                continue;
            for (const BuiltinCalledFunction& called : builtins.calledFunctionsOf(builtin)) {
                if (called.function == entry.id)
                    out.print(" row-of:", builtins.tokenOf(builtin));
            }
        }
    }
}

// For each of the last rounds that logged an edge: how many edges each statement logged for the first time, the busiest statements first.
static void printLateRounds(PrintStream& out, ValueFlowSolution& solution, uint32_t numberOfRounds)
{
    constexpr size_t numberOfStatementsPrinted = 4;
    const ValueFlowSolver& solver = solution.solver();
    std::span<const ValueFlowSolver::LoggedEdge> logged = solver.edgesInOrderLogged();
    if (logged.empty())
        return;
    uint32_t lastRound = logged.back().round;
    uint32_t firstRound = lastRound > numberOfRounds ? lastRound - numberOfRounds + 1 : 0;
    size_t index = 0;
    while (index < logged.size() && logged[index].round < firstRound)
        ++index;
    while (index < logged.size()) {
        uint32_t round = logged[index].round;
        Vector<uint32_t> statements;
        for (; index < logged.size() && logged[index].round == round; ++index)
            statements.append(logged[index].statement.index());
        AOT::sort(statements);
        Vector<std::pair<uint32_t, uint32_t>> counts;
        for (uint32_t statement : statements) {
            if (counts.isEmpty() || counts.last().second != statement)
                counts.append({ 0, statement });
            ++counts.last().first;
        }
        AOT::sort(counts, [](auto first, auto second) {
            return std::pair { second.first, first.second } < std::pair { first.first, second.second };
        });
        out.print("round ", round, " edges ", statements.size(), " statements ", counts.size(), ":");
        for (auto [count, statement] : counts.span().first(std::min(counts.size(), numberOfStatementsPrinted))) {
            out.print("  ", count, " ");
            printStatement(out, solver, ValueFlowStatementID(statement));
        }
        out.println();
    }
}

// The functions in the order of the rounds in which they were reached, each with the statement that reached it.
static void printFunctionsInOrderReached(PrintStream& out, ValueFlowSolution& solution)
{
    const ValueFlowSolver& solver = solution.solver();
    const Program& program = solution.program();
    Vector<FunctionID> reached;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (program.functions[index].hasBody() && solver.isReached(FunctionID(index)))
            reached.append(FunctionID(index));
    }
    AOT::stableSort(reached, { }, [&](FunctionID function) {
        return solver.reachedBy(function).round;
    });
    for (FunctionID function : reached) {
        ValueFlowReachWitness witness = solver.reachedBy(function);
        out.print("round ", witness.round, " f", function.index(), " <- ");
        if (witness.statement) {
            printStatement(out, solver, witness.statement);
            if (solver.statementAt(witness.statement).kind == ValueFlowStatementKind::UsedBuiltins)
                printUsedBuiltinsBehind(out, solution, function);
        }
        else if (witness.enteringBody >= program.functions.size())
            out.print("the host");
        else
            out.print("entered by f", witness.enteringBody);
        out.println();
    }
    out.println("reached ", reached.size());
}

static void printWhyHeld(PrintStream& out, ValueFlowSolution& solution, ValueFlowNodeID node, ValueFlowObjectID member)
{
    const ValueFlowSolver& solver = solution.solver();
    Vector<ValueFlowNodeID> way = solver.explain(node, member);
    if (way.isEmpty()) {
        out.println("  not held");
        return;
    }
    for (size_t index = 0; index < way.size(); ++index) {
        out.print("  ");
        printNode(out, solution, way[index]);
        if (index + 1 < way.size()) {
            if (ValueFlowStatementID statement = solver.statementThatLogged({ way[index + 1], way[index] })) {
                out.print("   by ");
                printStatement(out, solver, statement);
            }
            if (std::optional<ValueFlowUnknownSource> source = solver.unknownSourceOf(ValueFlowEdge { way[index + 1], way[index] }))
                out.print("   source ", nameOf(*source));
        }
        out.println();
    }
}

enum class SortOfObject : uint8_t {
    Builtin,
    Closure,
    PrototypeObject,
    Instance,
    RecordSite,
    InstanceSite,
    OtherLayoutSite,
    BuiltinClassSite,
    ResultOfBuiltin,
    Other,
};
constexpr size_t numberOfSortsOfObjects = 10;
constexpr ASCIILiteral namesOfSortsOfObjects[numberOfSortsOfObjects] { "built-in"_s, "closure"_s, "prototype-object"_s, "instance"_s, "record-site"_s, "instance-site"_s, "other-layout-site"_s, "built-in-class-site"_s, "result-of-built-in"_s, "other"_s };

static SortOfObject sortOf(const Program& program, const ValueFlowUniverse& universe, ValueFlowObjectID object)
{
    switch (universe.kindOf(object)) {
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin:
        return SortOfObject::Builtin;
    case ValueFlowObjectKind::Closure:
        return SortOfObject::Closure;
    case ValueFlowObjectKind::Prototype:
        return SortOfObject::PrototypeObject;
    case ValueFlowObjectKind::Instance:
        return SortOfObject::Instance;
    case ValueFlowObjectKind::ResultOfBuiltin:
        return SortOfObject::ResultOfBuiltin;
    case ValueFlowObjectKind::Site: {
        const ValueFlowSite& site = *universe.siteOf(object);
        if (site.origin == ValueFlowSiteOrigin::BuiltinResult)
            return SortOfObject::ResultOfBuiltin;
        if (site.builtinClass)
            return SortOfObject::BuiltinClassSite;
        if (!site.layout)
            return SortOfObject::Other;
        switch (program.at(site.layout).kind) {
        case LayoutKind::Record:
            return SortOfObject::RecordSite;
        case LayoutKind::Instance:
            return SortOfObject::InstanceSite;
        default:
            return SortOfObject::OtherLayoutSite;
        }
    }
    default:
        return SortOfObject::Other;
    }
}

static void printObject(PrintStream& out, ValueFlowSolution& solution, ValueFlowObjectID object)
{
    const ValueFlowUniverse& universe = solution.bodies().universe();
    if (object == ValueFlowUniverse::unknownObject())
        out.print("unknown");
    else if (object == ValueFlowUniverse::untrackedObject())
        out.print("untracked");
    else if (FunctionID function = universe.closureFunction(object))
        out.print("f", function.index());
    else if (BuiltinID builtin = universe.builtinOf(object))
        out.print(solution.program().builtins().tokenOf(builtin));
    else if (universe.kindOf(object) == ValueFlowObjectKind::FunctionPrototype)
        out.print("prototype@f", universe.functionOf(object).index());
    else if (universe.kindOf(object) == ValueFlowObjectKind::ConstructedObject)
        out.print("constructed@f", universe.functionOf(object).index());
    else if (FunctionID function = universe.functionOf(object))
        out.print("object", object.index(), "@f", function.index());
    else
        out.print("object", object.index());
}

namespace {

// How an object has escaped: the statement that logged the last edge of a shortest way into `escaped`, and where the edge comes out of what is written into an escaped object, that object.
class WaysOfEscape {
public:
    struct Step {
        ValueFlowStatementID logger;
        ValueFlowObjectID holder;
        // Without a logger: the opcodes of the operations that take the node from which the last edge starts, or what that node is.
        String takers;
    };

    explicit WaysOfEscape(ValueFlowSolution& solution)
        : m_solver(solution.solver())
        , m_bodies(solution.bodies())
    {
    }

    Step stepOf(ValueFlowObjectID object)
    {
        auto found = m_steps.find(object.index());
        if (found != m_steps.end())
            return found->value;
        ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();
        ValueSetID escaped = m_solver.valueOf(escapedNode);
        Step step;
        Vector<ValueFlowNodeID> way = m_solver.explain(escapedNode, object);
        if (way.size() > 1)
            step.logger = m_solver.statementThatLogged({ way[1], way[0] });
        auto holdsWhatIsWritten = [&](ValueFlowNodeID node) {
            const ValueFlowNodeKey* key = m_solver.keyOfMadeNode(node);
            return key && (key->kind == ValueFlowMadeNodeKind::Written || key->kind == ValueFlowMadeNodeKind::WrittenUnderKind) && key->set;
        };
        if (!step.logger && way.size() > 1 && !holdsWhatIsWritten(way[1]))
            step.logger = loggerOfEdgeIntoRetiredNode(way[1]);
        if (step.logger ? isStatementOfEscape(step.logger) : way.size() > 1 && holdsWhatIsWritten(way[1])) {
            for (ValueFlowNodeID node : way) {
                if (!holdsWhatIsWritten(node))
                    continue;
                const ValueFlowNodeKey* key = m_solver.keyOfMadeNode(node);
                m_solver.sets().forEachMember(key->set, [&](ValueFlowObjectID member) {
                    if (!step.holder && member != object && m_solver.sets().contains(escaped, member))
                        step.holder = member;
                });
                break;
            }
        }
        if (!step.logger)
            step.takers = step.holder ? String("what is written into an escaped object"_s) : way.size() > 1 ? takersOf(way[1]) : String("nothing"_s);
        m_steps.add(object.index(), step);
        return step;
    }

    ValueFlowObjectID rootOf(ValueFlowObjectID object)
    {
        for (unsigned steps = 0; steps < 64; ++steps) {
            ValueFlowObjectID holder = stepOf(object).holder;
            if (!holder)
                break;
            object = holder;
        }
        return object;
    }

private:
    // The statement of kind Escape, or one that it has made for a family of escaped instances.
    bool isStatementOfEscape(ValueFlowStatementID id) const
    {
        ValueFlowStatementKind kind = m_solver.statementAt(id).kind;
        return kind == ValueFlowStatementKind::Escape || (kind == ValueFlowStatementKind::Lookup && m_solver.keyOf(id).name().kind == ValueFlowNameKind::FoundThroughEscapedInstances);
    }

    // The graph retires a node all of whose members end up in `escaped`, and gives each node with an edge into it an edge of its own straight into `escaped`, which nobody has logged.
    ValueFlowStatementID loggerOfEdgeIntoRetiredNode(ValueFlowNodeID from)
    {
        if (!std::exchange(m_hasLookedForRetiredNodes, true)) {
            for (const ValueFlowSolver::LoggedEdge& logged : m_solver.edgesInOrderLogged()) {
                if (m_solver.graph().isRetired(logged.edge.to))
                    m_loggerOfEdgeIntoRetiredNode.add(logged.edge.from.index(), logged.statement);
            }
        }
        return m_loggerOfEdgeIntoRetiredNode.get(from.index());
    }

    String takersOf(ValueFlowNodeID node) const
    {
        if (const ValueFlowNodeKey* key = m_solver.keyOfMadeNode(node)) {
            if (key->kind != ValueFlowMadeNodeKind::Masked)
                return makeString("a node that a rule has made: "_s, nameOf(key->kind));
            node = key->maskedNode();
            if (m_solver.keyOfMadeNode(node))
                return "a node that a rule has made, behind a mask"_s;
        }
        FunctionID function = m_bodies.nodes().functionOf(node);
        if (!function)
            return "a variable or a field"_s;
        Vector<Opcode, 4> opcodes;
        for (const ValueFlowStatement& statement : m_bodies.statementsOf(function.index())) {
            if (statement.kind != ValueFlowStatementKind::Operation || opcodes.contains(statement.opcode))
                continue;
            if (std::ranges::contains(m_bodies.operandsOf(statement), node, &ValueFlowOperand::node))
                opcodes.append(statement.opcode);
        }
        if (opcodes.isEmpty())
            return "a node that no operation with a rule takes"_s;
        AOT::sort(opcodes);
        StringBuilder names;
        for (Opcode opcode : opcodes)
            names.append(names.isEmpty() ? ""_s : "+"_s, nameOf(opcode));
        return names.toString();
    }

    const ValueFlowSolver& m_solver;
    const ValueFlowBodies& m_bodies;
    HashMap<uint32_t, Step, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_steps;
    bool m_hasLookedForRetiredNodes { false };
    HashMap<uint32_t, ValueFlowStatementID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_loggerOfEdgeIntoRetiredNode;
};

} // anonymous namespace

// An object that has escaped because an escaped object holds it is counted for that holder, and so on up to an object that a statement other than Escape has let escape.
static void printRootsOfEscape(PrintStream& out, ValueFlowSolution& solution)
{
    constexpr size_t numberOfRootsPrinted = 30;
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowUniverse& universe = solution.bodies().universe();
    ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();
    ValueSetID escaped = solver.valueOf(escapedNode);
    WaysOfEscape ways(solution);

    struct Root {
        ValueFlowObjectID object;
        uint32_t closures { 0 };
    };
    Vector<Root> roots;
    solver.sets().forEachMember(escaped, [&](ValueFlowObjectID object) {
        if (!universe.closureFunction(object))
            return;
        ValueFlowObjectID root = ways.rootOf(object);
        size_t place = roots.findIf([&](const Root& other) {
            return other.object == root;
        });
        if (place == notFound) {
            place = roots.size();
            roots.append({ root, 0 });
        }
        ++roots[place].closures;
    });

    AOT::stableSort(roots, std::greater { }, &Root::closures);
    out.println("roots ", roots.size());
    for (const Root& root : roots.span().first(std::min(roots.size(), numberOfRootsPrinted))) {
        out.print(root.closures, " closures behind ");
        printObject(out, solution, root.object);
        out.println();
        printWhyHeld(out, solution, escapedNode, root.object);
    }
}

static void printValue(PrintStream& out, ValueFlowSolution& solution, ValueSetID value)
{
    constexpr unsigned numberOfMembersPrinted = 24;
    const ValueFlowUniverse& universe = solution.bodies().universe();
    unsigned printed = 0;
    out.print(solution.sets().size(value), " members:");
    solution.sets().forEachMember(value, [&](ValueFlowObjectID member) {
        if (++printed > numberOfMembersPrinted)
            return;
        if (member == ValueFlowUniverse::unknownObject())
            out.print(" unknown");
        else if (member == ValueFlowUniverse::untrackedObject())
            out.print(" untracked");
        else if (FunctionID function = universe.closureFunction(member))
            out.print(" f", function.index());
        else if (BuiltinID builtin = universe.builtinOf(member))
            out.print(" ", solution.program().builtins().tokenOf(builtin));
        else if (PropertyKeyID key = universe.keyOf(member))
            out.print(" key", key.index());
        else if (!universe.isObject(member))
            out.print(" primitive", member.index());
        else if (universe.kindOf(member) == ValueFlowObjectKind::FunctionPrototype)
            out.print(" prototype@f", universe.functionOf(member).index());
        else if (universe.kindOf(member) == ValueFlowObjectKind::ConstructedObject)
            out.print(" constructed@f", universe.functionOf(member).index());
        else if (FunctionID function = universe.functionOf(member))
            out.print(" object", member.index(), "@f", function.index());
        else
            out.print(" object", member.index());
    });
    out.println(printed > numberOfMembersPrinted ? " .." : "");
}

enum class EscapeIsCountedFor : uint8_t { ObjectItself, Root };

// For each counted object that has escaped, the statement that logged the last edge of a shortest way into `escaped`: who lets how many objects escape, and how many closures among them.
static void printWhoLetsEscape(PrintStream& out, ValueFlowSolution& solution, WaysOfEscape& ways, EscapeIsCountedFor countedFor, NOESCAPE const Invocable<bool(ValueFlowObjectID)> auto& isCounted)
{
    struct Line {
        ValueFlowStatementKind kind;
        Opcode opcode;
        // Of a statement that a rule has made: the kind of name or of call in its key.
        uint8_t detail { 0 };
        uint32_t objects { 0 };
        uint32_t closures { 0 };
        ValueFlowStatementID example;
        std::array<uint32_t, numberOfSortsOfObjects> sorts { };
    };
    struct StatementLine {
        ValueFlowStatementID statement;
        uint32_t objects { 0 };
    };
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowUniverse& universe = solution.bodies().universe();
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();

    Vector<Line> lines;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> objectsOfStatement;
    uint32_t byEdgesOfNoStatement = 0;
    HashMap<String, uint32_t> objectsOfTakers;
    solver.sets().forEachMember(solver.valueOf(escaped), [&](ValueFlowObjectID object) {
        if (!universe.isObject(object) || ValueFlowResultInputs::isPlaceholder(object) || !isCounted(object))
            return;
        ValueFlowObjectID subject = countedFor == EscapeIsCountedFor::Root ? ways.rootOf(object) : object;
        ValueFlowStatementID logger = ways.stepOf(subject).logger;
        if (!logger) {
            ++byEdgesOfNoStatement;
            ++objectsOfTakers.add(ways.stepOf(subject).takers, 0).iterator->value;
            return;
        }
        const ValueFlowStatement& statement = solver.statementAt(logger);
        uint8_t detail = solver.isMadeByRule(logger) && statement.kind != ValueFlowStatementKind::Escape ? solver.keyOf(logger).detail : 0;
        size_t found = lines.findIf([&](const Line& line) {
            return line.kind == statement.kind && line.opcode == statement.opcode && line.detail == detail;
        });
        if (found == notFound) {
            found = lines.size();
            lines.append({ statement.kind, statement.opcode, detail, 0, 0, logger, { } });
        }
        ++lines[found].objects;
        lines[found].closures += !!universe.closureFunction(object);
        ++lines[found].sorts[static_cast<size_t>(sortOf(solution.program(), universe, subject))];
        ++objectsOfStatement.add(logger.index(), 0).iterator->value;
    });

    AOT::sort(lines, std::greater { }, &Line::objects);
    out.println("by an edge that no statement has logged: ", byEdgesOfNoStatement, "; by the operations that take the node from which it starts:");
    Vector<std::pair<String, uint32_t>> takers;
    for (auto& entry : objectsOfTakers)
        takers.append({ entry.key, entry.value });
    AOT::sort(takers, [](const auto& first, const auto& second) {
        return first.second != second.second ? first.second > second.second : codePointCompareLessThan(first.first, second.first);
    });
    for (auto& [names, objects] : takers)
        out.println("  ", names, ": ", objects);
    for (const Line& line : lines) {
        if (line.kind == ValueFlowStatementKind::Operation)
            out.print(line.opcode);
        else
            out.print(nameOf(line.kind));
        out.print(": ", line.objects, " objects, ", line.closures, " closures; for one: ");
        printStatement(out, solver, line.example);
        out.println();
        out.print("   ");
        for (size_t sort = 0; sort < numberOfSortsOfObjects; ++sort) {
            if (line.sorts[sort])
                out.print(" ", namesOfSortsOfObjects[sort], " ", line.sorts[sort]);
        }
        out.println();
    }

    Vector<StatementLine> statements;
    for (auto& entry : objectsOfStatement)
        statements.append({ ValueFlowStatementID(entry.key), entry.value });
    AOT::sort(statements, [](const StatementLine& first, const StatementLine& second) {
        return std::pair { second.objects, first.statement.index() } < std::pair { first.objects, second.statement.index() };
    });
    constexpr size_t numberOfStatementsPrinted = 16;
    out.println("the statements that let most objects escape:");
    for (const StatementLine& line : statements.span().first(std::min(statements.size(), numberOfStatementsPrinted))) {
        out.print("  ", line.objects, ": ");
        printStatement(out, solver, line.statement);
        out.println();
        if (solver.isMadeByRule(line.statement) && solver.statementAt(line.statement).kind != ValueFlowStatementKind::Escape && solver.keyOf(line.statement).set) {
            out.print("    its set: ");
            printValue(out, solution, solver.keyOf(line.statement).set);
        }
    }
}

// Where the unknown object in the first operand of the counted operations of reached functions was born: by what the rule that made it has named as its source, then the operations behind the most operands.
static void printBirthsOfUnknownObject(PrintStream& out, ValueFlowSolution& solution, BuildContext& build, NOESCAPE const Invocable<bool(Opcode)> auto& isCounted)
{
    constexpr size_t numberOfBirthsPrinted = 40;
    struct Birth {
        Witness witness;
        uint32_t operands { 0 };
    };
    const Program& program = solution.program();
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowBodies& bodies = solution.bodies();
    ValueFlowResultInputs inputs { program, solution, build };
    ValueFlowBirthsOfUnknownObject birthOfNode(inputs);

    Vector<Birth> births;
    HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> placeOfBirth;
    std::array<uint32_t, numberOfUnknownSources> operandsOfSource { };
    uint32_t counted = 0;
    uint32_t withUntrackedObject = 0;
    uint32_t withUnknownObject = 0;
    uint32_t withoutSource = 0;
    uint32_t withoutBirth = 0;
    // The typed bodies, whatever else the program wrote, its generic versions, the library.
    enum Part : uint8_t { TypedBodies, Program, GenericVersions, Library, NumberOfParts };
    std::array<uint32_t, NumberOfParts> countedInPart { };
    std::array<uint32_t, NumberOfParts> withUnknownObjectInPart { };
    Vector<std::pair<FunctionID, ValueID>> examplesInTypedBodies;
    HashMap<String, uint32_t> producersOfOperandsWithUnknownObject;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!solver.isReached(FunctionID(index)))
            continue;
        FunctionRole role = program.functions[index].role;
        Part part = role == FunctionRole::GenericVersion ? GenericVersions : isWrittenInProgram(program.functions[index]) ? Program : Library;
        if (part == Program && role == FunctionRole::Source && bodies.universe().genericVersionOf(FunctionID(index)))
            part = TypedBodies;
        for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || !isCounted(statement.opcode) || !statement.operandCount)
                continue;
            ++counted;
            ++countedInPart[part];
            const ValueFlowOperand& operand = bodies.operandsOf(statement)[0];
            ValueSetID held = solution.valueOf(operand);
            withUntrackedObject += solution.sets().contains(held, ValueFlowUniverse::untrackedObject());
            if (!solution.sets().contains(held, ValueFlowUniverse::unknownObject()))
                continue;
            ++withUnknownObject;
            ++withUnknownObjectInPart[part];
            if (part == TypedBodies || part == Program) {
                // What the front end says of the operand, and which operation has made it.
                const Function& function = program.functions[index];
                ValueID value = function.at(statement.site).operand(0);
                const Value& producer = function.at(value);
                const Type& type = program.at(producer.type);
                ASCIILiteral kindOfType = type.isAnything() ? "anything"_s : type.anyObject ? "any object"_s : (type.anyFunction || !type.functions.isEmpty()) ? "functions"_s : !type.classes.isEmpty() ? "built-in classes"_s : type.layouts ? "layouts"_s : "primitives"_s;
                uint32_t count = ++producersOfOperandsWithUnknownObject.add(makeString(part == TypedBodies ? "typed body, "_s : "rest, "_s, kindOfType, ", "_s, nameOf(producer.opcode)), 0).iterator->value;
                if (type.isAnything() && !(count % 300))
                    out.println("typed anything: ", nameOf(producer.opcode), " f", index, ":v", value.index(), " read at v", statement.site.index(), " position ", function.at(statement.site).position);
            }
            // One in every 400, so that they come from all over the program.
            if (part == TypedBodies && !(withUnknownObjectInPart[part] % 400))
                examplesInTypedBodies.append({ FunctionID(index), statement.site });
            if (std::optional<ValueFlowUnknownSource> source = operand.node ? solver.unknownSourceOf(operand.node) : std::nullopt)
                ++operandsOfSource[static_cast<size_t>(*source)];
            else
                ++withoutSource;

            Witness witness = birthOfNode.witnessOf(operand.node);
            if (!hasFunction(witness)) {
                ++withoutBirth;
                continue;
            }
            uint32_t place = placeOfBirth.ensure(static_cast<uint64_t>(witness.function) << 32 | witness.value, [&] {
                births.append({ witness, 0 });
                return static_cast<uint32_t>(births.size() - 1);
            }).iterator->value;
            ++births[place].operands;
        }
    }

    out.println("operations ", counted, " with the untracked object ", withUntrackedObject, " with the unknown object ", withUnknownObject, " from no operation ", withoutBirth);
    out.println("in typed bodies ", countedInPart[TypedBodies], " with the unknown object ", withUnknownObjectInPart[TypedBodies], "; in the rest of the program ", countedInPart[Program], " with ", withUnknownObjectInPart[Program], "; in its generic versions ", countedInPart[GenericVersions], " with ", withUnknownObjectInPart[GenericVersions], "; in the library ", countedInPart[Library], " with ", withUnknownObjectInPart[Library]);
    for (auto [function, site] : examplesInTypedBodies)
        out.println("in a typed body: f", function.index(), ":v", site.index());
    Vector<std::pair<String, uint32_t>> producers;
    for (auto& [name, count] : producersOfOperandsWithUnknownObject)
        producers.append({ name, count });
    AOT::sort(producers, [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : codePointCompareLessThan(a.first, b.first);
    });
    for (auto& [name, count] : producers)
        out.println("made by: ", count, " ", name);
    for (size_t source = 0; source < numberOfUnknownSources; ++source) {
        if (operandsOfSource[source])
            out.println(nameOf(static_cast<ValueFlowUnknownSource>(source)), ": ", operandsOfSource[source], " operands");
    }
    if (withoutSource)
        out.println("a seed of a body: ", withoutSource, " operands");

    AOT::stableSort(births, std::greater { }, &Birth::operands);
    out.println("births ", births.size());
    for (const Birth& birth : births.span().first(std::min(births.size(), numberOfBirthsPrinted))) {
        out.print(birth.operands, " <- f", birth.witness.function);
        if (birth.witness.value != Wire::none)
            out.print(" v", birth.witness.value, " ", program.functions[birth.witness.function].at(ValueID(birth.witness.value)).opcode);
        out.println();
    }
}

namespace {

// What arrives at a check, the worst first.
enum class ArrivalAtCheck : uint8_t { UnknownObject, UntrackedObject, SomeDoNotFit, AllFit, Nothing };
constexpr size_t numberOfArrivalsAtChecks = 5;

// A member of a set without a placeholder that is not sure to pass the check at which it arrives.
enum class StrangerAtCheck : uint8_t {
    OtherLayoutOfProgram,
    AllocationThatPicksItsLayout,
    InstanceOfBuiltinClass,
    OtherLayoutOfTable,
    ObjectWithoutLayout,
    BuiltinObject,
    Closure,
    UndefinedOrNull,
    Absent,
    OtherPrimitive,
};
constexpr size_t numberOfStrangersAtChecks = 10;
constexpr ASCIILiteral namesOfStrangersAtChecks[numberOfStrangersAtChecks] { "another-layout-of-the-program"_s, "allocation-that-picks-its-layout"_s, "instance-of-a-built-in-class"_s, "another-layout-of-the-table"_s, "object-without-a-layout"_s, "built-in-object"_s, "closure"_s, "undefined-or-null"_s, "absent"_s, "another-primitive"_s };

struct CountsOfChecks {
    std::array<uint32_t, numberOfArrivalsAtChecks> arrivals { };
    // A check counts once for each kind of stranger that arrives at it.
    std::array<uint32_t, numberOfStrangersAtChecks> strangers { };

    uint32_t total() const { return arrivals[0] + arrivals[1] + arrivals[2] + arrivals[3] + arrivals[4]; }
};

// Who enters a function that checks its parameters, the worst first. From PassUnknownObject on the solve has followed every caller, and the function counts by the worst of its parameters.
enum class CallersOfCheckedFunction : uint8_t { ClosureHasEscaped, ClosureIsLost, ClosureIsHeldByNativeCode, PassUnknownObject, PassUntrackedObject, PassStranger, PassWhatFits, PassNothing };
constexpr size_t numberOfCallersOfCheckedFunctions = 8;
constexpr ASCIILiteral namesOfCallersOfCheckedFunctions[numberOfCallersOfCheckedFunctions] {
    "the closure has escaped"_s,
    "the closure is lost"_s,
    "native code holds the closure"_s,
    "none of these, and the unknown object arrives"_s,
    "none of these, and the untracked object arrives"_s,
    "every set is closed, some member does not surely fit"_s,
    "every set is closed, all fit"_s,
    "nothing arrives"_s,
};

} // anonymous namespace

// checks: the checks of the reached functions, by what arrives at them: a placeholder; no placeholder and every member is sure to fit the type; no placeholder and some member is not.
static void printChecks(PrintStream& out, ValueFlowSolution& solution, BuildContext& build)
{
    constexpr size_t numberOfSiteKinds = numberOfEnumerators<TypeSiteKind>;
    // Behind the kinds of sites: no site, and the site of a parameter at a check that is not at the head of the function: a store to the parameter.
    constexpr size_t rowWithoutSite = numberOfSiteKinds;
    constexpr size_t rowOfStoresToParameters = numberOfSiteKinds + 1;
    constexpr size_t numberOfRows = numberOfSiteKinds + 2;
    constexpr size_t numberOfBirthsPrinted = 24;
    enum Part : uint8_t { OfProgram, OfGenericVersions, OfLibrary, NumberOfParts };
    constexpr ASCIILiteral namesOfParts[NumberOfParts] { "the program"_s, "its generic versions"_s, "the library"_s };

    const Program& program = solution.program();
    const ValueFlowBodies& bodies = solution.bodies();
    const ValueFlowUniverse& universe = bodies.universe();
    const ValueFlowSolver& solver = solution.solver();
    const ValueSets& sets = solution.sets();
    ValueFlowResultInputs inputs { program, solution, build };
    ValueFlowClassesOfBuiltinResults classesOfBuiltinResults = computeClassesOfBuiltinResults(inputs);
    inputs.classesOfBuiltinResults = &classesOfBuiltinResults;
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();
    ScratchTypeArena arena(program.types);
    ValueFlowPositionCounts positionCounts(nullptr);
    ValueFlowPositions positions(inputs, arena.arena(), positionCounts, 0, &maker);

    auto kindOfStranger = [&](ValueFlowObjectID member) {
        if (!universe.isObject(member)) {
            if (member == ValueFlowUniverse::primitive(PrimitiveSet::Undefined) || member == ValueFlowUniverse::primitive(PrimitiveSet::Null))
                return StrangerAtCheck::UndefinedOrNull;
            return member == ValueFlowUniverse::primitive(PrimitiveSet::Absent) ? StrangerAtCheck::Absent : StrangerAtCheck::OtherPrimitive;
        }
        if (universe.closureFunction(member))
            return StrangerAtCheck::Closure;
        if (universe.builtinOf(member))
            return StrangerAtCheck::BuiltinObject;
        if (sets.contains(universe.objectsThatPickTheirLayout(), member))
            return StrangerAtCheck::AllocationThatPicksItsLayout;
        if (sets.contains(universe.placeholders().objectsWithLayoutOfProgram, member))
            return StrangerAtCheck::OtherLayoutOfProgram;
        if (inputs.classOf(member))
            return StrangerAtCheck::InstanceOfBuiltinClass;
        return universe.layoutOf(member) ? StrangerAtCheck::OtherLayoutOfTable : StrangerAtCheck::ObjectWithoutLayout;
    };
    // One bit for each kind.
    auto strangersIn = [&](ValueSetID held, const Type& type) {
        uint32_t kinds = 0;
        sets.forEachMember(maker.intersect(held, ValueSets::complement(universe.membersSurelyInside(maker, program, type))), [&](ValueFlowObjectID member) {
            if (sets.contains(universe.objectsThatPickTheirLayout(), member) && positions.isSurelyInside(maker.of(member), type))
                return;
            kinds |= 1u << static_cast<unsigned>(kindOfStranger(member));
        });
        return kinds;
    };

    struct CheckedFunction {
        FunctionID function;
        ArrivalAtCheck worst { ArrivalAtCheck::Nothing };
        uint32_t checks { 0 };
        uint32_t strangers { 0 };
        // Of the first parameter at which the worst arrives.
        ValueFlowNodeID node;
    };
    Vector<CheckedFunction> checkedFunctions;
    std::array<std::array<CountsOfChecks, numberOfRows>, NumberOfParts> counts { };
    for (const ValueFlowObservedValue& observed : bodies.observedValues()) {
        if (!observed.test || !solver.isReached(observed.function))
            continue;
        const Function& function = program.at(observed.function);
        const Value& test = function.at(observed.test);
        if (test.opcode != Opcode::Check)
            continue;
        Part part = function.role == FunctionRole::GenericVersion ? OfGenericVersions : isWrittenInProgram(function) ? OfProgram : OfLibrary;
        const Type& type = program.at(test.castType());
        ValueSetID held = inputs.valueOf(maker, observed.operand);
        ArrivalAtCheck arrival = sets.contains(held, ValueFlowUniverse::unknownObject()) ? ArrivalAtCheck::UnknownObject
            : sets.contains(held, ValueFlowUniverse::untrackedObject()) ? ArrivalAtCheck::UntrackedObject
            : sets.isEmpty(held) ? ArrivalAtCheck::Nothing
            : positions.isSurelyInside(held, type) ? ArrivalAtCheck::AllFit : ArrivalAtCheck::SomeDoNotFit;
        uint32_t strangers = arrival == ArrivalAtCheck::SomeDoNotFit ? strangersIn(held, type) : 0;

        TypeSiteID site = test.site();
        bool checksParameter = site && program.at(site).kind == TypeSiteKind::Parameter;
        bool isAtHead = function.at(entryBlock).parameters.find(test.operand(0)) != notFound;
        size_t row = !site ? rowWithoutSite : checksParameter && !isAtHead ? rowOfStoresToParameters : static_cast<size_t>(program.at(site).kind);
        CountsOfChecks& countsOfRow = counts[part][row];
        ++countsOfRow.arrivals[static_cast<size_t>(arrival)];
        for (size_t kind = 0; kind < numberOfStrangersAtChecks; ++kind)
            countsOfRow.strangers[kind] += strangers >> kind & 1;

        if (part != OfProgram || !checksParameter || !isAtHead)
            continue;
        if (checkedFunctions.isEmpty() || checkedFunctions.last().function != observed.function)
            checkedFunctions.append({ observed.function, ArrivalAtCheck::Nothing, 0, 0, { } });
        CheckedFunction& checked = checkedFunctions.last();
        ++checked.checks;
        checked.strangers |= strangers;
        if (arrival < checked.worst) {
            checked.worst = arrival;
            checked.node = observed.operand.node;
        }
    }

    for (size_t part = 0; part < NumberOfParts; ++part) {
        CountsOfChecks all;
        for (const CountsOfChecks& row : counts[part]) {
            for (size_t arrival = 0; arrival < numberOfArrivalsAtChecks; ++arrival)
                all.arrivals[arrival] += row.arrivals[arrival];
        }
        out.println("checks in ", namesOfParts[part], ": with a placeholder ", all.arrivals[0] + all.arrivals[1], "; without one: all fit ", all.arrivals[3], ", some do not ", all.arrivals[2], ", nothing arrives ", all.arrivals[4]);
    }
    for (size_t part = 0; part < NumberOfParts; ++part) {
        out.println("checks in ", namesOfParts[part], " by the kind of site: all | the unknown object | the untracked object alone | closed, all fit | closed, some do not | nothing arrives");
        for (size_t row = 0; row < numberOfRows; ++row) {
            const CountsOfChecks& line = counts[part][row];
            if (!line.total())
                continue;
            out.print("  ", row == rowWithoutSite ? "no site"_s : row == rowOfStoresToParameters ? "Parameter, stored to in the body"_s : nameOf(static_cast<TypeSiteKind>(row)));
            out.println(": ", line.total(), " | ", line.arrivals[0], " | ", line.arrivals[1], " | ", line.arrivals[3], " | ", line.arrivals[2], " | ", line.arrivals[4]);
            if (!line.arrivals[2])
                continue;
            out.print("    what does not fit:");
            for (size_t kind = 0; kind < numberOfStrangersAtChecks; ++kind) {
                if (line.strangers[kind])
                    out.print(" ", namesOfStrangersAtChecks[kind], " ", line.strangers[kind]);
            }
            out.println();
        }
    }

    struct CountsOfFunctions {
        uint32_t functions { 0 };
        uint32_t checks { 0 };
    };
    std::array<CountsOfFunctions, numberOfCallersOfCheckedFunctions> byCallers { };
    std::array<uint32_t, numberOfStrangersAtChecks> functionsWithStranger { };
    std::array<uint32_t, numberOfUnknownSources> functionsOfSource { };
    uint32_t functionsWithoutSource = 0;
    uint32_t functionsWithoutBirth = 0;
    uint32_t numberOfChecks = 0;
    struct Birth {
        Witness witness;
        uint32_t functions { 0 };
    };
    Vector<Birth> births;
    HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> placeOfBirth;
    ValueFlowBirthsOfUnknownObject birthOfNode(inputs);
    BitVector isClosureThatHasEscaped(universe.numberOfObjects());
    for (const CheckedFunction& checked : checkedFunctions) {
        ValueFlowObjectID closure = universe.closureOf(checked.function);
        CallersOfCheckedFunction callers = static_cast<CallersOfCheckedFunction>(static_cast<size_t>(CallersOfCheckedFunction::PassUnknownObject) + static_cast<size_t>(checked.worst));
        if (inputs.hasEscaped(closure))
            callers = CallersOfCheckedFunction::ClosureHasEscaped;
        else if (inputs.isLost(closure))
            callers = CallersOfCheckedFunction::ClosureIsLost;
        else if (inputs.isHeldByNativeCode(closure))
            callers = CallersOfCheckedFunction::ClosureIsHeldByNativeCode;
        ++byCallers[static_cast<size_t>(callers)].functions;
        byCallers[static_cast<size_t>(callers)].checks += checked.checks;
        numberOfChecks += checked.checks;
        switch (callers) {
        case CallersOfCheckedFunction::ClosureHasEscaped:
            isClosureThatHasEscaped.quickSet(closure.index());
            break;
        case CallersOfCheckedFunction::PassStranger:
            for (size_t kind = 0; kind < numberOfStrangersAtChecks; ++kind)
                functionsWithStranger[kind] += checked.strangers >> kind & 1;
            break;
        case CallersOfCheckedFunction::PassUnknownObject: {
            if (std::optional<ValueFlowUnknownSource> source = checked.node ? solver.unknownSourceOf(checked.node) : std::nullopt)
                ++functionsOfSource[static_cast<size_t>(*source)];
            else
                ++functionsWithoutSource;
            Witness witness = birthOfNode.witnessOf(checked.node);
            if (!hasFunction(witness)) {
                ++functionsWithoutBirth;
                break;
            }
            uint32_t place = placeOfBirth.ensure(static_cast<uint64_t>(witness.function) << 32 | witness.value, [&] {
                births.append({ witness, 0 });
                return static_cast<uint32_t>(births.size() - 1);
            }).iterator->value;
            ++births[place].functions;
            break;
        }
        default:
            break;
        }
    }

    out.println("functions of the program with a check of a parameter at their head: ", checkedFunctions.size(), " with ", numberOfChecks, " such checks");
    for (size_t callers = 0; callers < numberOfCallersOfCheckedFunctions; ++callers)
        out.println("  ", namesOfCallersOfCheckedFunctions[callers], ": ", byCallers[callers].functions, " functions, ", byCallers[callers].checks, " checks");
    out.print("what does not fit, by function, once for each kind:");
    for (size_t kind = 0; kind < numberOfStrangersAtChecks; ++kind) {
        if (functionsWithStranger[kind])
            out.print(" ", namesOfStrangersAtChecks[kind], " ", functionsWithStranger[kind]);
    }
    out.println();

    out.println("where the unknown object that a known caller passes was born, by function:");
    for (size_t source = 0; source < numberOfUnknownSources; ++source) {
        if (functionsOfSource[source])
            out.println("  ", nameOf(static_cast<ValueFlowUnknownSource>(source)), ": ", functionsOfSource[source]);
    }
    if (functionsWithoutSource)
        out.println("  a seed of a body: ", functionsWithoutSource);
    AOT::stableSort(births, std::greater { }, &Birth::functions);
    out.println("  births ", births.size(), ", from no operation ", functionsWithoutBirth);
    for (const Birth& birth : births.span().first(std::min(births.size(), numberOfBirthsPrinted))) {
        out.print("  ", birth.functions, " <- f", birth.witness.function);
        if (birth.witness.value != Wire::none)
            out.print(" v", birth.witness.value, " ", program.functions[birth.witness.function].at(ValueID(birth.witness.value)).opcode);
        out.println();
    }

    WaysOfEscape ways(solution);
    auto isCounted = [&](ValueFlowObjectID object) {
        return isClosureThatHasEscaped.quickGet(object.index());
    };
    out.println("who has let the closures of these functions escape:");
    printWhoLetsEscape(out, solution, ways, EscapeIsCountedFor::ObjectItself, isCounted);
    out.println("who has let escape what holds them in the end:");
    printWhoLetsEscape(out, solution, ways, EscapeIsCountedFor::Root, isCounted);
}

// In words that somebody can find in the program: the function and what of it the node holds, the variable, or the field.
static void printPlaceOfNode(PrintStream& out, ValueFlowSolution& solution, ValueFlowNodeID node)
{
    const Program& program = solution.program();
    const ValueFlowProgramNodes& nodes = solution.bodies().nodes();
    auto printText = [&](StringID text) {
        if (text)
            out.print(" \"", String::fromUTF8(program.string(text)), "\"");
    };
    printNode(out, solution, node);
    if (solution.solver().keyOfMadeNode(node) || node.index() < 2)
        return;
    if (FunctionID id = nodes.functionOf(node); id && id.index() < program.functions.size()) {
        const Function& function = program.at(id);
        printText(function.name);
        uint32_t offset = node.index() - nodes.firstNodeOf(id);
        if (offset < function.numberOfEntryParameters())
            out.print(" entry parameter ", offset);
        else if (node == nodes.thrown(id))
            out.print(" thrown");
        else if (node.index() < nodes.thrown(id).index())
            out.print(" result");
        if (function.module)
            printText(program.at(function.module).displayPath);
        out.print(" @", function.position);
        return;
    }
    uint32_t firstVariable = nodes.variable(VariableID(0u)).index();
    if (node.index() >= firstVariable && node.index() - firstVariable < program.variables.size()) {
        out.print(" g", node.index() - firstVariable);
        printText(program.variables[node.index() - firstVariable].name);
        return;
    }
    for (uint32_t layout = 0; layout < program.layouts.size(); ++layout) {
        uint32_t firstField = nodes.field(LayoutID(layout), 0).index();
        if (node.index() < firstField || node.index() - firstField >= program.layouts[layout].fields.size())
            continue;
        out.print(" L", layout, ".", node.index() - firstField);
        printText(program.at(program.layouts[layout].fields[node.index() - firstField].key).text);
        return;
    }
}

// choke-points: the dominators of the graph along which the unknown object flows.
static void printChokePoints(PrintStream& out, ValueFlowSolution& solution, BuildContext& build)
{
    constexpr size_t numberOfChildrenPrinted = 40;
    constexpr unsigned numberOfStepsPrinted = 8;
    constexpr uint32_t none = UINT32_MAX;
    const Program& program = solution.program();
    const ValueFlowBodies& bodies = solution.bodies();
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowGraph& graph = solver.graph();
    const ValueSets& sets = solution.sets();
    ValueFlowResultInputs inputs { program, solution, build };
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();
    uint32_t numberOfNodes = graph.numberOfNodes();
    uint32_t root = numberOfNodes;

    BitVector carries(numberOfNodes);
    BitVector isConstant(numberOfNodes);
    for (uint32_t index = 0; index < numberOfNodes; ++index) {
        if (!sets.contains(solver.valueOf(ValueFlowNodeID(index)), ValueFlowUniverse::unknownObject()))
            continue;
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(ValueFlowNodeID(index));
        if (key && key->kind == ValueFlowMadeNodeKind::Constant)
            isConstant.quickSet(index);
        else
            carries.quickSet(index);
    }
    // A node that carries the object and gets it from no node that does has it from the start.
    BitVector isBirth(numberOfNodes);
    Vector<uint32_t> births;
    for (uint32_t index : carries) {
        bool hasCarryingPredecessor = false;
        bool hasConstantPredecessor = false;
        graph.forEachPredecessor(ValueFlowNodeID(index), [&](ValueFlowNodeID predecessor) {
            hasCarryingPredecessor |= carries.quickGet(predecessor.index());
            hasConstantPredecessor |= isConstant.quickGet(predecessor.index());
        });
        if (hasConstantPredecessor || !hasCarryingPredecessor) {
            isBirth.quickSet(index);
            births.append(index);
        }
    }

    // The edges between nodes that carry the object, by the node at which they start.
    Vector<uint32_t> firstSuccessorOf(FillWith { }, numberOfNodes + 2, 0u);
    for (uint32_t index : carries) {
        graph.forEachPredecessor(ValueFlowNodeID(index), [&](ValueFlowNodeID predecessor) {
            if (carries.quickGet(predecessor.index()))
                ++firstSuccessorOf[predecessor.index() + 2];
        });
    }
    for (uint32_t index = 2; index < firstSuccessorOf.size(); ++index)
        firstSuccessorOf[index] += firstSuccessorOf[index - 1];
    Vector<uint32_t> successors(FillWith { }, firstSuccessorOf.last(), 0u);
    for (uint32_t index : carries) {
        graph.forEachPredecessor(ValueFlowNodeID(index), [&](ValueFlowNodeID predecessor) {
            if (carries.quickGet(predecessor.index()))
                successors[firstSuccessorOf[predecessor.index() + 1]++] = index;
        });
    }

    // Postorder from the root.
    Vector<uint32_t> postorder;
    Vector<uint32_t> numberInPostorder(FillWith { }, numberOfNodes + 1, none);
    {
        struct Frame {
            uint32_t node;
            uint32_t next;
        };
        BitVector isSeen(numberOfNodes);
        Vector<Frame> stack;
        for (uint32_t birth : births) {
            if (isSeen.quickSet(birth))
                continue;
            stack.append({ birth, firstSuccessorOf[birth] });
            while (!stack.isEmpty()) {
                Frame& frame = stack.last();
                if (frame.next == firstSuccessorOf[frame.node + 1]) {
                    numberInPostorder[frame.node] = postorder.size();
                    postorder.append(frame.node);
                    stack.removeLast();
                    continue;
                }
                uint32_t successor = successors[frame.next++];
                if (!isSeen.quickSet(successor))
                    stack.append({ successor, firstSuccessorOf[successor] });
            }
        }
        numberInPostorder[root] = postorder.size();
        postorder.append(root);
    }

    // Cooper, Harvey and Kennedy: a simple, fast dominance algorithm.
    Vector<uint32_t> dominatorOf(FillWith { }, numberOfNodes + 1, none);
    dominatorOf[root] = root;
    auto meet = [&](uint32_t first, uint32_t second) {
        while (first != second) {
            while (numberInPostorder[first] < numberInPostorder[second])
                first = dominatorOf[first];
            while (numberInPostorder[second] < numberInPostorder[first])
                second = dominatorOf[second];
        }
        return first;
    };
    unsigned sweeps = 0;
    for (bool hasChanged = true; hasChanged; ++sweeps) {
        hasChanged = false;
        for (size_t position = postorder.size() - 1; position--;) {
            uint32_t node = postorder[position];
            uint32_t dominator = isBirth.quickGet(node) ? root : none;
            if (dominator == none) {
                graph.forEachPredecessor(ValueFlowNodeID(node), [&](ValueFlowNodeID predecessor) {
                    if (!carries.quickGet(predecessor.index()) || dominatorOf[predecessor.index()] == none)
                        return;
                    dominator = dominator == none ? predecessor.index() : meet(predecessor.index(), dominator);
                });
            }
            if (dominator != none && dominatorOf[node] != dominator) {
                dominatorOf[node] = dominator;
                hasChanged = true;
            }
        }
    }

    struct Weight {
        uint32_t checks { 0 };
        uint32_t accesses { 0 };
    };
    Vector<Weight> weightBelow(numberOfNodes + 1);
    uint32_t withoutNode = 0;
    auto count = [&](const ValueFlowOperand& operand, uint32_t Weight::* weight) {
        if (!sets.contains(inputs.valueOf(maker, operand), ValueFlowUniverse::unknownObject()))
            return;
        if (!operand.node || dominatorOf[operand.node.index()] == none) {
            ++withoutNode;
            return;
        }
        ++(weightBelow[operand.node.index()].*weight);
    };
    auto isCounted = [&](FunctionID id) {
        const Function& function = program.at(id);
        return solver.isReached(id) && isWrittenInProgram(function) && function.role != FunctionRole::GenericVersion;
    };
    for (const ValueFlowObservedValue& observed : bodies.observedValues()) {
        if (observed.test && isCounted(observed.function) && program.at(observed.function).at(observed.test).opcode == Opcode::Check)
            count(observed.operand, &Weight::checks);
    }
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!isCounted(FunctionID(index)))
            continue;
        for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || !statement.operandCount)
                continue;
            unsigned operandIndex = receiverOf(statement.opcode).value_or(0);
            if ((PayloadWord::receivers(statement.opcode) || PayloadWord::callees(statement.opcode)) && operandIndex < statement.operandCount)
                count(bodies.operandsOf(statement)[operandIndex], &Weight::accesses);
        }
    }
    for (uint32_t node : postorder) {
        if (node == root)
            continue;
        weightBelow[dominatorOf[node]].checks += weightBelow[node].checks;
        weightBelow[dominatorOf[node]].accesses += weightBelow[node].accesses;
    }

    // The children of each node that is printed: the tree has no lists of children.
    Vector<uint32_t> heaviestChildOf(FillWith { }, numberOfNodes + 1, none);
    Vector<uint32_t> childrenOfRoot;
    auto weighs = [&](uint32_t node) {
        return static_cast<uint64_t>(weightBelow[node].checks) + weightBelow[node].accesses;
    };
    for (uint32_t node : postorder) {
        if (node == root)
            continue;
        uint32_t dominator = dominatorOf[node];
        if (dominator == root)
            childrenOfRoot.append(node);
        if (heaviestChildOf[dominator] == none || weighs(node) > weighs(heaviestChildOf[dominator]))
            heaviestChildOf[dominator] = node;
    }
    AOT::sort(childrenOfRoot, [&](uint32_t first, uint32_t second) {
        return std::pair { weighs(second), first } < std::pair { weighs(first), second };
    });

    uint32_t birthsAmongChildren = 0;
    uint32_t childrenWithWeight = 0;
    for (uint32_t child : childrenOfRoot) {
        birthsAmongChildren += isBirth.quickGet(child);
        childrenWithWeight += !!weighs(child);
    }
    out.println("nodes that carry the unknown object ", carries.bitCount(), ", births ", births.size(), ", sweeps ", sweeps);
    out.println("checks of the program with it ", weightBelow[root].checks, ", receivers and callees with it ", weightBelow[root].accesses, ", at no node ", withoutNode);
    out.println("children of the root ", childrenOfRoot.size(), ", births among them ", birthsAmongChildren, ", with a check or an access below them ", childrenWithWeight);

    // The births by the source that the rule has named and by the kind of the statement that has logged the edge, with what hangs below these births alone.
    struct BirthLine {
        String name;
        uint32_t births { 0 };
        Weight weight;
    };
    Vector<BirthLine> birthLines;
    for (uint32_t birth : births) {
        StringBuilder name;
        graph.forEachPredecessor(ValueFlowNodeID(birth), [&](ValueFlowNodeID predecessor) {
            if (!isConstant.quickGet(predecessor.index()) || !name.isEmpty())
                return;
            std::optional<ValueFlowUnknownSource> source = solver.unknownSourceOf(ValueFlowEdge { predecessor, ValueFlowNodeID(birth) });
            name.append(source ? nameOf(*source) : "no source"_s, " by "_s);
            if (ValueFlowStatementID logger = solver.statementThatLogged({ predecessor, ValueFlowNodeID(birth) })) {
                const ValueFlowStatement& statement = solver.statementAt(logger);
                name.append(statement.kind == ValueFlowStatementKind::Operation ? nameOf(statement.opcode) : nameOf(statement.kind));
            } else
                name.append("no statement"_s);
        });
        if (name.isEmpty())
            name.append("from the start"_s);
        String line = name.toString();
        size_t place = birthLines.findIf([&](const BirthLine& other) {
            return other.name == line;
        });
        if (place == notFound) {
            place = birthLines.size();
            birthLines.append({ line, 0, { } });
        }
        ++birthLines[place].births;
        birthLines[place].weight.checks += weightBelow[birth].checks;
        birthLines[place].weight.accesses += weightBelow[birth].accesses;
    }
    AOT::sort(birthLines, std::greater { }, &BirthLine::births);
    for (const BirthLine& line : birthLines)
        out.println("  births: ", line.births, " ", line.name, "; below them alone ", line.weight.checks, " checks ", line.weight.accesses, " accesses");
    auto printLine = [&](uint32_t node) {
        out.print(weightBelow[node].checks, " checks ", weightBelow[node].accesses, " accesses below ");
        printPlaceOfNode(out, solution, ValueFlowNodeID(node));
        if (!isBirth.quickGet(node)) {
            out.println();
            return;
        }
        out.print("   BIRTH");
        graph.forEachPredecessor(ValueFlowNodeID(node), [&](ValueFlowNodeID predecessor) {
            if (!isConstant.quickGet(predecessor.index()))
                return;
            if (std::optional<ValueFlowUnknownSource> source = solver.unknownSourceOf(ValueFlowEdge { predecessor, ValueFlowNodeID(node) }))
                out.print(" ", nameOf(*source));
            if (ValueFlowStatementID statement = solver.statementThatLogged({ predecessor, ValueFlowNodeID(node) })) {
                out.print(" by ");
                printStatement(out, solver, statement);
            }
        });
        out.println();
    };
    for (uint32_t child : childrenOfRoot.span().first(std::min(childrenOfRoot.size(), numberOfChildrenPrinted))) {
        printLine(child);
        // Down the heaviest children while they keep most of the weight: where the way fans out.
        uint32_t node = child;
        for (unsigned step = 0; step < numberOfStepsPrinted; ++step) {
            uint32_t next = heaviestChildOf[node];
            if (next == none || weighs(next) * 10 < weighs(child) * 5 || weighs(child) < 200)
                break;
            out.print("    ");
            printLine(next);
            node = next;
        }
    }
}

// leaving: the reached functions that test their arguments and the reached typed bodies, by the worst of what their tests, checks and reads turn away.
static void printWhoLeavesTypedCode(PrintStream& out, ValueFlowSolution& solution)
{
    const Program& program = solution.program();
    const ValueFlowBodies& bodies = solution.bodies();
    const ValueFlowUniverse& universe = bodies.universe();
    ValueSetID escaped = solution.solver().valueOf(ValueFlowProgramNodes::escaped());
    struct Counts {
        uint32_t nothing { 0 };
        uint32_t primitivesAlone { 0 };
        uint32_t knownObjects { 0 };
        uint32_t placeholderAndHasEscaped { 0 };
        uint32_t placeholderFromKnownCode { 0 };
    };
    Counts ofTests;
    Counts ofTypedBodies;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!solution.solver().isReached(FunctionID(index)))
            continue;
        for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::EntryChecks)
                continue;
            bool turnsAwayPrimitive = false;
            bool turnsAwayKnownObject = false;
            bool turnsAwayPlaceholder = false;
            for (const ValueFlowOperand& turnedAway : bodies.operandsOf(statement)) {
                solution.sets().forEachMember(solution.valueOf(turnedAway), [&](ValueFlowObjectID member) {
                    if (member == ValueFlowUniverse::unknownObject() || member == ValueFlowUniverse::untrackedObject())
                        turnsAwayPlaceholder = true;
                    else if (universe.isObject(member))
                        turnsAwayKnownObject = true;
                    else
                        turnsAwayPrimitive = true;
                });
            }
            bool isTests = program.functions[index].role == FunctionRole::EntryChecks;
            Counts& counts = isTests ? ofTests : ofTypedBodies;
            if (turnsAwayPlaceholder)
                ++(isTests && solution.sets().contains(escaped, universe.closureOf(FunctionID(index))) ? counts.placeholderAndHasEscaped : counts.placeholderFromKnownCode);
            else if (turnsAwayKnownObject)
                ++counts.knownObjects;
            else if (turnsAwayPrimitive)
                ++counts.primitivesAlone;
            else
                ++counts.nothing;
        }
    }
    auto print = [&](ASCIILiteral name, const Counts& counts) {
        out.println(name, ": turn away nothing ", counts.nothing, "; primitives alone ", counts.primitivesAlone, "; known objects ", counts.knownObjects, "; a placeholder, and the function has escaped ", counts.placeholderAndHasEscaped, "; a placeholder from known code ", counts.placeholderFromKnownCode);
    };
    print("functions that test their arguments"_s, ofTests);
    print("typed bodies"_s, ofTypedBodies);
}

// receiver-layouts: the reads and stores of named properties in the reached functions of the program but for its generic versions, by what their receiver may be.
static void printLayoutsOfReceivers(PrintStream& out, ValueFlowSolution& solution)
{
    const Program& program = solution.program();
    const ValueFlowBodies& bodies = solution.bodies();
    const ValueFlowUniverse& universe = bodies.universe();
    uint32_t withPlaceholder = 0;
    uint32_t withoutObject = 0;
    uint32_t withObjectWithoutLayout = 0;
    uint32_t onlyBuiltinObjects = 0;
    std::array<uint32_t, 6> byNumberOfLayouts { };
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!solution.solver().isReached(FunctionID(index)) || !isWrittenInProgram(function) || function.role == FunctionRole::GenericVersion)
            continue;
        for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || (statement.opcode != Opcode::PropGet && statement.opcode != Opcode::PropSet))
                continue;
            ValueSetID held = solution.valueOf(bodies.operandsOf(statement)[0]);
            if (solution.sets().contains(held, ValueFlowUniverse::unknownObject()) || solution.sets().contains(held, ValueFlowUniverse::untrackedObject())) {
                ++withPlaceholder;
                continue;
            }
            Vector<LayoutID, 8> layouts;
            bool holdsObject = false;
            bool holdsObjectWithoutLayout = false;
            bool holdsOtherThanBuiltin = false;
            solution.sets().forEachMember(held, [&](ValueFlowObjectID member) {
                if (!universe.isObject(member))
                    return;
                holdsObject = true;
                if (universe.builtinOf(member))
                    return;
                holdsOtherThanBuiltin = true;
                if (LayoutID layout = universe.layoutOf(member)) {
                    if (!layouts.contains(layout))
                        layouts.append(layout);
                } else
                    holdsObjectWithoutLayout = true;
            });
            if (!holdsObject)
                ++withoutObject;
            else if (!holdsOtherThanBuiltin)
                ++onlyBuiltinObjects;
            else if (holdsObjectWithoutLayout)
                ++withObjectWithoutLayout;
            else
                ++byNumberOfLayouts[layouts.size() <= 4 ? layouts.size() : 5];
        }
    }
    out.println("receivers of named reads and stores: with a placeholder ", withPlaceholder, "; no object ", withoutObject, "; built-in objects alone ", onlyBuiltinObjects, "; some object without a layout ", withObjectWithoutLayout,
        "; layouts: one ", byNumberOfLayouts[1], ", two ", byNumberOfLayouts[2], ", three ", byNumberOfLayouts[3], ", four ", byNumberOfLayouts[4], ", more ", byNumberOfLayouts[5]);
}

static void printMembersByName(PrintStream& out, ValueFlowSolution& solution, ValueSetID set, unsigned numberOfMembersPrinted = 6)
{
    const ValueFlowUniverse& universe = solution.bodies().universe();
    if (!set || ValueSets::isComplement(set))
        return;
    unsigned printed = 0;
    solution.sets().forEachMember(set, [&](ValueFlowObjectID member) {
        if (++printed > numberOfMembersPrinted)
            return;
        if (member == ValueFlowUniverse::unknownObject())
            out.print(" unknown");
        else if (member == ValueFlowUniverse::untrackedObject())
            out.print(" untracked");
        else if (FunctionID function = universe.closureFunction(member))
            out.print(" f", function.index());
        else if (BuiltinID builtin = universe.builtinOf(member))
            out.print(" ", solution.program().builtins().tokenOf(builtin));
        else if (FunctionID function = universe.functionOf(member))
            out.print(" object", member.index(), "@f", function.index());
        else
            out.print(" #", member.index());
    });
    if (printed > numberOfMembersPrinted)
        out.print(" ..");
}

// What a statement got from read(), written(), lookup(), group(), hub() and masked(), each with what the node holds, and the same for the statements that were made with those.
static void printWhatIsAskedFor(PrintStream& out, ValueFlowSolution& solution, ValueFlowStatementID id, unsigned depth)
{
    constexpr unsigned deepestLevel = 3;
    const ValueFlowSolver& solver = solution.solver();
    solver.forEachMadeNodeAskedForBy(id, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made, ValueFlowNodeID first) {
        for (unsigned level = 0; level <= depth; ++level)
            out.print("  ");
        printKey(out, solver, key);
        out.print(" of");
        printMembersByName(out, solution, key.set);
        ValueSetID value = solver.valueOf(first);
        out.print(" holds ", ValueSets::isComplement(value) ? 0 : solver.sets().size(value), ":");
        printMembersByName(out, solution, value);
        out.println();
        if (made && made != id && depth < deepestLevel)
            printWhatIsAskedFor(out, solution, made, depth + 1);
    });
}

// The statements that have brought the unknown object into being, or would have if their source were not left out: in the order of the rounds, each once for each source, with what the statement was made for by name.
static void printSeeds(PrintStream& out, ValueFlowSolution& solution)
{
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowUniverse& universe = solution.bodies().universe();
    const Program& program = solution.program();
    auto printNameOfKey = [&](PropertyKeyID id) {
        const PropertyKey& key = program.at(id);
        if (key.text)
            out.print(" name ", String::fromUTF8(program.string(key.text)));
        else
            out.print(" key", id.index());
    };
    auto printWithNames = [&](ValueFlowStatementID id) {
        if (!id) {
            out.print("no statement");
            return;
        }
        printStatement(out, solver, id);
        const ValueFlowStatement& statement = solver.statementAt(id);
        if (statement.kind == ValueFlowStatementKind::Operation && statement.operandCount) {
            out.print(" on");
            printMembersByName(out, solution, solution.valueOf(solution.bodies().operandsOf(statement)[0]));
            return;
        }
        if (!solver.isMadeByRule(id) || statement.kind == ValueFlowStatementKind::Escape)
            return;
        const ValueFlowNodeKey& key = solver.keyOf(id);
        if (key.kind == ValueFlowMadeNodeKind::Lookup && key.name().kind == ValueFlowNameKind::Property)
            printNameOfKey(PropertyKeyID(key.payload));
        out.print(" of");
        printMembersByName(out, solution, key.set);
    };

    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> seen;
    std::array<uint32_t, numberOfUnknownSources> statementsOfSource { };
    for (const ValueFlowSolver::LoggedEdge& logged : solver.edgesInOrderLogged()) {
        std::optional<ValueFlowUnknownSource> source = solver.unknownSourceOf(logged.edge);
        if (!source || !seen.add(static_cast<uint64_t>(logged.statement.index()) << 8 | static_cast<uint8_t>(*source)).isNewEntry)
            continue;
        ++statementsOfSource[static_cast<size_t>(*source)];
        out.print("seed round ", logged.round, " ", nameOf(*source), " by ");
        printWithNames(logged.statement);
        out.println();
    }
    for (size_t source = 0; source < numberOfUnknownSources; ++source) {
        if (statementsOfSource[source])
            out.println("seeds of ", nameOf(static_cast<ValueFlowUnknownSource>(source)), ": ", statementsOfSource[source], " statements");
    }

    const ValueFlowBodies& bodies = solution.bodies();
    uint32_t seedsOfBodies = 0;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!solver.isReached(FunctionID(index)))
            continue;
        const ValueFlowBodyRange& range = bodies.ranges()[index];
        for (const ValueFlowNodeSet& seed : bodies.seedsOnceReached().subspan(range.seedOnceReached, bodies.ranges()[index + 1].seedOnceReached - range.seedOnceReached)) {
            if (!solution.sets().contains(seed.set, ValueFlowUniverse::unknownObject()))
                continue;
            ++seedsOfBodies;
            out.print("seed of body f", index, " into ");
            printNode(out, solution, seed.node);
            for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
                if (statement.result == seed.node)
                    out.print(" ", statement.opcode, " v", statement.site.index());
            }
            out.println();
        }
    }
    out.println("seeds of bodies: ", seedsOfBodies);

    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    uint32_t closures = 0;
    solver.sets().forEachMember(solver.valueOf(escaped), [&](ValueFlowObjectID object) {
        FunctionID function = universe.closureFunction(object);
        if (!function)
            return;
        ++closures;
        Vector<ValueFlowNodeID> way = solver.explain(escaped, object);
        out.print("escaped f", function.index(), " by ");
        printWithNames(way.size() > 1 ? solver.statementThatLogged({ way[1], way[0] }) : ValueFlowStatementID());
        out.println();
    });
    out.println("escaped closures: ", closures);
}

// The nodes of which the functor says yes: how many there are, then the first of them.
static void printNodesWhere(PrintStream& out, ValueFlowSolution& solution, ASCIILiteral title, NOESCAPE const Invocable<bool(ValueFlowNodeID)> auto& functor)
{
    constexpr uint32_t numberOfNodesPrinted = 40;
    uint32_t found = 0;
    for (uint32_t index = 0; index < solution.solver().graph().numberOfNodes(); ++index)
        found += functor(ValueFlowNodeID(index));
    out.println(title, " ", found);

    uint32_t printed = 0;
    for (uint32_t index = 0; index < solution.solver().graph().numberOfNodes() && printed < numberOfNodesPrinted; ++index) {
        if (!functor(ValueFlowNodeID(index)))
            continue;
        ++printed;
        out.print("  ");
        printNode(out, solution, ValueFlowNodeID(index));
        out.println();
    }
}

// What the operands of the operation hold, in the order of the operands, and what its result holds.
static void printValuesAt(PrintStream& out, ValueFlowSolution& solution, const ValueFlowStatement& statement)
{
    out.println("  ", statement.opcode, solution.solver().isReached(statement.function) ? "" : " in a function that is not reached");
    unsigned index = 0;
    for (const ValueFlowOperand& operand : solution.bodies().operandsOf(statement)) {
        out.print("  operand ", index++, " ");
        if (operand.node)
            out.print("n", operand.node.index(), " ");
        printValue(out, solution, solution.valueOf(operand));
    }
    if (statement.result) {
        out.print("  result n", statement.result.index(), " ");
        printValue(out, solution, solution.solver().valueOf(statement.result));
    }
}

static void printFalsifiers(PrintStream& out, ValueFlowSolution& solution, std::span<const LayoutID> layouts)
{
    const Program& program = solution.program();
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowBodies& bodies = solution.bodies();
    const ValueFlowUniverse& universe = bodies.universe();
    const ValueSets& sets = solution.sets();
    BorrowedValueSetMaker borrowedMaker(solution.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();
    ValueSetID escaped = solver.valueOf(ValueFlowProgramNodes::escaped());
    ValueSetID lost = solver.valueOf(ValueFlowProgramNodes::lost());

    auto nameOfKey = [&](PropertyKeyID id) -> String {
        const PropertyKey& key = program.at(id);
        return key.text ? String::fromUTF8(program.string(key.text)) : makeString("key"_s, id.index());
    };
    auto whatIsDone = [&](ValueFlowNameKind kind) -> ASCIILiteral {
        switch (kind) {
        case ValueFlowNameKind::Property:
            return "store"_s;
        case ValueFlowNameKind::UnknownKey:
            return "store-under-unknown-key"_s;
        case ValueFlowNameKind::Getter:
            return "getter"_s;
        case ValueFlowNameKind::Setter:
            return "setter"_s;
        case ValueFlowNameKind::GetterUnderUnknownKey:
            return "getter-under-unknown-key"_s;
        case ValueFlowNameKind::SetterUnderUnknownKey:
            return "setter-under-unknown-key"_s;
        case ValueFlowNameKind::DeletedMember:
            return "delete"_s;
        case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
            return "delete-under-unknown-key"_s;
        case ValueFlowNameKind::RedefinedMember:
            return "redefine"_s;
        case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
            return "redefine-under-unknown-key"_s;
        case ValueFlowNameKind::CopiedFrom:
            return "copy-onto"_s;
        case ValueFlowNameKind::PrototypeLink:
            return "prototype-link"_s;
        case ValueFlowNameKind::InternalFields:
            return "internal-fields"_s;
        default:
            return { };
        }
    };
    auto hasName = [](ValueFlowNameKind kind) {
        return kind == ValueFlowNameKind::Property || kind == ValueFlowNameKind::Getter || kind == ValueFlowNameKind::Setter || kind == ValueFlowNameKind::DeletedMember || kind == ValueFlowNameKind::RedefinedMember;
    };
    constexpr ASCIILiteral namesOfPrimitives[ValueFlowUniverse::numberOfPrimitiveKinds] { "undefined"_s, "null"_s, "false"_s, "true"_s, "integer"_s, "whole"_s, "fraction"_s, "string"_s, "symbol"_s, "bigint"_s, "absent"_s, "literal-string"_s, "literal-symbol"_s, "p13"_s, "p14"_s, "p15"_s };
    auto printBrought = [&](ValueSetID value) {
        if (ValueSets::isComplement(value)) {
            out.print("anything");
            return;
        }
        Vector<LayoutID, 8> layoutsBrought;
        unsigned keys = 0;
        unsigned closures = 0;
        unsigned builtins = 0;
        unsigned others = 0;
        bool isFirst = true;
        auto word = [&](auto... parts) {
            out.print(isFirst ? "" : " ", parts...);
            isFirst = false;
        };
        sets.forEachMember(value, [&](ValueFlowObjectID member) {
            if (member.index() < ValueFlowUniverse::numberOfPrimitiveKinds)
                word(namesOfPrimitives[member.index()]);
            else if (member == ValueFlowUniverse::unknownObject())
                word("UNKNOWN");
            else if (member == ValueFlowUniverse::untrackedObject())
                word("UNTRACKED");
            else if (universe.keyOf(member))
                ++keys;
            else if (universe.closureFunction(member))
                ++closures;
            else if (universe.builtinOf(member))
                ++builtins;
            else if (LayoutID layout = universe.layoutOf(member)) {
                if (!layoutsBrought.contains(layout))
                    layoutsBrought.append(layout);
            } else
                ++others;
        });
        if (keys)
            word("keys:", keys);
        if (closures)
            word("closures:", closures);
        if (builtins)
            word("built-ins:", builtins);
        if (others)
            word("other-objects:", others);
        for (LayoutID layout : layoutsBrought.span().first(std::min<size_t>(layoutsBrought.size(), 6)))
            word("L", layout.index());
        if (layoutsBrought.size() > 6)
            word("+", layoutsBrought.size() - 6, "layouts");
        if (isFirst)
            out.print("nothing");
    };

    struct Family {
        LayoutID layout;
        ValueSetID objects;
        bool hasEscaped;
        bool isLost;
    };
    Vector<Family> families;
    for (LayoutID layout : layouts) {
        ValueSetID objects = maker.intersect(universe.objectsWith(layout), universe.knownObjects());
        families.append({ layout, objects, sets.meet(objects, escaped), sets.meet(objects, lost) });
        out.println("family L", layout.index(), " objects ", sets.size(objects), " escaped ", sets.size(maker.intersect(objects, escaped)), " lost ", sets.size(maker.intersect(objects, lost)));
    }

    constexpr OperandUses usesThatChange { OperandUse::WritesContents, OperandUse::MovesElements, OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
        OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype };
    const BuiltinTable& builtins = program.builtins();
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID builtin(index);
        OptionSet<BuiltinUse> usesBySolve = solver.usesOf(builtin);
        if (usesBySolve.isEmpty() || builtins.canonicalID(builtin) != builtin)
            continue;
        for (const BuiltinOverload& overload : builtins.overloadsOf(builtin)) {
            auto printParameter = [&](const BuiltinParameter& parameter) {
                OperandUses changes = parameter.uses & usesThatChange;
                if (changes.isEmpty())
                    return;
                out.print("B\t", builtins.tokenOf(builtin), "\t");
                for (BuiltinUse use : usesBySolve)
                    out.print(nameOf(use), " ");
                out.print("\t", sets.contains(escaped, universe.objectOf(builtin)) ? "escaped" : "held", "\t", overload.isUndeclared() ? "undeclared" : "declared", "\t");
                if (parameter.operand.isReceiver())
                    out.print("receiver");
                else
                    out.print("argument", parameter.operand.argumentIndex());
                out.print("\t");
                if (parameter.uses == OperandUses::all())
                    out.print("every use");
                else {
                    for (OperandUse use : changes)
                        out.print(nameOf(use), " ");
                }
                out.println();
            };
            if (overload.receiver)
                printParameter(*overload.receiver);
            for (const BuiltinParameter& parameter : overload.parameters)
                printParameter(parameter);
        }
    }

    for (uint32_t index = 0; index < solver.numberOfStatements(); ++index) {
        ValueFlowStatementID id(index);
        const ValueFlowStatement& statement = solver.statementAt(id);
        solver.forEachMadeNodeAskedForBy(id, [&](const ValueFlowNodeKey& key, ValueFlowStatementID, ValueFlowNodeID first) {
            if (key.kind != ValueFlowMadeNodeKind::Written || !key.set)
                return;
            ASCIILiteral done = whatIsDone(key.name().kind);
            if (done.isNull())
                return;
            for (const Family& family : families) {
                bool lists = ValueSets::isComplement(key.set) || sets.meet(key.set, family.objects);
                bool throughUnknown = family.hasEscaped && sets.contains(key.set, ValueFlowUniverse::unknownObject());
                bool throughUntracked = family.isLost && sets.contains(key.set, ValueFlowUniverse::untrackedObject());
                if (!lists && !throughUnknown && !throughUntracked)
                    continue;
                out.print("F\tL", family.layout.index(), "\t", done, "\t", hasName(key.name().kind) ? nameOfKey(PropertyKeyID(key.payload)) : String("-"_s), "\t", lists ? "listed" : throughUnknown ? "unknown" : "untracked", "\t");
                printStatement(out, solver, id);
                out.print("\t");
                if (statement.function && statement.function.index() < program.functions.size()) {
                    const Function& function = program.at(statement.function);
                    out.print(function.name ? String::fromUTF8(program.string(function.name)) : String("(anonymous)"_s), "\t", nameOf(function.role), "\t");
                    Wire::SourceLocation location = statement.site ? function.locationOf(statement.site) : Wire::SourceLocation { function.module ? function.module.index() : UINT32_MAX, function.position };
                    if (location.module != UINT32_MAX)
                        out.print(String::fromUTF8(program.string(program.at(ModuleID(location.module)).path)), "\t", location.position);
                    else
                        out.print("-\t0");
                } else
                    out.print("-\t-\t-\t0");
                out.print("\t");
                std::optional<uint32_t> operandBrought;
                std::optional<uint32_t> operandOfKey;
                if (statement.kind == ValueFlowStatementKind::Operation && (key.name().kind == ValueFlowNameKind::Property || key.name().kind == ValueFlowNameKind::UnknownKey)) {
                    switch (statement.opcode) {
                    case Opcode::PropSet:
                        operandBrought = OperandIndex::PropSet::value;
                        break;
                    case Opcode::PropDefine:
                        operandBrought = OperandIndex::PropDefine::value;
                        break;
                    case Opcode::ElemSet:
                        operandBrought = OperandIndex::ElemSet::value;
                        operandOfKey = OperandIndex::ElemSet::property;
                        break;
                    case Opcode::ElemDefine:
                        operandBrought = OperandIndex::ElemDefine::value;
                        operandOfKey = OperandIndex::ElemDefine::property;
                        break;
                    default:
                        break;
                    }
                }
                if (operandBrought) {
                    printBrought(solution.valueOf(bodies.operandsOf(statement)[*operandBrought]));
                    if (operandOfKey) {
                        out.print("\tkey: ");
                        printBrought(solution.valueOf(bodies.operandsOf(statement)[*operandOfKey]));
                    }
                    out.println();
                    continue;
                }
                ValueSetID brought = ValueSets::nothing;
                bool hasLoggedEdge = false;
                solver.graph().forEachPredecessor(first, [&](ValueFlowNodeID predecessor) {
                    if (solver.statementThatLogged({ predecessor, first }) != id)
                        return;
                    hasLoggedEdge = true;
                    brought = maker.unite(brought, solver.valueOf(predecessor));
                });
                if (hasLoggedEdge)
                    printBrought(brought);
                else {
                    out.print("(all that lies there) ");
                    printBrought(solver.valueOf(first));
                }
                out.println();
            }
        });
    }
}

static std::optional<uint32_t> numberAfter(StringView word, size_t start)
{
    size_t end = start;
    while (end < word.length() && isASCIIDigit(word[end]))
        ++end;
    return parseInteger<uint32_t>(word.substring(start, end - start));
}

void runValueFlowWhyTestCommand(TestCommandContext& context)
{
    prepareForFlow(context);
    const Program& program = *context.program;
    uint32_t mostRounds = UINT32_MAX;
    for (const String& word : context.arguments) {
        if (word.startsWith("rounds="_s))
            mostRounds = parseInteger<uint32_t>(StringView(word).substring(7)).value_or(UINT32_MAX);
    }
    // without=Source,Source: what holds if no rule brings the unknown object into being for these reasons.
    EnumSet<ValueFlowUnknownSource> unknownSourcesLeftOut;
    for (const String& word : context.arguments) {
        if (!word.startsWith("without="_s))
            continue;
        for (StringView name : StringView(word).substring(8).split(',')) {
            for (size_t source = 0; source < numberOfUnknownSources; ++source) {
                if (name == nameOf(static_cast<ValueFlowUnknownSource>(source)))
                    unknownSourcesLeftOut.add(static_cast<ValueFlowUnknownSource>(source));
            }
        }
    }
    ValueFlowSolution solution(*context.program, context.build, ValueFlowSolver::Rounds::Due, ValueFlowSolution::Witnesses::ForEveryEdge, mostRounds, unknownSourcesLeftOut);
    if (!solution.hasResult()) {
        context.out.println("stopped at a limit");
        return;
    }
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowBodies& bodies = solution.bodies();

    if (context.arguments.empty()) {
        struct Line {
            ValueFlowStatementKind kind;
            Opcode opcode;
            uint32_t functions { 0 };
        };
        Vector<Line> lines;
        uint32_t enteredByBodies = 0;
        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            if (!program.functions[index].hasBody() || !solver.isReached(FunctionID(index)))
                continue;
            ValueFlowReachWitness witness = solver.reachedBy(FunctionID(index));
            if (!witness.statement) {
                ++enteredByBodies;
                continue;
            }
            const ValueFlowStatement& statement = solver.statementAt(witness.statement);
            size_t found = lines.findIf([&](const Line& line) {
                return line.kind == statement.kind && line.opcode == statement.opcode;
            });
            if (found == notFound) {
                found = lines.size();
                lines.append({ statement.kind, statement.opcode, 0 });
            }
            ++lines[found].functions;
        }
        AOT::sort(lines, std::greater { }, &Line::functions);
        context.out.println("entered by a body or the host: ", enteredByBodies);
        for (const Line& line : lines) {
            if (line.kind == ValueFlowStatementKind::Operation)
                context.out.println(line.opcode, ": ", line.functions);
            else
                context.out.println(nameOf(line.kind), ": ", line.functions);
        }
        return;
    }

    for (const String& word : context.arguments) {
        if (word.startsWith("rounds="_s) || word.startsWith("without="_s) || word == "split"_s || word == "guarded"_s || word == "unguarded"_s)
            continue;
        if (word == "seeds"_s) {
            printSeeds(context.out, solution);
            continue;
        }
        if (word.startsWith("falsifiers="_s)) {
            Vector<LayoutID> layouts;
            for (StringView name : StringView(word).substring(11).split(',')) {
                if (std::optional<uint32_t> number = parseInteger<uint32_t>(name.substring(1)); number && *number < program.layouts.size())
                    layouts.append(LayoutID(*number));
            }
            printFalsifiers(context.out, solution, layouts.span());
            continue;
        }
        if (word == "timeline"_s) {
            printTimeline(context.out, solution);
            continue;
        }
        if (word == "order"_s) {
            printFunctionsInOrderReached(context.out, solution);
            continue;
        }
        if (word == "escapes"_s) {
            WaysOfEscape ways(solution);
            printWhoLetsEscape(context.out, solution, ways, EscapeIsCountedFor::ObjectItself, [](ValueFlowObjectID) {
                return true;
            });
            continue;
        }
        if (word == "callee-births"_s) {
            printBirthsOfUnknownObject(context.out, solution, context.build, [](Opcode opcode) {
                return opcode == Opcode::Call || opcode == Opcode::CallList || opcode == Opcode::Construct || opcode == Opcode::ConstructList;
            });
            continue;
        }
        if (word == "awaited-births"_s) {
            printBirthsOfUnknownObject(context.out, solution, context.build, [](Opcode opcode) {
                return opcode == Opcode::Await;
            });
            continue;
        }
        if (word == "receiver-births"_s) {
            printBirthsOfUnknownObject(context.out, solution, context.build, [](Opcode opcode) {
                return opcode == Opcode::PropGet || opcode == Opcode::PropSet || opcode == Opcode::ElemGet || opcode == Opcode::ElemSet;
            });
            continue;
        }
        if (word == "escape-roots"_s) {
            printRootsOfEscape(context.out, solution);
            continue;
        }
        if (word == "saturated"_s) {
            printNodesWhere(context.out, solution, "saturated"_s, [&](ValueFlowNodeID node) {
                return solver.isSaturated(node);
            });
            continue;
        }
        if (word.startsWith("late="_s)) {
            std::optional<uint32_t> rounds = numberAfter(word, 5);
            if (!rounds)
                return printBadArgument(context.out, word);
            printLateRounds(context.out, solution, *rounds);
            continue;
        }
        if (word.startsWith("large="_s)) {
            std::optional<uint32_t> size = numberAfter(word, 6);
            if (!size)
                return printBadArgument(context.out, word);
            auto isLarge = [&](ValueFlowNodeID node) {
                ValueSetID value = solver.valueOf(node);
                return !ValueSets::isComplement(value) && solver.sets().size(value) > *size;
            };
            printNodesWhere(context.out, solution, "large"_s, isLarge);
            uint64_t members = 0;
            uint64_t membersThatHaveEscaped = 0;
            uint32_t holdingUnknownObject = 0;
            HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> distinct;
            ValueSetMaker maker(bodies.sets());
            ValueSetID escaped = solver.valueOf(ValueFlowProgramNodes::escaped());
            for (uint32_t index = 0; index < solver.graph().numberOfNodes(); ++index) {
                if (!isLarge(ValueFlowNodeID(index)))
                    continue;
                ValueSetID value = solver.valueOf(ValueFlowNodeID(index));
                holdingUnknownObject += solver.sets().contains(value, ValueFlowUniverse::unknownObject());
                if (!distinct.add(value.index()).isNewEntry)
                    continue;
                members += solver.sets().size(value);
                membersThatHaveEscaped += solver.sets().size(maker.intersect(value, escaped));
            }
            context.out.println("  holding the unknown object ", holdingUnknownObject, "; distinct sets ", distinct.size(), " with ", members, " members, of which ", membersThatHaveEscaped, " have escaped");
            continue;
        }
        if (word.startsWith("holds="_s)) {
            // holds=nN:#M
            size_t colon = word.find(':');
            std::optional<uint32_t> node = numberAfter(word, 7);
            std::optional<uint32_t> object = colon == notFound ? std::nullopt : numberAfter(word, colon + 2);
            if (!node || !object || *node >= solver.graph().numberOfNodes() || *object >= bodies.universe().numberOfObjects())
                return printBadArgument(context.out, word);
            context.out.println(word);
            printWhyHeld(context.out, solution, ValueFlowNodeID(*node), ValueFlowObjectID(*object));
            continue;
        }
        if (word == "leaving"_s) {
            printWhoLeavesTypedCode(context.out, solution);
            continue;
        }
        if (word == "receiver-layouts"_s) {
            printLayoutsOfReceivers(context.out, solution);
            continue;
        }
        if (word == "checks"_s) {
            printChecks(context.out, solution, context.build);
            continue;
        }
        if (word == "choke-points"_s) {
            printChokePoints(context.out, solution, context.build);
            continue;
        }
        if (word == "outside-families"_s) {
            printNodesWhere(context.out, solution, "outside-families"_s, [&](ValueFlowNodeID node) {
                return solver.holdsObjectOutsideFamily(node);
            });
            continue;
        }
        if (word == "evaluators"_s) {
            const BuiltinTable& builtins = program.builtins();
            solver.sets().forEachMember(solver.valueOf(ValueFlowProgramNodes::escaped()), [&](ValueFlowObjectID object) {
                BuiltinID builtin = bodies.universe().builtinOf(object);
                if (!builtin || !builtins.letsInUnseenCode(builtin))
                    return;
                context.out.println("escaped ", builtins.tokenOf(builtin));
                printWhyHeld(context.out, solution, ValueFlowProgramNodes::escaped(), object);
            });
            continue;
        }
        if (word == "hubs"_s) {
            for (uint32_t index = 0; index < solver.graph().numberOfNodes(); ++index) {
                const ValueFlowNodeKey* key = solver.keyOfMadeNode(ValueFlowNodeID(index));
                if (!key || key->kind != ValueFlowMadeNodeKind::Hub || !solver.sets().contains(solver.valueOf(ValueFlowNodeID(index)), ValueFlowUniverse::unknownObject()))
                    continue;
                context.out.println("hub ", key->payload);
                printWhyHeld(context.out, solution, ValueFlowNodeID(index), ValueFlowUniverse::unknownObject());
            }
            continue;
        }
        size_t equals = word.find('=');
        if (word.startsWith("flag="_s) || word.startsWith("held="_s) || word.startsWith("untracked-in="_s)) {
            bool namesHostNode = !word.startsWith("held="_s);
            std::optional<uint32_t> number = numberAfter(word, equals + 2);
            if (!number || *number >= (namesHostNode ? numberOfValueFlowHostNodes : solver.graph().numberOfNodes()))
                return printBadArgument(context.out, word);
            context.out.println(word);
            printWhyHeld(context.out, solution, namesHostNode ? bodies.nodes().ofHost(static_cast<ValueFlowHostNode>(*number)) : ValueFlowNodeID(*number), word.startsWith("untracked-in="_s) ? ValueFlowUniverse::untrackedObject() : ValueFlowUniverse::unknownObject());
            continue;
        }
        if (word.startsWith("raisers=h"_s)) {
            // flag=hN names the first of them. Whoever takes that one away sees here who takes over.
            std::optional<uint32_t> number = numberAfter(word, equals + 2);
            if (!number || *number >= numberOfValueFlowHostNodes)
                return printBadArgument(context.out, word);
            context.out.println(word);
            ValueFlowNodeID node = bodies.nodes().ofHost(static_cast<ValueFlowHostNode>(*number));
            Vector<uint32_t> raisers;
            solver.graph().forEachPredecessor(node, [&](ValueFlowNodeID predecessor) {
                if (solver.valueOf(predecessor) == ValueSets::nothing)
                    return;
                if (ValueFlowStatementID logger = solver.statementThatLogged({ predecessor, node }))
                    raisers.append(logger.index());
            });
            AOT::sort(raisers);
            removeRepeatedElements(raisers);
            for (uint32_t raiser : raisers) {
                context.out.print("  ");
                printStatement(context.out, solver, ValueFlowStatementID(raiser));
                context.out.println();
            }
            continue;
        }
        if (word.startsWith("field="_s) || word.startsWith("variable="_s)) {
            // field=L1:2 | variable=g3: what the node of a field with a node of its own, or of a variable, holds, and how each member that does not surely fit the declared type has come there.
            std::optional<uint32_t> owner = numberAfter(word, equals + 2);
            size_t colon = word.find(':');
            std::optional<uint32_t> field = colon == notFound ? std::nullopt : numberAfter(word, colon + 1);
            bool isField = word.startsWith("field="_s);
            if (!owner || (isField ? *owner >= program.layouts.size() || !field || *field >= program.layouts[*owner].fields.size() : *owner >= program.variables.size()))
                return printBadArgument(context.out, word);
            ValueFlowNodeID node = isField ? bodies.nodes().field(LayoutID(*owner), *field) : bodies.nodes().variable(VariableID(*owner));
            TypeID declared = isField ? program.layouts[*owner].fields[*field].declared.type : program.variables[*owner].declared.type;
            context.out.print(word, " n", node.index(), " ");
            printValue(context.out, solution, solver.valueOf(node));
            ValueFlowResultInputs inputs { program, solution, context.build };
            BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
            ValueSetID outside = borrowedMaker.get().intersect(solver.valueOf(node), ValueSets::complement(bodies.universe().membersSurelyInside(borrowedMaker.get(), program, declared)));
            unsigned printed = 0;
            solver.sets().forEachMember(outside, [&](ValueFlowObjectID member) {
                if (++printed > 8)
                    return;
                context.out.print("outside the declared type: ");
                printValue(context.out, solution, borrowedMaker.get().of(member));
                printWhyHeld(context.out, solution, node, member);
            });
            continue;
        }
        if (word.startsWith("escaped-builtin="_s) || word.startsWith("lost-builtin="_s) || word.startsWith("used-builtin="_s) || word.startsWith("written-builtin="_s)) {
            const BuiltinTable& builtins = program.builtins();
            StringView token = StringView(word).substring(equals + 1);
            BuiltinID named;
            for (uint32_t index = 0; index < builtins.builtinCount() && !named; ++index) {
                if (token == builtins.tokenOf(BuiltinID(index)))
                    named = BuiltinID(index);
            }
            if (!named)
                return printBadArgument(context.out, word);
            context.out.println(word);
            ValueFlowNodeID holding = word.startsWith("used"_s) ? bodies.nodes().ofHost(ValueFlowHostNode::UsedBuiltins) : word.startsWith("written"_s) ? bodies.nodes().ofHost(ValueFlowHostNode::WrittenBuiltins) : word.startsWith("lost"_s) ? ValueFlowProgramNodes::lost() : ValueFlowProgramNodes::escaped();
            printWhyHeld(context.out, solution, holding, bodies.universe().objectOf(named));
            Vector<ValueFlowObjectID, 32> objects;
            bodies.universe().appendObjectsOf(named, objects);
            unsigned held = 0;
            for (ValueFlowObjectID object : objects.span().subspan(1)) {
                if (!solution.sets().contains(solver.valueOf(holding), object))
                    continue;
                if (++held > 6)
                    continue;
                context.out.println("  of module m", bodies.universe().moduleOf(object).index());
                printWhyHeld(context.out, solution, holding, object);
            }
            if (objects.size() > 1)
                context.out.println("  held: those of ", held, " of ", objects.size() - 1, " modules");
            continue;
        }
        if (word.startsWith("escaped-layout="_s) || word.startsWith("lost-layout="_s)) {
            std::optional<uint32_t> layout = numberAfter(word, equals + 2);
            if (!layout || *layout >= program.layouts.size())
                return printBadArgument(context.out, word);
            context.out.println(word);
            ValueFlowNodeID holding = word.startsWith("lost"_s) ? ValueFlowProgramNodes::lost() : ValueFlowProgramNodes::escaped();
            solver.sets().forEachMember(solver.valueOf(holding), [&](ValueFlowObjectID object) {
                if (bodies.universe().layoutOf(object) != LayoutID(*layout))
                    return;
                printObject(context.out, solution, object);
                context.out.println();
                printWhyHeld(context.out, solution, holding, object);
            });
            continue;
        }
        constexpr unsigned mostCalleesPrinted = 256;
        if (word.startsWith("group-askers=o"_s)) {
            std::optional<uint32_t> object = numberAfter(word, equals + 2);
            if (!object || *object >= bodies.universe().numberOfObjects())
                return printBadArgument(context.out, word);
            context.out.println(word);
            for (uint32_t index = 0; index < solver.numberOfStatements(); ++index) {
                ValueFlowStatementID id(index);
                solver.forEachMadeNodeAskedForBy(id, [&](const ValueFlowNodeKey& asked, ValueFlowStatementID) {
                    if (asked.kind != ValueFlowMadeNodeKind::CallGroup || ValueSets::isComplement(asked.set) || !solver.sets().contains(asked.set, ValueFlowObjectID(*object)))
                        return;
                    context.out.print("  ");
                    printStatement(context.out, solver, id);
                    context.out.print(" asks for ");
                    printKey(context.out, solver, asked);
                    context.out.print(" of");
                    printMembersByName(context.out, solution, asked.set, mostCalleesPrinted);
                    context.out.println();
                });
            }
            continue;
        }
        if (word.startsWith("groups=d"_s)) {
            std::optional<uint32_t> kind = numberAfter(word, equals + 2);
            if (!kind)
                return printBadArgument(context.out, word);
            context.out.println(word);
            for (uint32_t index = 0; index < solver.numberOfStatements(); ++index) {
                ValueFlowStatementID id(index);
                if (solver.statementAt(id).kind != ValueFlowStatementKind::CallGroup || !solver.isMadeByRule(id) || solver.keyOf(id).detail != *kind)
                    continue;
                context.out.print("  ");
                printKey(context.out, solver, solver.keyOf(id));
                context.out.print(" of");
                printMembersByName(context.out, solution, solver.keyOf(id).set, mostCalleesPrinted);
                context.out.println();
            }
            continue;
        }
        if (word.startsWith("askers=k"_s)) {
            size_t colon = word.find(':');
            std::optional<uint32_t> key = numberAfter(word, equals + 2);
            std::optional<uint32_t> object = colon == notFound ? std::nullopt : numberAfter(word, colon + 2);
            if (!key || !object || *object >= bodies.universe().numberOfObjects())
                return printBadArgument(context.out, word);
            context.out.println(word);
            for (uint32_t index = 0; index < solver.numberOfStatements(); ++index) {
                ValueFlowStatementID id(index);
                bool hasAsked = false;
                solver.forEachMadeNodeAskedForBy(id, [&](const ValueFlowNodeKey& asked, ValueFlowStatementID) {
                    hasAsked |= asked.kind == ValueFlowMadeNodeKind::Lookup && asked.payload == *key && solver.sets().contains(asked.set, ValueFlowObjectID(*object));
                });
                if (!hasAsked)
                    continue;
                context.out.print("  ");
                printStatement(context.out, solver, id);
                context.out.println();
            }
            continue;
        }
        // asked=dN:oM: the statements that have asked for a node of the name kind N over a set of at most eight members that holds the object M. asked=lN:oM.
        if (word.startsWith("asked=d"_s) || word.startsWith("asked=l"_s) || word.startsWith("asked=g"_s)) {
            bool asksForGroups = word.startsWith("asked=g"_s);
            bool asksForLookupsAlone = word.startsWith("asked=l"_s);
            size_t colon = word.find(':');
            std::optional<uint32_t> kind = numberAfter(word, equals + 2);
            std::optional<uint32_t> object = colon == notFound ? std::nullopt : numberAfter(word, colon + 2);
            if (!kind || !object || *object >= bodies.universe().numberOfObjects())
                return printBadArgument(context.out, word);
            context.out.println(word);
            for (uint32_t index = 0; index < solver.numberOfStatements(); ++index) {
                ValueFlowStatementID id(index);
                bool hasAsked = false;
                solver.forEachMadeNodeAskedForBy(id, [&](const ValueFlowNodeKey& asked, ValueFlowStatementID) {
                    if (asksForGroups ? asked.kind != ValueFlowMadeNodeKind::CallGroup : asked.kind != ValueFlowMadeNodeKind::Lookup && (asksForLookupsAlone || asked.kind != ValueFlowMadeNodeKind::Read))
                        return;
                    hasAsked |= static_cast<uint32_t>(asked.detail) == *kind && !ValueSets::isComplement(asked.set) && solver.sets().size(asked.set) <= 8 && solver.sets().contains(asked.set, ValueFlowObjectID(*object));
                });
                if (!hasAsked)
                    continue;
                context.out.print("  ");
                printStatement(context.out, solver, id);
                context.out.println();
                // A statement that a rule has made belongs to nobody: its set, and the first who has asked for it, down to an operation of a function.
                ValueFlowStatementID made = id;
                for (unsigned depth = 0; depth < 6 && solver.isMadeByRule(made) && solver.statementAt(made).kind != ValueFlowStatementKind::Escape; ++depth) {
                    context.out.print("      of");
                    printMembersByName(context.out, solution, solver.keyOf(made).set, 16);
                    context.out.println();
                    ValueFlowStatementID asker;
                    for (uint32_t other = 0; other < solver.numberOfStatements() && !asker; ++other) {
                        solver.forEachMadeNodeAskedForBy(ValueFlowStatementID(other), [&](const ValueFlowNodeKey&, ValueFlowStatementID statementOfNode) {
                            if (statementOfNode == made && other != made.index())
                                asker = ValueFlowStatementID(other);
                        });
                    }
                    if (!asker)
                        break;
                    context.out.print("      asked for by ");
                    printStatement(context.out, solver, asker);
                    context.out.println();
                    made = asker;
                }
            }
            continue;
        }
        if (word.startsWith("escaped-object="_s) || word.startsWith("lost-object="_s)) {
            std::optional<uint32_t> object = numberAfter(word, equals + 2);
            if (!object || *object >= bodies.universe().numberOfObjects())
                return printBadArgument(context.out, word);
            context.out.print(word, " ");
            printObject(context.out, solution, ValueFlowObjectID(*object));
            context.out.println();
            printWhyHeld(context.out, solution, word.startsWith("lost"_s) ? ValueFlowProgramNodes::lost() : ValueFlowProgramNodes::escaped(), ValueFlowObjectID(*object));
            continue;
        }
        std::optional<uint32_t> function = equals == notFound ? std::nullopt : numberAfter(word, equals + 2);
        if (!function || *function >= program.functions.size())
            return printBadArgument(context.out, word);
        context.out.println(word);
        if (word.startsWith("leaves="_s)) {
            const Function& record = program.functions[*function];
            context.out.println("  ", record.role, " at position ", record.position, " of module m", record.module ? record.module.index() : 0);
            for (const ValueFlowStatement& statement : bodies.statementsOf(*function)) {
                if (statement.kind != ValueFlowStatementKind::EntryChecks)
                    continue;
                unsigned index = 0;
                for (const ValueFlowOperand& turnedAway : bodies.operandsOf(statement)) {
                    ValueSetID held = solution.valueOf(turnedAway);
                    if (!solution.sets().isEmpty(held)) {
                        context.out.print("  test ", index);
                        if (turnedAway.node) {
                            context.out.print(" at ");
                            printNode(context.out, solution, turnedAway.node);
                        }
                        context.out.print(" turns away ");
                        printValue(context.out, solution, held);
                    }
                    ++index;
                }
            }
        } else if (word.startsWith("reached="_s))
            printWhyReached(context.out, solution, FunctionID(*function));
        else if (word.startsWith("escaped="_s))
            printWhyHeld(context.out, solution, ValueFlowProgramNodes::escaped(), bodies.universe().closureOf(FunctionID(*function)));
        else if (word.startsWith("lost="_s))
            printWhyHeld(context.out, solution, ValueFlowProgramNodes::lost(), bodies.universe().closureOf(FunctionID(*function)));
        else if (word.startsWith("unknown="_s) || word.startsWith("untracked="_s) || word.startsWith("values="_s) || word.startsWith("asks="_s)) {
            size_t colon = word.find(':');
            std::optional<uint32_t> site = colon == notFound ? std::nullopt : numberAfter(word, colon + 2);
            if (!site)
                return printBadArgument(context.out, word);
            for (const ValueFlowStatement& statement : bodies.statementsOf(*function)) {
                if (statement.site.index() != *site)
                    continue;
                if (word.startsWith("values="_s))
                    printValuesAt(context.out, solution, statement);
                else if (word.startsWith("asks="_s))
                    printWhatIsAskedFor(context.out, solution, ValueFlowStatementID(static_cast<uint32_t>(&statement - bodies.statements().data())), 0);
                else if (statement.operandCount && bodies.operandsOf(statement)[0].node)
                    printWhyHeld(context.out, solution, bodies.operandsOf(statement)[0].node, word.startsWith("untracked="_s) ? ValueFlowUniverse::untrackedObject() : ValueFlowUniverse::unknownObject());
            }
        } else
            return printBadArgument(context.out, word);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
