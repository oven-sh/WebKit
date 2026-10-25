/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinUses.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableBindings.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfRules.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTForwardedCalls.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTSplitUnions.h"
#include "AOTTypeTestSite.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class CallQueryStorage {
    WTF_MAKE_NONCOPYABLE(CallQueryStorage);
public:
    CallQueryStorage(const BuiltinUseInputs& inputs, BuiltinID callee, BuiltinCallMode mode, const BuiltinCallOperands& operands, NullishReceiver nullishReceiver)
        : m_typing(inputs.typing())
        , m_proven(operandTypesOf(m_typing, operands.receiver, operands.arguments.span()))
        , m_expected(operandTypesOf(m_typing.expecting(), operands.receiver, operands.arguments.span()))
        , m_query { { m_typing, callee, mode, m_proven, m_expected }, inputs.effects, nullishReceiver }
    {
        expectArgumentsToBePresent(isCalledOnlyDirectly(inputs.program.builtins(), callee));
        expectArgumentsRefusedWhenAbsentToBePresent(inputs.program.builtins(), callee, mode);
        expectArgumentsToBeWhatEveryOverloadTakes(inputs.program.builtins(), callee, mode);
        if (mode == BuiltinCallMode::Call)
            expectReceiverToBePresent(inputs.program.builtins(), callee);
    }

    const BuiltinCallQuery& query() const LIFETIME_BOUND { return m_query; }

