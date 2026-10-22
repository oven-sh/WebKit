/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBlockFrequencies.h"
#include "AOTEffectsOfOperands.h"
#include "AOTHeat.h"

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
            if (usesLookedAt == UsesLookedAt::InLikelyCode && function.at(use.block).frequency != BlockFrequency::Normal)
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
        return takesCells(program, function.at(objectUse.use.user));
    case Use::Kind::TerminatorOperand:
        return true;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// `function count(...args)`: the caller builds the list for the call alone. The summary of a callee says that it retains a parameter as soon as one general read in its rare code may hand
// the list to a getter. Its body says more: where all that its likely code does with the list is answered by a virtual object, the allocation goes away there, and sinks into the
// unlikely code that takes a cell. A list for each turn of a loop is what pays for a copy of the callee.
class CalleeOfCall {
public:
    CalleeOfCall(FunctionPassContext& context, ValueID call)
        : m_context(context)
        , m_id(context.function.at(call).function())
    {
        BlockID block = context.analyses.useLists().blockOf(call);
        m_isRepeated = isHot(heatClassOf(context, block)) || context.analyses.blockFrequencies().frequencyOf(block) >= HeatModel::minWeightOfCallInLoop;
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

enum class SeenOfAllocation : uint8_t { Nothing, NeedsCellElsewhere, Escapes };

// What needsCellOnlyFor() finds false, seen without the solve: from the uses of the value that makes the object, and of the values that it is handed on to.
static SeenOfAllocation lookAtUsesOf(FunctionAnalyses& analyses, ValueID maker, ValueID call, CalleeOfCall& callee)
{
    constexpr size_t maxNames = 12;

    const Function& function = analyses.function();
    const UseLists& useLists = analyses.useLists();
    SeenOfAllocation result = SeenOfAllocation::Nothing;
    Vector<ValueID, maxNames> names { maker };
    auto addName = [&](ValueID name) {
        if (names.size() < maxNames)
            names.appendIfNotContains(name);
    };
    for (size_t next = 0; next < names.size(); ++next) {
        for (const Use& use : useLists.usesOf(names[next])) {
            if (use.isEdgeUse()) {
                const CompactList<ValueID>& parameters = function.at(targetOf(function, use.edge()).block).parameters;
                if (use.operandIndex < parameters.size())
                    addName(parameters[use.operandIndex]);
                continue;
            }
            bool isOperandOfCall = use.kind == Use::Kind::Operand && use.user == call;
            switch (classifyUseAlone(analyses, use)) {
            case UseSeenAlone::Unknown:
                break;
            case UseSeenAlone::HandsOn:
                addName(use.user);
                break;
            case UseSeenAlone::IsListedAsEscape:
                if (!(isOperandOfCall && callee.onlyInterpretsRestArgumentsInLikelyCode(use.operandIndex)))
                    return SeenOfAllocation::Escapes;
                break;
            case UseSeenAlone::IsListed:
                if (!isOperandOfCall && needsCell(analyses.program(), function, ObjectUse { .use = use }))
                    result = SeenOfAllocation::NeedsCellElsewhere;
                break;
            }
        }
    }
    return result;
}

// Nearly every question ends with none, and the function is another one behind each call that is inlined: the solve is left to the few calls that may gain something.
static bool isSeenToPassNoAllocationBuiltOnlyFor(FunctionAnalyses& analyses, ValueID call, CalleeOfCall& callee)
{
    const Function& function = analyses.function();
    bool result = true;
    function.at(call).forEachOperand([&](ValueID operand, uint32_t) {
        if (!result)
            return;
        std::optional<Vector<ValueID, 8>> makers = findMakersOfObjectsBehind(analyses, operand);
        if (!makers) {
            result = false;
            return;
        }
        // A value that makes an object of its own names no other. Else the operand may name what stands in the objects too: that escapes where its holder does.
        bool namesItsOwnObject = makers->size() == 1 && makers->first() == operand;
        for (ValueID maker : *makers) {
            SeenOfAllocation seen = lookAtUsesOf(analyses, maker, call, callee);
            result = result && (seen == SeenOfAllocation::Escapes || (seen == SeenOfAllocation::NeedsCellElsewhere && namesItsOwnObject));
        }
    });
    return result;
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
    bool isSeenToPassNone = !analyses.computed().contains(FunctionAnalysis::Escape) && isSeenToPassNoAllocationBuiltOnlyFor(analyses, call, callee);
    if (isSeenToPassNone && !analyses.context().build.options().verifiesAfterEveryPass)
        return 0;

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
    RELEASE_ASSERT(!isSeenToPassNone || !count);
    return count;
}

Vector<SunkAllocation> predictSinking(FunctionAnalyses& analyses)
{
    AllocationSinking sinking(analyses, SinkingPolicy { .recordsLosses = false, .leavesWhenNoAllocationPays = true });
    if (sinking.collectCandidates())
        sinking.analyze();
    return sinking.outcomes();
}

unsigned sinkableAllocationsAfterInlining(FunctionPassContext& context, ValueID call, NOESCAPE const ScopedLambda<bool()>& calleeReturnsFreshAggregate)
{
    if (context.function.at(call).opcode != Opcode::CallDirect)
        return 0;

    CalleeOfCall callee(context, call);
    unsigned count = countAllocationsBuiltOnlyFor(context.analyses, call, callee);
    if (isOnlyInterpreted(context.analyses, call, UsesLookedAt::All) && calleeReturnsFreshAggregate())
        ++count;
    return count;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
