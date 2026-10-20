/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyAccessInternal.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(accessOf(Opcode::PutSlot) == Access::Store && accessOf(Opcode::PutSlotOrAdd) == Access::Store);
static_assert(accessOf(Opcode::PutModuleVariable) == Access::None);

namespace {

struct FieldArmState {
    bool isAlwaysFilled { true };
    bool hasConstructor { false };
    bool cannotMoveOut { true };
    bool mayLieOutOfLine { false };
    bool isAbsentOnPrototypes { true };
    bool inheritsNothingUnderName { true };
    bool valueFits { true };
    bool createdOnlyAtConstruction { true };
    bool hasImmutableMember { false };
    bool hasDeferredMember { false };
    bool deferralIsUniform { true };
    // Unknown code may have stored another function into the slot of an object that reached it.
    bool holdsOnlyDeferredFunction { true };
    ClosureDeferral deferral { ClosureDeferral::Unknown };
    FunctionID deferredFunction;
    ReceiverState receivers;
};

struct CreationTests {
    ProofSet proofs;
    MemberConditions conditions;

    bool isEmpty() const { return proofs.isEmpty() && conditions.isEmpty(); }
};

} // anonymous namespace

static constexpr OptionSet<FieldStability> requiredToStayInSlot { FieldStability::IsNeverRedefined, FieldStability::IsNeverStoredWithOtherType };
static constexpr OptionSet<FieldStability> requiredToStayFilled { FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined, FieldStability::IsNeverStoredWithOtherType };

static FieldArmState stateOfFieldArm(const Program& program, const PropertyAccessQuery& query, const MemberArm& arm)
{
    FieldArmState state;
    for (LayoutID layout : arm.layouts) {
        std::optional<FieldReference> reference = findField(program, layout, query.key, BaseChain::Proven);
        RELEASE_ASSERT(reference);
        const Field& field = fieldAt(program, *reference);
        const FieldFlow& flow = flowOf(program, *reference);

        bool memberIsAlwaysFilled = flow.occupancy == Occupancy::AlwaysFilled && flow.stability.containsAll(requiredToStayFilled);
        state.isAlwaysFilled &= memberIsAlwaysFilled;
        state.hasConstructor |= !!program.at(declaredLayoutOf(program, layout)).constructor;
        state.cannotMoveOut &= flow.stability.containsAll(requiredToStayInSlot);
        state.mayLieOutOfLine |= program.at(layout).mayKeepFieldsOutOfLine() && flow.location.initialization != InitializationState::SetAtCreation;
        // A lookup that always finds the slot filled never goes on to the prototypes. The store that fills the slot does.
        bool inheritsNothingUnderName = query.isPrivate || flow.prototypeName == PrototypeNameState::AbsentOnPrototypes;
        state.isAbsentOnPrototypes &= memberIsAlwaysFilled || inheritsNothingUnderName;
        state.inheritsNothingUnderName &= inheritsNothingUnderName;
        state.valueFits &= query.stored && query.stored->isSubsetOf(program.at(field.declared.type), program);
        state.createdOnlyAtConstruction &= program.at(layout).flow.fieldCreation == FieldCreation::OnlyAtConstruction;
        state.hasImmutableMember |= field.flags.contains(FieldFlags::IsImmutable);

        const LocationFlow& location = flow.location;
        state.hasDeferredMember |= location.deferral != ClosureDeferral::Unknown;
        state.holdsOnlyDeferredFunction &= location.stores == StoreCount::AtMostOnce;
        if (layout == arm.layouts[0]) {
            state.deferral = location.deferral;
            state.deferredFunction = location.deferredFunction;
        } else if (location.deferral != state.deferral || location.deferredFunction != state.deferredFunction)
            state.deferralIsUniform = false;
    }
    state.receivers = receiverStateOf(program, arm.layouts.span());
    return state;
}