private:
    void expectArgumentsToBePresent(bool isCalledOnlyDirectly)
    {
        TypeArena& types = m_typing.types;
        for (size_t index = 0; index < m_expected.arguments.size(); ++index) {
            const Type& expected = types.at(m_expected.arguments[index]);
            bool isAsFrontEndHandsOver = !expected.anyObject && (expected.primitives & Primitive::UndefinedOrNull) == Primitive::UndefinedOrNull;
            if (isCalledOnlyDirectly ? m_expected.arguments[index] != m_proven.arguments[index] : !isAsFrontEndHandsOver)
                continue;
            Type present = expected.withoutPrimitives(Primitive::UndefinedOrNull);
            if (!present.isNever())
                m_expected.arguments[index] = types.intern(WTF::move(present));
        }
    }

    void expectArgumentsRefusedWhenAbsentToBePresent(const BuiltinTable& table, BuiltinID callee, BuiltinCallMode mode)
    {
        TypeArena& types = m_typing.types;
        for (size_t index = 0; index < m_expected.arguments.size() && index <= BuiltinOperand::maxArgumentIndex; ++index) {
            TypeID proven = m_proven.arguments[index];
            if (m_expected.arguments[index] != proven || !types.at(proven).mayBeUndefinedOrNull())
                continue;
            TypeID present = types.intern(types.at(proven).withoutPrimitives(Primitive::UndefinedOrNull));
            if (present == TypeTable::never)
                continue;
            bool isTakenAsItIs = false;
            bool refusesAbsentArgument = std::ranges::all_of(table.overloadIDsOf(callee), [&](BuiltinOverloadID id) {
                const BuiltinOverload& overload = table.at(id);
                if (overload.mode != mode)
                    return true;
                const BuiltinParameter* parameter = table.parameterFor(overload, BuiltinOperand::argument(static_cast<unsigned>(index)));
                if (!parameter || parameter->mode != BuiltinParameterMode::Required || parameter->conversion != BuiltinConversion::None)
                    return false;
                isTakenAsItIs |= fitOf(m_typing, present, parameter->converted ? parameter->converted : parameter->accepted).fit == TypeFit::Inside;
                return fitOf(m_typing, TypeTable::undefined, parameter->accepted).fit == TypeFit::Outside
                    && fitOf(m_typing, TypeTable::null, parameter->accepted).fit == TypeFit::Outside;
            });
            if (refusesAbsentArgument && isTakenAsItIs)
                m_expected.arguments[index] = present;
        }
    }

    void expectArgumentsToBeWhatEveryOverloadTakes(const BuiltinTable& table, BuiltinID callee, BuiltinCallMode mode)
    {
        TypeArena& types = m_typing.types;
        for (size_t index = 0; index < m_expected.arguments.size() && index <= BuiltinOperand::maxArgumentIndex; ++index) {
            BuiltinTypeID taken;
            bool hasCodeInLine = false;
            bool allTakeTheSame = std::ranges::all_of(table.overloadIDsOf(callee), [&](BuiltinOverloadID id) {
                const BuiltinOverload& overload = table.at(id);
                if (overload.mode != mode)
                    return true;
                const BuiltinParameter* parameter = table.parameterFor(overload, BuiltinOperand::argument(static_cast<unsigned>(index)));
                if (overload.isUndeclared() || !parameter || parameter->mode != BuiltinParameterMode::Required || parameter->conversion != BuiltinConversion::None)
                    return false;
                if (taken && taken != parameter->accepted)
                    return false;
                taken = parameter->accepted;
                hasCodeInLine |= overload.code.implementation.kind == BuiltinImplementationKind::Inline;
                return true;
            });
            if (!allTakeTheSame || !hasCodeInLine || !taken || fitOf(m_typing, m_expected.arguments[index], taken).fit != TypeFit::Unknown)
                continue;
            Type expected = types.at(m_proven.arguments[index]).intersectWith(typeOfBuiltinType({ m_typing }, taken), types);
            if (!expected.isNever())
                m_expected.arguments[index] = types.intern(WTF::move(expected));
        }
    }

    void expectReceiverToBePresent(const BuiltinTable& table, BuiltinID callee)
    {
        TypeArena& types = m_typing.types;
        if (!m_proven.receiver || m_expected.receiver != m_proven.receiver || !types.at(m_proven.receiver).mayBeUndefinedOrNull())
            return;
        bool refusesAbsentReceiver = std::ranges::all_of(table.overloadIDsOf(callee), [&](BuiltinOverloadID id) {
            const BuiltinOverload& overload = table.at(id);
            if (overload.mode != BuiltinCallMode::Call)
                return true;
            return overload.receiver
                && fitOf(m_typing, TypeTable::undefined, overload.receiver->accepted).fit == TypeFit::Outside
                && fitOf(m_typing, TypeTable::null, overload.receiver->accepted).fit == TypeFit::Outside;
        });
        if (!refusesAbsentReceiver)
            return;
        TypeID present = types.intern(types.at(m_proven.receiver).withoutPrimitives(Primitive::UndefinedOrNull));
        if (present == TypeTable::never)
            return;
        bool isTakenAsItIs = std::ranges::any_of(table.overloadIDsOf(callee), [&](BuiltinOverloadID id) {
            const BuiltinOverload& overload = table.at(id);
            if (overload.mode != BuiltinCallMode::Call)
                return false;
            BuiltinTypeID workedOn = overload.receiver->converted ? overload.receiver->converted : overload.receiver->accepted;
            return fitOf(m_typing, present, workedOn).fit == TypeFit::Inside;
        });
        if (isTakenAsItIs)
            m_expected.receiver = present;
    }

    TypingContext m_typing;
    BuiltinOperandTypes m_proven;
    BuiltinOperandTypes m_expected;
    BuiltinCallQuery m_query;
};

} // anonymous namespace

const UseLists& BuiltinUseInputs::useLists() const
{
    if (asksAboutPlace)
        *asksAboutPlace = true;
    return analyses->useLists();
}

static constexpr Effects nullishCheckEffects = declaredBoundOf(Opcode::RequireObjectCoercible);

