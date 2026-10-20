/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTSupportTestCommands.h"
#include "AOTValueFlowAnalysis.h"
#include "AOTValueFlowLog.h"
#include <algorithm>
#include <array>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
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
    case ValueFlowMadeNodeKind::Part:
        return "part"_s;
    case ValueFlowMadeNodeKind::WrittenUnderKind:
        return "written-under-kind"_s;
    case ValueFlowMadeNodeKind::ReadUnderKind:
        return "read-under-kind"_s;
    }
    return "?"_s;
}

static void printKey(PrintStream& out, const ValueFlowSolver& solver, const ValueFlowNodeKey& key)
{
    out.print(nameOf(key.kind), " detail ", key.detail, " payload ", key.payload);
    if (key.set)
        out.print(" set of ", solver.sets().size(key.set));
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
    else if (FunctionID function = universe.closureFunction(object))
        out.print("f", function.index());
    else if (BuiltinID builtin = universe.builtinOf(object))
        out.print(solution.program().builtins().tokenOf(builtin));
    else if (FunctionID function = universe.functionOf(object))
        out.print("object", object.index(), "@f", function.index());
    else
        out.print("object", object.index());
}

// An object that has escaped because an escaped object holds it is counted for that holder, and so on up to an object that a statement other than Escape has let escape.
// Prints those objects by the number of closures that have escaped behind them, each with its own way into `escaped`.
static void printRootsOfEscape(PrintStream& out, ValueFlowSolution& solution)
{
    constexpr size_t numberOfRootsPrinted = 30;
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowUniverse& universe = solution.bodies().universe();
    ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();
    ValueSetID escaped = solver.valueOf(escapedNode);

    HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> holderOf;
    auto findHolder = [&](ValueFlowObjectID object) -> ValueFlowObjectID {
        auto found = holderOf.find(object.index());
        if (found != holderOf.end())
            return found->value == UINT32_MAX - 1 ? ValueFlowObjectID() : ValueFlowObjectID(found->value);
        ValueFlowObjectID holder;
        Vector<ValueFlowNodeID> way = solver.explain(escapedNode, object);
        ValueFlowStatementID logger = way.size() > 1 ? solver.statementThatLogged({ way[1], way[0] }) : ValueFlowStatementID();
        if (logger && solver.statementAt(logger).kind == ValueFlowStatementKind::Escape) {
            for (ValueFlowNodeID node : way) {
                const ValueFlowNodeKey* key = solver.keyOfMadeNode(node);
                if (!key || key->kind != ValueFlowMadeNodeKind::Written || !key->set)
                    continue;
                solver.sets().forEachMember(key->set, [&](ValueFlowObjectID member) {
                    if (!holder && member != object && solver.sets().contains(escaped, member))
                        holder = member;
                });
                break;
            }
        }
        holderOf.add(object.index(), holder ? holder.index() : UINT32_MAX - 1);
        return holder;
    };

    struct Root {
        ValueFlowObjectID object;
        uint32_t closures { 0 };
    };
    Vector<Root> roots;
    solver.sets().forEachMember(escaped, [&](ValueFlowObjectID object) {
        if (!universe.closureFunction(object))
            return;
        ValueFlowObjectID root = object;
        for (unsigned steps = 0; steps < 64; ++steps) {
            ValueFlowObjectID holder = findHolder(root);
            if (!holder)
                break;
            root = holder;
        }
        size_t place = roots.findIf([&](const Root& other) {
            return other.object == root;
        });
        if (place == notFound) {
            place = roots.size();
            roots.append({ root, 0 });
        }
        ++roots[place].closures;
    });

    std::ranges::stable_sort(roots, std::greater { }, &Root::closures);
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
        else if (FunctionID function = universe.closureFunction(member))
            out.print(" f", function.index());
        else if (BuiltinID builtin = universe.builtinOf(member))
            out.print(" ", solution.program().builtins().tokenOf(builtin));
        else if (PropertyKeyID key = universe.keyOf(member))
            out.print(" key", key.index());
        else if (!universe.isObject(member))
            out.print(" primitive", member.index());
        else if (FunctionID function = universe.functionOf(member))
            out.print(" object", member.index(), "@f", function.index());
        else
            out.print(" object", member.index());
    });
    out.println(printed > numberOfMembersPrinted ? " .." : "");
}

