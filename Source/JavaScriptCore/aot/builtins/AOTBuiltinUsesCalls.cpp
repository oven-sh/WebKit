/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinUses.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTTypeRelations.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CalleeAnswer {
    BuiltinID callee;
    TypeID testedCalleeType;
    TypeID testedReceiverType;
};

enum class ThisValueHandling : uint8_t { KeepsGenericCall, PassedUnmatched, Matched };

struct ArrayMatch {
    BuiltinMatchState state { BuiltinMatchState::Unknown };
    TypeID testedType;
};

} // anonymous namespace

static bool canBecomeBuiltinCall(const BuiltinTable& table, BuiltinID callee)
{
    return isCallableBuiltin(table, callee) && table.at(callee).scope == BuiltinScope::Realm;
}

static CalleeAnswer answerWithTestedCallee(const BuiltinUseInputs& inputs, const Value& call, BuiltinID callee, TypeID testedReceiverType)
{
    Type testedCallee = Type::forBuiltin(callee);
    if (!inputs.program.typeOf(inputs.function, call.calleeOperand()).overlaps(testedCallee, inputs.program))
        return { };
    return { callee, inputs.types.intern(testedCallee), testedReceiverType };
}

static CalleeAnswer findPredictedCallee(const BuiltinUseInputs& inputs, const Value& call)
{
    TypeID prediction = inputs.function.at(call.calleeOperand()).prediction();
    if (!prediction)
        return { };
    BuiltinID predicted = singleBuiltinOf(inputs.types.at(prediction));
    if (!predicted)
        return { };
    return answerWithTestedCallee(inputs, call, inputs.program.builtins().canonicalID(predicted), { });
}

static CalleeAnswer findMethodCallee(const BuiltinUseInputs& inputs, const Value& call)
{
    if (call.opcode != Opcode::Call || !call.calleeKey() || !call.thisValueOperand())
        return { };

    const Program& program = inputs.program;
    if (!builtinMemberNameOf(program, call.calleeKey()))
        return { };

    const BuiltinTable& table = program.builtins();
    ValueID receiver = call.thisValueOperand();
    Type receiverType = program.typeOf(inputs.function, receiver).withoutNullish();
    BuiltinReceiver start = builtinReceiverOf(program, receiverType);
    bool isPredicted = !start;
    if (isPredicted) {
        TypeID prediction = inputs.function.at(receiver).prediction();
        if (!prediction)
            return { };
        receiverType = inputs.types.at(prediction).intersectWith(receiverType, inputs.types).withoutNullish();
        start = builtinReceiverOf(program, receiverType);
    }

    BuiltinLookupContext lookup = inputs.lookup();
    BuiltinMemberAnswer found;
    TypeID testedReceiverType;
    switch (start.kind) {
    case BuiltinReceiverKind::Unknown:
        return { };
    case BuiltinReceiverKind::BuiltinObject: {
        Type testedReceiver = Type::forBuiltin(start.object);
        if (isPredicted && !testedReceiver.isSingleValue())
            return { };
        found = findMemberFrom(lookup, start.object, call.calleeKey());
        if (isPredicted)
            testedReceiverType = inputs.types.intern(testedReceiver);
        break;
    }
    case BuiltinReceiverKind::Instance:
    case BuiltinReceiverKind::Primitive:
        found = findMemberOfInstances(lookup, start.builtinClass, call.calleeKey());
        if (isPredicted) {
            std::optional<Type> testedReceiver = testedTypeOfInstances(program, start.builtinClass, receiverType);
            if (!testedReceiver)
                return { };
            testedReceiverType = inputs.types.intern(*testedReceiver);
        }
        break;
    }
    if (!found.isFound() || !canBecomeBuiltinCall(table, found.member))
        return { };
    return answerWithTestedCallee(inputs, call, found.member, testedReceiverType);
}

// Whatever else the callee may be is a primitive, as the `undefined` of a `var` that is read early: the call of it throws.
static CalleeAnswer findOnlyObjectAmongCallees(const BuiltinUseInputs& inputs, const Value& call)
{
    BuiltinID callee = singleBuiltinOf(inputs.program.typeOf(inputs.function, call.calleeOperand()).withOnlyObjects());
    if (!callee)
        return { };
    return answerWithTestedCallee(inputs, call, inputs.program.builtins().canonicalID(callee), { });
}

