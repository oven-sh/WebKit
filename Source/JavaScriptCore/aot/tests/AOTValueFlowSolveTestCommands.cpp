/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTAddGenericVersion.h"
#include "AOTSupportTestCommands.h"
#include "AOTValueFlowAnalysis.h"
#include "AOTValueFlowResultsUnseenCode.h"

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

static void printCallees(PrintStream& out, ValueFlowSolution& solution, const ValueFlowStatement& statement, bool printsPrimitives)
{
    const ValueFlowUniverse& universe = solution.bodies().universe();
    out.print("f", statement.function.index(), " v", statement.site.index(), " ->");
    solution.sets().forEachMember(solution.possibleValuesOf(solution.bodies().operandsOf(statement)[0]), [&](ValueFlowObjectID callee) {
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
                bool looks = solution.sets().contains(solution.possibleValuesOf(subject), ValueFlowUniverse::unknownObject());
                bool gives = statement.result && solution.sets().contains(solver.possibleValuesOf(statement.result), ValueFlowUniverse::unknownObject());
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

void prepareForFlow(TestCommandContext& context)
{
    if (context.program->form() == IRForm::AsRead)
        guardTaintedPositions(*context.program, context.build, TaintedPositionsRun::BeforeFlow);
}

void runValueFlowSolveTestCommand(TestCommandContext& context)
{
    prepareForFlow(context);
    const Program& program = *context.program;
    bool printsCalls = false;
    bool printsPrimitives = false;
    bool printsUnreachedFunctions = false;
    bool printsUnseenCode = false;
    bool printsUsedBuiltins = false;
    bool printsProtocols = false;
    for (const String& word : context.arguments) {
        if (word == "calls"_s)
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
        else
            return printBadArgument(context.out, word);
    }

    ValueFlowSolution solution(*context.program, context.build);
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowBodies& bodies = solution.bodies();
    const ValueFlowSolver::Counters& counters = solver.counters();
    if (!solution.hasResult()) {
        context.out.println("stopped at a limit: edges-logged ", counters.edgesLogged, " rounds ", counters.rounds);
        return;
    }
    const ValueFlowUniverse& universe = bodies.universe();

    uint32_t functionsWithBody = 0;
    uint32_t functionsReached = 0;
    uint32_t calls = 0;
    uint32_t withOneFunction = 0;
    uint32_t withOneBuiltin = 0;
    uint32_t withSeveral = 0;
    uint32_t withUnknownCallee = 0;
    uint32_t withUntrackedCallee = 0;
    uint32_t withNoCallee = 0;
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
            uint32_t functions = 0;
            uint32_t builtins = 0;
            bool mayBeUnknown = false;
            bool mayBeUntracked = false;
            solution.sets().forEachMember(solution.possibleValuesOf(bodies.operandsOf(statement)[0]), [&](ValueFlowObjectID callee) {
                if (callee == ValueFlowUniverse::unknownObject())
                    mayBeUnknown = true;
                else if (callee == ValueFlowUniverse::untrackedObject())
                    mayBeUntracked = true;
                else if (universe.closureFunction(callee))
                    ++functions;
                else if (universe.isObject(callee))
                    ++builtins;
            });
            ++calls;
            if (mayBeUnknown)
                ++withUnknownCallee;
            else if (mayBeUntracked)
                ++withUntrackedCallee;
            else if (!functions && !builtins)
                ++withNoCallee;
            else if (functions == 1 && !builtins)
                ++withOneFunction;
            else if (!functions && builtins == 1)
                ++withOneBuiltin;
            else
                ++withSeveral;
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
        ValueSetID run = solver.possibleValuesOf(bodies.nodes().ofHost(ValueFlowHostNode::RunBuiltins));
        solution.sets().forEachMember(solver.possibleValuesOf(bodies.nodes().ofHost(ValueFlowHostNode::UsedBuiltins)), [&](ValueFlowObjectID object) {
            context.out.println(solution.sets().contains(run, object) ? "run " : "used ", program.builtins().tokenOf(universe.builtinOf(object)));
        });
    }
    if (printsProtocols)
        printProtocols(context.out, solution);
    if (printsCalls || printsUnreachedFunctions || printsUnseenCode || printsUsedBuiltins || printsProtocols)
        return;

    context.out.println("functions ", functionsWithBody, " reached ", functionsReached);
    context.out.println("calls ", calls, " one-function ", withOneFunction, " one-builtin ", withOneBuiltin, " several ", withSeveral, " unknown ", withUnknownCallee, " untracked ", withUntrackedCallee, " none ", withNoCallee);
    ValueSetID escaped = solver.possibleValuesOf(ValueFlowProgramNodes::escaped());
    uint32_t reachedAndEscaped = 0;
    for (uint32_t index = 0; index < program.functions.size(); ++index)
        reachedAndEscaped += program.functions[index].hasBody() && solver.isReached(FunctionID(index)) && solution.sets().contains(escaped, universe.closureOf(FunctionID(index)));
    auto sizeOf = [&](ValueFlowHostNode node) {
        return solution.sets().size(solver.possibleValuesOf(bodies.nodes().ofHost(node)));
    };
    context.out.println("escaped ", solution.sets().size(escaped), " objects ", universe.numberOfObjects(), " reached-functions-escaped ", reachedAndEscaped);
    context.out.println("used-builtins ", sizeOf(ValueFlowHostNode::UsedBuiltins), " written-builtins ", sizeOf(ValueFlowHostNode::WrittenBuiltins), " unseen-code ", sizeOf(ValueFlowHostNode::UnseenCodeRuns), " required-by-name ", sizeOf(ValueFlowHostNode::ModulesAreRequiredByName), " imported-by-name ", sizeOf(ValueFlowHostNode::ModulesAreImportedByName));
    context.out.println("nodes-of-bodies ", bodies.nodes().numberOfNodes(), " edges-of-bodies ", bodies.edges().size(), " statements ", bodies.statements().size(), " statements-made ", solver.numberOfStatements() - bodies.statements().size());
    context.out.println("rounds ", counters.rounds, " full-rounds ", counters.fullRounds, " statement-runs ", counters.statementRuns, " edges-logged ", counters.edgesLogged, " sets ", solution.sets().numberOfStoredSets());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
