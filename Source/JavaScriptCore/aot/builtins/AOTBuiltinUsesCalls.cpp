/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinUses.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTTypeRelations.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct BuiltinCalleeAnswer {
    BuiltinID callee;
    TypeID testedCalleeType;
    TypeID testedReceiverType;
    bool testsCodeOfCallee { false };
};

enum class ThisValueHandling : uint8_t { KeepsGenericCall, PassedUnmatched, Matched };

struct ArrayMatch {
    BuiltinMatchState state { BuiltinMatchState::Unknown };
    TypeID testedType;
};

} // anonymous namespace

static bool hasPassedTestOfFunction(const BuiltinUseInputs& inputs, ValueID subject, BuiltinID member)
{
    return std::ranges::any_of(inputs.passedTests, [&](const Value& test) {
        return test.opcode == Opcode::IsFunctionOfBuiltin && test.member() == member && sameObjectThroughCasts(inputs.function, test.valueOperand()) == sameObjectThroughCasts(inputs.function, subject);
    });
}

static bool canBecomeBuiltinCall(const BuiltinTable& table, BuiltinID callee)
{
    return isCallableBuiltin(table, callee) && (!isCalledOnlyDirectly(table, callee) || hasDirectFunction(table, callee));
}

static BuiltinCalleeAnswer answerWithTestedCallee(const BuiltinUseInputs& inputs, const Value& call, BuiltinID callee, TypeID testedReceiverType)
{
    Type testedCallee = Type::forBuiltin(callee);
    if (!inputs.program.typeOf(inputs.function, call.calleeOperand()).overlaps(testedCallee, inputs.program))
        return { };
    if (isCalledOnlyDirectly(inputs.program.builtins(), callee))
        return { callee, { }, testedReceiverType, true };
    return { callee, inputs.types.intern(testedCallee), testedReceiverType };
}

static BuiltinID builtinHeldBy(const BuiltinTable& table, BuiltinID member)
{
    if (table.at(member).kind != BuiltinKind::DataProperty)
        return member;
    for (const BuiltinOverload& overload : table.overloadsOf(member)) {
        if (overload.mode != BuiltinCallMode::Get || overload.isUndeclared())
            continue;
        const BuiltinType& type = table.at(overload.result.type);
        bool namesOneBuiltin = type.flags.isEmpty() && type.primitives.isEmpty() && type.stringLiterals.empty() && type.parts.size() == 1 && type.parts[0].kind == BuiltinTypePartKind::Builtin;
        if (namesOneBuiltin)
            return table.canonicalID(type.parts[0].builtin);
    }
    return member;
}

static BuiltinCalleeAnswer findExpectedCallee(const BuiltinUseInputs& inputs, const Value& call)
{
    TypeID expectedType = inputs.function.at(call.calleeOperand()).expected().predicted();
    if (!expectedType)
        return { };
    BuiltinID expected = singleBuiltinOf(inputs.types.at(expectedType));
    if (!expected)
        return { };
    const BuiltinTable& table = inputs.program.builtins();
    return answerWithTestedCallee(inputs, call, builtinHeldBy(table, table.canonicalID(expected)), { });
}

