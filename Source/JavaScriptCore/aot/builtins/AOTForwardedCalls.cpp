/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTForwardedCalls.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ForwardedOperands {
    BuiltinOperand callee;
    std::optional<BuiltinOperand> thisValue;
    std::optional<BuiltinOperand> newTarget;
    std::optional<BuiltinOperand> list;
    std::optional<unsigned> firstArgument;
};

} // anonymous namespace

static bool isSingleOperand(BuiltinOperand operand)
{
    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
    case BuiltinOperand::Kind::Argument:
        return true;
    case BuiltinOperand::Kind::ArgumentsFrom:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<BuiltinOperand> singleOperandOf(const BuiltinPlace& place, BuiltinPlaceKind kind)
{
    if (place.kind != kind || !place.operand || !isSingleOperand(*place.operand))
        return std::nullopt;
    return place.operand;
}

static const BuiltinCallback* findForwardedCallback(const BuiltinOverload& overload, bool constructs)
{
    const BuiltinFlow& flow = overload.flow;
    if (flow.callbacks.size() != 1 || !flow.transfers.empty() || !flow.propertyActions.empty())
        return nullptr;
    if (overload.mode != BuiltinCallMode::Call || !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode) || !overload.effects.writes.isEmpty())
        return nullptr;

    const BuiltinCallback& callback = flow.callbacks.front();
    if (callback.times != BuiltinCallbackTimes::Once || callback.constructs != constructs || callback.resultUse != BuiltinCallbackResultUse::Returned)
        return nullptr;
    return &callback;
}

static std::optional<ForwardedOperands> forwardedOperandsOf(const BuiltinCallback& callback)
{
    if (!isSingleOperand(callback.callee))
        return std::nullopt;
    ForwardedOperands operands { callback.callee, { }, { }, { }, { } };

    if (callback.thisValue.kind != BuiltinPlaceKind::None) {
        operands.thisValue = singleOperandOf(callback.thisValue, BuiltinPlaceKind::Operand);
        if (!operands.thisValue)
            return std::nullopt;
    }
    if (callback.newTarget.kind != BuiltinPlaceKind::None) {
        operands.newTarget = singleOperandOf(callback.newTarget, BuiltinPlaceKind::Operand);
        if (!operands.newTarget)
            return std::nullopt;
    }

    const BuiltinPlace& spread = callback.spreadArguments;
    bool forwardsRestArguments = spread.kind == BuiltinPlaceKind::Operand && spread.operand && !isSingleOperand(*spread.operand);
    if (forwardsRestArguments) {
        if (!callback.arguments.empty())
            return std::nullopt;
        operands.firstArgument = spread.operand->argumentIndex();
    } else if (spread.kind != BuiltinPlaceKind::None) {
        operands.list = singleOperandOf(spread, BuiltinPlaceKind::OwnValues);
        if (!operands.list || !callback.arguments.empty())
            return std::nullopt;
    }

    if (!callback.arguments.empty()) {
        const BuiltinPlace& passed = callback.arguments.front();
        if (callback.arguments.size() != 1 || passed.kind != BuiltinPlaceKind::Operand || !passed.operand || isSingleOperand(*passed.operand))
            return std::nullopt;
        operands.firstArgument = passed.operand->argumentIndex();
    }
    return operands;
}

static bool hasOtherOperandProtocols(const BuiltinOverload& overload, const ForwardedOperands& operands)
{
    auto runsProtocols = [&](const BuiltinParameter& parameter) {
        return parameter.operand != operands.list && parameter.operand != operands.newTarget && !parameter.protocols.isEmpty();
    };
    return (overload.receiver && runsProtocols(*overload.receiver)) || std::ranges::any_of(overload.parameters, runsProtocols);
}

static std::optional<ForwardedOperands> forwardedOperandsOf(const BuiltinOverload& overload, bool constructs = false)
{
    const BuiltinCallback* callback = findForwardedCallback(overload, constructs);
    if (!callback)
        return std::nullopt;
    std::optional<ForwardedOperands> operands = forwardedOperandsOf(*callback);
    if (!operands || hasOtherOperandProtocols(overload, *operands))
        return std::nullopt;
    return operands;
}

