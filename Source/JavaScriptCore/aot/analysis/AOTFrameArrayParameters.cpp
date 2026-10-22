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

// The parameters of the caller keep the answer only if the callee may be handed an array in its entry parameter.
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

struct AskedParameter {
    FunctionID function;
    uint32_t entryParameter { 0 };
};

class ParameterScanner {
public:
    ParameterScanner(const Program&, const Function&, uint32_t parameters);

    ScannedParameters run();

private:
    void findWhereArraysArrive();
    uint32_t arrivingOn(Edge) const;
    std::optional<uint32_t> parameterTestedBy(ValueID tested) const;

    void propagate();
    bool add(BlockID, ValueID, uint32_t parameters);

    void checkOperandsOf(BlockID, const Value&);
    void checkOperandsOf(BlockID, const Terminator&);
    void handOn(const Value& call, uint32_t operandIndex, uint32_t parameters);

    uint32_t parametersIn(ValueID id) const { return id ? m_mayBe[id.index()] : 0; }

    const Program& m_program;
    const Function& m_function;
    CFG m_cfg;
    ValueIdentities m_identities;
    uint32_t m_asked;
    uint32_t m_refused { 0 };
    // Bit i: the block is reached in a call that was handed an array in a frame for entry parameter i.
    Vector<uint32_t> m_arrivesIn;
    // Bit i: the value may be entry parameter i.
    Vector<uint32_t> m_mayBe;
    Vector<HandedOnArray> m_handedOn;
};

} // anonymous namespace

// A saved frame holds what is live in it, and the function goes on when its caller has long gone on.
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

ParameterScanner::ParameterScanner(const Program& program, const Function& function, uint32_t parameters)
    : m_program(program)
    , m_function(function)
    , m_cfg(function)
    , m_identities(function, m_cfg)
    , m_asked(parameters)
    , m_arrivesIn(FillWith { }, function.blocks.size(), 0)
    , m_mayBe(FillWith { }, function.values.size(), 0)
{
}

ScannedParameters ParameterScanner::run()
{
    findWhereArraysArrive();
    propagate();
    for (BlockID block : m_cfg.reversePostOrder()) {
        if (!m_arrivesIn[block.index()])
            continue;
        for (ValueID id : m_function.at(block).values)
            checkOperandsOf(block, m_function.at(id));
        checkOperandsOf(block, m_function.at(block).terminator);
    }

    ScannedParameters result { m_asked, m_refused, { } };
    for (HandedOnArray entry : m_handedOn) {
        entry.parameters &= ~m_refused;
        if (entry.parameters)
            result.handedOn.append(entry);
    }
    return result;
}

std::optional<uint32_t> ParameterScanner::parameterTestedBy(ValueID tested) const
{
    const CompactList<ValueID>& parameters = m_function.at(entryBlock).parameters;
    size_t index = parameters.find(m_identities.identityOf(tested));
    if (index == notFound || index >= maxFrameArrayParameters)
        return std::nullopt;
    return static_cast<uint32_t>(index);
}

uint32_t ParameterScanner::arrivingOn(Edge edge) const
{
    uint32_t arriving = m_arrivesIn[edge.from.index()];
    const Terminator& terminator = m_function.at(edge.from).terminator;
    if (edge.isHandlerEdge() || terminator.kind != TerminatorKind::Branch)
        return arriving;
    std::optional<FrameArrayTest> test = testOfFrameArray(m_program, m_function.at(terminator.conditionOperand()));
    bool isTakenEdge = !edge.targetIndex;
    if (!test || test->passes == isTakenEdge)
        return arriving;
    std::optional<uint32_t> parameter = parameterTestedBy(test->tested);
    return parameter ? arriving & ~(1u << *parameter) : arriving;
}

void ParameterScanner::findWhereArraysArrive()
{
    m_arrivesIn[entryBlock.index()] = m_asked;
    Vector<BlockID, 16> worklist { entryBlock };
    while (!worklist.isEmpty()) {
        BlockID block = worklist.takeLast();
        forEachOutgoingEdge(m_function, block, [&](Edge edge, const Target& target) {
            uint32_t arriving = arrivingOn(edge);
            uint32_t& known = m_arrivesIn[target.block.index()];
            if (!(arriving & ~known))
                return;
            known |= arriving;
            worklist.append(target.block);
        });
    }
}

