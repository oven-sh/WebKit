/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinUses.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfRules.h"
#include "AOTFlowResults.h"
#include "AOTForwardedCalls.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTSplitUnions.h"
#include "AOTTypeTestSite.h"
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
        , m_predicted(operandTypesOf(m_typing.predicting(), operands.receiver, operands.arguments.span()))
        , m_query { { m_typing, callee, mode, m_proven, m_predicted }, inputs.effects, nullishReceiver }
    {
    }

    const BuiltinCallQuery& query() const LIFETIME_BOUND { return m_query; }

private:
    TypingContext m_typing;
    BuiltinOperandTypes m_proven;
    BuiltinOperandTypes m_predicted;
    BuiltinCallQuery m_query;
};

} // anonymous namespace

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

// A test decides these types exactly: no proof about a class, no layout, no single function.
static bool canClaimExactly(const Type& type)
{
    return type == type.withoutClassProofs() && type.builtins.isEmpty() && type.functions.isEmpty() && !type.layouts;
}

BuiltinUsePlan planBuiltinUse(const BuiltinUseInputs& inputs, ValueID use, ProgramPoint point)
{
    BuiltinUsePlan plan = planByOpcode(inputs, use, point);
    if (!plan || resultFitOf(inputs, use, plan) == ResultFit::Within)
        return plan;

    // The written types of the library are predictions, so its functions declare less than the table says of their entries. A claim may throw, as the entry may.
    TypeID current = inputs.function.at(use).type;
    bool canClaimResult = plan.kind == BuiltinUseKind::LibraryCall && inputs.program.builtins().at(plan.call.overload).throws() && canClaimExactly(inputs.types.at(current));
    if (!canClaimResult)
        return { };
    plan.claimedResult = current;
    return plan;
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
    return type.hasOnlyLayouts() && std::ranges::all_of(membersOf(program, type), [&](LayoutID member) {
        return program.at(member).flow.allocation == AllocationState::NeverAllocated;
    });
}

static bool mergeRequirement(TypeArena& types, UseRequirement& requirement, const UseRequirement& other)
{
    RELEASE_ASSERT(requirement.subject == other.subject);
    bool isPossible = true;
    if (other.requiredType)
        requirement.requiredType = requirement.requiredType ? types.intersectionOf(requirement.requiredType, other.requiredType) : other.requiredType;
    if (requirement.requiredType == TypeTable::never)
        isPossible = false;
    if (other.pristine) {
        if (requirement.pristine && requirement.pristine.builtinClass != other.pristine.builtinClass)
            isPossible = false;
        requirement.pristine = { other.pristine.builtinClass, requirement.pristine.aspects | other.pristine.aspects };
    }
    return isPossible;
}

bool appendRequirement(TypeArena& types, BuiltinUsePlan& plan, const UseRequirement& requirement)
{
    auto existing = std::ranges::find(plan.requirements, requirement.subject, &UseRequirement::subject);
    if (existing != plan.requirements.end())
        return mergeRequirement(types, *existing, requirement);
    plan.requirements.append(requirement);
    return requirement.requiredType != TypeTable::never;
}

bool prependRequirement(TypeArena& types, BuiltinUsePlan& plan, UseRequirement requirement)
{
    bool isPossible = requirement.requiredType != TypeTable::never;
    auto existing = std::ranges::find(plan.requirements, requirement.subject, &UseRequirement::subject);
    if (existing != plan.requirements.end()) {
        isPossible = mergeRequirement(types, requirement, *existing);
        plan.requirements.removeAt(existing - plan.requirements.begin());
    }
    plan.requirements.insert(0, WTF::move(requirement));
    return isPossible;
}

// A value that is validated against nothing is the value, which is what the function of the library behind the entry returns. No pass expands such a call.
static RuntimeFunctionID libraryFunctionOf(const Program& program, const BuiltinOverload& overload, ExpectedResultID expected)
{
    bool validatesResult = expected && !program.at(expected).validation.isEmpty();
    if (overload.result.typedResultForm == TypedResultForm::ValidatedValue && !validatesResult)
        return overload.slowPathFunction();
    return overload.libraryFunction();
}

