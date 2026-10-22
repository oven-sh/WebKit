/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTAddGenericVersion.h"
#include "AOTBuiltinTable.h"
#include "AOTSplitBehindEntryChecks.h"
#include "AOTSupportTestCommands.h"
#include "AOTValueFlowAnalysis.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CallCounts {
    uint32_t calls { 0 };
    uint32_t withOneFunction { 0 };
    uint32_t withOneBuiltin { 0 };
    uint32_t withSeveral { 0 };
    uint32_t withUnknownCallee { 0 };
    uint32_t withUntrackedCallee { 0 };
    uint32_t withNoCallee { 0 };
};

} // anonymous namespace

static bool isCall(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Call:
    case Opcode::CallList:
    case Opcode::Construct:
    case Opcode::ConstructList:
    case Opcode::SuperConstruct:
    case Opcode::SuperConstructList:
        return true;
    default:
        return false;
    }
}

static void count(ValueFlowSolution& solution, const ValueFlowStatement& statement, CallCounts& counts)
{
    const ValueFlowUniverse& universe = solution.bodies().universe();
    ValueSetID callees = solution.valueOf(solution.bodies().operandsOf(statement)[0]);
    uint32_t functions = 0;
    uint32_t builtins = 0;
    bool mayBeUnknown = false;
    bool mayBeUntracked = false;
    solution.sets().forEachMember(callees, [&](ValueFlowObjectID callee) {
        if (callee == ValueFlowUniverse::unknownObject())
            mayBeUnknown = true;
        else if (callee == ValueFlowUniverse::untrackedObject())
            mayBeUntracked = true;
        else if (universe.closureFunction(callee))
            ++functions;
        else if (universe.isObject(callee))
            ++builtins;
    });
    ++counts.calls;
    if (mayBeUnknown)
        ++counts.withUnknownCallee;
    else if (mayBeUntracked)
        ++counts.withUntrackedCallee;
    else if (!functions && !builtins)
        ++counts.withNoCallee;
    else if (functions == 1 && !builtins)
        ++counts.withOneFunction;
    else if (!functions && builtins == 1)
        ++counts.withOneBuiltin;
    else
        ++counts.withSeveral;
}

static void printCallees(PrintStream& out, ValueFlowSolution& solution, const ValueFlowStatement& statement, bool printsPrimitives)
{
    const ValueFlowUniverse& universe = solution.bodies().universe();
    out.print("f", statement.function.index(), " v", statement.site.index(), " ->");
    solution.sets().forEachMember(solution.valueOf(solution.bodies().operandsOf(statement)[0]), [&](ValueFlowObjectID callee) {
        if (callee == ValueFlowUniverse::unknownObject())
            out.print(" unknown");
        else if (callee == ValueFlowUniverse::untrackedObject())
            out.print(" untracked");
        else if (FunctionID function = universe.closureFunction(callee))
            out.print(" f", function.index());
        else if (BuiltinID builtin = universe.builtinOf(callee))
            out.print(" ", solution.program().builtins().tokenOf(builtin));
        else if (universe.isObject(callee))
            out.print(" object");
        else if (printsPrimitives && universe.keyOf(callee))
            out.print(" key");
        else if (printsPrimitives)
            out.print(" primitive", callee.index());
    });
    out.println();
}

// For each operation that runs a protocol of the language: how often it is reached, how often what it looks at holds the unknown object, how often what it gives does,
// and the first places where it gives the unknown object though it looks at known values alone.
static void printProtocols(PrintStream& out, ValueFlowSolution& solution)
{
    constexpr unsigned numberOfPlacesPrinted = 12;
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowBodies& bodies = solution.bodies();
    for (Opcode opcode : { Opcode::IteratorOpen, Opcode::IteratorResume, Opcode::AppendSpread, Opcode::Await }) {
        uint32_t reached = 0;
        uint32_t looksAtUnknownObject = 0;
        uint32_t givesUnknownObject = 0;
        Vector<const ValueFlowStatement*> places;
        for (uint32_t index = 0; index < solution.program().functions.size(); ++index) {
            if (!solver.isReached(FunctionID(index)))
                continue;
            for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
                if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != opcode)
                    continue;
                ++reached;
                const ValueFlowOperand& subject = bodies.operandsOf(statement)[opcode == Opcode::AppendSpread ? 1 : 0];
                bool looks = solution.sets().contains(solution.valueOf(subject), ValueFlowUniverse::unknownObject());
                bool gives = statement.result && solution.sets().contains(solver.valueOf(statement.result), ValueFlowUniverse::unknownObject());
                looksAtUnknownObject += looks;
                givesUnknownObject += gives;
                if (gives && !looks)
                    places.append(&statement);
            }
        }
        out.println(opcode, " reached ", reached, " looks-at-unknown ", looksAtUnknownObject, " gives-unknown ", givesUnknownObject, " gives-unknown-from-known ", places.size());
        for (const ValueFlowStatement* statement : places.span().first(std::min<size_t>(places.size(), numberOfPlacesPrinted)))
            out.println("  f", statement->function.index(), " v", statement->site.index());
    }
}