bool ParameterScanner::add(BlockID block, ValueID id, uint32_t parameters)
{
    uint32_t& known = m_mayBe[id.index()];
    parameters &= m_arrivesIn[block.index()] & ~known;
    if (!parameters)
        return false;
    known |= parameters;
    return true;
}

void ParameterScanner::propagate()
{
    const CompactList<ValueID>& parameters = m_function.at(entryBlock).parameters;
    for (uint32_t index = 0; index < parameters.size() && index < maxFrameArrayParameters; ++index)
        m_mayBe[parameters[index].index()] = m_asked & 1u << index;

    for (bool changed = true; changed;) {
        changed = false;
        for (BlockID block : m_cfg.reversePostOrder()) {
            if (!m_arrivesIn[block.index()])
                continue;
            for (ValueID id : m_function.at(block).values) {
                uint32_t given = 0;
                forEachReturnedOperand(m_function, m_function.at(id), [&](ValueID operand) {
                    given |= parametersIn(operand);
                });
                changed |= add(block, id, given);
            }
            forEachOutgoingEdge(m_function, block, [&](Edge edge, const Target& target) {
                uint32_t arriving = arrivingOn(edge);
                const CompactList<ValueID>& targetParameters = m_function.at(target.block).parameters;
                for (uint32_t index = 0; arriving && index < targetParameters.size(); ++index) {
                    if (ValueID passed = valuePassedOn(m_function, edge, index))
                        changed |= add(target.block, targetParameters[index], parametersIn(passed) & arriving);
                }
            });
        }
    }
}

void ParameterScanner::handOn(const Value& call, uint32_t operandIndex, uint32_t parameters)
{
    CalleeList callees = calleesOfCall(m_program, call);
    bool isArgument = !call.operandUses(operandIndex).containsAny(callingUses);
    if (!isArgument || !callees.isComplete || !callees.builtins.isEmpty()) {
        m_refused |= parameters;
        return;
    }
    for (FunctionID callee : callees.functions) {
        std::optional<uint32_t> entryParameter = entryParameterOf(m_program.at(callee), call, operandIndex);
        if (!entryParameter || *entryParameter >= maxFrameArrayParameters) {
            m_refused |= parameters;
            return;
        }
        m_handedOn.append(HandedOnArray { callee, *entryParameter, parameters });
    }
}

void ParameterScanner::checkOperandsOf(BlockID block, const Value& user)
{
    user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        uint32_t parameters = parametersIn(operand) & m_arrivesIn[block.index()] & ~m_refused;
        if (!parameters || user.operandKind(operandIndex) == OperandKind::Guard)
            return;
        if (givesSameArray(user.opcode, operandIndex) || readsArrayWhereItLies(m_program, m_function, user, operandIndex))
            return;
        if (effectRuleOf(user.opcode) == EffectRule::Call)
            handOn(user, operandIndex, parameters);
        else
            m_refused |= parameters;
    });
}

void ParameterScanner::checkOperandsOf(BlockID block, const Terminator& terminator)
{
    terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (terminator.operandKind(operandIndex) != OperandKind::Guard)
            m_refused |= parametersIn(operand) & m_arrivesIn[block.index()];
    });
}

static void collectQuestions(const Program& program, BuildContext& build, const Function& function, Vector<AskedParameter>& questions)
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
                for (FunctionID callee : callees->functions) {
                    if (std::optional<uint32_t> entryParameter = entryParameterOf(program.at(callee), call, operandIndex); entryParameter && *entryParameter < maxFrameArrayParameters)
                        questions.append(AskedParameter { callee, *entryParameter });
                }
            });
        }
    }
}

// The greatest fixpoint, so functions that hand a parameter to each other keep it.
static void refuseWhatCalleesRefuse(Vector<uint32_t>& words, const Vector<ScannedParameters>& scanned, std::span<const FunctionID> functions)
{
    for (bool changed = true; changed;) {
        changed = false;
        for (FunctionID id : functions) {
            uint32_t& word = words[id.index()];
            for (const HandedOnArray& entry : scanned[id.index()].handedOn) {
                if (!(word & entry.parameters) || words[entry.callee.index()] >> entry.entryParameter & 1)
                    continue;
                word &= ~entry.parameters;
                changed = true;
            }
        }
    }
}