static CalleeAnswer findCallee(const BuiltinUseInputs& inputs, const Value& call)
{
    if (BuiltinID callee = singleBuiltinCalleeOf(inputs.program, inputs.function, call))
        return { callee, { }, { } };
    if (CalleeAnswer onlyObject = findOnlyObjectAmongCallees(inputs, call); onlyObject.callee)
        return onlyObject;
    CalleeAnswer method = findMethodCallee(inputs, call);
    // The type of the receiver is a proof and the callee is only predicted: the member that the class holds under the name is the better expectation.
    bool isMemberOfProvenReceiver = method.callee && !method.testedReceiverType;
    if (isMemberOfProvenReceiver)
        return method;
    CalleeAnswer predicted = findPredictedCallee(inputs, call);
    // Where both name one function, the test of the receiver is the one that makes the read of the method a constant.
    if (predicted.callee && predicted.callee != method.callee)
        return predicted;
    return method;
}

static bool mayObserveThisValue(const BuiltinTable& table, BuiltinID callee)
{
    return std::ranges::any_of(table.overloadsOf(callee), [](const BuiltinOverload& overload) {
        return overload.mode == BuiltinCallMode::Call && (overload.receiver || overload.isUndeclared());
    });
}

static ThisValueHandling thisValueHandlingOf(const BuiltinUseInputs& inputs, BuiltinID callee, ValueID thisValue)
{
    const BuiltinTable& table = inputs.program.builtins();
    const Builtin& entry = table.at(callee);
    if (!thisValue) {
        bool observesHolderPassedInstead = entry.holder && mayObserveThisValue(table, callee);
        return observesHolderPassedInstead ? ThisValueHandling::KeepsGenericCall : ThisValueHandling::Matched;
    }
    if (!entry.flags.contains(BuiltinFlag::RequiresHolderAsThis))
        return ThisValueHandling::Matched;

    const Type& type = inputs.program.typeOf(inputs.function, thisValue);
    bool isRequiredValue = entry.holder
        ? singleBuiltinOf(type) == table.canonicalID(entry.holder)
        : type.hasOnlyPrimitives(Primitive::Undefined);
    return isRequiredValue ? ThisValueHandling::Matched : ThisValueHandling::PassedUnmatched;
}

BuiltinUsePlan planCallOfBuiltin(const BuiltinUseInputs& inputs, ValueID use, ProgramPoint point)
{
    const Function& function = inputs.function;
    const Value& call = function.at(use);
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (arguments.size() > defaultLimitValue(LimitID::MaxBuiltinCallArguments, hostArchitecture()))
        return { };

    CalleeAnswer answer = findCallee(inputs, call);
    if (!answer.callee || !canBecomeBuiltinCall(inputs.program.builtins(), answer.callee))
        return { };

    KnownBuiltinCall known;
    known.callee = answer.callee;
    ThisValueHandling thisValueHandling = ThisValueHandling::Matched;
    if (call.opcode == Opcode::Construct) {
        ValueID newTarget = call.newTargetOperand();
        if (newTarget && sameObjectThroughCasts(function, newTarget) != sameObjectThroughCasts(function, call.calleeOperand()))
            return { };
        known.mode = BuiltinCallMode::Construct;
    } else {
        known.operands.receiver = call.thisValueOperand();
        known.stamps.expectedResult = call.expectedResult();
        thisValueHandling = thisValueHandlingOf(inputs, answer.callee, known.operands.receiver);
    }
    if (thisValueHandling == ThisValueHandling::KeepsGenericCall)
        return { };
    known.operands.arguments.append(arguments);
    known.stamps.accessSite = call.accessSite();
    if (known.stamps.accessSite)
        known.stamps.allocationSite = inputs.flow.resultSiteOf(known.stamps.accessSite);

    BuiltinUsePlan plan = planKnownBuiltinCall(inputs, known, point);
    if (!plan)
        return { };
    if (thisValueHandling == ThisValueHandling::PassedUnmatched)
        degradeToUnmatchedCall(plan);
    if (plan.kind == BuiltinUseKind::UnmatchedCall && answer.testedCalleeType)
        return { };

    bool isPossible = true;
    if (answer.testedReceiverType)
        isPossible &= prependRequirement(inputs.types, plan, { known.operands.receiver, answer.testedReceiverType, { } });
    if (answer.testedCalleeType)
        isPossible &= prependRequirement(inputs.types, plan, { call.calleeOperand(), answer.testedCalleeType, { } });
    if (!isPossible)
        return { };

    plan.isNeverInlined = call.opcode == Opcode::Call && call.isNeverInlined();
    return plan;
}

// Flow links at the call what it saw arrive there as the parent, and only that is expected. Function.prototype, the parent of a class without one, constructs nothing.
static BuiltinID findExpectedParent(const Program& program, const Value& call)
{
    if (!call.callees())
        return { };

    const BuiltinTable& table = program.builtins();
    BuiltinID expected;
    for (BuiltinID seen : program.at(call.callees()).builtins) {
        bool isConstructor = std::ranges::any_of(table.overloadsOf(seen), [](const BuiltinOverload& overload) {
            return overload.mode == BuiltinCallMode::Construct;
        });
        if (!isConstructor)
            continue;
        if (expected)
            return { };
        expected = seen;
    }
    return expected;
}