static bool canCallWithoutFieldRead(const Program& program, const FieldArmState& state)
{
    if (!state.deferralIsUniform || !state.deferredFunction || !state.holdsOnlyDeferredFunction)
        return false;
    const Function& function = program.at(state.deferredFunction);
    if (function.has(FunctionFlags::TakesCallee))
        return false;
    switch (state.deferral) {
    case ClosureDeferral::Unknown:
        return false;
    case ClosureDeferral::CapturesNothing:
        return function.captures.isEmpty();
    case ClosureDeferral::CapturesObject:
        return function.captures.size() == 1;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void setSlotOperation(PropertyAccessPlan& plan, Opcode operation)
{
    plan.form = AccessForm::Slot;
    plan.operation = operation;
}

static void requireFilledSlot(const FieldArmState& state, PropertyAccessPlan& plan)
{
    if (!state.isAlwaysFilled)
        plan.requiredProofs.add(Proof::SlotFilled);
}

static void requireStoresAllowed(const PropertyAccessQuery& query, const FieldArmState& state, PropertyAccessPlan& plan)
{
    if (!query.isPrivate && !state.receivers.cannot(ObjectMutation::ChangeIntegrity))
        plan.requiredProofs.add(Proof::NotFrozen);
}

static bool mayStoreIntoUnfinishedObject(const PropertyAccessContext& context, const PropertyAccessQuery& query)
{
    return !context.flow || !context.flow->seesOnlyFinishedObjects(query.site);
}

// No read sees the field of a class empty when the constructor fills it before the object leaks. The store that fills it does.
static bool mayFindSlotEmpty(const PropertyAccessContext& context, const PropertyAccessQuery& query, const FieldArmState& state)
{
    if (!state.isAlwaysFilled)
        return true;
    if (!state.hasConstructor || query.isListedByAllocation)
        return false;
    return mayStoreIntoUnfinishedObject(context, query);
}

// A store that finds the slot empty creates the property only where the object has it nowhere else and inherits nothing of that name.
static std::optional<CreationTests> testsToCreateField(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, const FieldArmState& state)
{
    CreationTests tests;
    if (!state.cannotMoveOut)
        tests.proofs.add(Proof::NoExtraProperties);
    if (state.inheritsNothingUnderName)
        return tests;
    for (LayoutID layout : arm.layouts) {
        MemberAnswer inherited = findMemberOnPrototypes(context, { MemberStart::Instance, layout, query.key, Access::Store, false });
        if (inherited.kind != MemberKind::Absent)
            return std::nullopt;
        tests.proofs |= inherited.receiverProofs;
        addConditions(tests.conditions, inherited.conditions.span());
    }
    return tests;
}

static Opcode creatingStoreFor(const FieldArmState& state)
{
    return state.receivers.keys == KeyEnumeration::NeverEnumerated && !state.mayLieOutOfLine ? Opcode::PutSlot : Opcode::PutSlotOrAdd;
}

static void planFieldRead(const Program& program, const PropertyAccessQuery& query, const FieldArmState& state, PropertyAccessPlan& plan)
{
    if (query.isOnlyCalled && canCallWithoutFieldRead(program, state)) {
        plan.form = AccessForm::Method;
        plan.member.method = state.deferredFunction;
        plan.capturesReceiver = state.deferral == ClosureDeferral::CapturesObject;
        requireFilledSlot(state, plan);
        return;
    }
    // The slot of a private field never defers its closure.
    if (state.hasDeferredMember && !query.isPrivate) {
        setSlotOperation(plan, Opcode::GetDeferredClosureSlot);
        requireFilledSlot(state, plan);
        return;
    }
    if (!state.isAlwaysFilled && state.cannotMoveOut && state.isAbsentOnPrototypes && !query.isPrivate) {
        setSlotOperation(plan, Opcode::GetSlotOrUndefined);
        return;
    }
    setSlotOperation(plan, Opcode::GetSlot);
    requireFilledSlot(state, plan);
    plan.emptySlotMeansNoOwnProperty = !state.isAlwaysFilled && state.cannotMoveOut && !query.isPrivate;
}

static void planFieldTest(const PropertyAccessQuery& query, const FieldArmState& state, PropertyAccessPlan& plan)
{
    if (state.isAlwaysFilled) {
        plan.form = AccessForm::Constant;
        plan.operation = Opcode::Boolean;
        plan.booleanConstant = true;
        return;
    }
    if (state.cannotMoveOut && (state.isAbsentOnPrototypes || query.isOwnLookup)) {
        setSlotOperation(plan, Opcode::IsSlotFilled);
        return;
    }
    plan.form = AccessForm::ByName;
}

static void planFieldStore(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, const FieldArmState& state, PropertyAccessPlan& plan)
{
    if (state.hasImmutableMember) {
        plan.form = AccessForm::ByName;
        return;
    }

    plan.storedValueFits = state.valueFits;
    requireStoresAllowed(query, state, plan);
    setSlotOperation(plan, Opcode::PutSlot);
    if (query.isPrivate || !mayFindSlotEmpty(context, query, state)) {
        requireFilledSlot(state, plan);
        return;
    }

    // Where a creating store has to be paid for with tests, the stores of a constructor and of what it calls pay. Any other store expects a filled slot, which is one test,
    // and pays where that test has failed.
    std::optional<CreationTests> tests = testsToCreateField(context, query, arm, state);
    if (tests && (tests->isEmpty() || query.expectsEmptySlot || mayStoreIntoUnfinishedObject(context, query))) {
        plan.requiredProofs |= tests->proofs;
        addConditions(plan.conditions, tests->conditions.span());
        setSlotOperation(plan, creatingStoreFor(state));
        return;
    }
    // The store of a base constructor under a name that a subclass defines later finds the slot empty and goes to what the object inherits: a setter, a property that is read-only.
    plan.requiredProofs.add(Proof::SlotFilled);
}

static void planFieldDefinition(const PropertyAccessContext& context, const PropertyAccessQuery& query, const FieldArmState& state, PropertyAccessPlan& plan)
{
    plan.storedValueFits = state.valueFits;
    requireStoresAllowed(query, state, plan);
    if (!query.isPrivate && !mayFindSlotEmpty(context, query, state)) {
        setSlotOperation(plan, Opcode::PutSlot);
        return;
    }
    if (query.isPrivate && !state.createdOnlyAtConstruction) {
        plan.form = AccessForm::ByName;
        return;
    }
    if (!query.isPrivate && !state.cannotMoveOut)
        plan.requiredProofs.add(Proof::NoExtraProperties);
    setSlotOperation(plan, creatingStoreFor(state));
}

void planFieldArm(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, PropertyAccessPlan& plan)
{
    const Program& program = context.program;
    FieldArmState state = stateOfFieldArm(program, query, arm);

    if (query.isPrivate && program.at(query.declaringLayout).classFlow.evaluations != ExecutionCount::AtMostOnce)
        addCondition(plan.conditions, { MemberCondition::Kind::ClassEvaluatedOnce, query.declaringLayout, ClassSide::Prototype, { } });

    switch (query.access) {
    case Access::Read:
        planFieldRead(program, query, state, plan);
        return;
    case Access::Has:
        planFieldTest(query, state, plan);
        return;
    case Access::Store:
        planFieldStore(context, query, arm, state, plan);
        return;
    case Access::Define:
        planFieldDefinition(context, query, state, plan);
        return;
    case Access::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void planVariableRead(const Variable& variable, PropertyAccessPlan& plan)
{
    bool isStaticField = !!variable.owner;
    if (isStaticField && variable.flow.initialization == InitializationState::Unknown) {
        plan.form = AccessForm::ByName;
        plan.cause = UnprovenCause::PropertyAdded;
        return;
    }
    plan.form = AccessForm::Variable;
    plan.operation = Opcode::GetModuleVariable;
}

static void planVariableStore(const Program& program, const PropertyAccessQuery& query, const Variable& variable, PropertyAccessPlan& plan)
{
    bool isStaticField = !!variable.owner;
    bool valueFits = query.stored && query.stored->isSubsetOf(program.at(variable.declared.type), program);
    if (!isStaticField || variable.flags.contains(VariableFlags::IsImmutable) || !valueFits) {
        plan.form = AccessForm::ByName;
        return;
    }
    plan.form = AccessForm::Variable;
    plan.operation = Opcode::PutModuleVariable;
    plan.storedValueFits = true;
}

void planVariableArm(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, PropertyAccessPlan& plan)
{
    const Program& program = context.program;
    const Variable& variable = program.at(arm.answer.variable);

    switch (query.access) {
    case Access::Read:
        planVariableRead(variable, plan);
        return;
    case Access::Store:
    case Access::Define:
        planVariableStore(program, query, variable, plan);
        return;
    case Access::Has:
    case Access::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Type storedTypeOf(const Program& program, std::span<const LayoutID> receivers, PropertyKeyID key, TypeArena& arena)
{
    Type result = Type::anything();
    bool hasField = false;
    for (LayoutID receiver : receivers) {
        std::optional<FieldReference> reference = findField(program, receiver, key, BaseChain::Proven);
        if (!reference)
            continue;
        hasField = true;
        result = result.intersectWith(program.at(fieldAt(program, *reference).declared.type), arena);
    }
    if (!hasField)
        return Type::never();
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
