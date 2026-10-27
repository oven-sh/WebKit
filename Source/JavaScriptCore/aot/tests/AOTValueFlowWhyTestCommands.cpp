/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTSupportTestCommands.h"
#include "AOTValueFlowAnalysis.h"
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

ASCIILiteral nameOf(ValueFlowStatementKind kind)
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

static void printValue(PrintStream& out, ValueFlowSolution& solution, ValueSetID value)
{
    constexpr unsigned numberOfMembersPrinted = 24;
    const ValueFlowUniverse& universe = solution.bodies().universe();
    unsigned printed = 0;
    out.print(solution.sets().size(value), " members:");
    solution.sets().forEachMember(value, [&](ValueFlowObjectID member) {
        if (++printed > numberOfMembersPrinted)
            return;
        out.print(" ");
        if (universe.isObject(member))
            printObject(out, solution, member);
        else if (PropertyKeyID key = universe.keyOf(member))
            out.print("key", key.index());
        else
            out.print("primitive", member.index());
    });
    out.println(printed > numberOfMembersPrinted ? " .." : "");
}

static void printTimeline(PrintStream& out, ValueFlowSolution& solution)
{
    constexpr unsigned numberOfEdgesPrinted = 60;
    constexpr ValueFlowHostNode flags[] {
        ValueFlowHostNode::UnseenCodeRuns, ValueFlowHostNode::UnknownCodeWrites, ValueFlowHostNode::HostChoosesOperands, ValueFlowHostNode::ModulesAreRequiredByName,
        ValueFlowHostNode::ModulesAreImportedByName, ValueFlowHostNode::LibraryModulesAreRequiredByName, ValueFlowHostNode::ObjectNobodyKnowsIsWritten,
        ValueFlowHostNode::UnseenCodeChangesRealm, ValueFlowHostNode::ValueNobodyKnowsIsCalled, ValueFlowHostNode::UnseenCodeHasLoader, ValueFlowHostNode::CodeOutsideImageIsLoaded,
    };
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowProgramNodes& nodes = solution.bodies().nodes();
    const ValueFlowUniverse& universe = solution.bodies().universe();
    const BuiltinTable& builtins = solution.program().builtins();
    auto holdsEvaluator = [&](ValueFlowNodeID node) {
        bool holds = false;
        ValueSetID value = solver.possibleValuesOf(node);
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
        printValue(out, solution, solver.possibleValuesOf(logged.edge.from));
        out.print("      by ");
        printStatement(out, solver, logged.statement);
        out.println();
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
            if (std::optional<ValueFlowUnknownSource> source = solver.unknownSourceOf(ValueFlowEdge { way[index + 1], way[index] }))
                out.print("   source ", nameOf(*source));
        }
        out.println();
    }
}

