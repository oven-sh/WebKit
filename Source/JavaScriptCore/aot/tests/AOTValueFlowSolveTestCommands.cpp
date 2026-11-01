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
#include "AOTValueFlowResults.h"
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

static void printCallees(PrintStream& out, ValueFlowSolution& solution, const ValueFlowStatement& statement, bool printsPrimitives, bool printsWhatIsSeen)
{
    const ValueFlowUniverse& universe = solution.bodies().universe();
    out.print("f", statement.function.index(), " v", statement.site.index(), " ->");
    const ValueFlowOperand& operand = solution.bodies().operandsOf(statement)[0];
    ValueSetID possible = solution.possibleValuesOf(operand);
    auto printCallee = [&](ValueFlowObjectID callee) {
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
    };
    solution.sets().forEachMember(possible, printCallee);
    if (ValueSetID seen = printsWhatIsSeen && operand.node ? solution.solver().seenValuesOf(operand.node) : ValueSets::nothing; !ValueSets::isComplement(seen)) {
        solution.sets().forEachMember(seen, [&](ValueFlowObjectID callee) {
            if (!solution.sets().contains(possible, callee))
                printCallee(callee);
        });
    }
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

static void printUnseenCode(PrintStream& out, ValueFlowSolution& solution)
{
    for (const UnseenCodeRecord& record : unseenCodeRecordsOf(solution)) {
        out.print("unseen ", nameOf(record.cause), " ");
        if (record.builtin != Wire::none)
            out.print(solution.program().builtins().tokenOf(BuiltinID(record.builtin)), " ");
        if (record.witness.function != Wire::none)
            out.print("f", record.witness.function);
        if (record.witness.value != Wire::none)
            out.print(":v", record.witness.value);
        out.println();
    }
    out.println("unseen-code ", unseenCodeCanRun(solution) ? "yes" : "no");
    out.println("modules-by-name ", modulesAreLoadedByName(solution) ? "yes" : "no");
    for (FunctionID function : functionsHeldButNotCalled(solution))
        out.println("held f", function.index());
}

static void printUnseenCodeInEveryOrder(TestCommandContext& context)
{
    constexpr unsigned numberOfOrders = 512;
    Vector<std::pair<String, Vector<unsigned>>> answers;
    for (unsigned order = 0; order < numberOfOrders; ++order) {
        uint32_t seed = 1 + order / 256 + 4 * (order % 256);
        ValueFlowSolution solution(*context.program, context.build, ValueFlowSolver::Rounds::Due, ValueFlowSolution::Witnesses::None, UINT32_MAX, { }, ValueFlowSolution::WaysOfValues::AreNotAskedFor, ValueFlowSolution::CoveredMembers::StayListed, ValueFlowSolution::Reports::AreLeftOut, { .seedOfSchedule = seed });
        StringPrintStream out;
        printUnseenCode(out, solution);
        String answer = out.toString();
        size_t index = answers.findIf([&](const auto& known) { return known.first == answer; });
        if (index == notFound) {
            index = answers.size();
            answers.append({ answer, { } });
        }
        answers[index].second.append(order);
    }
    context.out.println("orders ", numberOfOrders, " answers ", answers.size());
    for (const auto& [answer, orders] : answers) {
        context.out.print("in ", orders.size(), " orders, the first ", orders.first(), ":\n", answer);
    }
}

static void printFeaturesInEveryOrder(TestCommandContext& context)
{
    constexpr unsigned numberOfOrders = 128;
    constexpr unsigned isListed = UINT_MAX;
    struct StateOfPlace {
        BuiltinLocationState state;
        unsigned order;
    };
    Vector<std::pair<Vector<uint32_t>, Vector<unsigned>>> answers;
    HashMap<uint64_t, StateOfPlace> statesOfPlaces;
    Vector<String> flips;
    auto nameOfState = [](BuiltinLocationState state) { return state == BuiltinLocationState::Constant ? "Constant"_s : "Unknown"_s; };
    for (unsigned order = 0; order < numberOfOrders; ++order) {
        Wire::CompileOptions options = *context.options;
        options.testing = static_cast<Wire::TestingSwitch>(static_cast<uint32_t>(options.testing) | static_cast<uint32_t>(Wire::TestingSwitch::DelayFlowRules));
        options.bisection.flowSchedule.number = order;
        options.threadCount = 1;
        BuildContext build(options);
        FlowResults results = analyzeValueFlow(*context.program, context.program->types, build);
        Vector<uint32_t> features;
        for (const FeatureUseRecord& use : results.featureUses())
            features.append(static_cast<uint32_t>(use.feature));
        std::ranges::sort(features);
        features.shrink(std::ranges::unique(features).begin() - features.begin());
        size_t index = answers.findIf([&](const auto& known) { return known.first == features; });
        if (index == notFound) {
            index = answers.size();
            answers.append({ WTF::move(features), { } });
        }
        answers[index].second.append(order);
        for (const BuiltinLocationFlow& location : results.builtinLocations()) {
            uint64_t place = (static_cast<uint64_t>(location.holder.index()) << 32 | location.key.index()) + 1;
            StateOfPlace& seen = statesOfPlaces.add(place, StateOfPlace { location.state, order }).iterator->value;
            if (seen.state == location.state || seen.order == isListed)
                continue;
            flips.append(makeString("flip "_s, context.program->builtins().tokenOf(location.holder), " key"_s, location.key.index(), ' ', nameOfState(seen.state), " under "_s, seen.order, ", "_s, nameOfState(location.state), " under "_s, order));
            seen.order = isListed;
        }
    }
    context.out.println("orders ", numberOfOrders, " features ", answers.size());
    for (const auto& [features, orders] : answers) {
        context.out.print("in ", orders.size(), " orders, the first ", orders.first(), ": ", features.size(), " features:");
        for (uint32_t feature : features)
            context.out.print(" ", feature);
        context.out.println();
    }
    context.out.println("orders ", numberOfOrders, " flips ", flips.size());
    for (const String& flip : flips)
        context.out.println(flip);
}

static void printLostWithEdgesLetGo(PrintStream& out, ValueFlowSolution& solution)
{
    const ValueFlowSolver& solver = solution.solver();
    auto print = [&](ValueFlowNodeID node) {
        if (const ValueFlowNodeKey* key = solver.keyOfMadeNode(node))
            out.print("made kind ", static_cast<unsigned>(key->kind), " detail ", key->detail, " payload ", key->payload);
        else
            out.print("no made node");
    };
    for (const ValueFlowSolver::LostWithEdgeLetGo& entry : solver.lostWithEdgesLetGo()) {
        out.print("let-go from ");
        print(entry.from);
        out.print(" to ");
        print(entry.to);
        out.print(" loses");
        solution.sets().forEachMember(entry.lost, [&](ValueFlowObjectID member) {
            out.print(" ", member.index());
        });
        out.println();
    }
    out.println("let-go-loses ", solver.lostWithEdgesLetGo().size());
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
    bool printsLibraryFunctionsLacked = false;
    bool printsProtocols = false;
    bool printsNodesGivenMore = false;
    bool printsCells = false;
    bool printsWrittenObjects = false;
    bool printsWhatIsSeen = false;
    bool printsLostWithEdgesLetGo = false;
    ValueFlowSolution::CoveredMembers coveredMembers = ValueFlowSolution::CoveredMembers::StayListed;
    for (const String& word : context.arguments) {
        if (word == "every-order"_s) {
            printUnseenCodeInEveryOrder(context);
            if (context.options)
                printFeaturesInEveryOrder(context);
            return;
        }
        if (word == "let-go"_s)
            printsLostWithEdgesLetGo = true;
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
        else if (word == "lacked"_s)
            printsLibraryFunctionsLacked = true;
        else if (word == "protocols"_s)
            printsProtocols = true;
        else if (word == "given-more"_s)
            printsNodesGivenMore = true;
        else if (word == "cells"_s)
            printsCells = true;
        else if (word == "written-objects"_s)
            printsWrittenObjects = true;
        else if (word == "proofs"_s)
            coveredMembers = ValueFlowSolution::CoveredMembers::AreLeftOut;
        else if (word == "seen"_s) {
            printsWhatIsSeen = true;
            coveredMembers = ValueFlowSolution::CoveredMembers::AreLeftOut;
        }
        else
            return printBadArgument(context.out, word);
    }

    ValueFlowSolution solution(*context.program, context.build, ValueFlowSolver::Rounds::Due, ValueFlowSolution::Witnesses::None, UINT32_MAX, { }, ValueFlowSolution::WaysOfValues::AreAskedFor, coveredMembers, ValueFlowSolution::Reports::GoToBuild, { .looksAtEdgesLetGo = printsLostWithEdgesLetGo });
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowBodies& bodies = solution.bodies();
    const ValueFlowSolver::Counters& counters = solver.counters();
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
                printCallees(context.out, solution, statement, printsPrimitives, printsWhatIsSeen);
        }
    }
    if (printsUnseenCode)
        printUnseenCode(context.out, solution);
    if (printsLostWithEdgesLetGo)
        printLostWithEdgesLetGo(context.out, solution);
    if (printsUsedBuiltins) {
        ValueSetID run = solver.possibleValuesOf(bodies.nodes().ofHost(ValueFlowHostNode::RunBuiltins));
        solution.sets().forEachMember(solver.possibleValuesOf(bodies.nodes().ofHost(ValueFlowHostNode::UsedBuiltins)), [&](ValueFlowObjectID object) {
            context.out.println(solution.sets().contains(run, object) ? "run " : "used ", program.builtins().tokenOf(universe.builtinOf(object)));
        });
    }
    if (printsWrittenObjects) {
        solution.sets().forEachMember(solver.possibleValuesOf(bodies.nodes().ofHost(ValueFlowHostNode::BuiltinObjectsThatStatementsWrite)), [&](ValueFlowObjectID object) {
            context.out.println("written-object ", program.builtins().tokenOf(universe.builtinOf(object)));
        });
    }
    if (printsLibraryFunctionsLacked) {
        for (size_t function : solver.libraryFunctionsLacked())
            context.out.println("lacked ", function);
    }
    if (printsProtocols)
        printProtocols(context.out, solution);
    if (printsNodesGivenMore) {
        context.out.println("given-more ", solver.numberOfNodesGivenMore());
        for (uint32_t index = 0; solver.input().seedOfWidening && index < program.functions.size(); ++index) {
            if (!solver.isReached(FunctionID(index)) || !isWrittenInProgram(program.functions[index]))
                continue;
            for (uint32_t node = bodies.nodes().firstNodeOf(FunctionID(index)); node < bodies.nodes().endOfNodesOf(FunctionID(index)); ++node) {
                ValueFlowSolver::GivenToNode given = solver.whatIsGivenTo(ValueFlowNodeID(node));
                if (given == ValueFlowSolver::GivenToNode::Nothing)
                    continue;
                context.out.print("given f", index);
                if (ValueID value = solver.subjectOf(ValueFlowNodeID(node)).value)
                    context.out.print(":v", value.index());
                else
                    context.out.print(" +", node - bodies.nodes().firstNodeOf(FunctionID(index)));
                if (given == ValueFlowSolver::GivenToNode::WhatAnotherNodeOfBodyHolds)
                    context.out.println(" what +", solver.nodeWhoseValueIsGivenTo(ValueFlowNodeID(node)).index() - bodies.nodes().firstNodeOf(FunctionID(index)), " holds");
                else
                    context.out.println(given == ValueFlowSolver::GivenToNode::UnknownObject ? " the unknown object" : " a value nobody knows");
            }
        }
    }
    if (printsCells)
        context.out.println("names-with-several-cells ", counters.namesWithSeveralCells);
    if (printsLostWithEdgesLetGo || printsCalls || printsUnreachedFunctions || printsUnseenCode || printsUsedBuiltins || printsLibraryFunctionsLacked || printsProtocols || printsNodesGivenMore || printsCells || printsWrittenObjects)
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