// valueflow-solve [full] [times] [calls] [primitives] [unreached] [unseen] [used] [protocols] [views]: solves the program and prints what the solve found: in numbers, or call by call, or the functions that are never reached.
// primitives: `calls` also lists what is no object among the callees, as valueflow-why prints it: a test that calls a loop variable, an awaited value or what a read answers states all that flow lets it be.
void prepareAsInFrontOfFlow(TestCommandContext& context)
{
    guardTaintedPositions(*context.program, context.build, TaintedPositionsRun::BeforeFlow);
    ProgramEditor editor(*context.program);
    splitBehindEntryChecks(editor, nullptr, context.build);
}

void prepareForFlow(TestCommandContext& context)
{
    auto isGiven = [&](ASCIILiteral word) {
        return std::ranges::any_of(context.arguments, [&](const String& argument) { return argument == word; });
    };
    if (isGiven("split"_s))
        return prepareAsInFrontOfFlow(context);
    if (!isGiven("unguarded"_s) && context.program->form() == IRForm::AsRead)
        guardTaintedPositions(*context.program, context.build, TaintedPositionsRun::BeforeFlow);
}

void runValueFlowSolveTestCommand(TestCommandContext& context)
{
    prepareForFlow(context);
    const Program& program = *context.program;
    bool runsEveryStatementInEveryRound = false;
    bool printsTimes = false;
    bool printsCalls = false;
    bool printsPrimitives = false;
    bool printsUnreachedFunctions = false;
    bool printsUnseenCode = false;
    bool printsUsedBuiltins = false;
    bool printsProtocols = false;
    bool printsViews = false;
    for (const String& word : context.arguments) {
        if (word == "full"_s)
            runsEveryStatementInEveryRound = true;
        else if (word == "times"_s)
            printsTimes = true;
        else if (word == "calls"_s)
            printsCalls = true;
        else if (word == "primitives"_s)
            printsPrimitives = true;
        else if (word == "unreached"_s)
            printsUnreachedFunctions = true;
        else if (word == "unseen"_s)
            printsUnseenCode = true;
        else if (word == "used"_s)
            printsUsedBuiltins = true;
        else if (word == "protocols"_s)
            printsProtocols = true;
        else if (word == "views"_s)
            printsViews = true;
        else if (word != "split"_s && word != "unguarded"_s)
            return printBadArgument(context.out, word);
    }

    ValueFlowSolution solution(*context.program, context.build, runsEveryStatementInEveryRound ? ValueFlowSolver::Rounds::Full : ValueFlowSolver::Rounds::Due);
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowBodies& bodies = solution.bodies();
    if (!solution.hasResult()) {
        context.out.println("stopped at a limit: edges-logged ", solver.counters().edgesLogged, " rounds ", solver.counters().rounds);
        return;
    }

    uint32_t functionsWithBody = 0;
    uint32_t functionsReached = 0;
    CallCounts counts;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!program.functions[index].hasBody())
            continue;
        ++functionsWithBody;
        if (!solver.isReached(FunctionID(index))) {
            if (printsUnreachedFunctions)
                context.out.println("unreached f", index);
            continue;
        }
        ++functionsReached;
        for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
            if (!isCall(statement.opcode))
                continue;
            count(solution, statement, counts);
            if (printsCalls)
                printCallees(context.out, solution, statement, printsPrimitives);
        }
    }
    if (printsUnseenCode) {
        for (const UnseenCodeRecord& record : unseenCodeRecordsOf(solution)) {
            context.out.print("unseen ", nameOf(record.cause), " ");
            if (record.builtin != Wire::none)
                context.out.print(program.builtins().tokenOf(BuiltinID(record.builtin)), " ");
            if (record.witness.function != Wire::none)
                context.out.print("f", record.witness.function);
            if (record.witness.value != Wire::none)
                context.out.print(":v", record.witness.value);
            context.out.println();
        }
        context.out.println("unseen-code ", unseenCodeCanRun(solution) ? "yes" : "no");
        context.out.println("modules-by-name ", modulesAreLoadedByName(solution) ? "yes" : "no");
        for (FunctionID function : functionsHeldButNotCalled(solution))
            context.out.println("held f", function.index());
    }
    if (printsUsedBuiltins) {
        ValueSetID run = solver.valueOf(bodies.nodes().ofHost(ValueFlowHostNode::RunBuiltins));
        solution.sets().forEachMember(solver.valueOf(bodies.nodes().ofHost(ValueFlowHostNode::UsedBuiltins)), [&](ValueFlowObjectID object) {
            context.out.println(solution.sets().contains(run, object) ? "run " : "used ", program.builtins().tokenOf(bodies.universe().builtinOf(object)));
        });
    }
    if (printsProtocols)
        printProtocols(context.out, solution);
    if (printsViews) {
        ValueSetMaker maker(solution.bodies().sets());
        ValueSetID escaped = solver.valueOf(ValueFlowProgramNodes::escaped());
        ValueSetID lost = solver.valueOf(ValueFlowProgramNodes::lost());
        for (uint32_t index = 0; index < bodies.nodes().numberOfNodes(); ++index) {
            ValueFlowNodeID node(index);
            ValueSetID value = solver.valueOf(node);
            if (node != ValueFlowProgramNodes::escaped() && solution.sets().contains(value, ValueFlowUniverse::unknownObject()))
                value = maker.intersect(value, ValueSets::complement(maker.intersect(escaped, bodies.universe().knownObjects())));
            if (node != ValueFlowProgramNodes::lost() && solution.sets().contains(value, ValueFlowUniverse::untrackedObject()))
                value = maker.intersect(value, ValueSets::complement(maker.intersect(lost, bodies.universe().knownObjects())));
            if (node != ValueFlowProgramNodes::escaped() && solution.sets().contains(value, ValueFlowUniverse::primitive(PrimitiveSet::String)))
                value = maker.intersect(value, ValueSets::complement(maker.intersect(bodies.universe().strings(), bodies.universe().keys())));
            if (node != ValueFlowProgramNodes::escaped() && solution.sets().contains(value, ValueFlowUniverse::primitive(PrimitiveSet::Symbol)))
                value = maker.intersect(value, ValueSets::complement(maker.intersect(bodies.universe().symbols(), bodies.universe().keys())));
            if (value != ValueSets::nothing)
                context.out.println("view n", index, " ", solution.sets().size(value), " ", solution.sets().hashOfMembers(value), " keys ", solution.sets().size(maker.intersect(value, bodies.universe().keys())), " closures ", solution.sets().size(maker.intersect(value, bodies.universe().closures())), " builtins ", solution.sets().size(maker.intersect(value, bodies.universe().builtinObjects())), " objects ", solution.sets().size(maker.intersect(value, bodies.universe().knownObjects())), solution.sets().contains(value, ValueFlowUniverse::unknownObject()) ? " unknown" : "", solution.sets().contains(value, ValueFlowUniverse::untrackedObject()) ? " untracked" : "", solver.graph().maskOf(node) != ValueSets::anything ? " masked" : "", solver.graph().isRetired(node) ? " retired" : "", solver.graph().flowsIntoNodeCoveredBy(ValueFlowUniverse::unknownObject(), node) ? " escapes" : "", " f", bodies.nodes().functionOf(node) ? bodies.nodes().functionOf(node).index() : 0);
        }
        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            if (solver.isReached(FunctionID(index)))
                context.out.println("view reached f", index);
        }
        for (uint32_t index = 0; index < bodies.universe().numberOfObjects(); ++index) {
            if (OptionSet<ObjectMutation> marks = solver.marksOf(ValueFlowObjectID(index)))
                context.out.println("view marks o", index, " ", marks.toRaw());
        }
        for (uint32_t index = 0; index < program.builtins().builtinCount(); ++index) {
            if (OptionSet<BuiltinUse> uses = solver.usesOf(BuiltinID(index)))
                context.out.println("view uses b", index, " ", uses.toRaw());
        }
        return;
    }
    if (printsCalls || printsUnreachedFunctions || printsUnseenCode || printsUsedBuiltins || printsProtocols)
        return;

    const ValueFlowSolver::Counters& counters = solver.counters();
    context.out.println("functions ", functionsWithBody, " reached ", functionsReached);
    context.out.println("calls ", counts.calls, " one-function ", counts.withOneFunction, " one-builtin ", counts.withOneBuiltin, " several ", counts.withSeveral, " unknown ", counts.withUnknownCallee, " untracked ", counts.withUntrackedCallee, " none ", counts.withNoCallee);
    ValueSetID escaped = solver.valueOf(ValueFlowProgramNodes::escaped());
    uint32_t reachedAndEscaped = 0;
    for (uint32_t index = 0; index < program.functions.size(); ++index)
        reachedAndEscaped += program.functions[index].hasBody() && solver.isReached(FunctionID(index)) && solution.sets().contains(escaped, bodies.universe().closureOf(FunctionID(index)));
    auto sizeOf = [&](ValueFlowHostNode node) {
        return solution.sets().size(solver.valueOf(bodies.nodes().ofHost(node)));
    };
    context.out.println("escaped ", solution.sets().size(escaped), " objects ", bodies.universe().numberOfObjects(), " reached-functions-escaped ", reachedAndEscaped);
    context.out.println("used-builtins ", sizeOf(ValueFlowHostNode::UsedBuiltins), " written-builtins ", sizeOf(ValueFlowHostNode::WrittenBuiltins), " unseen-code ", sizeOf(ValueFlowHostNode::UnseenCodeRuns), " required-by-name ", sizeOf(ValueFlowHostNode::ModulesAreRequiredByName), " imported-by-name ", sizeOf(ValueFlowHostNode::ModulesAreImportedByName));
    context.out.println("nodes-of-bodies ", bodies.nodes().numberOfNodes(), " edges-of-bodies ", bodies.edges().size(), " statements ", bodies.statements().size(), " statements-made ", solver.numberOfStatements() - bodies.statements().size());
    context.out.println("rounds ", counters.rounds, " full-rounds ", counters.fullRounds, " statement-runs ", counters.statementRuns, " edges-logged ", counters.edgesLogged, " sets ", solution.sets().numberOfStoredSets());
    if (printsTimes) {
        context.out.println("seconds bodies ", solution.timeToReduceBodies().seconds(), " solve ", solution.timeToSolve().seconds(), " rules ", counters.rules.seconds(), " merge ", counters.merge.seconds(), " values ", counters.values.seconds());
        context.out.println("bytes solver ", solver.memoryInBytes(), " sets ", solution.sets().memoryInBytes());
        const ValueFlowGraph::Counters& graphCounters = solver.graph().counters();
        context.out.println("graph nodes ", solver.graph().numberOfNodes(), " edges ", solver.graph().numberOfEdges(), " evaluations ", graphCounters.evaluations, " edges-looked-at ", graphCounters.edgesLookedAt, " waves ", graphCounters.waves, " seconds rows ", graphCounters.rows.seconds(), " waving ", graphCounters.waving.seconds());
        uint64_t membersListed = 0;
        uint32_t nodesHoldingUnknownObject = 0;
        for (uint32_t index = 0; index < solver.graph().numberOfNodes(); ++index) {
            ValueSetID value = solver.valueOf(ValueFlowNodeID(index));
            if (ValueSets::isComplement(value))
                continue;
            membersListed += solution.sets().size(value);
            nodesHoldingUnknownObject += solution.sets().contains(value, ValueFlowUniverse::unknownObject());
        }
        context.out.println("covered flowing-into-covered-nodes ", graphCounters.nodesFlowingIntoCoveredNodes, " retired ", graphCounters.nodesRetired, " edges-straight-into-covered-nodes ", graphCounters.edgesStraightIntoCoveredNodes, " saturated ", counters.nodesSaturated, " tests-whether-sets-meet ", counters.testsWhetherSetsMeet, " made-nodes-at-rest ", counters.madeNodesAtRest, " holding-unknown-object ", nodesHoldingUnknownObject, " members-listed ", membersListed);
        struct Line {
            ValueFlowStatementKind kind;
            Opcode opcode;
            uint64_t runs;
            uint64_t runsThatAddedNothing;
            uint64_t runsThatWereNotDueAndAdded;
            uint64_t connections;
            uint64_t edgesLogged;
        };
        Vector<Line> lines;
        solver.forEachKindOfStatementRun([&](ValueFlowStatementKind kind, Opcode opcode, const auto& counts) {
            lines.append({ kind, opcode, counts.runs, counts.runsThatAddedNothing, counts.runsThatWereNotDueAndAdded, counts.connections, counts.edgesLogged });
        });
        std::ranges::sort(lines, std::greater { }, &Line::connections);
        for (const Line& line : lines.span().first(std::min<size_t>(lines.size(), 16))) {
            if (line.kind == ValueFlowStatementKind::Operation)
                context.out.println("  ", line.opcode, " runs ", line.runs, " added-nothing ", line.runsThatAddedNothing, " not-due-and-added ", line.runsThatWereNotDueAndAdded, " connections ", line.connections, " edges ", line.edgesLogged);
            else
                context.out.println("  statement-kind ", static_cast<unsigned>(line.kind), " runs ", line.runs, " added-nothing ", line.runsThatAddedNothing, " not-due-and-added ", line.runsThatWereNotDueAndAdded, " connections ", line.connections, " edges ", line.edgesLogged);
        }
        for (const ValueFlowSolver::Round& round : solver.rounds())
            context.out.println("  round", round.isFull ? " full" : "", " turns ", round.turns, " runs ", round.statementRuns, " functions-reached ", round.functionsReached, " nodes-made ", round.nodesMade, " edges-added ", round.edgesAdded, " nodes-moved ", round.nodesMoved, " waves ", round.waves, " evaluations ", round.evaluations);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