bool forwardsCall(const BuiltinOverload& overload)
{
    return !!forwardedOperandsOf(overload);
}

bool forwardsCallOfAnyReceiver(const BuiltinOverload& overload)
{
    return overload.builtin == Builtins::FunctionPrototypeCall || overload.builtin == Builtins::FunctionPrototypeApply;
}

std::optional<BuiltinOperand> operandCalledBy(const BuiltinOverload& overload)
{
    std::optional<ForwardedOperands> operands = forwardedOperandsOf(overload);
    return operands ? std::optional(operands->callee) : std::nullopt;
}

// Function.prototype.apply takes undefined and null for an empty list. A built-in that requires its list refuses them.
static bool mayOmit(const BuiltinOverload& overload, BuiltinOperand operand)
{
    auto parameter = std::ranges::find(overload.parameters, operand, &BuiltinParameter::operand);
    return parameter != overload.parameters.end() && parameter->mode == BuiltinParameterMode::Optional;
}

static std::optional<ForwardedCallPlan> planForwardedCall(const Program& program, const Function& function, const BuiltinCall& call, bool constructs)
{
    std::optional<ForwardedOperands> operands = forwardedOperandsOf(program.builtins().at(call.overload), constructs);
    if (!operands)
        return std::nullopt;

    ForwardedCallPlan plan;
    plan.constructs = constructs;
    plan.callee = call.operandAt(operands->callee);
    if (!plan.callee)
        return std::nullopt;
    plan.calleeMayRefuse = constructs || !program.typeOf(function, plan.callee).isSubsetOf(program.at(TypeTable::function), program);
    if (operands->thisValue)
        plan.thisValue = call.operandAt(*operands->thisValue);
    if (operands->newTarget) {
        plan.newTarget = call.operandAt(*operands->newTarget);
        if (!plan.newTarget)
            return std::nullopt;
    }

    ValueID list = operands->list ? call.operandAt(*operands->list) : ValueID();
    if (list && !program.typeOf(function, list).isUndefinedOrNull()) {
        plan.list = list;
        plan.operandOfList = operands->list;
    } else if (operands->list && !mayOmit(program.builtins().at(call.overload), *operands->list))
        return std::nullopt;

    size_t numberOfArguments = call.arguments.size();
    size_t firstForwardedArgument = operands->firstArgument ? std::min<size_t>(*operands->firstArgument, numberOfArguments) : numberOfArguments;
    plan.firstForwardedArgument = static_cast<uint32_t>(firstForwardedArgument);
    return plan;
}

std::optional<ForwardedCallPlan> planForwardedCall(const Program& program, const Function& function, const BuiltinCall& call)
{
    return planForwardedCall(program, function, call, false);
}

std::optional<ForwardedCallPlan> planForwardedConstruct(const Program& program, const Function& function, const BuiltinCall& call)
{
    return planForwardedCall(program, function, call, true);
}

bool isAlwaysArray(const Type& type)
{
    const ClassPart* part = type.singleBuiltinClass();
    return part && part->builtinClass == BuiltinClasses::Array && !type.mayBeUndefinedOrNull();
}

void becomeForwardedCall(Value& call, const ForwardedCallPlan& plan)
{
    AccessSiteID accessSite = call.accessSite();
    StringID calleeName = call.calleeName();
    Vector<ValueID, 8> arguments(call.argumentsOperands().subspan(plan.firstForwardedArgument));

    if (plan.constructs) {
        call.become(plan.list ? Opcode::ConstructList : Opcode::Construct);
        call.setNewTargetOperand(plan.newTarget);
    } else {
        call.become(plan.list ? Opcode::CallList : Opcode::Call);
        call.setThisValueOperand(plan.thisValue);
    }
    call.setCalleeOperand(plan.callee);
    if (plan.list)
        call.setListOperand(plan.list);
    else
        call.setArgumentsOperands(arguments.span());
    call.setAccessSite(accessSite);
    if (calleeName && plan.calleeMayRefuse)
        call.setCalleeName(calleeName);
    call.type = TypeTable::anything;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