// `super(..)` in a class below a built-in constructs the built-in for the class that `new` named, which only a constructor of the library can be told.
BuiltinUsePlan planSuperCallOfBuiltin(const BuiltinUseInputs& inputs, ValueID use, ProgramPoint point)
{
    const Program& program = inputs.program;
    const Value& call = inputs.function.at(use);
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (arguments.size() > defaultLimitValue(LimitID::MaxBuiltinCallArguments, hostArchitecture()))
        return { };

    const Type& parentType = program.typeOf(inputs.function, call.parentOperand());
    BuiltinID parent = singleBuiltinOf(parentType);
    bool isParentProven = !!parent;
    if (!isParentProven)
        parent = findExpectedParent(program, call);
    if (!parent || !canBecomeBuiltinCall(program.builtins(), program.builtins().canonicalID(parent)))
        return { };

    KnownBuiltinCall known;
    known.callee = program.builtins().canonicalID(parent);
    known.mode = BuiltinCallMode::Construct;
    known.operands.arguments.append(arguments);
    known.stamps.accessSite = call.accessSite();
    if (known.stamps.accessSite)
        known.stamps.allocationSite = inputs.flow.resultSiteOf(known.stamps.accessSite);

    BuiltinUsePlan plan = planKnownBuiltinCall(inputs, known, point);
    if (plan.kind != BuiltinUseKind::LibraryCall || !program.at(plan.libraryFunction).has(FunctionFlags::HasNewTarget))
        return { };
    plan.newTarget = call.newTargetOperand();
    if (isParentProven)
        return plan;

    Type testedParent = Type::forBuiltin(known.callee);
    if (!parentType.overlaps(testedParent, program) || !prependRequirement(inputs.types, plan, { call.parentOperand(), inputs.types.intern(testedParent), { } }))
        return { };
    return plan;
}

