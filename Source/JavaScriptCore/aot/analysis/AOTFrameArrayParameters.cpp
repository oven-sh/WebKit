/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameArrayParameters.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTIR.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct HandedOnArray {
    FunctionID callee;
    uint32_t entryParameter { 0 };
    uint32_t parameters { 0 };
};

struct ScannedParameters {
    uint32_t asked { 0 };
    uint32_t refused { 0 };
    Vector<HandedOnArray> handedOn;
};

struct FunctionsThatEndTheirTailCalls {
    BitVector calledDirectly;
    BitVector calledByValue;
};

} // anonymous namespace

static bool mayBeHandedArrays(const Function& function)
{
    if (!function.hasBody() || !function.hasOrdinaryFrame() || function.isUnsplitCoroutine())
        return false;
    return std::ranges::none_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return maySuspend(function.at(id).opcode);
        });
    });
}

static ScannedParameters scanParametersHandedArrays(const Program& program, const Function& function, uint32_t asked)
{
    CFG cfg(function);
    ValueIdentities identities(function, cfg);
    ScannedParameters result { asked, 0, { } };
    uint32_t& refused = result.refused;
    Vector<uint32_t> arrivesIn(FillWith { }, function.blocks.size(), 0);
    Vector<uint32_t> mayBe(FillWith { }, function.values.size(), 0);
    const CompactList<ValueID>& entryParameters = function.at(entryBlock).parameters;

    auto parametersIn = [&](ValueID id) -> uint32_t { return id ? mayBe[id.index()] : 0; };
    auto arrivingOn = [&](Edge edge) {
        uint32_t arriving = arrivesIn[edge.from.index()];
        const Terminator& terminator = function.at(edge.from).terminator;
        if (edge.isHandlerEdge() || terminator.kind != TerminatorKind::Branch)
            return arriving;
        std::optional<FrameArrayTest> test = testOfFrameArray(program, function.at(terminator.conditionOperand()));
        bool isTakenEdge = !edge.targetIndex;
        if (!test || test->passes == isTakenEdge)
            return arriving;
        size_t tested = entryParameters.find(identities.identityOf(test->tested));
        return tested == notFound || tested >= maxFrameArrayParameters ? arriving : arriving & ~(1u << tested);
    };
    auto add = [&](BlockID block, ValueID id, uint32_t parameters) {
        uint32_t& known = mayBe[id.index()];
        parameters &= arrivesIn[block.index()] & ~known;
        if (!parameters)
            return false;
        known |= parameters;
        return true;
    };
    auto handOn = [&](const Value& call, uint32_t operandIndex, uint32_t parameters) {
        CalleeList callees = calleesOfCall(program, call);
        bool isArgument = !call.operandUses(operandIndex).containsAny(callingUses);
        std::optional<ProvenValue<std::span<const FunctionID>>> functions = callees.functionsAlone();
        if (!isArgument || !functions) {
            refused |= parameters;
            return;
        }
        for (FunctionID callee : functions->value()) {
            std::optional<uint32_t> entryParameter = entryParameterOf(program.at(callee), call, operandIndex);
            if (!entryParameter || *entryParameter >= maxFrameArrayParameters) {
                refused |= parameters;
                return;
            }
            result.handedOn.append(HandedOnArray { callee, *entryParameter, parameters });
        }
    };

    arrivesIn[entryBlock.index()] = asked;
    Vector<BlockID, 16> worklist { entryBlock };
    while (!worklist.isEmpty()) {
        forEachOutgoingEdge(function, worklist.takeLast(), [&](Edge edge, const Target& target) {
            uint32_t arriving = arrivingOn(edge);
            uint32_t& known = arrivesIn[target.block.index()];
            if (!(arriving & ~known))
                return;
            known |= arriving;
            worklist.append(target.block);
        });
    }

    for (uint32_t index = 0; index < entryParameters.size() && index < maxFrameArrayParameters; ++index)
        mayBe[entryParameters[index].index()] = asked & 1u << index;
    for (bool changed = true; changed;) {
        changed = false;
        for (BlockID block : cfg.reversePostOrder()) {
            if (!arrivesIn[block.index()])
                continue;
            for (ValueID id : function.at(block).values) {
                uint32_t given = 0;
                forEachReturnedOperand(function, function.at(id), [&](ValueID operand) {
                    given |= parametersIn(operand);
                });
                changed |= add(block, id, given);
            }
            forEachOutgoingEdge(function, block, [&](Edge edge, const Target& target) {
                uint32_t arriving = arrivingOn(edge);
                const CompactList<ValueID>& targetParameters = function.at(target.block).parameters;
                for (uint32_t index = 0; arriving && index < targetParameters.size(); ++index) {
                    if (ValueID passed = valuePassedOn(function, edge, index))
                        changed |= add(target.block, targetParameters[index], parametersIn(passed) & arriving);
                }
            });
        }
    }

    for (BlockID block : cfg.reversePostOrder()) {
        uint32_t arriving = arrivesIn[block.index()];
        if (!arriving)
            continue;
        for (ValueID id : function.at(block).values) {
            const Value& user = function.at(id);
            user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                uint32_t parameters = parametersIn(operand) & arriving & ~refused;
                if (!parameters || user.operandKind(operandIndex) == OperandKind::Guard)
                    return;
                if (givesSameArray(user.opcode, operandIndex) || readsArrayWhereItLies(program, function, user, operandIndex))
                    return;
                if (effectRuleOf(user.opcode) == EffectRule::Call)
                    handOn(user, operandIndex, parameters);
                else
                    refused |= parameters;
            });
        }
        const Terminator& terminator = function.at(block).terminator;
        terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (terminator.operandKind(operandIndex) != OperandKind::Guard)
                refused |= parametersIn(operand) & arriving;
        });
    }

    for (HandedOnArray& entry : result.handedOn)
        entry.parameters &= ~refused;
    result.handedOn.removeAllMatching([](const HandedOnArray& entry) {
        return !entry.parameters;
    });
    return result;
}

