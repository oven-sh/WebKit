/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAnalysisTestCommands.h"

#if ENABLE(AOT)

#include "AOTEffectSummaries.h"
#include "AOTForwardDataflow.h"
#include <wtf/EnumTraits.h>

namespace JSC { namespace AOT { namespace Typed {

static void runOnRequestedFunctions(TestCommandContext& context, CommandWords& words, NOESCAPE const Invocable<void(Session&)> auto& run)
{
    Program& program = *context.program;
    FlowPreparation preparation = prepareFlow(context, words);
    bool summarizesWithoutFlow = words.parseFlag("summaries"_s);
    if (summarizesWithoutFlow && preparation.flow)
        words.fail("summaries"_s);
    std::optional<FunctionID> onlyFunction = words.parseOptional("function"_s, functionParser(program));
    if (!words.finish())
        return;

    if (summarizesWithoutFlow) {
        preparation.flow.emplace();
        preparation.effects.emplace(computeEffectSummaries(program, *preparation.flow, context.build));
    }

    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        if (!program.at(id).hasBody() || (onlyFunction && *onlyFunction != id))
            continue;
        Session session(context, id, preparation.flowOrNull(), preparation.effectsOrNull());
        context.out.println("function f", id.index());
        run(session);
    }
}

static void printAvailableGuards(PrintStream& out, Session& session)
{
    const GuardAvailability& guards = session.analyses.guards();
    Vector<ValueID> guardsWithFacts = idsMatching<ValueID>(session.function.values.size(), [&](ValueID id) {
        return !guards.factsOf(id).facts.isEmpty();
    });

    for (BlockID block : session.analyses.cfg().reversePostOrder()) {
        GuardAvailability::Cursor cursor(guards, block);
        uint32_t numberOfValues = session.function.at(block).values.size();
        for (uint32_t index = 0; index <= numberOfValues; ++index) {
            cursor.advanceTo(index);
            Vector<ValueID> available;
            for (ValueID guard : guardsWithFacts) {
                if (cursor.stateOf(guard) == GuardState::Available)
                    available.append(guard);
            }
            dumpLine(out, ProgramPoint { block, index }, " available: "_s, available);
        }
    }
}

static void printGuardUses(PrintStream& out, Session& session)
{
    for (BlockID block : session.analyses.cfg().reversePostOrder()) {
        for (ValueID user : session.function.at(block).values) {
            const Value& value = session.function.at(user);
            value.forEachOperand([&](ValueID guard, uint32_t operandIndex) {
                if (value.operandKind(operandIndex) != OperandKind::Guard)
                    return;
                GuardState state = session.analyses.guardStateAt(guard, Use { Use::Kind::Operand, block, user, operandIndex, 0 });
                dumpLine(out, user, " guard "_s, guard, ": "_s, enumName(state));
            });
        }
    }
}

void runGuardAvailability(TestCommandContext& context)
{
    CommandWords words(context);
    bool printsPoints = words.parseFlag("points"_s);
    bool printsUses = words.parseFlag("uses"_s);
    runOnRequestedFunctions(context, words, [&](Session& session) {
        session.analyses.guards().dump(context.out);
        if (printsPoints)
            printAvailableGuards(context.out, session);
        if (printsUses)
            printGuardUses(context.out, session);
    });
}

void runFailedTests(TestCommandContext& context)
{
    CommandWords words(context);
    runOnRequestedFunctions(context, words, [&](Session& session) {
        std::span<const BlockID> order = session.analyses.cfg().reversePostOrder();
        Vector<ValueID> tests;
        for (BlockID block : order) {
            for (ValueID value : session.function.at(block).values) {
                if (provesOf(session.function.at(value).opcode) != Proof::None)
                    tests.append(value);
            }
        }
        for (BlockID block : order) {
            for (ValueID test : tests) {
                EarlierTest earlierTest = session.analyses.findEarlierTest(session.function.at(test), block);
                if (earlierTest == EarlierTest::Failed)
                    dumpLine(context.out, test, " at "_s, block, ": "_s, enumName(earlierTest));
            }
        }
    });
}

void runInductionVariables(TestCommandContext& context)
{
    CommandWords words(context);
    runOnRequestedFunctions(context, words, [&](Session& session) {
        const InductionVariables& inductionVariables = session.analyses.inductionVariables();
        inductionVariables.dump(context.out);
        for (const LoopInduction& loop : inductionVariables.loops()) {
            const InductionVariable* counter = loop.counter();
            dumpLine(context.out, "loop "_s, loop.header, " counter: "_s, counter ? counter->parameter : ValueID());
        }
    });
}

void runBlockFrequencies(TestCommandContext& context)
{
    CommandWords words(context);
    runOnRequestedFunctions(context, words, [&](Session& session) {
        session.analyses.blockFrequencies().dump(context.out, session.function);
    });
}

namespace {

struct ExecutedOnEveryPath {
    using State = BitVector;

    State stateAtEntry() { return { }; }
    void execute(State& state, const DataflowStep& step) { state.set(step.id.index()); }
    void executeTerminator(State&, const DataflowTerminatorStep&) { }
    void enterEdge(State&, Edge) { }

    bool merge(State& into, const State& from)
    {
        State before = into;
        into.filter(from);
        return into != before;
    }
};

struct ExecutedOnEveryPathWithThrowTransfer : ExecutedOnEveryPath {
    void executeThrow(State&, const DataflowStep&) { }
};

struct ExecutedOnSomePath : ExecutedOnEveryPath {
    bool merge(State& into, const State& from)
    {
        State before = into;
        into.merge(from);
        return into != before;
    }
};

struct ExecutedOnSomePathWithThrowTransfer : ExecutedOnSomePath {
    void executeThrow(State&, const DataflowStep&) { }
};

} // anonymous namespace

static_assert(!HasThrowTransfer<ExecutedOnEveryPath> && HasThrowTransfer<ExecutedOnEveryPathWithThrowTransfer>);
static_assert(!HasThrowTransfer<ExecutedOnSomePath> && HasThrowTransfer<ExecutedOnSomePathWithThrowTransfer>);

template<typename Problem>
static void printStatesAtHeads(PrintStream& out, Session& session)
{
    Problem problem;
    Vector<std::optional<BitVector>> heads = session.analyses.solveForward(problem);
    for (BlockID block : session.analyses.cfg().reversePostOrder()) {
        const std::optional<BitVector>& head = heads[block.index()];
        RELEASE_ASSERT(head);
        dumpLine(out, block, " head: "_s, idsIn<ValueID>(*head));
    }
}

void runForwardDataflow(TestCommandContext& context)
{
    CommandWords words(context);
    bool hasThrowTransfer = words.parseFlag("throw-transfer"_s);
    bool meetsByUnion = words.parseFlag("some-path"_s);
    runOnRequestedFunctions(context, words, [&](Session& session) {
        if (meetsByUnion && hasThrowTransfer)
            printStatesAtHeads<ExecutedOnSomePathWithThrowTransfer>(context.out, session);
        else if (meetsByUnion)
            printStatesAtHeads<ExecutedOnSomePath>(context.out, session);
        else if (hasThrowTransfer)
            printStatesAtHeads<ExecutedOnEveryPathWithThrowTransfer>(context.out, session);
        else
            printStatesAtHeads<ExecutedOnEveryPath>(context.out, session);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