// A call that a built-in only forwards becomes the forwarded call, which ResolveCalls makes of the CallBuiltin.
static FunctionID findLibraryFunction(const Program& program, const BuiltinOverload& overload, ExpectedResultID expected)
{
    if (forwardsCall(overload))
        return { };
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, libraryFunctionOf(program, overload, expected));
    if (!entry || !entry->function)
        return { };
    if (rejectsOnFailedConversion(program, entry->function))
        return { };
    if (overload.mode == BuiltinCallMode::Construct && canConstructWithoutClosure(program, entry->function))
        return entry->function;
    if (!canCallWithoutClosure(program, entry->function))
        return { };
    if (overload.mode == BuiltinCallMode::Construct && program.at(entry->function).hasAny({ FunctionFlags::HasThis, FunctionFlags::HasNewTarget }))
        return { };
    return entry->function;
}

// The operation goes on calling or reading whatever the property holds: the entry may have no function object of its own.
// A call with a typed result is expanded from its CallBuiltin.
static bool staysGenericOperation(const BuiltinOverload& overload)
{
    return overload.code.implementation.kind == BuiltinImplementationKind::Generic && !overload.hasTypedResult();
}

// A direct call whose arguments need a conversion enters the version with the written types.
static FunctionID writtenVersionOf(const BuiltinUseInputs& inputs, FunctionID libraryFunction)
{
    return versionToEnter(inputs.program, inputs.flow, libraryFunction, { });
}

BuiltinUsePlan planKnownBuiltinCall(const BuiltinUseInputs& inputs, const KnownBuiltinCall& known, ProgramPoint point)
{
    const BuiltinTable& table = inputs.program.builtins();
    // A CallBuiltin names no module.
    if (table.at(known.callee).scope != BuiltinScope::Realm)
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
        degradeToUnmatchedCall(plan);
        return plan;
    }

    for (const OperandRequirement& requirement : plan.call.requirements) {
        ValueID subject = operandAt(known.operands, requirement.operand);
        RELEASE_ASSERT(subject);
        PristineRequirement unproven = findUnprovenPristine(inputs, subject, requirement.pristine, point);
        if (unproven && pristineTestOf(table, unproven.builtinClass) == PristineTest::NeverPasses) {
            degradeToUnmatchedCall(plan);
            return plan;
        }
        if (!requirement.requiredType && !unproven)
            continue;
        bool testNeverPasses = requirement.requiredType && hasOnlyNeverAllocatedLayouts(inputs.program, inputs.types.at(requirement.requiredType));
        if (testNeverPasses || !appendRequirement(inputs.types, plan, { subject, requirement.requiredType, unproven })) {
            degradeToUnmatchedCall(plan);
            return plan;
        }
    }

    plan.kind = BuiltinUseKind::Call;
    plan.form = BuiltinCallForm::Whole;
    if (FunctionID libraryFunction = findLibraryFunction(inputs.program, table.at(plan.call.overload), known.stamps.expectedResult)) {
        plan.kind = BuiltinUseKind::LibraryCall;
        plan.libraryFunction = libraryFunction;
    }
    return plan;
}

Vector<Value, 4> createTestsOf(std::span<const UseRequirement> requirements)
{
    Vector<Value, 4> tests;
    for (const UseRequirement& requirement : requirements) {
        if (requirement.requiredType)
            tests.append(makeTypeTest(requirement.subject, requirement.requiredType));
    }
    for (const UseRequirement& requirement : requirements) {
        if (requirement.pristine)
            tests.append(makePristineTest(requirement.subject, requirement.pristine.builtinClass, requirement.pristine.aspects));
    }
    return tests;
}

Vector<Value, 4> createTestsOf(const BuiltinUsePlan& plan)
{
    return createTestsOf(plan.requirements.span());
}

