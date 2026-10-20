/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowAnalysisInternal.h"
#include "AOTFlowGraph.h"
#include "AOTFlowRecords.h"
#include "AOTSupportTestCommands.h"
#include "AOTValueFlowAnalysis.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

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

// valueflow-compare: solves the program with both analyses. The one with calling contexts is the finer, so what it finds has to lie inside what this one finds:
// every function it reaches is reached, and every function it lets a call operation enter is among that operation's callees, unless they hold the unknown object and the function has escaped.
// A model is left out, as a function and as a caller and a callee: no code is made of it, and the two analyses need not follow the same entries through their models.
// Prints what is missing, then both counts.
// valueflow-compare beyond: prints instead the first functions that this analysis reaches and the finer one does not, in the order of the rounds, each with the statement that reached it.
void runValueFlowCompareTestCommand(TestCommandContext& context)
{
    bool printsWhatIsReachedBeyond = context.arguments.size() == 1 && context.arguments[0] == "beyond"_s;
    if (!context.arguments.empty() && !printsWhatIsReachedBeyond)
        return printBadArgument(context.out, context.arguments[0]);

    Program& program = *context.program;
    std::unique_ptr<FlowSolvedProgram> finer = solveFlow(program, program.types, context.build);
    ValueFlowSolution solution(program, context.build);
    if (!solution.hasResult()) {
        context.out.println("stopped at a limit");
        return;
    }
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowBodies& bodies = solution.bodies();
    const ValueFlowUniverse& universe = bodies.universe();
    ValueSetID escaped = solver.valueOf(ValueFlowProgramNodes::escaped());

    if (printsWhatIsReachedBeyond) {
        constexpr size_t numberOfFunctionsPrinted = 60;
        Vector<FunctionID> beyond;
        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            if (program.functions[index].hasBody() && solver.isReached(FunctionID(index)) && !finer->graph->isReached(FunctionID(index)))
                beyond.append(FunctionID(index));
        }
        std::ranges::stable_sort(beyond, { }, [&](FunctionID function) {
            return solver.reachedBy(function).round;
        });
        for (FunctionID function : beyond.span().first(std::min(beyond.size(), numberOfFunctionsPrinted))) {
            ValueFlowReachWitness witness = solver.reachedBy(function);
            context.out.print("round ", witness.round, " f", function.index(), " <- ");
            if (!witness.statement) {
                context.out.println("entered by f", witness.enteringBody);
                continue;
            }
            const ValueFlowStatement& statement = solver.statementAt(witness.statement);
            if (statement.kind == ValueFlowStatementKind::Operation)
                context.out.println(statement.opcode, " f", statement.function.index(), " v", statement.site.index());
            else if (solver.isMadeByRule(witness.statement) && statement.kind != ValueFlowStatementKind::Escape)
                context.out.println("statement of kind ", static_cast<unsigned>(statement.kind), " detail ", solver.keyOf(witness.statement).detail, " payload ", solver.keyOf(witness.statement).payload, " set of ", solution.sets().size(solver.keyOf(witness.statement).set));
            else
                context.out.println("statement of kind ", static_cast<unsigned>(statement.kind));
        }
        context.out.println("beyond ", beyond.size());
        return;
    }

    uint32_t reachedByFiner = 0;
    uint32_t reached = 0;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID function(index);
        if (!program.at(function).hasBody() || program.at(function).role == FunctionRole::Model)
            continue;
        reachedByFiner += finer->graph->isReached(function);
        reached += solver.isReached(function);
        if (finer->graph->isReached(function) && !solver.isReached(function))
            context.out.println("not reached f", index);
    }

    Vector<FlowCallLink> links;
    for (const FlowCallLink& link : finer->records.callLinks()) {
        // A call of an async function also runs `then` of what the function returns, and a call of a built-in its callbacks: those are no callees of the operation, and they are among the functions reached.
        bool entersCallee = link.kind == FlowCallLinkKind::Call || link.kind == FlowCallLinkKind::Construct;
        if (!finer->graph->isHost(link.caller) && (program.at(link.caller).role == FunctionRole::Model || program.at(link.callee).role == FunctionRole::Model))
            continue;
        if (entersCallee && link.site && !finer->graph->isHost(link.caller) && isCall(program.at(link.caller).at(link.site).opcode))
            links.append(link);
    }
    auto placeOf = [](const FlowCallLink& link) {
        return std::tuple { link.caller, link.site, link.callee };
    };
    std::ranges::sort(links, { }, placeOf);
    links.shrink(std::ranges::unique(links, { }, placeOf).begin() - links.begin());

    uint32_t missingCallees = 0;
    size_t next = 0;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
            if (!isCall(statement.opcode))
                continue;
            while (next < links.size() && std::tuple { links[next].caller, links[next].site } < std::tuple { statement.function, statement.site })
                ++next;
            if (next == links.size() || links[next].caller != statement.function || links[next].site != statement.site)
                continue;
            ValueSetID callees = solution.valueOf(bodies.operandsOf(statement)[0]);
            bool mayBeUnknown = solution.sets().contains(callees, ValueFlowUniverse::unknownObject());
            for (; next < links.size() && links[next].caller == statement.function && links[next].site == statement.site; ++next) {
                ValueFlowObjectID closure = universe.closureOf(links[next].callee);
                if (solution.sets().contains(callees, closure) || (mayBeUnknown && solution.sets().contains(escaped, closure)))
                    continue;
                // A call of call, apply or a bound function enters the target, which is no callee of the operation.
                if (solver.isReached(links[next].callee) && (solution.sets().meet(callees, universe.builtinObjects()) || solution.sets().meet(callees, universe.otherCallables())))
                    continue;
                ++missingCallees;
                context.out.println("callee missing f", statement.function.index(), " v", statement.site.index(), " -> f", links[next].callee.index());
            }
        }
    }

    context.out.println("reached ", reached, " finer ", reachedByFiner, " callees-missing ", missingCallees);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