// For each object that has escaped, the statement that logged the last edge of a shortest way into `escaped`: who lets how many objects escape, and how many closures among them.
static void printWhoLetsEscape(PrintStream& out, ValueFlowSolution& solution)
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
    solver.sets().forEachMember(solver.valueOf(escaped), [&](ValueFlowObjectID object) {
        if (!universe.isObject(object) || object == ValueFlowUniverse::unknownObject())
            return;
        Vector<ValueFlowNodeID> way = solver.explain(escaped, object);
        ValueFlowStatementID logger = way.size() > 1 ? solver.statementThatLogged({ way[1], way[0] }) : ValueFlowStatementID();
        if (!logger) {
            ++byEdgesOfNoStatement;
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
        ++lines[found].sorts[static_cast<size_t>(sortOf(solution.program(), universe, object))];
        ++objectsOfStatement.add(logger.index(), 0).iterator->value;
    });

    std::ranges::sort(lines, std::greater { }, &Line::objects);
    out.println("by an edge of a body or of the solver: ", byEdgesOfNoStatement);
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
    std::ranges::sort(statements, [](const StatementLine& first, const StatementLine& second) {
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

// Where the unknown object in the first operand of the counted operations of reached functions was born: the first node behind a constant on a shortest way to the operand, and the statement that
// put the constant there. One walk from all such nodes at once, along the edges, through the nodes that hold the unknown object.
static void printBirthsOfUnknownObject(PrintStream& out, ValueFlowSolution& solution, NOESCAPE const Invocable<bool(Opcode)> auto& isCounted)
{
    constexpr size_t numberOfBirthsPrinted = 40;
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowBodies& bodies = solution.bodies();
    uint32_t numberOfNodes = solver.graph().numberOfNodes();
    auto holdsUnknownObject = [&](ValueFlowNodeID node) {
        return solver.sets().contains(solver.valueOf(node), ValueFlowUniverse::unknownObject());
    };
    auto isConstant = [&](ValueFlowNodeID node) {
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(node);
        return key && key->kind == ValueFlowMadeNodeKind::Constant;
    };

    Vector<Vector<uint32_t, 2>> successors(numberOfNodes);
    Vector<uint32_t> birthOf;
    birthOf.fill(UINT32_MAX, numberOfNodes);
    Vector<ValueFlowNodeID> constantBehind(numberOfNodes);
    Vector<uint32_t> found;
    for (uint32_t index = 0; index < numberOfNodes; ++index) {
        ValueFlowNodeID node(index);
        if (!holdsUnknownObject(node) || isConstant(node))
            continue;
        solver.graph().forEachPredecessor(node, [&](ValueFlowNodeID predecessor) {
            if (!holdsUnknownObject(predecessor))
                return;
            if (!isConstant(predecessor)) {
                successors[predecessor.index()].append(index);
                return;
            }
            if (birthOf[index] != UINT32_MAX)
                return;
            birthOf[index] = index;
            constantBehind[index] = predecessor;
            found.append(index);
        });
    }
    for (size_t next = 0; next < found.size(); ++next) {
        for (uint32_t successor : successors[found[next]]) {
            if (birthOf[successor] != UINT32_MAX)
                continue;
            birthOf[successor] = birthOf[found[next]];
            found.append(successor);
        }
    }

    Vector<uint32_t> operandsBornAt;
    operandsBornAt.fill(0, numberOfNodes);
    uint32_t counted = 0;
    uint32_t withUnknownObject = 0;
    uint32_t withoutBirth = 0;
    for (uint32_t index = 0; index < solution.program().functions.size(); ++index) {
        if (!solver.isReached(FunctionID(index)))
            continue;
        for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || !isCounted(statement.opcode) || !statement.operandCount)
                continue;
            ++counted;
            const ValueFlowOperand& operand = bodies.operandsOf(statement)[0];
            if (!solution.sets().contains(solution.valueOf(operand), ValueFlowUniverse::unknownObject()))
                continue;
            ++withUnknownObject;
            if (operand.node && birthOf[operand.node.index()] != UINT32_MAX)
                ++operandsBornAt[birthOf[operand.node.index()]];
            else
                ++withoutBirth;
        }
    }

    struct Line {
        ValueFlowStatementKind kind;
        Opcode opcode;
        uint8_t detail { 0 };
        bool isSeedOfBody { false };
        uint32_t operands { 0 };
        uint32_t births { 0 };
    };
    Vector<Line> lines;
    Vector<uint32_t> births;
    for (uint32_t index = 0; index < numberOfNodes; ++index) {
        if (!operandsBornAt[index])
            continue;
        births.append(index);
        ValueFlowStatementID logger = solver.statementThatLogged({ constantBehind[index], ValueFlowNodeID(index) });
        Line line { ValueFlowStatementKind::Operation, Opcode { }, 0, !logger, 0, 0 };
        if (logger) {
            const ValueFlowStatement& statement = solver.statementAt(logger);
            line.kind = statement.kind;
            line.opcode = statement.opcode;
            line.detail = solver.isMadeByRule(logger) && statement.kind != ValueFlowStatementKind::Escape ? solver.keyOf(logger).detail : 0;
        }
        size_t place = lines.findIf([&](const Line& other) {
            return other.kind == line.kind && other.opcode == line.opcode && other.detail == line.detail && other.isSeedOfBody == line.isSeedOfBody;
        });
        if (place == notFound) {
            place = lines.size();
            lines.append(line);
        }
        lines[place].operands += operandsBornAt[index];
        ++lines[place].births;
    }

    out.println("operations ", counted, " with the unknown object ", withUnknownObject, " in a constant of their own ", withoutBirth);
    std::ranges::stable_sort(lines, std::greater { }, &Line::operands);
    for (const Line& line : lines) {
        if (line.isSeedOfBody)
            out.print("a seed of a body");
        else if (line.kind == ValueFlowStatementKind::Operation)
            out.print(line.opcode);
        else
            out.print(nameOf(line.kind), " detail ", line.detail);
        out.println(": ", line.operands, " operands from ", line.births, " births");
    }
    std::ranges::stable_sort(births, std::greater { }, [&](uint32_t index) {
        return operandsBornAt[index];
    });
    for (uint32_t index : births.span().first(std::min(births.size(), numberOfBirthsPrinted))) {
        out.print(operandsBornAt[index], " <- ");
        printNode(out, solution, ValueFlowNodeID(index));
        if (ValueFlowStatementID logger = solver.statementThatLogged({ constantBehind[index], ValueFlowNodeID(index) })) {
            out.print("   by ");
            printStatement(out, solver, logger);
        }
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

static std::optional<uint32_t> numberAfter(StringView word, size_t start)
{
    size_t end = start;
    while (end < word.length() && isASCIIDigit(word[end]))
        ++end;
    return parseInteger<uint32_t>(word.substring(start, end - start));
}

// valueflow-why reached=fN | escaped=fN | unknown=fN:vM | values=fN:vM | flag=hN | held=nN | hubs | escapes | evaluators ..: why a function is reached; how its closure has escaped; how the unknown object has come into the
// first operand of an operation; what the operands and the result of an operation hold; how the unknown object has come into a node of the host, into any node, into each hub that holds it; who lets how many objects escape, by kind of statement;
// how each built-in that lets unseen code in has escaped.
// escaped-builtin=Token | used-builtin=Token: how the object of a built-in has escaped, or has come to count as used.
// escape-roots: the objects that a statement other than Escape has let escape, by the number of closures that have escaped because those objects hold them, directly or not.
// callee-births | receiver-births: where the unknown object in the callees of calls, or in the receivers of property and element accesses, was born: by kind of statement, then the births that reach the most.
// Without a word: who reached how many functions, by kind of statement.
void runValueFlowWhyTestCommand(TestCommandContext& context)
{
    const Program& program = *context.program;
    ValueFlowSolution solution(program, context.build, ValueFlowSolver::Rounds::Due, ValueFlowSolution::Witnesses::ForEveryEdge);
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
        std::ranges::sort(lines, std::greater { }, &Line::functions);
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
        if (word == "escapes"_s) {
            printWhoLetsEscape(context.out, solution);
            continue;
        }
        if (word == "callee-births"_s) {
            printBirthsOfUnknownObject(context.out, solution, [](Opcode opcode) {
                return opcode == Opcode::Call || opcode == Opcode::CallList || opcode == Opcode::Construct || opcode == Opcode::ConstructList;
            });
            continue;
        }
        if (word == "receiver-births"_s) {
            printBirthsOfUnknownObject(context.out, solution, [](Opcode opcode) {
                return opcode == Opcode::PropGet || opcode == Opcode::PropSet || opcode == Opcode::ElemGet || opcode == Opcode::ElemSet;
            });
            continue;
        }
        if (word == "escape-roots"_s) {
            printRootsOfEscape(context.out, solution);
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
        if (word.startsWith("flag="_s) || word.startsWith("held="_s)) {
            std::optional<uint32_t> number = numberAfter(word, equals + 2);
            if (!number || *number >= (word.startsWith("flag="_s) ? numberOfValueFlowHostNodes : solver.graph().numberOfNodes()))
                return printBadArgument(context.out, word);
            context.out.println(word);
            printWhyHeld(context.out, solution, word.startsWith("flag="_s) ? bodies.nodes().ofHost(static_cast<ValueFlowHostNode>(*number)) : ValueFlowNodeID(*number), ValueFlowUniverse::unknownObject());
            continue;
        }
        if (word.startsWith("escaped-builtin="_s) || word.startsWith("used-builtin="_s)) {
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
            printWhyHeld(context.out, solution, word.startsWith("used"_s) ? bodies.nodes().ofHost(ValueFlowHostNode::UsedBuiltins) : ValueFlowProgramNodes::escaped(), bodies.universe().objectOf(named));
            continue;
        }
        std::optional<uint32_t> function = equals == notFound ? std::nullopt : numberAfter(word, equals + 2);
        if (!function || *function >= program.functions.size())
            return printBadArgument(context.out, word);
        context.out.println(word);
        if (word.startsWith("reached="_s))
            printWhyReached(context.out, solution, FunctionID(*function));
        else if (word.startsWith("escaped="_s))
            printWhyHeld(context.out, solution, ValueFlowProgramNodes::escaped(), bodies.universe().closureOf(FunctionID(*function)));
        else if (word.startsWith("unknown="_s) || word.startsWith("values="_s)) {
            size_t colon = word.find(':');
            std::optional<uint32_t> site = colon == notFound ? std::nullopt : numberAfter(word, colon + 2);
            if (!site)
                return printBadArgument(context.out, word);
            for (const ValueFlowStatement& statement : bodies.statementsOf(*function)) {
                if (statement.site.index() != *site)
                    continue;
                if (word.startsWith("values="_s))
                    printValuesAt(context.out, solution, statement);
                else if (statement.operandCount && bodies.operandsOf(statement)[0].node)
                    printWhyHeld(context.out, solution, bodies.operandsOf(statement)[0].node, ValueFlowUniverse::unknownObject());
            }
        } else
            return printBadArgument(context.out, word);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