static BuiltinUsePlan planByOpcode(const BuiltinUseInputs& inputs, ValueID use, ProgramPoint point)
{
    Opcode opcode = inputs.function.at(use).opcode;
    if (!builtinUseOpcodes.contains(opcode))
        return { };

    switch (opcode) {
    case Opcode::PropGet:
        return planBuiltinRead(inputs, use, point);
    case Opcode::PropSet:
        return planBuiltinStore(inputs, use, point);
    case Opcode::Call:
    case Opcode::Construct:
        return planCallOfBuiltin(inputs, use, point);
    case Opcode::SuperConstruct:
        return planSuperCallOfBuiltin(inputs, use, point);
    case Opcode::CallBuiltin:
        return planRematch(inputs, use, point);
    case Opcode::ArraySpeciesCreate:
        return planArraySpeciesCreate(inputs, use, point);
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinUsePlan planBuiltinUse(const BuiltinUseInputs& inputs, ValueID use, ProgramPoint point)
{
    BuiltinUsePlan plan = planByOpcode(inputs, use, point);
    if (!plan || resultFitOf(inputs, use, plan) == ResultFit::Within)
        return plan;

    if (plan.kind != BuiltinUseKind::LibraryCall)
        return { };
    plan.assumedResult = inputs.function.at(use).type;
    return plan;
}

bool mayFindProvenPlan(const BuiltinUseInputs& inputs, const Value& use)
{
    switch (use.opcode) {
    case Opcode::PropGet:
    case Opcode::PropSet:
        return mayFindProvenAccess(inputs, use);
    case Opcode::Call:
    case Opcode::Construct:
        return mayFindProvenCallee(inputs, use);
    default:
        return true;
    }
}

static ValueID operandAt(const BuiltinCallOperands& operands, BuiltinOperand operand)
{
    return BuiltinCall { { }, BuiltinCallForm::Whole, 0, operands.receiver, operands.arguments.span() }.operandAt(operand);
}

static PristineRequirement findUnprovenPristine(const BuiltinUseInputs& inputs, ValueID subject, const PristineRequirement& required, ProgramPoint point)
{
    PristineQuery query = inputs.pristine();
    if (pristineProofOf(query, subject, required, point) == PristineProof::Proven)
        return { };
    return { required.builtinClass, required.aspects - aspectsProvenAt(query, subject, required.builtinClass, point) };
}

void degradeToUnmatchedCall(BuiltinUsePlan& plan)
{
    plan.kind = BuiltinUseKind::UnmatchedCall;
    plan.form = BuiltinCallForm::AnyReceiver;
    plan.call = BuiltinCallPlan { .overload = plan.call.overload };
    plan.libraryFunction = { };
    plan.newTarget = { };
    plan.requirements.clear();
}

bool hasOnlyNeverAllocatedLayouts(const Program& program, const Type& type)
{
    return type.hasOnlyLayouts(program) && std::ranges::all_of(membersOf(program, type), [&](LayoutID member) {
        return program.at(member).flow.allocation == AllocationState::NeverAllocated;
    });
}

static size_t findRequirementToMergeWith(const BuiltinUsePlan& plan, const UseRequirement& requirement)
{
    RELEASE_ASSERT(!requirement.subject == !!requirement.unchangedLocation);
    return plan.requirements.findIf([&](const UseRequirement& candidate) {
        return candidate.subject == requirement.subject && candidate.unchangedLocation == requirement.unchangedLocation;
    });
}

static bool mergeRequirement(TypeArena& types, UseRequirement& requirement, const UseRequirement& other)
{
    RELEASE_ASSERT(requirement.subject == other.subject && requirement.unchangedLocation == other.unchangedLocation);
    bool isPossible = true;
    if (other.requiredType)
        requirement.requiredType = requirement.requiredType ? types.intersectionOf(requirement.requiredType, other.requiredType) : other.requiredType;
    if (requirement.requiredType == TypeTable::never)
        isPossible = false;
    if (other.functionOfBuiltin) {
        if (requirement.functionOfBuiltin && requirement.functionOfBuiltin != other.functionOfBuiltin)
            isPossible = false;
        requirement.functionOfBuiltin = other.functionOfBuiltin;
    }
    if (other.pristine) {
        if (requirement.pristine && requirement.pristine.builtinClass != other.pristine.builtinClass)
            isPossible = false;
        requirement.pristine = { other.pristine.builtinClass, requirement.pristine.aspects | other.pristine.aspects };
    }
    return isPossible;
}

bool appendRequirement(TypeArena& types, BuiltinUsePlan& plan, const UseRequirement& requirement)
{
    size_t existing = findRequirementToMergeWith(plan, requirement);
    if (existing != notFound)
        return mergeRequirement(types, plan.requirements[existing], requirement);
    plan.requirements.append(requirement);
    return requirement.requiredType != TypeTable::never;
}

bool prependRequirement(TypeArena& types, BuiltinUsePlan& plan, UseRequirement requirement)
{
    bool isPossible = requirement.requiredType != TypeTable::never;
    size_t existing = findRequirementToMergeWith(plan, requirement);
    if (existing != notFound) {
        isPossible = mergeRequirement(types, requirement, plan.requirements[existing]);
        plan.requirements.removeAt(existing);
    }
    plan.requirements.insert(0, WTF::move(requirement));
    return isPossible;
}

static FunctionID findLibraryFunction(const Program& program, const BuiltinOverload& overload)
{
    if (forwardsCall(overload))
        return { };
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, overload.libraryFunction());
    if (!entry || !entry->function)
        return { };
    if (overload.mode == BuiltinCallMode::Construct && canConstructWithoutClosure(program, entry->function))
        return entry->function;
    if (!canCallWithoutClosure(program, entry->function))
        return { };
    if (overload.mode == BuiltinCallMode::Construct && program.at(entry->function).hasAny({ FunctionFlags::HasThis, FunctionFlags::HasNewTarget }))
        return { };
    return entry->function;
}

static bool staysGenericOperation(const BuiltinOverload& overload)
{
    return overload.code.implementation.kind == BuiltinImplementationKind::Generic && !overload.hasTypedResult();
}

bool isCalledOnlyDirectly(const BuiltinTable& table, BuiltinID callee)
{
    return table.at(callee).scope == BuiltinScope::Module;
}

bool hasDirectFunction(const BuiltinTable& table, BuiltinID callee)
{
    return std::ranges::any_of(table.overloadIDsOf(callee), [](BuiltinOverloadID overload) {
        return !!BuiltinBindings::directFunctionNumberOf(overload);
    });
}

BuiltinUsePlan planKnownBuiltinCall(const BuiltinUseInputs& inputs, const KnownBuiltinCall& known, ProgramPoint point)
{
    const BuiltinTable& table = inputs.program.builtins();
    if (isCalledOnlyDirectly(table, known.callee) && !hasDirectFunction(table, known.callee))
        return { };
    if (inputs.program.mayBeTrimmed(known.callee))
        return { };
    CallQueryStorage storage(inputs, known.callee, known.mode, known.operands, known.nullishReceiver);

    BuiltinUsePlan plan;
    plan.member = known.callee;
    plan.operands = known.operands;
    plan.stamps = known.stamps;
    plan.call = planBuiltinCall(storage.query());
    if (!plan.call.overload)
        return { };
    if (staysGenericOperation(table.at(plan.call.overload)))
        return { };
    if (plan.call.state == BuiltinMatchState::Unknown) {
        if (isCalledOnlyDirectly(table, known.callee))
            return { };
        degradeToUnmatchedCall(plan);
        return plan;
    }

    bool isCallAsItStands = known.overloadOfWholeCall == plan.call.overload
        && !insertsOperations(plan.call)
        && !findLibraryFunction(inputs.program, table.at(plan.call.overload));
    if (isCallAsItStands)
        return { };

    for (const OperandRequirement& requirement : plan.call.requirements) {
        ValueID subject = operandAt(known.operands, requirement.operand);
        RELEASE_ASSERT(subject);
        PristineRequirement unproven = findUnprovenPristine(inputs, subject, requirement.pristine, point);
        if (unproven && pristineTestOf(table, unproven.builtinClass) == PristineTest::NeverPasses) {
            if (isCalledOnlyDirectly(table, known.callee))
                return { };
            degradeToUnmatchedCall(plan);
            return plan;
        }
        if (!requirement.requiredType && !unproven)
            continue;
        bool testNeverPasses = requirement.requiredType && hasOnlyNeverAllocatedLayouts(inputs.program, inputs.types.at(requirement.requiredType));
        if (testNeverPasses || !appendRequirement(inputs.types, plan, { subject, requirement.requiredType, unproven })) {
            if (isCalledOnlyDirectly(table, known.callee))
                return { };
            degradeToUnmatchedCall(plan);
            return plan;
        }
    }

    if (isCalledOnlyDirectly(table, known.callee) && !BuiltinBindings::directFunctionNumberOf(plan.call.overload, known.operands.arguments.size()))
        return { };

    plan.kind = BuiltinUseKind::Call;
    plan.form = BuiltinCallForm::Whole;
    if (FunctionID libraryFunction = findLibraryFunction(inputs.program, table.at(plan.call.overload))) {
        plan.kind = BuiltinUseKind::LibraryCall;
        plan.libraryFunction = libraryFunction;
    }
    return plan;
}

static Value makeFunctionTest(ValueID subject, BuiltinID member)
{
    Value test = IRBuilder::make(Opcode::IsFunctionOfBuiltin, { subject });
    test.setMember(member);
    return test;
}

static Value makeLocationTest(BuiltinID member)
{
    Value test = IRBuilder::make(Opcode::IsBuiltinLocationUnchanged);
    test.setMember(member);
    return test;
}

Vector<Value, 4> createTestsOf(std::span<const UseRequirement> requirements)
{
    Vector<Value, 4> tests;
    for (const UseRequirement& requirement : requirements) {
        if (requirement.functionOfBuiltin)
            tests.append(makeFunctionTest(requirement.subject, requirement.functionOfBuiltin));
    }
    for (const UseRequirement& requirement : requirements) {
        if (requirement.requiredType)
            tests.append(makeTypeTest(requirement.subject, requirement.requiredType));
    }
    for (const UseRequirement& requirement : requirements) {
        if (requirement.pristine)
            tests.append(makePristineTest(requirement.subject, requirement.pristine.builtinClass, requirement.pristine.aspects));
    }
    for (const UseRequirement& requirement : requirements) {
        if (requirement.unchangedLocation)
            tests.append(makeLocationTest(requirement.unchangedLocation));
    }
    return tests;
}

Vector<Value, 4> createTestsOf(const BuiltinUsePlan& plan)
{
    return createTestsOf(plan.requirements.span());
}

static Value makeConstant(const Program& program, const BuiltinUsePlan& plan)
{
    if (plan.constant)
        return *plan.constant;
    if (plan.key) {
        Value constant = IRBuilder::make(Opcode::KeyConstant);
        constant.setKey(plan.key);
        return constant;
    }
    std::optional<Value> constant = tryMakeBuiltinConstant(program, plan.member);
    RELEASE_ASSERT(constant);
    return WTF::move(*constant);
}

static Value makeHostValueRead(const BuiltinUsePlan& plan)
{
    Value read = IRBuilder::make(Opcode::GetBuiltinValue);
    read.setBuiltin(plan.member);
    read.setOverload(plan.call.overload);
    return read;
}

static Effects effectsOfCallWhenApplied(const BuiltinUseInputs& inputs, const BuiltinUsePlan& plan)
{
    const BuiltinOverload& overload = inputs.program.builtins().at(plan.call.overload);
    CallQueryStorage storage(inputs, overload.builtin, overload.mode, plan.operands, NullishReceiver::ThrowsInBuiltin);
    Effects effects = effectsOfPlannedCall(storage.query(), plan.call, plan.form);
    effects.merge(effectsOfTypedResult(inputs.effects, plan.call.overload, plan.stamps.expectedResult).normal);
    return effects;
}

Effects effectsWhenApplied(const BuiltinUseInputs& inputs, const BuiltinUsePlan& plan)
{
    switch (plan.kind) {
    case BuiltinUseKind::None:
        return Effects();
    case BuiltinUseKind::Constant: {
        Effects effects = declaredBoundOf(makeConstant(inputs.program, plan).opcode);
        effects.merge(nullishCheckEffects);
        return effects;
    }
    case BuiltinUseKind::Undefined:
        return declaredBoundOf(Opcode::Undefined);
    case BuiltinUseKind::HostValue: {
        Effects effects = declaredBoundOf(Opcode::GetBuiltinValue);
        effects.merge(nullishCheckEffects);
        return effects;
    }
    case BuiltinUseKind::Call:
        return effectsOfCallWhenApplied(inputs, plan);
    case BuiltinUseKind::UnmatchedCall: {
        Effects effects = effectsOfCallWhenApplied(inputs, plan);
        effects.merge(nullishCheckEffects);
        return effects;
    }
    case BuiltinUseKind::LibraryCall: {
        Effects effects = effectsOfCalling(inputs.effects, plan.libraryFunction).normal;
        effects.merge(effectsOfCallWhenApplied(inputs, plan));
        return effects;
    }
    case BuiltinUseKind::NewArray:
        return declaredBoundOf(Opcode::NewArrayLike);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Type typeWhenApplied(const BuiltinUseInputs& inputs, const BuiltinUsePlan& plan)
{
    TypingContext typing = inputs.typing();
    switch (plan.kind) {
    case BuiltinUseKind::None:
        break;
    case BuiltinUseKind::Constant:
        return typeOfConstant(typing, makeConstant(inputs.program, plan));
    case BuiltinUseKind::Undefined:
        return Type::fromPrimitives(Primitive::Undefined);
    case BuiltinUseKind::HostValue: {
        BuiltinOperandTypes noOperands;
        return resultTypeOfBuiltinCall(typing, { plan.call.overload, BuiltinCallForm::Whole, 0, noOperands }, 0);
    }
    case BuiltinUseKind::Call:
    case BuiltinUseKind::UnmatchedCall: {
        BuiltinOperandTypes operands = operandTypesOf(typing, plan.operands.receiver, plan.operands.arguments.span());
        return resultTypeOfBuiltinCall(typing, { plan.call.overload, plan.form, 0, operands }, 0);
    }
    case BuiltinUseKind::LibraryCall:
        return typing.typeOf(inputs.program.at(plan.libraryFunction).result());
    case BuiltinUseKind::NewArray:
        return Type::builtinClass(BuiltinClasses::Array);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ResultFit resultFitOf(const BuiltinUseInputs& inputs, ValueID use, const BuiltinUsePlan& plan)
{
    if (!plan)
        return ResultFit::Unknown;

    const Value& value = inputs.function.at(use);
    if (resultRuleOf(value.opcode) == ResultRule::None)
        return ResultFit::Within;
    const Type& current = inputs.types.at(value.type);
    if (current.isAnything())
        return ResultFit::Within;
    return typeWhenApplied(inputs, plan).isSubsetOf(current, inputs.program) ? ResultFit::Within : ResultFit::Unknown;
}

static void finish(const BuiltinUseRewrite& rewrite, const ReplacedEffects& replaced)
{
    FunctionPassContext& context = rewrite.context;
    Value& value = context.function.edit(rewrite.use);
    value.type = resultTypeOf(context.program, context.function, value, context.types, &context.typingMemo);
    value.setExpected(expectedTypeOf(context.program, context.function, value, context.types, &context.typingMemo));
    checkRewrite(context, ValueSite { rewrite.block, 0, rewrite.use }, replaced);
}

static ValueID insertReceiverCheck(const FunctionPassContext& context, ValueID receiver, const NullishCheck& check, const InsertBeforeCall& insert)
{
    if (!context.program.typeOf(context.function, receiver).mayBeUndefinedOrNull())
        return receiver;

    BuiltinCallPlan checkOnly;
    checkOnly.state = BuiltinMatchState::Proven;
    checkOnly.hasReceiver = true;
    checkOnly.steps.append(OperandStep { BuiltinOperand::receiver(), OperandStepKind::RequireObjectCoercible, BuiltinConversion::None });
    return insertOperandSteps(checkOnly, { receiver, { } }, check, insert).receiver;
}

static void replaceKeepingOrigin(Value& value, Value&& replacement)
{
    Origin origin = value.origin();
    value = WTF::move(replacement);
    value.setOrigin(origin);
}

static bool reliesOnPristineOperand(const BuiltinCallPlan& call)
{
    return call.requirements.containsIf([](const OperandRequirement& requirement) {
        return !!requirement.pristine;
    });
}

static void becomePlannedCall(Value& value, const BuiltinUsePlan& plan, const BuiltinCallOperands& operands)
{
    bool isDemotedTailCall = FlagMask::isDemotedTailCall(value.opcode) && value.isDemotedTailCall();
    becomeBuiltinCall(value, plan.call.overload, plan.form, operands, plan.stamps);
    value.setNeedsPristineReceiver(reliesOnPristineOperand(plan.call));
    value.setIsDemotedTailCall(isDemotedTailCall);
}

static ValueID insertNewTarget(const BuiltinUseRewrite& rewrite, const BuiltinUsePlan& plan, Origin origin)
{
    FunctionPassContext& context = rewrite.context;
    if (!context.program.at(plan.libraryFunction).has(FunctionFlags::HasNewTarget))
        return { };
    if (plan.newTarget)
        return plan.newTarget;
    ValueID constructor = rewrite.insert(makeConstant(context.program, plan));
    context.function.edit(constructor, ValueEdit::Definition).setOrigin(origin);
    return constructor;
}

static Value makeLibraryCall(const BuiltinUseRewrite& rewrite, const BuiltinUsePlan& plan, const BuiltinCallOperands& operands)
{
    FunctionPassContext& context = rewrite.context;
    Origin origin = context.function.at(rewrite.use).origin();
    bool isConstruction = context.program.builtins().at(plan.call.overload).mode == BuiltinCallMode::Construct;
    ValueID newTarget = isConstruction ? insertNewTarget(rewrite, plan, origin) : ValueID();

    const Value& use = context.function.at(rewrite.use);
    DirectCallRequest request {
        .callee = plan.libraryFunction,
        .thisValue = operands.receiver,
        .newTarget = newTarget,
        .arguments = operands.arguments.span(),
        .accessSite = plan.stamps.accessSite,
        .isConstruction = isConstruction,
        .isNeverInlined = plan.isNeverInlined,
        .isDemotedTailCall = FlagMask::isDemotedTailCall(use.opcode) && use.isDemotedTailCall(),
    };
    return makeDirectCall(ValueInsertion { context.analyses.context(), context.function, context.types, origin, rewrite.insert }, request);
}

void applyBuiltinUse(const BuiltinUseRewrite& rewrite, const BuiltinUsePlan& plan)
{
    RELEASE_ASSERT(plan.isProven());
    FunctionPassContext& context = rewrite.context;
    Function& function = context.function;

    ReplacedEffects replaced;
    if (context.build.options().verifiesProgram)
        replaced.add({ context.program, context.inputs.effects }, function, function.at(rewrite.use));

    Origin origin = function.at(rewrite.use).origin();
    bool isPropertyAccess = accessOf(function.at(rewrite.use).opcode) != Access::None;
    auto insertWithOrigin = [&](auto&& inserted) {
        ValueID id = rewrite.insert(WTF::move(inserted));
        function.edit(id, ValueEdit::Definition).setOrigin(origin);
        return id;
    };
    InsertBeforeCall insert = insertWithOrigin;

    switch (plan.kind) {
    case BuiltinUseKind::None:
        RELEASE_ASSERT_NOT_REACHED();
    case BuiltinUseKind::Constant:
        insertReceiverCheck(context, function.at(rewrite.use).objectOperand(), plan.nullishCheck, insert);
        replaceKeepingOrigin(function.edit(rewrite.use), makeConstant(context.program, plan));
        break;
    case BuiltinUseKind::Undefined:
        replaceKeepingOrigin(function.edit(rewrite.use), IRBuilder::make(Opcode::Undefined));
        break;
    case BuiltinUseKind::HostValue:
        insertReceiverCheck(context, function.at(rewrite.use).objectOperand(), plan.nullishCheck, insert);
        replaceKeepingOrigin(function.edit(rewrite.use), makeHostValueRead(plan));
        break;
    case BuiltinUseKind::Call: {
        BuiltinCallOperands operands = insertOperandSteps(plan.call, plan.operands, plan.nullishCheck, insert);
        becomePlannedCall(function.edit(rewrite.use), plan, operands);
        break;
    }
    case BuiltinUseKind::UnmatchedCall: {
        BuiltinCallOperands operands = plan.operands;
        if (isPropertyAccess)
            operands.receiver = insertReceiverCheck(context, operands.receiver, plan.nullishCheck, insert);
        becomePlannedCall(function.edit(rewrite.use), plan, operands);
        break;
    }
    case BuiltinUseKind::LibraryCall: {
        BuiltinCallOperands operands = insertOperandSteps(plan.call, plan.operands, plan.nullishCheck, insert);
        Value call = makeLibraryCall(rewrite, plan, operands);
        if (plan.assumedResult) {
            ValueID result = insert(WTF::move(call));
            if (context.build.validates())
                insert(makeValidateType(result, plan.assumedResult));
            call = makeAssumeType(result, plan.assumedResult);
        }
        replaceKeepingOrigin(function.edit(rewrite.use), WTF::move(call));
        break;
    }
    case BuiltinUseKind::NewArray: {
        const Value& creation = function.at(rewrite.use);
        replaceKeepingOrigin(function.edit(rewrite.use), IRBuilder::make(Opcode::NewArrayLike, { creation.lengthOperand(), creation.arrayOperand() }));
        break;
    }
    }

    finish(rewrite, replaced);
}

static PropertyKeyID memberKeyOf(const Value& operation)
{
    switch (operation.opcode) {
    case Opcode::PropGet:
    case Opcode::PropSet:
        return operation.key();
    case Opcode::Call:
        return operation.calleeKey();
    default:
        return { };
    }
}

BuiltinUseResolution resolutionWithReceiver(const BuiltinUseInputs& inputs, const Value& operation, const Type& receiver)
{
    PropertyKeyID key = memberKeyOf(operation);
    if (!key)
        return BuiltinUseResolution::Unknown;

    BuiltinReceiver start = builtinReceiverOf(inputs.program, receiver.withoutNullish());
    BuiltinMemberAnswer answer;
    switch (start.kind) {
    case BuiltinReceiverKind::Unknown:
        return BuiltinUseResolution::Unknown;
    case BuiltinReceiverKind::BuiltinObject:
        answer = findMemberFrom(inputs.lookup(), start.object, key);
        break;
    case BuiltinReceiverKind::Instance:
    case BuiltinReceiverKind::Primitive:
        answer = findMemberOfInstances(inputs.lookup(), start.builtinClass, key);
        break;
    }
    return answer.isFound() ? BuiltinUseResolution::Resolves : BuiltinUseResolution::Unknown;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