static BuiltinCalleeAnswer findMethodCallee(const BuiltinUseInputs& inputs, const Value& call)
{
    if (call.opcode != Opcode::Call || !call.calleeKey() || !call.thisValueOperand())
        return { };

    const Program& program = inputs.program;
    if (!builtinMemberNameOf(program, call.calleeKey()))
        return { };

    const BuiltinTable& table = program.builtins();
    ValueID receiver = call.thisValueOperand();
    Type present = program.typeOf(inputs.function, receiver).withoutNullish();
    Type receiverType = present;
    BuiltinReceiver start = builtinReceiverOf(program, receiverType);
    bool isExpected = !start;
    if (isExpected) {
        TypeID expected = inputs.function.at(receiver).expected().predicted();
        if (!expected)
            return { };
        receiverType = inputs.types.at(expected).intersectWith(receiverType, inputs.types).withoutNullish();
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
        if (isExpected && !testedReceiver.isSingleValue())
            return { };
        found = findMemberFrom(lookup, start.object, call.calleeKey());
        if (isExpected)
            testedReceiverType = inputs.types.intern(testedReceiver);
        break;
    }
    case BuiltinReceiverKind::Instance:
    case BuiltinReceiverKind::Primitive:
        found = findMemberOfInstances(lookup, start.builtinClass, call.calleeKey());
        if (isExpected) {
            std::optional<Type> testedReceiver = testedTypeOfExpectedInstances(program, inputs.types, start.builtinClass, receiverType, present);
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

static BuiltinCalleeAnswer findOnlyObjectAmongCallees(const BuiltinUseInputs& inputs, const Value& call)
{
    BuiltinID callee = singleBuiltinOf(inputs.program.typeOf(inputs.function, call.calleeOperand()).withOnlyObjects());
    if (!callee)
        return { };
    return answerWithTestedCallee(inputs, call, inputs.program.builtins().canonicalID(callee), { });
}

static bool mayBeAmongCallees(const Program& program, const Value& call, BuiltinID callee)
{
    if (!Value::opcodesWithCallees.contains(call.opcode) || !call.callees())
        return true;
    std::optional<ProvenValue<Callees>> callees = program.at(call.callees()).all();
    return !callees || std::ranges::contains(callees->value().builtins, program.builtins().canonicalID(callee));
}

static Expected<bool> isOnlyBuiltinSeenAtCall(const Program& program, const Value& call, BuiltinID callee)
{
    if (!Value::opcodesWithCallees.contains(call.opcode) || !call.callees())
        return false;
    std::span<const BuiltinID> seen = program.at(call.callees()).seen().predicted().builtins;
    return seen.size() == 1 && program.builtins().canonicalID(seen[0]) == callee;
}

static BuiltinCalleeAnswer findCalleeByTypes(const BuiltinUseInputs&, const Value& call);

static BuiltinCalleeAnswer findCallee(const BuiltinUseInputs& inputs, const Value& call)
{
    BuiltinCalleeAnswer answer = findCalleeByTypes(inputs, call);
    if (answer.callee && !mayBeAmongCallees(inputs.program, call, answer.callee))
        return { };
    return answer;
}

static BuiltinCalleeAnswer findCalleeSeenWithDirectFunction(const BuiltinUseInputs& inputs, const Value& call)
{
    if (!Value::opcodesWithCallees.contains(call.opcode) || !call.callees())
        return { };
    const BuiltinTable& table = inputs.program.builtins();
    const CalleeSet& callees = inputs.program.at(call.callees());
    std::optional<ProvenValue<Callees>> all = callees.all();
    for (BuiltinID seen : callees.seen().predicted().builtins) {
        BuiltinID callee = table.canonicalID(seen);
        if (!isCalledOnlyDirectly(table, callee) || !hasDirectFunction(table, callee))
            continue;
        bool isOnlyCallee = all && all->value().functions.empty() && all->value().builtins.size() == 1;
        if (!isOnlyCallee)
            return { callee, { }, { }, true };
        bool isFunction = inputs.program.typeOf(inputs.function, call.calleeOperand()).isSubsetOf(Type::function(), inputs.program);
        return { callee, isFunction ? TypeID() : inputs.types.intern(Type::function()), { } };
    }
    return { };
}

static bool hasReceiverOfBuiltinClass(const BuiltinUseInputs& inputs, const Value& call)
{
    if (call.opcode != Opcode::Call || !call.thisValueOperand())
        return false;
    ValueID receiver = call.thisValueOperand();
    Type present = inputs.program.typeOf(inputs.function, receiver).withoutNullish();
    if (builtinReceiverOf(inputs.program, present))
        return true;
    TypeID expected = inputs.function.at(receiver).expected().predicted();
    return expected && builtinReceiverOf(inputs.program, inputs.types.at(expected).intersectWith(present, inputs.types).withoutNullish());
}

static BuiltinCalleeAnswer findCalleeByTypes(const BuiltinUseInputs& inputs, const Value& call)
{
    if (BuiltinID callee = singleBuiltinCalleeOf(inputs.program, inputs.function, call))
        return { callee, { }, { } };
    if (!hasReceiverOfBuiltinClass(inputs, call)) {
        if (BuiltinCalleeAnswer seen = findCalleeSeenWithDirectFunction(inputs, call); seen.callee)
            return seen;
    }
    if (BuiltinCalleeAnswer onlyObject = findOnlyObjectAmongCallees(inputs, call); onlyObject.callee)
        return onlyObject;
    BuiltinCalleeAnswer method = findMethodCallee(inputs, call);
    bool isMemberOfProvenReceiver = method.callee && !method.testedReceiverType;
    if (isMemberOfProvenReceiver)
        return method;
    BuiltinCalleeAnswer expected = findExpectedCallee(inputs, call);
    if (expected.callee && expected.callee != method.callee)
        return expected;
    return method;
}

bool mayFindProvenCallee(const BuiltinUseInputs& inputs, const Value& call)
{
    if (singleBuiltinOf(inputs.program.typeOf(inputs.function, call.calleeOperand())))
        return true;
    return Value::opcodesWithCallees.contains(call.opcode) && call.callees() && !inputs.program.at(call.callees()).seen().predicted().builtins.empty();
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

    BuiltinCalleeAnswer answer = findCallee(inputs, call);
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
        // `f.call(..)` names `f.call` where `f` refuses. What such a call has become names nothing more.
        if (call.calleeKey() && (answer.callee == Builtins::FunctionPrototypeCall || answer.callee == Builtins::FunctionPrototypeApply))
            known.stamps.calleeName = call.calleeName();
        thisValueHandling = thisValueHandlingOf(inputs, answer.callee, known.operands.receiver);
    }
    if (thisValueHandling == ThisValueHandling::KeepsGenericCall)
        return { };
    known.operands.arguments.append(arguments);
    known.stamps.accessSite = call.accessSite();

    BuiltinUsePlan plan = planKnownBuiltinCall(inputs, known, point);
    if (!plan)
        return { };
    if (isCalledOnlyDirectly(inputs.program.builtins(), answer.callee)) {
        bool isDirect = plan.kind == BuiltinUseKind::Call && thisValueHandling == ThisValueHandling::Matched && BuiltinBindings::directFunctionNumberOf(plan.call.overload, plan.operands.arguments.size());
        if (!isDirect)
            return { };
    }
    if (thisValueHandling == ThisValueHandling::PassedUnmatched)
        degradeToUnmatchedCall(plan);
    bool calleeIsProven = !answer.testedReceiverType && !answer.testedCalleeType && !answer.testsCodeOfCallee;
    if (inputs.asksOnlyForProvenPlans && calleeIsProven && plan.kind == BuiltinUseKind::Call && !plan.isProven() && !isCalledOnlyDirectly(inputs.program.builtins(), answer.callee))
        degradeToUnmatchedCall(plan);
    if (plan.kind == BuiltinUseKind::UnmatchedCall && answer.testedCalleeType && !(answer.testedReceiverType && isOnlyBuiltinSeenAtCall(inputs.program, call, answer.callee).predicted()))
        return { };

    bool isPossible = true;
    if (answer.testedReceiverType)
        isPossible &= prependRequirement(inputs.types, plan, { known.operands.receiver, answer.testedReceiverType, { } });
    if (answer.testedCalleeType)
        isPossible &= prependRequirement(inputs.types, plan, { call.calleeOperand(), answer.testedCalleeType, { } });
    if (answer.testsCodeOfCallee && !hasPassedTestOfFunction(inputs, call.calleeOperand(), answer.callee))
        isPossible &= prependRequirement(inputs.types, plan, { .subject = call.calleeOperand(), .functionOfBuiltin = answer.callee });
    if (!isPossible)
        return { };

    plan.isNeverInlined = call.opcode == Opcode::Call && call.isNeverInlined();
    return plan;
}

static BuiltinID findExpectedParent(const Program& program, const Value& call)
{
    if (!call.callees())
        return { };

    const BuiltinTable& table = program.builtins();
    BuiltinID expected;
    for (BuiltinID seen : program.at(call.callees()).seen().predicted().builtins) {
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

bool insertsOperations(const BuiltinCallPlan& plan)
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
    known.stamps = { call.expectedResult(), call.accessSite(), call.allocationSite(), call.calleeName() };

    auto withoutSameCall = [&](BuiltinUsePlan&& plan) -> BuiltinUsePlan {
        if (plan.kind == BuiltinUseKind::UnmatchedCall)
            return { };
        bool isSameCall = form == BuiltinCallForm::Whole && plan.kind == BuiltinUseKind::Call && plan.call.overload == call.overload() && !insertsOperations(plan.call);
        if (isSameCall)
            return { };
        return WTF::move(plan);
    };

    bool comparesWithWholePlan = inputs.analyses && inputs.analyses->context().build.options().verifiesAfterEveryPass;
    bool hasWholePlan = comparesWithWholePlan && !!withoutSameCall(planKnownBuiltinCall(inputs, known, point));
    if (form == BuiltinCallForm::Whole)
        known.overloadOfWholeCall = call.overload();
    BuiltinUsePlan plan = withoutSameCall(planKnownBuiltinCall(inputs, known, point));
    RELEASE_ASSERT(!comparesWithWholePlan || hasWholePlan == !!plan);
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
    const UseLists& useLists = inputs.useLists();

    CastChain chain(function, function.at(call).listOperand());
    const Value& allocation = function.at(chain.last());
    if (allocation.opcode != Opcode::NewArray || !allocation.elementsOperands().empty() || !allocation.holes().empty() || allocation.capacityOperand())
        return { };

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
        TypeID expected = inputs.function.at(value).expected().predicted();
        if (!expected)
            return { };
        tested = inputs.types.at(expected).intersectWith(tested, inputs.types).withoutNullish();
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