static Value makeConstant(const BuiltinTable& table, const BuiltinUsePlan& plan)
{
    if (plan.key) {
        Value constant = IRBuilder::make(Opcode::KeyConstant);
        constant.setKey(plan.key);
        return constant;
    }
    std::optional<Value> constant = tryMakeBuiltinConstant(table, plan.member);
    RELEASE_ASSERT(constant);
    return WTF::move(*constant);
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
        Effects effects = declaredBoundOf(makeConstant(inputs.program.builtins(), plan).opcode);
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
        effects.merge(effectsOfCalling(inputs.effects, writtenVersionOf(inputs, plan.libraryFunction)).normal);
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
        return plan.key ? Type::symbolLiteral(plan.key) : typeOfBuiltinConstant(inputs.program.builtins(), plan.member);
    case BuiltinUseKind::Call:
    case BuiltinUseKind::UnmatchedCall: {
        BuiltinOperandTypes operands = operandTypesOf(typing, plan.operands.receiver, plan.operands.arguments.span());
        return resultTypeOfBuiltinCall(typing, { plan.call.overload, plan.form, 0, operands }, 0);
    }
    case BuiltinUseKind::LibraryCall: {
        const Type& result = typing.typeOf(inputs.program.at(plan.libraryFunction).result());
        const Type& resultOfWrittenVersion = typing.typeOf(inputs.program.at(writtenVersionOf(inputs, plan.libraryFunction)).result());
        return result.unionWith(resultOfWrittenVersion, inputs.types);
    }
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
    Value& value = context.function.at(rewrite.use);
    value.type = resultTypeOf(context.program, context.function, value, context.types);
    value.setWritten(writtenTypeOf(context.program, context.function, value, context.types));
    value.setPrediction(predictionOf(context.program, context.function, value, context.types));
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

// `new C(..)` constructs for C itself.
static ValueID insertNewTarget(const BuiltinUseRewrite& rewrite, const BuiltinUsePlan& plan, Origin origin)
{
    FunctionPassContext& context = rewrite.context;
    if (!context.program.at(plan.libraryFunction).has(FunctionFlags::HasNewTarget))
        return { };
    if (plan.newTarget)
        return plan.newTarget;
    ValueID constructor = rewrite.insert(makeConstant(context.program.builtins(), plan));
    context.function.at(constructor).setOrigin(origin);
    return constructor;
}

static std::optional<Value> tryMakeLibraryCall(const BuiltinUseRewrite& rewrite, const BuiltinUsePlan& plan, const BuiltinCallOperands& operands)
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
    return tryMakeDirectCall(ValueInsertion { context.analyses.context(), context.function, context.types, origin, rewrite.insert }, request);
}

void applyBuiltinUse(const BuiltinUseRewrite& rewrite, const BuiltinUsePlan& plan)
{
    RELEASE_ASSERT(plan.isProven());
    FunctionPassContext& context = rewrite.context;
    Function& function = context.function;

    ReplacedEffects replaced;
    replaced.add({ context.program, context.inputs.effects }, function, function.at(rewrite.use));

    Origin origin = function.at(rewrite.use).origin();
    bool isPropertyAccess = accessOf(function.at(rewrite.use).opcode) != Access::None;
    // ScopedLambda hands its arguments to the functor as lvalues.
    auto insertWithOrigin = [&](auto&& inserted) {
        ValueID id = rewrite.insert(WTF::move(inserted));
        function.at(id).setOrigin(origin);
        return id;
    };
    InsertBeforeCall insert = insertWithOrigin;

    switch (plan.kind) {
    case BuiltinUseKind::None:
        RELEASE_ASSERT_NOT_REACHED();
    case BuiltinUseKind::Constant:
        insertReceiverCheck(context, function.at(rewrite.use).objectOperand(), plan.nullishCheck, insert);
        replaceKeepingOrigin(function.at(rewrite.use), makeConstant(context.program.builtins(), plan));
        break;
    case BuiltinUseKind::Call: {
        BuiltinCallOperands operands = insertOperandSteps(plan.call, plan.operands, plan.nullishCheck, insert);
        becomePlannedCall(function.at(rewrite.use), plan, operands);
        break;
    }
    case BuiltinUseKind::UnmatchedCall: {
        BuiltinCallOperands operands = plan.operands;
        if (isPropertyAccess)
            operands.receiver = insertReceiverCheck(context, operands.receiver, plan.nullishCheck, insert);
        becomePlannedCall(function.at(rewrite.use), plan, operands);
        break;
    }
    case BuiltinUseKind::LibraryCall: {
        BuiltinCallOperands operands = insertOperandSteps(plan.call, plan.operands, plan.nullishCheck, insert);
        std::optional<Value> call = tryMakeLibraryCall(rewrite, plan, operands);
        // A CallBuiltin constructs for the built-in itself.
        RELEASE_ASSERT(call || !plan.newTarget);
        if (!call) {
            becomePlannedCall(function.at(rewrite.use), plan, operands);
            break;
        }
        if (plan.claimedResult) {
            Value claim = IRBuilder::make(Opcode::Claim, { insert(WTF::move(*call)) });
            claim.setCastType(plan.claimedResult);
            claim.setCause(CheckCause::AddedByPass);
            call = WTF::move(claim);
        }
        replaceKeepingOrigin(function.at(rewrite.use), WTF::move(*call));
        break;
    }
    case BuiltinUseKind::NewArray: {
        const Value& creation = function.at(rewrite.use);
        replaceKeepingOrigin(function.at(rewrite.use), IRBuilder::make(Opcode::NewArrayLike, { creation.lengthOperand(), creation.arrayOperand() }));
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
