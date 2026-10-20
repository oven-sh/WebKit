/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInternal.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperands.h"

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

static bool isResultOnlyInterpreted(FunctionAnalyses& analyses, ValueID call)
{
    const Function& function = analyses.function();
    const UseLists& useLists = analyses.useLists();

    BitVector visited(function.values.size());
    Vector<ValueID, 8> worklist;
    visited.quickSet(call.index());
    worklist.append(call);
    while (!worklist.isEmpty()) {
        for (const Use& use : useLists.usesOf(worklist.takeLast())) {
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

// A callee that only reads an object lets nothing escape, and the call still takes a cell. An object that the callee lets escape is built in a copy of the callee as well.
static bool needsCellOnlyFor(FunctionAnalyses& analyses, ObjectID object, ValueID call)
{
    bool isPassedToCall = false;
    for (const ObjectUse& objectUse : analyses.escape().usesOf(object)) {
        if (objectUse.isEscape)
            return false;
        if (objectUse.use.kind == Use::Kind::Operand && objectUse.use.user == call)
            isPassedToCall = true;
        else if (needsCell(analyses.program(), analyses.function(), objectUse))
            return false;
    }
    return isPassedToCall;
}

static unsigned countAllocationsBuiltOnlyFor(FunctionAnalyses& analyses, ValueID call)
{
    const EscapeAnalysis& escape = analyses.escape();
    BitVector passedObjects;
    analyses.function().at(call).forEachOperand([&](ValueID operand, uint32_t) {
        passedObjects.merge(escape.pointsTo(operand).objects.bits());
    });

    unsigned count = 0;
    for (size_t index : passedObjects) {
        ObjectID object(index);
        const TrackedObject& tracked = escape.at(object);
        if (tracked.kind != ObjectKind::Allocation || !needsCellOnlyFor(analyses, object, call))
            continue;
        if (describeVirtualObject(analyses, tracked.definition).support == VirtualObjectSupport::Supported)
            ++count;
    }
    return count;
}

Vector<SunkAllocation> predictSinking(FunctionAnalyses& analyses)
{
    AllocationSinking sinking(analyses, SinkingPolicy { ReturnedObjects::AreBuilt, false });
    if (sinking.collectCandidates())
        sinking.analyze();
    return sinking.outcomes();
}

unsigned sinkableAllocationsAfterInlining(FunctionPassContext& context, ValueID call, bool calleeReturnsFreshAggregate)
{
    if (context.function.at(call).opcode != Opcode::CallDirect)
        return 0;

    unsigned count = countAllocationsBuiltOnlyFor(context.analyses, call);
    if (calleeReturnsFreshAggregate && isResultOnlyInterpreted(context.analyses, call))
        ++count;
    return count;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