static bool insertsOperations(const BuiltinCallPlan& plan)
{
    return std::ranges::any_of(plan.steps, [](const OperandStep& step) {
        switch (step.kind) {
        case OperandStepKind::Convert:
        case OperandStepKind::RequireObjectCoercible:
            return true;
        case OperandStepKind::Pass:
        case OperandStepKind::PassUndefined:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

BuiltinUsePlan planRematch(const BuiltinUseInputs& inputs, ValueID use, ProgramPoint point)
{
    const BuiltinTable& table = inputs.program.builtins();
    const Value& call = inputs.function.at(use);
    BuiltinCallForm form = call.builtinCallForm();
    switch (form) {
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix:
    case BuiltinCallForm::List:
        return { };
    case BuiltinCallForm::Whole:
    case BuiltinCallForm::AnyReceiver:
        break;
    }

    const BuiltinOverload& overload = table.at(call.overload());
    ValueID receiver = call.receiverOperand();
    if (overload.mode == BuiltinCallMode::Call && receiver && thisValueHandlingOf(inputs, overload.builtin, receiver) != ThisValueHandling::Matched)
        return { };

    KnownBuiltinCall known;
    known.callee = overload.builtin;
    known.mode = overload.mode;
    known.operands.receiver = receiver;
    known.operands.arguments.append(call.argumentsOperands());
    known.stamps = { call.expectedResult(), call.accessSite(), call.allocationSite() };

    BuiltinUsePlan plan = planKnownBuiltinCall(inputs, known, point);
    if (plan.kind == BuiltinUseKind::UnmatchedCall)
        return { };
    bool isSameCall = form == BuiltinCallForm::Whole && plan.kind == BuiltinUseKind::Call && plan.call.overload == call.overload() && !insertsOperations(plan.call);
    if (isSameCall)
        return { };
    return plan;
}

static BuiltinOverloadID findListOverload(const BuiltinTable& table, BuiltinID callee)
{
    for (BuiltinOverloadID id : table.overloadIDsOf(callee)) {
        const BuiltinOverload& overload = table.at(id);
        if (overload.mode == BuiltinCallMode::Call && overload.code.listForm && !overload.receiver)
            return id;
    }
    return { };
}

ValueID findOnlySpreadIntoListOf(const BuiltinUseInputs& inputs, ValueID call)
{
    const Function& function = inputs.function;
    const UseLists& useLists = inputs.analyses->useLists();

    CastChain chain(function, function.at(call).listOperand());
    const Value& allocation = function.at(chain.last());
    if (allocation.opcode != Opcode::NewArray || !allocation.elementsOperands().empty() || !allocation.holes().empty() || allocation.capacityOperand())
        return { };

    // Each link serves the link in front of it, which for the first one is the call, and one of them the spread.
    ValueID spread;
    ValueID listUser = call;
    for (ValueID link : chain) {
        bool reachesCall = false;
        for (const Use& use : useLists.usesOf(link)) {
            if (use.kind != Use::Kind::Operand)
                return { };
            if (use.user == listUser && !reachesCall) {
                reachesCall = true;
                continue;
            }
            bool isSpread = function.at(use.user).opcode == Opcode::AppendSpread && use.operandIndex == OperandIndex::AppendSpread::array;
            if (!isSpread || spread)
                return { };
            spread = use.user;
        }
        if (!reachesCall)
            return { };
        listUser = link;
    }
    return spread;
}

static ArrayMatch matchArray(const BuiltinUseInputs& inputs, ValueID value)
{
    auto isArray = [](const Type& type) {
        const ClassPart* part = type.singleBuiltinClass();
        return part && part->builtinClass == BuiltinClasses::Array;
    };

    const Type& type = inputs.program.typeOf(inputs.function, value);
    Type tested = type.withoutNullish();
    if (isArray(tested) && !type.mayBeUndefinedOrNull())
        return { BuiltinMatchState::Proven, { } };

    if (!isArray(tested)) {
        TypeID prediction = inputs.function.at(value).prediction();
        if (!prediction)
            return { };
        tested = inputs.types.at(prediction).intersectWith(tested, inputs.types).withoutNullish();
        if (!isArray(tested))
            return { };
    }
    return { BuiltinMatchState::NeedsTests, inputs.types.intern(typeOfInstances(inputs.program.builtins(), BuiltinClasses::Array, tested)) };
}

static PristineRequirement pristineArrayRequirement(PristineAspectSet aspects)
{
    if (aspects.isEmpty())
        return { };
    return { BuiltinClasses::Array, aspects };
}

static constexpr PristineAspectSet aspectsOfArgumentArray { PristineAspects::Names, PristineAspects::Storage };

std::optional<UseRequirement> requirementOfArgumentArray(const BuiltinUseInputs& inputs, ValueID array, ProgramPoint point)
{
    RELEASE_ASSERT(inputs.analyses);
    ArrayMatch match = matchArray(inputs, array);
    if (match.state == BuiltinMatchState::Unknown)
        return std::nullopt;
    PristineRequirement unprovenAtPoint = pristineArrayRequirement(aspectsOfArgumentArray - aspectsProvenAt(inputs.pristine(), array, BuiltinClasses::Array, point));
    return UseRequirement { array, match.testedType, unprovenAtPoint };
}

BuiltinUsePlan planListCallOfBuiltin(const BuiltinUseInputs& inputs, ValueID use, ValueID iterable, ProgramPoint point)
{
    const Program& program = inputs.program;
    const Value& call = inputs.function.at(use);
    RELEASE_ASSERT(call.opcode == Opcode::CallList);
    RELEASE_ASSERT(inputs.analyses);

    BuiltinID callee = singleBuiltinCalleeOf(program, inputs.function, call);
    if (!callee || !canBecomeBuiltinCall(program.builtins(), callee))
        return { };
    BuiltinOverloadID overload = findListOverload(program.builtins(), callee);
    if (!overload)
        return { };
    std::optional<UseRequirement> requirement = requirementOfArgumentArray(inputs, iterable, point);
    if (!requirement)
        return { };
    PristineRequirement unprovenByType = pristineArrayRequirement(aspectsOfArgumentArray - pristineAspectsProvenBy(program.typeOf(inputs.function, iterable).withoutNullish(), BuiltinClasses::Array));

    BuiltinUsePlan plan;
    plan.kind = BuiltinUseKind::Call;
    plan.member = callee;
    plan.form = BuiltinCallForm::List;
    plan.call.state = requirement->requiredType ? BuiltinMatchState::NeedsTests : BuiltinMatchState::Proven;
    plan.call.overload = overload;
    plan.call.steps.append(OperandStep { BuiltinOperand::argument(0), OperandStepKind::Pass, BuiltinConversion::None });
    plan.call.numberOfArguments = 1;
    if (requirement->requiredType || unprovenByType)
        plan.call.requirements.append(OperandRequirement { BuiltinOperand::argument(0), requirement->requiredType, unprovenByType });
    plan.operands.arguments.append(iterable);
    plan.stamps.expectedResult = call.expectedResult();
    plan.stamps.accessSite = call.accessSite();
    if (requirement->requiredType || requirement->pristine)
        plan.requirements.append(*requirement);

    if (resultFitOf(inputs, use, plan) != ResultFit::Within)
        return { };
    return plan;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