static void printRootsOfEscape(PrintStream& out, ValueFlowSolution& solution)
{
    constexpr size_t numberOfRootsPrinted = 30;
    using ByNumber = WTF::UnsignedWithZeroKeyHashTraits<uint32_t>;
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowUniverse& universe = solution.bodies().universe();
    ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();
    ValueSetID escaped = solver.possibleValuesOf(escapedNode);

    HashMap<uint32_t, ValueFlowStatementID, IntHash<uint32_t>, ByNumber> loggerOfEdgeIntoRetiredNode;
    for (const ValueFlowSolver::LoggedEdge& logged : solver.edgesInOrderLogged()) {
        if (solver.graph().isRetired(logged.edge.to))
            loggerOfEdgeIntoRetiredNode.add(logged.edge.from.index(), logged.statement);
    }
    auto holdsWhatIsWritten = [&](ValueFlowNodeID node) {
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(node);
        return key && (key->kind == ValueFlowMadeNodeKind::Written || key->kind == ValueFlowMadeNodeKind::WrittenUnderKind) && key->set;
    };
    auto isStatementOfEscape = [&](ValueFlowStatementID id) {
        ValueFlowStatementKind kind = solver.statementAt(id).kind;
        return kind == ValueFlowStatementKind::Escape || (kind == ValueFlowStatementKind::Lookup && solver.keyOf(id).name().kind == ValueFlowNameKind::FoundThroughEscapedInstances);
    };
    HashMap<uint32_t, ValueFlowObjectID, IntHash<uint32_t>, ByNumber> holders;
    auto holderOf = [&](ValueFlowObjectID object) {
        if (auto found = holders.find(object.index()); found != holders.end())
            return found->value;
        ValueFlowObjectID holder;
        Vector<ValueFlowNodeID> way = solver.explain(escapedNode, object);
        if (way.size() > 1) {
            ValueFlowStatementID logger = solver.statementThatLogged({ way[1], way[0] });
            if (!logger && !holdsWhatIsWritten(way[1]))
                logger = loggerOfEdgeIntoRetiredNode.get(way[1].index());
            if (logger ? isStatementOfEscape(logger) : holdsWhatIsWritten(way[1])) {
                size_t written = way.findIf(holdsWhatIsWritten);
                if (written != notFound) {
                    solver.sets().forEachMember(solver.keyOfMadeNode(way[written])->set, [&](ValueFlowObjectID member) {
                        if (!holder && member != object && solver.sets().contains(escaped, member))
                            holder = member;
                    });
                }
            }
        }
        holders.add(object.index(), holder);
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
            ValueFlowObjectID holder = holderOf(root);
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

    AOT::stableSort(roots, std::greater { }, &Root::closures);
    out.println("roots ", roots.size());
    for (const Root& root : roots.span().first(std::min(roots.size(), numberOfRootsPrinted))) {
        out.print(root.closures, " closures behind ");
        printObject(out, solution, root.object);
        out.println();
        printWhyHeld(out, solution, escapedNode, root.object);
    }
}

static void printMembersByName(PrintStream& out, ValueFlowSolution& solution, ValueSetID set)
{
    constexpr unsigned numberOfMembersPrinted = 6;
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
        ValueSetID value = solver.possibleValuesOf(first);
        out.print(" holds ", ValueSets::isComplement(value) ? 0 : solver.sets().size(value), ":");
        printMembersByName(out, solution, value);
        out.println();
        if (made && made != id && depth < deepestLevel)
            printWhatIsAskedFor(out, solution, made, depth + 1);
    });
}

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
            printMembersByName(out, solution, solution.possibleValuesOf(solution.bodies().operandsOf(statement)[0]));
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
    solver.sets().forEachMember(solver.possibleValuesOf(escaped), [&](ValueFlowObjectID object) {
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

static void printValuesAt(PrintStream& out, ValueFlowSolution& solution, const ValueFlowStatement& statement)
{
    out.println("  ", statement.opcode, solution.solver().isReached(statement.function) ? "" : " in a function that is not reached");
    unsigned index = 0;
    for (const ValueFlowOperand& operand : solution.bodies().operandsOf(statement)) {
        out.print("  operand ", index++, " ");
        if (operand.node)
            out.print("n", operand.node.index(), " ");
        printValue(out, solution, solution.possibleValuesOf(operand));
    }
    if (statement.result) {
        out.print("  result n", statement.result.index(), " ");
        printValue(out, solution, solution.solver().possibleValuesOf(statement.result));
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
    ValueSetID escaped = solver.possibleValuesOf(ValueFlowProgramNodes::escaped());
    ValueSetID lost = solver.possibleValuesOf(ValueFlowProgramNodes::lost());

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
        case ValueFlowNameKind::Elements:
        case ValueFlowNameKind::EveryProperty:
        case ValueFlowNameKind::EveryGetter:
        case ValueFlowNameKind::EverySetter:
        case ValueFlowNameKind::PrivateField:
        case ValueFlowNameKind::Slot:
        case ValueFlowNameKind::HiddenSlot:
        case ValueFlowNameKind::SteppedValues:
        case ValueFlowNameKind::ReturnedValue:
        case ValueFlowNameKind::SentValues:
        case ValueFlowNameKind::OpenedBy:
        case ValueFlowNameKind::ResolvedPromises:
        case ValueFlowNameKind::RejectedPromises:
        case ValueFlowNameKind::AllocatedAs:
        case ValueFlowNameKind::OpenedIterators:
        case ValueFlowNameKind::IteratedValues:
        case ValueFlowNameKind::IteratorResults:
        case ValueFlowNameKind::AwaitedValue:
        case ValueFlowNameKind::MemberWritten:
        case ValueFlowNameKind::EveryMemberWritten:
        case ValueFlowNameKind::EveryMemberRead:
        case ValueFlowNameKind::RestArgument:
        case ValueFlowNameKind::AddedNames:
        case ValueFlowNameKind::FoundThroughEscapedInstances:
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
                    printBrought(solution.possibleValuesOf(bodies.operandsOf(statement)[*operandBrought]));
                    if (operandOfKey) {
                        out.print("\tkey: ");
                        printBrought(solution.possibleValuesOf(bodies.operandsOf(statement)[*operandOfKey]));
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
                    brought = maker.unite(brought, solver.possibleValuesOf(predecessor));
                });
                if (hasLoggedEdge)
                    printBrought(brought);
                else {
                    out.print("(all that lies there) ");
                    printBrought(solver.possibleValuesOf(first));
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
    PrintStream& out = context.out;
    ValueFlowSolution solution(*context.program, context.build, ValueFlowSolver::Rounds::Due, ValueFlowSolution::Witnesses::ForEveryEdge);
    if (!solution.hasResult()) {
        out.println("stopped at a limit");
        return;
    }
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowBodies& bodies = solution.bodies();
    const ValueFlowUniverse& universe = bodies.universe();
    const BuiltinTable& builtins = program.builtins();
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    ValueFlowNodeID lost = ValueFlowProgramNodes::lost();

    if (context.arguments.empty())
        return printBadArgument(out, "valueflow-why takes at least one word"_s);
    for (const String& word : context.arguments) {
        if (word == "seeds"_s) {
            printSeeds(out, solution);
            continue;
        }
        if (word.startsWith("falsifiers="_s)) {
            Vector<LayoutID> layouts;
            for (StringView name : StringView(word).substring(11).split(',')) {
                if (std::optional<uint32_t> number = parseInteger<uint32_t>(name.substring(1)); number && *number < program.layouts.size())
                    layouts.append(LayoutID(*number));
            }
            printFalsifiers(out, solution, layouts.span());
            continue;
        }
        if (word == "timeline"_s) {
            printTimeline(out, solution);
            continue;
        }
        if (word == "escape-roots"_s) {
            printRootsOfEscape(out, solution);
            continue;
        }
        if (word.startsWith("holds="_s)) {
            size_t colon = word.find(':');
            std::optional<uint32_t> node = numberAfter(word, 7);
            std::optional<uint32_t> object = colon == notFound ? std::nullopt : numberAfter(word, colon + 2);
            if (!node || !object || *node >= solver.graph().numberOfNodes() || *object >= universe.numberOfObjects())
                return printBadArgument(out, word);
            out.println(word);
            printWhyHeld(out, solution, ValueFlowNodeID(*node), ValueFlowObjectID(*object));
            continue;
        }
        if (word == "evaluators"_s) {
            solver.sets().forEachMember(solver.possibleValuesOf(escaped), [&](ValueFlowObjectID object) {
                BuiltinID builtin = universe.builtinOf(object);
                if (!builtin || !builtins.letsInUnseenCode(builtin))
                    return;
                out.println("escaped ", builtins.tokenOf(builtin));
                printWhyHeld(out, solution, escaped, object);
            });
            continue;
        }
        size_t equals = word.find('=');
        if (word.startsWith("flag="_s)) {
            std::optional<uint32_t> number = numberAfter(word, equals + 2);
            if (!number || *number >= numberOfValueFlowHostNodes)
                return printBadArgument(out, word);
            out.println(word);
            printWhyHeld(out, solution, bodies.nodes().ofHost(static_cast<ValueFlowHostNode>(*number)), ValueFlowUniverse::unknownObject());
            continue;
        }
        if (word.startsWith("escaped-builtin="_s) || word.startsWith("lost-builtin="_s) || word.startsWith("used-builtin="_s) || word.startsWith("written-builtin="_s)) {
            StringView token = StringView(word).substring(equals + 1);
            BuiltinID named;
            for (uint32_t index = 0; index < builtins.builtinCount() && !named; ++index) {
                if (token == builtins.tokenOf(BuiltinID(index)))
                    named = BuiltinID(index);
            }
            if (!named)
                return printBadArgument(out, word);
            out.println(word);
            ValueFlowNodeID holding = word.startsWith("used"_s) ? bodies.nodes().ofHost(ValueFlowHostNode::UsedBuiltins) : word.startsWith("written"_s) ? bodies.nodes().ofHost(ValueFlowHostNode::WrittenBuiltins) : word.startsWith("lost"_s) ? lost : escaped;
            printWhyHeld(out, solution, holding, universe.objectOf(named));
            Vector<ValueFlowObjectID, 32> objects;
            universe.appendObjectsOf(named, objects);
            unsigned held = 0;
            for (ValueFlowObjectID object : objects.span().subspan(1)) {
                if (!solution.sets().contains(solver.possibleValuesOf(holding), object))
                    continue;
                if (++held > 6)
                    continue;
                out.println("  of module m", universe.moduleOf(object).index());
                printWhyHeld(out, solution, holding, object);
            }
            if (objects.size() > 1)
                out.println("  held: those of ", held, " of ", objects.size() - 1, " modules");
            continue;
        }
        if (word.startsWith("escaped-layout="_s)) {
            std::optional<uint32_t> layout = numberAfter(word, equals + 2);
            if (!layout || *layout >= program.layouts.size())
                return printBadArgument(out, word);
            out.println(word);
            solver.sets().forEachMember(solver.possibleValuesOf(escaped), [&](ValueFlowObjectID object) {
                if (universe.layoutOf(object) != LayoutID(*layout))
                    return;
                printObject(out, solution, object);
                out.println();
                printWhyHeld(out, solution, escaped, object);
            });
            continue;
        }
        std::optional<uint32_t> function = equals == notFound ? std::nullopt : numberAfter(word, equals + 2);
        if (!function || *function >= program.functions.size())
            return printBadArgument(out, word);
        out.println(word);
        if (word.startsWith("reached="_s))
            printWhyReached(out, solution, FunctionID(*function));
        else if (word.startsWith("escaped="_s))
            printWhyHeld(out, solution, escaped, universe.closureOf(FunctionID(*function)));
        else if (word.startsWith("lost="_s))
            printWhyHeld(out, solution, lost, universe.closureOf(FunctionID(*function)));
        else if (word.startsWith("values="_s) || word.startsWith("asks="_s)) {
            size_t colon = word.find(':');
            std::optional<uint32_t> site = colon == notFound ? std::nullopt : numberAfter(word, colon + 2);
            if (!site)
                return printBadArgument(out, word);
            for (const ValueFlowStatement& statement : bodies.statementsOf(*function)) {
                if (statement.site.index() != *site)
                    continue;
                if (word.startsWith("values="_s))
                    printValuesAt(out, solution, statement);
                else
                    printWhatIsAskedFor(out, solution, ValueFlowStatementID(static_cast<uint32_t>(&statement - bodies.statements().data())), 0);
            }
        } else
            return printBadArgument(out, word);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