static void collectQuestions(const Program& program, BuildContext& build, const Function& function, Vector<HandedOnArray>& questions)
{
    if (!function.hasBody())
        return;
    Vector<ValueID, 8> allocations = findArraysThatMayLieInFrame(program, build, function);
    if (allocations.isEmpty())
        return;
    BitVector arrays(function.values.size());
    for (ValueID id : allocations)
        arrays.quickSet(id.index());

    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            BlockID block(blockIndex);
            for (ValueID id : function.at(block).values) {
                forEachReturnedOperand(function, function.at(id), [&](ValueID operand) {
                    if (arrays.get(operand.index()))
                        changed |= arrays.add(id.index());
                });
            }
            forEachOutgoingEdge(function, block, [&](Edge edge, const Target& target) {
                const CompactList<ValueID>& parameters = function.at(target.block).parameters;
                for (uint32_t index = 0; index < parameters.size(); ++index) {
                    ValueID passed = valuePassedOn(function, edge, index);
                    if (passed && arrays.get(passed.index()))
                        changed |= arrays.add(parameters[index].index());
                }
            });
        }
    }

    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& call = function.at(id);
            if (effectRuleOf(call.opcode) != EffectRule::Call)
                continue;
            std::optional<CalleeList> callees;
            call.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (!arrays.get(operand.index()))
                    return;
                if (!callees)
                    callees = calleesOfCall(program, call);
                for (FunctionID callee : callees->listed().functions) {
                    if (std::optional<uint32_t> entryParameter = entryParameterOf(program.at(callee), call, operandIndex); entryParameter && *entryParameter < maxFrameArrayParameters)
                        questions.append(HandedOnArray { callee, *entryParameter, 0 });
                }
            });
        }
    }
}