// The least fixpoint, so functions that call each other in tail position are left out.
static BitVector findFunctionsThatEndTheirTailCalls(const Program& program, const ParallelLoops& loops)
{
    struct TailCalls {
        FunctionID caller;
        Vector<FunctionID, 2> callees;
    };
    unsigned numberOfFunctions = program.functions.size();
    Vector<uint8_t> neverEnds(FillWith { }, numberOfFunctions, 0);
    Vector<TailCalls> tailCalls = collectInParallel<TailCalls>(loops, numberOfFunctions, largeFunctions(program, loops).span(), [&](unsigned index, Vector<TailCalls>& found) {
        const Function& function = program.functions[index];
        TailCalls calls { FunctionID(index), { } };
        neverEnds[index] = !hasBody(program, FunctionID(index));
        forEachBlockEndingWith(program, FunctionID(index), { TerminatorKind::ReturnCall }, [&](BlockID block) {
            CalleeList callees = calleesOfCall(program, function.at(function.at(block).terminator.callOperand()));
            if (!callees.isComplete || !callees.builtins.isEmpty())
                neverEnds[index] = true;
            calls.callees.appendRange(callees.functions.begin(), callees.functions.end());
        });
        if (!neverEnds[index] && !calls.callees.isEmpty())
            found.append(WTF::move(calls));
    });

    BitVector ends(numberOfFunctions);
    for (unsigned index = 0; index < numberOfFunctions; ++index) {
        if (!neverEnds[index])
            ends.quickSet(index);
    }
    for (const TailCalls& calls : tailCalls)
        ends.quickClear(calls.caller.index());
    for (bool changed = true; changed;) {
        changed = false;
        for (const TailCalls& calls : tailCalls) {
            if (ends.quickGet(calls.caller.index()))
                continue;
            bool everyCalleeEnds = std::ranges::all_of(calls.callees, [&](FunctionID callee) {
                return ends.quickGet(callee.index());
            });
            if (everyCalleeEnds) {
                ends.quickSet(calls.caller.index());
                changed = true;
            }
        }
    }
    return ends;
}

FrameArrayParameters computeFrameArrayParameters(const Program& program, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned numberOfFunctions = program.functions.size();
    FrameArrayParameters table;
    table.m_numberOfFunctions = numberOfFunctions;
    table.m_endsItsTailCalls = findFunctionsThatEndTheirTailCalls(program, loops);
    if (!build.limit(LimitID::MaxFrameArrayLength))
        return table;

    Vector<AskedParameter> questions = collectInParallel<AskedParameter>(loops, numberOfFunctions, largeFunctions(program, loops).span(), [&](unsigned index, Vector<AskedParameter>& found) {
        // canLieInFrame() holds for no other operation.
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
        for (const AskedParameter& question : questions) {
            uint32_t& word = asked[question.function.index()];
            if (word >> question.entryParameter & 1)
                continue;
            if (word == scanned[question.function.index()].asked) {
                round.append(question.function);
                if (!word)
                    everyFunction.append(question.function);
            }
            word |= 1u << question.entryParameter;
        }
        questions.shrink(0);

        // A function that is asked about one more parameter is scanned again, for all of them.
        forEachIndexInParallel(loops, round.size(), [&](unsigned index) {
            FunctionID id = round[index];
            const Function& function = program.at(id);
            uint32_t parameters = asked[id.index()];
            scanned[id.index()] = mayBeHandedArrays(function) ? ParameterScanner(program, function, parameters).run() : ScannedParameters { parameters, parameters, { } };
        });
        for (FunctionID id : round) {
            for (const HandedOnArray& entry : scanned[id.index()].handedOn)
                questions.append(AskedParameter { entry.callee, entry.entryParameter });
        }
    }

    table.m_words.fill(0, numberOfFunctions);
    for (FunctionID id : everyFunction)
        table.m_words[id.index()] = scanned[id.index()].asked & ~scanned[id.index()].refused;
    refuseWhatCalleesRefuse(table.m_words, scanned, everyFunction.span());
    return table;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
