/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBlockFrequencies.h"
#include "AOTEdgeResults.h"
#include "AOTEffectsOfOperands.h"
#include "AOTHeat.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

enum class CallResultUseKind : uint8_t { Unknown, Interpreted, Alias };

struct CallResultUse {
    CallResultUseKind kind { CallResultUseKind::Unknown };
    ValueID alias;
};

static OperandBehavior declaredBehaviorOf(const Program& program, const Value& user, uint32_t operandIndex)
{
    if (user.opcode == Opcode::CallBuiltin)
        return declaredBehaviorOf(program.builtins().at(user.overload()), builtinOperandAt(user, operandIndex));
    return declaredBehaviorOf(user.opcode, operandIndex);
}

static bool isOperandOnlyRead(const Program& program, const Value& user, uint32_t operandIndex)
{
    OperandBehavior behavior = declaredBehaviorOf(program, user, operandIndex);
    return behavior.uses.containsOnly({ OperandUse::Inspects, OperandUse::ReadsContents }) && behavior.protocols.isEmpty();
}

static CallResultUse classifyCallResultUse(const Program& program, const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        return { CallResultUseKind::Alias, function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex] };
    case Use::Kind::TerminatorOperand:
    case Use::Kind::BlockSubject:
        return { };
    case Use::Kind::Operand:
        break;
    }

    const Value& user = function.at(use.user);
    if (isHeldOperand(user, use.operandIndex))
        return { CallResultUseKind::Interpreted, { } };
    switch (interpretationOf(program, user)) {
    case Interpretation::Cast:
        return { CallResultUseKind::Alias, use.user };
    case Interpretation::Read:
    case Interpretation::Test:
        if (isOperandOnlyRead(program, user, use.operandIndex))
            return { CallResultUseKind::Interpreted, { } };
        return { };
    case Interpretation::List:
        return { CallResultUseKind::Interpreted, { } };
    case Interpretation::None:
    case Interpretation::Allocation:
    case Interpretation::Store:
    case Interpretation::Guards:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

enum class UsesLookedAt : uint8_t { All, InLikelyCode };

static bool isOnlyInterpreted(FunctionAnalyses& analyses, ValueID value, UsesLookedAt usesLookedAt)
{
    const Function& function = analyses.function();
    const UseLists& useLists = analyses.useLists();

    BitVector visited(function.values.size());
    Vector<ValueID, 8> worklist;
    visited.quickSet(value.index());
    worklist.append(value);
    while (!worklist.isEmpty()) {
        for (const Use& use : useLists.usesOf(worklist.takeLast())) {
            if (usesLookedAt == UsesLookedAt::InLikelyCode && function.at(use.block).frequency.predicted() != BlockFrequency::Normal)
                continue;
            CallResultUse classified = classifyCallResultUse(analyses.program(), function, use);
            switch (classified.kind) {
            case CallResultUseKind::Unknown:
                return false;
            case CallResultUseKind::Interpreted:
                break;
            case CallResultUseKind::Alias:
                if (!visited.quickSet(classified.alias.index()))
                    worklist.append(classified.alias);
                break;
            }
        }
    }
    return true;
}

static bool takesCells(const Program& program, const Value& user)
{
    switch (interpretationOf(program, user)) {
    case Interpretation::None:
        return true;
    case Interpretation::Allocation:
        return user.opcode == Opcode::CallBuiltin || user.opcode == Opcode::NewArgumentList;
    case Interpretation::Cast:
    case Interpretation::Read:
    case Interpretation::Store:
    case Interpretation::Test:
    case Interpretation::List:
    case Interpretation::Guards:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool needsCell(const Program& program, const Function& function, const ObjectUse& objectUse)
{
    switch (objectUse.use.kind) {
    case Use::Kind::Operand:
        return !isHeldOperand(function.at(objectUse.use.user), objectUse.use.operandIndex) && takesCells(program, function.at(objectUse.use.user));
    case Use::Kind::TerminatorOperand:
        return true;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// `function count(...args)`: the caller builds the list for the call alone.
class CalleeOfCall {
public:
    CalleeOfCall(FunctionPassContext& context, BlockID block, ValueID call)
        : m_context(context)
        , m_id(context.function.at(call).function())
    {
        m_isRepeated = isHot(heatClassOf(context, block)).predicted() || context.analyses.blockFrequencies().frequencyOf(block).predicted() >= HeatModel::minWeightOfCallInLoop;
    }

    bool onlyInterpretsRestArgumentsInLikelyCode(uint32_t entryParameter)
    {
        const Function& callee = m_context.program.at(m_id);
        if (!m_isRepeated || !callee.hasBody() || !callee.has(FunctionFlags::HasRestParameter) || entryParameter != callee.firstParameterIndex() + callee.numberOfPositionalParameters())
            return false;
        FunctionAnalyses analyses(AnalysisContext { m_context.build, m_context.program, &m_context.inputs.flow, &m_context.inputs.effects, m_id }, callee);
        return isOnlyInterpreted(analyses, callee.at(entryBlock).parameters[entryParameter], UsesLookedAt::InLikelyCode);
    }

private:
    FunctionPassContext& m_context;
    FunctionID m_id;
    bool m_isRepeated { false };
};

// A callee that only reads an object lets nothing escape, and the call still takes a cell. An object that the callee lets escape is built in a copy of the callee as well.
static bool needsCellOnlyFor(FunctionAnalyses& analyses, ObjectID object, ValueID call, CalleeOfCall& callee)
{
    bool isPassedToCall = false;
    for (const ObjectUse& objectUse : analyses.escape().usesOf(object)) {
        bool isOperandOfCall = objectUse.use.kind == Use::Kind::Operand && objectUse.use.user == call;
        if (objectUse.isEscape && !(isOperandOfCall && callee.onlyInterpretsRestArgumentsInLikelyCode(objectUse.use.operandIndex)))
            return false;
        if (isOperandOfCall)
            isPassedToCall = true;
        else if (needsCell(analyses.program(), analyses.function(), objectUse))
            return false;
    }
    return isPassedToCall;
}

enum class SeenOfAllocation : uint8_t { Nothing, NeedsCellElsewhere, Escapes, NeedsCellOnlyForCall };

// The parameters that take the operand of a test on the edges where the test has narrowed it: PointsToSolver::findNarrowedCopies() for one use.
static void forEachNarrowedCopy(FunctionAnalyses& analyses, const Use& use, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    const Function& function = analyses.function();
    auto visitTargetsOf = [&](BlockID block) {
        const Terminator& terminator = function.at(block).terminator;
        uint32_t numberOfTargets = terminator.targets.size();
        for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex) {
            const Target& target = terminator.targets[targetIndex];
            if (target.proofs.isEmpty() || targetResultsOf(terminator.kind, targetIndex, numberOfTargets) != TargetResults::Proofs)
                continue;
            const auto& parameters = function.at(target.block).parameters;
            size_t firstParameter = target.arguments.size();
            for (size_t resultIndex = 0; resultIndex < target.proofs.size() && firstParameter + resultIndex < parameters.size(); ++resultIndex) {
                if (target.proofs[resultIndex] == use.operandIndex)
                    functor(parameters[firstParameter + resultIndex]);
            }
        }
    };
    auto narrows = [](Proof proof) {
        return proof != Proof::None && !provesForStretch(proof);
    };

    switch (use.kind) {
    case Use::Kind::TerminatorOperand: {
        const Terminator& terminator = function.at(use.block).terminator;
        if (!terminator.isConditionalBranch() && narrows(provesOf(terminator.kind)) && terminator.operandKind(use.operandIndex) != OperandKind::Guard)
            visitTargetsOf(use.block);
        return;
    }
    case Use::Kind::Operand: {
        const Value& test = function.at(use.user);
        if (!narrows(provesOf(test.opcode)) || test.operandKind(use.operandIndex) == OperandKind::Guard)
            return;
        for (const Use& useOfTest : analyses.useLists().usesOf(use.user)) {
            if (useOfTest.kind != Use::Kind::TerminatorOperand)
                continue;
            const Terminator& terminator = function.at(useOfTest.block).terminator;
            if (terminator.isConditionalBranch() && terminator.conditionOperand() == use.user)
                visitTargetsOf(useOfTest.block);
        }
        return;
    }
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A listed use that keeps the object nowhere and calls nothing lets it escape by nothing that happens elsewhere.
static bool maySendObjectOn(FunctionAnalyses& analyses, const Use& use)
{
    static constexpr OperandUses usesThatSendOn = callingUses | OperandUses { OperandUse::Retains, OperandUse::Returns };

    const Function& function = analyses.function();
    switch (use.kind) {
    case Use::Kind::Operand:
        return summarizeOperand(analyses.effectContext(), function, function.at(use.user), use.operandIndex).uses.containsAny(usesThatSendOn);
    case Use::Kind::TerminatorOperand:
        return summarizeOperand(analyses.effectContext(), function, function.at(use.block).terminator, use.operandIndex).uses.containsAny(usesThatSendOn);
    case Use::Kind::BlockSubject:
        return false;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What needsCellOnlyFor() finds, seen without the solve: from the uses of the value that makes the object, and of the values that it is handed on to.
static SeenOfAllocation lookAtUsesOf(FunctionAnalyses& analyses, ValueID maker, ValueID call, CalleeOfCall& callee)
{
    constexpr size_t maxNames = 12;

    const Function& function = analyses.function();
    const UseLists& useLists = analyses.useLists();
    bool needsCellElsewhere = false;
    bool isPassedToCall = false;
    bool hasSeenAll = true;
    Vector<ValueID, maxNames> names { maker };
    auto addName = [&](ValueID name) {
        if (names.contains(name) || !mayBeReference(analyses.program().typeOf(function, name)))
            return;
        if (names.size() < maxNames)
            names.append(name);
        else
            hasSeenAll = false;
    };
    for (size_t next = 0; next < names.size(); ++next) {
        for (const Use& use : useLists.usesOf(names[next])) {
            if (use.isEdgeUse()) {
                const CompactList<ValueID>& parameters = function.at(targetOf(function, use.edge()).block).parameters;
                if (use.operandIndex < parameters.size())
                    addName(parameters[use.operandIndex]);
                else
                    hasSeenAll = false;
                continue;
            }
            forEachNarrowedCopy(analyses, use, addName);
            bool isOperandOfCall = use.kind == Use::Kind::Operand && use.user == call;
            switch (classifyUseAlone(analyses, use)) {
            case UseSeenAlone::Unknown:
                hasSeenAll = false;
                break;
            case UseSeenAlone::HandsOn:
                addName(use.user);
                break;
            case UseSeenAlone::IsListedAsEscape:
                if (!(isOperandOfCall && callee.onlyInterpretsRestArgumentsInLikelyCode(use.operandIndex)))
                    return SeenOfAllocation::Escapes;
                isPassedToCall = true;
                break;
            case UseSeenAlone::IsListed:
                hasSeenAll = hasSeenAll && !maySendObjectOn(analyses, use);
                if (isOperandOfCall)
                    isPassedToCall = true;
                else if (needsCell(analyses.program(), function, ObjectUse { .use = use }))
                    needsCellElsewhere = true;
                break;
            }
        }
    }
    if (needsCellElsewhere)
        return SeenOfAllocation::NeedsCellElsewhere;
    return hasSeenAll && isPassedToCall ? SeenOfAllocation::NeedsCellOnlyForCall : SeenOfAllocation::Nothing;
}

// Nearly every question ends with none, and the function is another one behind each call that is inlined: the solve is left to the few calls of which this does not tell.
static std::optional<unsigned> countAllocationsSeenToBeBuiltOnlyFor(FunctionAnalyses& analyses, ValueID call, CalleeOfCall& callee)
{
    bool isSeen = true;
    Vector<ValueID, 4> counted;
    analyses.function().at(call).forEachOperand([&](ValueID operand, uint32_t) {
        if (!isSeen)
            return;
        std::optional<Vector<ValueID, 8>> makers = findMakersOfObjectsBehind(analyses, operand);
        if (!makers) {
            isSeen = false;
            return;
        }
        // A value that makes an object of its own names no other. Else the operand may name what stands in the objects too: that escapes where its holder does.
        bool namesItsOwnObject = makers->size() == 1 && makers->first() == operand;
        for (ValueID maker : *makers) {
            switch (lookAtUsesOf(analyses, maker, call, callee)) {
            case SeenOfAllocation::Escapes:
                break;
            case SeenOfAllocation::NeedsCellElsewhere:
                isSeen = isSeen && namesItsOwnObject;
                break;
            case SeenOfAllocation::NeedsCellOnlyForCall:
                isSeen = isSeen && namesItsOwnObject;
                if (namesItsOwnObject && describeVirtualObject(analyses, maker).support == VirtualObjectSupport::Supported)
                    counted.appendIfNotContains(maker);
                break;
            case SeenOfAllocation::Nothing:
                isSeen = false;
                break;
            }
        }
    });
    if (!isSeen)
        return std::nullopt;
    return counted.size();
}

static unsigned countAllocationsBuiltOnlyFor(FunctionAnalyses& analyses, ValueID call, CalleeOfCall& callee)
{
    constexpr OpcodeSet allocations = opcodesInterpretedAs(Interpretation::Allocation);
    if (!analyses.opcodesOfValues().intersects(allocations))
        return 0;

    // An allocation with a use that lets it escape is built whatever becomes of the call. A checking build looks whether the objects say the same.
    bool mayPassUnescapedAllocation = false;
    analyses.function().at(call).forEachOperand([&](ValueID operand, uint32_t) {
        mayPassUnescapedAllocation = mayPassUnescapedAllocation || mayNameUnescapedAllocation(analyses.function(), operand);
    });
    if (!mayPassUnescapedAllocation && !analyses.context().build.options().verifiesAfterEveryPass)
        return 0;
    std::optional<unsigned> countSeen;
    if (!analyses.computed().contains(FunctionAnalysis::Escape))
        countSeen = countAllocationsSeenToBeBuiltOnlyFor(analyses, call, callee);
    if (countSeen && !analyses.context().build.options().verifiesAfterEveryPass)
        return *countSeen;

    const EscapeAnalysis& escape = analyses.escape();
    BitVector passedObjects;
    analyses.function().at(call).forEachOperand([&](ValueID operand, uint32_t) {
        passedObjects.merge(escape.pointsTo(operand).objects.bits());
    });

    unsigned count = 0;
    for (size_t index : passedObjects) {
        ObjectID object(index);
        const TrackedObject& tracked = escape.at(object);
        if (tracked.kind != ObjectKind::Allocation || !needsCellOnlyFor(analyses, object, call, callee))
            continue;
        if (describeVirtualObject(analyses, tracked.definition).support == VirtualObjectSupport::Supported)
            ++count;
    }
    RELEASE_ASSERT(mayPassUnescapedAllocation || !count);
    RELEASE_ASSERT(!countSeen || escape.gaveUp() || *countSeen == count);
    return count;
}

Vector<SunkAllocation> predictSinking(FunctionAnalyses& analyses)
{
    AllocationSinking sinking(analyses, SinkingPolicy { .recordsLosses = false, .leavesWhenNoAllocationPays = true });
    if (sinking.collectCandidates())
        sinking.analyze();
    return sinking.outcomes();
}

unsigned sinkableAllocationsAfterInlining(FunctionPassContext& context, BlockID block, ValueID call, NOESCAPE const ScopedLambda<bool()>& calleeReturnsFreshAggregate)
{
    if (context.function.at(call).opcode != Opcode::CallDirect)
        return 0;

    CalleeOfCall callee(context, block, call);
    unsigned count = countAllocationsBuiltOnlyFor(context.analyses, call, callee);
    if (isOnlyInterpreted(context.analyses, call, UsesLookedAt::All) && calleeReturnsFreshAggregate())
        ++count;
    return count;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