// A function ends its tail calls where each of them enters only functions that end theirs. Called by value, it is entered through its entrypoints, which have to end theirs too.
// Each question waits for the answers it needs and is answered once, and a callee set that many calls name is one question: a thousand methods that each leave by a call of any of the thousand are two thousand questions.
static FunctionsThatEndTheirTailCalls findFunctionsThatEndTheirTailCalls(const Program& program, const ParallelLoops& loops)
{
    struct TailCalls {
        FunctionID caller;
        Vector<FunctionID, 2> callees;
        Vector<FunctionID, 2> calleesByValue;
        Vector<CalleeSetID, 1> sets;
    };
    unsigned numberOfFunctions = program.functions.size();
    Vector<uint8_t> neverEnds(FillWith { }, numberOfFunctions, 0);
    Vector<TailCalls> tailCalls = collectInParallel<TailCalls>(loops, numberOfFunctions, largeFunctions(program, loops).span(), [&](unsigned index, Vector<TailCalls>& found) {
        const Function& function = program.functions[index];
        TailCalls calls { FunctionID(index), { }, { }, { } };
        neverEnds[index] = !hasBody(program, FunctionID(index));
        forEachBlockEndingWith(program, FunctionID(index), { TerminatorKind::ReturnCall }, [&](BlockID block) {
            const Value& call = function.at(function.at(block).terminator.callOperand());
            if (CalleeSetID set = call.opcode == Opcode::CallDirect ? CalleeSetID() : calleeSetOfCall(program, call); set && program.at(set).isLong()) {
                if (program.at(set).listsEveryCallee() && program.at(set).written().builtins.empty())
                    calls.sets.append(set);
                else
                    neverEnds[index] = true;
                return;
            }
            CalleeList callees = calleesOfCall(program, call);
            std::optional<ProvenValue<std::span<const FunctionID>>> functions = callees.functionsAlone();
            if (!functions) {
                neverEnds[index] = true;
                return;
            }
            auto& list = call.opcode == Opcode::CallDirect ? calls.callees : calls.calleesByValue;
            list.append(functions->value());
        });
        if (!neverEnds[index] && (!calls.callees.isEmpty() || !calls.calleesByValue.isEmpty() || !calls.sets.isEmpty()))
            found.append(WTF::move(calls));
    });

    // The questions: does the function end, does it end when called by value, does every function of the set.
    Vector<CalleeSetID> sets;
    for (const TailCalls& calls : tailCalls)
        sets.appendVector(calls.sets);
    AOT::sort(sets);
    sets.shrink(std::ranges::unique(sets).begin() - sets.begin());
    auto directly = [](FunctionID function) { return function.index(); };
    auto byValue = [&](FunctionID function) { return numberOfFunctions + function.index(); };
    auto everyOneOf = [&](CalleeSetID set) { return 2 * numberOfFunctions + static_cast<unsigned>(std::ranges::lower_bound(sets, set) - sets.begin()); };
    unsigned numberOfQuestions = 2 * numberOfFunctions + sets.size();

    Vector<std::pair<unsigned, unsigned>> waits;
    auto waitFor = [&](unsigned question, unsigned answer) { waits.append({ answer, question }); };
    for (const TailCalls& calls : tailCalls) {
        for (FunctionID callee : calls.callees)
            waitFor(directly(calls.caller), directly(callee));
        for (FunctionID callee : calls.calleesByValue)
            waitFor(directly(calls.caller), byValue(callee));
        for (CalleeSetID set : calls.sets)
            waitFor(directly(calls.caller), everyOneOf(set));
    }
    for (CalleeSetID set : sets) {
        for (FunctionID callee : program.at(set).written().functions)
            waitFor(everyOneOf(set), byValue(callee));
    }
    for (unsigned index = 0; index < numberOfFunctions; ++index) {
        waitFor(byValue(FunctionID(index)), index);
        if (const auto* record = program.functions[index].creator.tryAs<CreatorRecord::Entrypoint>())
            waitFor(byValue(record->target), index);
    }

    Vector<unsigned> numberAwaited(FillWith { }, numberOfQuestions, 0u);
    Vector<unsigned> firstWaiting(FillWith { }, numberOfQuestions + 1, 0u);
    for (auto [answer, question] : waits) {
        ++numberAwaited[question];
        ++firstWaiting[answer + 1];
    }
    for (unsigned index = 0; index < numberOfQuestions; ++index)
        firstWaiting[index + 1] += firstWaiting[index];
    Vector<unsigned> waiting(waits.size());
    Vector<unsigned> cursors(firstWaiting.span().first(numberOfQuestions));
    for (auto [answer, question] : waits)
        waiting[cursors[answer]++] = question;

    BitVector isAnsweredYes(numberOfQuestions);
    Vector<unsigned> worklist;
    for (unsigned index = 0; index < numberOfFunctions; ++index) {
        if (!neverEnds[index] && !numberAwaited[index])
            worklist.append(index);
    }
    while (!worklist.isEmpty()) {
        unsigned answer = worklist.takeLast();
        isAnsweredYes.quickSet(answer);
        for (unsigned question : waiting.subspan(firstWaiting[answer], firstWaiting[answer + 1] - firstWaiting[answer])) {
            if (!--numberAwaited[question] && (question >= numberOfFunctions || !neverEnds[question]))
                worklist.append(question);
        }
    }

    BitVector ends(numberOfFunctions);
    BitVector endsByValue(numberOfFunctions);
    for (unsigned index = 0; index < numberOfFunctions; ++index) {
        if (isAnsweredYes.quickGet(index))
            ends.quickSet(index);
        if (isAnsweredYes.quickGet(numberOfFunctions + index))
            endsByValue.quickSet(index);
    }
    return { WTF::move(ends), WTF::move(endsByValue) };
}

FrameArrayParameters computeFrameArrayParameters(const Program& program, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned numberOfFunctions = program.functions.size();
    FrameArrayParameters table;
    table.m_numberOfFunctions = numberOfFunctions;
    FunctionsThatEndTheirTailCalls ending = findFunctionsThatEndTheirTailCalls(program, loops);
    table.m_endsItsTailCalls = WTF::move(ending.calledDirectly);
    table.m_endsItsTailCallsWhenCalledByValue = WTF::move(ending.calledByValue);
    if (!build.limit(LimitID::MaxFrameArrayLength))
        return table;

    Vector<HandedOnArray> questions = collectInParallel<HandedOnArray>(loops, numberOfFunctions, largeFunctions(program, loops).span(), [&](unsigned index, Vector<HandedOnArray>& found) {
        if (mayListValueWith(program, FunctionID(index), { Opcode::NewArray, Opcode::CreateRest }))
            collectQuestions(program, build, program.functions[index], found);
    });
    if (questions.isEmpty())
        return table;

    Vector<ScannedParameters> scanned(numberOfFunctions);
    Vector<uint32_t> asked(FillWith { }, numberOfFunctions, 0);
    Vector<FunctionID> everyFunction;
    while (!questions.isEmpty()) {
        Vector<FunctionID> round;
        for (const HandedOnArray& question : questions) {
            uint32_t& word = asked[question.callee.index()];
            if (word >> question.entryParameter & 1)
                continue;
            if (word == scanned[question.callee.index()].asked) {
                round.append(question.callee);
                if (!word)
                    everyFunction.append(question.callee);
            }
            word |= 1u << question.entryParameter;
        }
        questions.shrink(0);

        forEachIndexInParallel(loops, round.size(), [&](unsigned index) {
            FunctionID id = round[index];
            const Function& function = program.at(id);
            uint32_t parameters = asked[id.index()];
            scanned[id.index()] = mayBeHandedArrays(function) ? scanParametersHandedArrays(program, function, parameters) : ScannedParameters { parameters, parameters, { } };
        });
        for (FunctionID id : round)
            questions.appendVector(scanned[id.index()].handedOn);
    }

    Vector<uint32_t>& words = table.m_words;
    words.fill(0, numberOfFunctions);
    for (FunctionID id : everyFunction)
        words[id.index()] = scanned[id.index()].asked & ~scanned[id.index()].refused;
    for (bool changed = true; changed;) {
        changed = false;
        for (FunctionID id : everyFunction) {
            uint32_t& word = words[id.index()];
            for (const HandedOnArray& entry : scanned[id.index()].handedOn) {
                if (!(word & entry.parameters) || words[entry.callee.index()] >> entry.entryParameter & 1)
                    continue;
                word &= ~entry.parameters;
                changed = true;
            }
        }
    }
    return table;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
