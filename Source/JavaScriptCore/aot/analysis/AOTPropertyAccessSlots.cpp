/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyAccessInternal.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPropertyAccessCache.h"
#include "AOTTypeArena.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static_assert(accessOf(Opcode::PutSlot) == Access::Store && accessOf(Opcode::PutSlotOrAdd) == Access::Store);
static_assert(accessOf(Opcode::PutModuleVariable) == Access::None);

static bool holdsGetterThatIsFunction(const Program& program, const Field& field)
{
    if (!field.slotType)
        return false;
    const Type& getter = program.at(field.slotType);
    return !getter.primitives && !getter.anyObject && getter.classes.isEmpty() && !getter.layouts && getter.builtins.isEmpty() && (getter.anyFunction || !getter.functions.isEmpty());
}

static bool listsOnlyFunctions(const Type& type)
{
    return !type.functions.isEmpty() && !(type.primitives & ~Primitive::UndefinedOrNull) && !type.anyFunction && !type.anyObject && !type.layouts && type.classes.isEmpty() && type.builtins.isEmpty() && type.isJavaScriptValue();
}

static FieldArmState computeStateOfFieldArm(const Program& program, std::span<const LayoutID> layouts, PropertyKeyID key, bool isPrivate)
{
    FieldArmState state;
    for (LayoutID layout : layouts) {
        std::optional<FieldReference> reference = findField(program, layout, key, BaseChain::Proven);
        RELEASE_ASSERT(reference);
        const Field& field = fieldAt(program, *reference);
        const FieldFlow& flow = flowOf(program, *reference);

        bool memberIsAlwaysFilled = staysFilled(program, *reference);
        state.isAlwaysFilled &= memberIsAlwaysFilled;
        state.isFilledAtBirth &= memberIsAlwaysFilled || isFilledAtBirth(program, *reference);
        state.hasSlotNowhere &= !isPrivate && isNeverStored(program, *reference) && !program.at(layout).flow.observedUses.contains(ObservedUse::ReachesTypedParse);
        state.isRequired &= field.flags.contains(FieldFlags::IsRequired);
        state.isAddedLate &= field.flags.contains(FieldFlags::IsAddedLate);
        state.isFilledByCreator &= field.flags.contains(FieldFlags::IsFilledByCreator);
        state.isAddedLateInSomeMember |= field.flags.contains(FieldFlags::IsAddedLate);
        state.isLeftOutAtConstruction |= reference->layout == layout && std::ranges::any_of(program.at(layout).flow.constructorOrders, [&](const FieldOrder& order) {
            return !order.fields.contains(reference->field);
        });
        state.admitsUndefined |= !!(program.at(program.provenTypeOf(field.declared)).primitives & Primitive::Undefined);
        state.hasConstructor |= !!program.at(declaredLayoutOf(program, layout)).constructor;
        state.isInContainer |= program.at(layout).kind == LayoutKind::Container;
        state.cannotMoveOut &= staysInSlot(program, *reference);
        state.mayLieOutOfLine |= program.at(layout).mayKeepFieldsOutOfLine() && flow.location.initialization != InitializationState::SetAtCreation;
        bool inheritsNothingUnderName = isPrivate || flow.prototypeName == PrototypeNameState::AbsentOnPrototypes;
        state.isAbsentOnPrototypes &= memberIsAlwaysFilled || inheritsNothingUnderName;
        state.inheritsNothingUnderName &= inheritsNothingUnderName;
        state.createdOnlyAtConstruction &= program.at(layout).flow.fieldCreation == FieldCreation::OnlyAtConstruction;
        state.hasImmutableMember |= field.flags.contains(FieldFlags::IsImmutable);
        state.form.add(field.form);
        state.isAccessorInEveryMember &= field.form.contains(FieldForm::IsAccessor) && holdsGetterThatIsFunction(program, field);
        state.isExpectedToHaveSetterInEveryMember &= !!field.setterType;

        TypeID slotType = slotTypeOf(program, field);
        state.slotTypes.add(slotType);
        state.holdsRawNumberOrBoolean &= program.at(slotType).isNumber() || program.at(slotType).isBoolean();
        state.holdsOnlyListedFunctions &= listsOnlyFunctions(program.at(slotType));
        state.slotsHoldUndefinedAndNull &= slotHoldsUndefinedAndNull(program, *reference);
        bool holdsRawValue = holdsRawValueFromBirth(program, *reference);
        state.holdsRawValuesWhileCellIsUnmarked &= holdsRawValue || (memberIsAlwaysFilled && !slotHoldsUndefinedAndNull(program, *reference));
        state.holdsRawValueFromBirthInSomeMember |= holdsRawValue;

        const LocationFlow& location = flow.location;
        state.hasDeferredMember |= location.deferral != ClosureDeferral::Unknown;
        state.holdsOnlyDeferredFunction &= location.stores == StoreCount::AtMostOnce;
        if (layout == layouts[0]) {
            state.deferral = location.deferral;
            state.deferredFunction = location.deferredFunction;
        } else if (location.deferral != state.deferral || location.deferredFunction != state.deferredFunction)
            state.deferralIsUniform = false;
    }
    state.receivers = receiverStateOf(program, layouts);
    return state;
}

static bool storedValueFits(const Program& program, const PropertyAccessQuery& query, const FieldArmState& state)
{
    if (!query.stored)
        return false;
    std::optional<Type> storedWithoutMarkers;
    const Type* stored = query.stored;
    bool mayStoreMarkers = state.slotsHoldUndefinedAndNull && (!state.holdsRawValueFromBirthInSomeMember || query.access == Access::Define);
    if (mayStoreMarkers && stored->mayBeUndefinedOrNull())
        stored = &storedWithoutMarkers.emplace(stored->withoutNullish());
    return std::ranges::all_of(state.slotTypes, [&](TypeID slotType) {
        return stored->isSubsetOf(program.at(slotType), program);
    });
}

static PropertyAccessCache* cacheFor(const Program& program, const LayoutList& layouts, PropertyKeyID key)
{
    if (layouts.size() < PropertyAccessCache::fewestLayouts || TypeTable::isProvisional(key))
        return nullptr;
    return program.propertyAccessCache();
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
    case ClosureDeferral::CapturesEnvironment:
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

static void requireFilledSlotForStore(const FieldArmState& state, PropertyAccessPlan& plan)
{
    plan.requiredProofs.add(Proof::SlotFilled);
    plan.testsDeclaredType = state.holdsRawNumberOrBoolean;
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

static bool isInCodeThatHoldsUnfinishedObjects(const PropertyAccessContext& context, const PropertyAccessQuery& query)
{
    return context.flow && context.flow->hasResults() && query.site && context.flow->at(query.site).function && !context.flow->seesOnlyFinishedObjects(query.site);
}

static bool isInBodyOfModuleThatOwnsLayouts(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm)
{
    if (!context.flow || !context.flow->hasResults() || !query.site)
        return false;
    FunctionID function = context.flow->at(query.site).function;
    if (!function || !context.program.at(function).isModuleBody())
        return false;
    ModuleID module = context.program.at(function).module;
    return module && std::ranges::all_of(arm.layouts, [&](LayoutID layout) {
        return context.program.at(layout).module == module;
    });
}

static bool mayFindSlotEmpty(const PropertyAccessContext& context, const PropertyAccessQuery& query, const FieldArmState& state)
{
    if (!state.isAlwaysFilled)
        return true;
    if (!state.hasConstructor || query.isListedByAllocation)
        return false;
    return mayStoreIntoUnfinishedObject(context, query);
}

static bool isAnsweredByTestOfCell(const PropertyAccessContext& context, const PropertyAccessQuery& query, const FieldArmState& state)
{
    if (state.isAlwaysFilled || !state.isFilledAtBirth || state.hasDeferredMember || !state.form.isEmpty())
        return false;
    if ((state.holdsRawNumberOrBoolean && !state.holdsRawValuesWhileCellIsUnmarked) || (state.isRequired && state.holdsOnlyListedFunctions))
        return false;
    return !mayStoreIntoUnfinishedObject(context, query);
}

static std::optional<TestsOfEmptySlot> computeTestsThatNothingIsInherited(const PropertyAccessContext& context, std::span<const LayoutID> layouts, PropertyKeyID key, Access access)
{
    TestsOfEmptySlot tests;
    for (LayoutID layout : layouts) {
        MemberAnswer inherited = findMemberOnPrototypes(context, { MemberStart::Instance, layout, key, access, false });
        if (inherited.kind != MemberKind::Absent)
            return std::nullopt;
        // A read has the test of the slot to fall back on. A store that creates the field has only the guards, which hold for the objects of the first closure while it is the only one.
        bool failsForEver = access != Access::Store && std::ranges::any_of(inherited.conditions, [&](const MemberCondition& condition) {
            return guardsAreNeverSet(context.program, context.program.at(condition.layout));
        });
        if (failsForEver)
            return std::nullopt;
        tests.proofs |= inherited.receiverProofs;
        addConditions(tests.conditions, inherited.conditions.span());
    }
    return tests;
}

static bool addTestsThatNothingIsInherited(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, Access access, TestsOfEmptySlot& tests)
{
    std::optional<TestsOfEmptySlot> computed;
    const std::optional<TestsOfEmptySlot>* inherited = &computed;
    if (PropertyAccessCache* cache = cacheFor(context.program, arm.layouts, query.key)) {
        PropertyAccessCache::CreationKey key { cache->lists.idOf(arm.layouts.span()), query.key, access, context.flow };
        inherited = &findOrCompute(cache->creations, key, computeHash(key.layouts, key.key, key.access, key.flow), [&] {
            return PropertyAccessCache::Creation { key, computeTestsThatNothingIsInherited(context, arm.layouts.span(), query.key, access) };
        }).value;
    } else
        computed = computeTestsThatNothingIsInherited(context, arm.layouts.span(), query.key, access);
    if (!*inherited)
        return false;
    tests.proofs |= (*inherited)->proofs;
    addConditions(tests.conditions, (*inherited)->conditions.span());
    return true;
}

static std::optional<TestsOfEmptySlot> testsOfEmptySlotFor(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, const FieldArmState& state, Access access)
{
    TestsOfEmptySlot tests;
    if (!state.cannotMoveOut)
        tests.proofs.add(Proof::NoFieldOutsideSlot);
    bool inheritsNothing = access == Access::Store ? state.inheritsNothingUnderName : state.isAbsentOnPrototypes;
    if (!inheritsNothing && !addTestsThatNothingIsInherited(context, query, arm, access, tests))
        return std::nullopt;
    return tests;
}

static bool isStoredLateOrNever(const FieldArmState& state)
{
    return state.isAddedLate && state.admitsUndefined && !state.isRequired && !state.holdsRawNumberOrBoolean;
}

static bool isLazilyInitialized(const FieldArmState& state)
{
    return state.hasConstructor && !state.isRequired && !state.isInContainer && state.admitsUndefined && !state.holdsRawNumberOrBoolean;
}

std::optional<TestsOfEmptySlot> testsThatOptionLeftOutReadsAsUndefined(const PropertyAccessContext& context, std::span<const LayoutID> receivers, PropertyKeyID key, OptionSet<OptionKind> kinds)
{
    LayoutList layoutsWithEmptySlots;
    bool hasOption = false;
    bool cannotMoveOut = true;
    for (LayoutID layout : receivers) {
        FieldArmState state = computeStateOfFieldArm(context.program, singleElementSpan(layout), key, false);
        if (!state.form.isEmpty() || state.hasDeferredMember)
            return std::nullopt;
        if (state.isAlwaysFilled)
            continue;
        layoutsWithEmptySlots.add(layout);
        hasOption |= kinds.contains(OptionKind::LazilyInitializedField) && isLazilyInitialized(state);
        if (kinds.contains(OptionKind::RecordOption)) {
            hasOption |= !state.isRequired && state.isLeftOutAtConstruction && !state.hasConstructor;
            hasOption |= isStoredLateOrNever(state);
        }
        cannotMoveOut &= state.cannotMoveOut;
    }
    if (!hasOption)
        return std::nullopt;
    std::optional<TestsOfEmptySlot> tests = computeTestsThatNothingIsInherited(context, layoutsWithEmptySlots.span(), key, Access::Read);
    if (tests && !cannotMoveOut)
        tests->proofs.add(Proof::NoFieldOutsideSlot);
    return tests;
}

MethodsAboveEmptySlot findMethodsAboveEmptySlot(const PropertyAccessContext& context, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    const Program& program = context.program;
    MethodsAboveEmptySlot found;
    Vector<MemberArm, 2> arms;
    bool cannotMoveOut = true;
    for (LayoutID layout : receivers) {
        FieldArmState state = computeStateOfFieldArm(program, singleElementSpan(layout), key, false);
        if (state.isAlwaysFilled && state.form.isEmpty() && !state.hasDeferredMember)
            continue;
        MemberAnswer answer;
        if (state.form.isEmpty() && !state.hasDeferredMember)
            answer = findMemberOnPrototypes(context, { MemberStart::Instance, layout, key, Access::Read, false });
        if (answer.kind != MemberKind::Method) {
            found.areAllReceivers = false;
            continue;
        }
        cannotMoveOut &= state.cannotMoveOut;
        size_t index = arms.findIf([&](const MemberArm& arm) {
            return belongToOneArm(arm.answer, answer);
        });
        if (index != notFound)
            mergeIntoArm(arms[index], layout, answer);
        else
            arms.append(MemberArm { WTF::move(answer), LayoutList { layout } });
    }

    for (const MemberArm& arm : arms) {
        PropertyAccessQuery query;
        query.key = key;
        query.receivers = ReceiverCandidates::underGuard(arm.layouts.span());
        PropertyAccessPlan plan = planMemberArm(context, query, arm);
        if (plan.form != AccessForm::Method || plan.capturesReceiver) {
            found.areAllReceivers = false;
            continue;
        }
        found.methods.append(MethodAboveEmptySlot { arm.layouts, plan.member.method, plan.requiredProofs, WTF::move(plan.conditions) });
        if (!cannotMoveOut)
            found.methods.last().proofs.add(Proof::NoFieldOutsideSlot);
    }
    return found;
}

static Opcode creatingStoreFor(const FieldArmState& state)
{
    return state.receivers.keys == KeyEnumeration::NeverEnumerated && !state.mayLieOutOfLine ? Opcode::PutSlot : Opcode::PutSlotOrAdd;
}

std::optional<LazyFieldCreation> planLazyFieldCreation(const PropertyAccessContext& context, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    FieldArmState state = computeStateOfFieldArm(context.program, receivers, key, false);
    if (!isLazilyInitialized(state) || !state.form.isEmpty() || state.hasImmutableMember || state.hasDeferredMember)
        return std::nullopt;
    LazyFieldCreation creation { creatingStoreFor(state), { }, { } };
    if (!state.cannotMoveOut)
        creation.proofs.add(Proof::NoFieldOutsideSlot);
    if (!state.receivers.cannot(ObjectMutation::ChangeIntegrity))
        creation.proofs.add(Proof::NotFrozen);
    if (!state.inheritsNothingUnderName) {
        std::optional<TestsOfEmptySlot> inherited = computeTestsThatNothingIsInherited(context, receivers, key, Access::Store);
        if (!inherited)
            return std::nullopt;
        creation.proofs |= inherited->proofs;
        creation.conditions = WTF::move(inherited->conditions);
    }
    return creation;
}

static void planFieldRead(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, const FieldArmState& state, PropertyAccessPlan& plan)
{
    const Program& program = context.program;
    if (state.form.contains(FieldForm::IsAccessor)) {
        if (!state.isAccessorInEveryMember)
            return;
        plan.form = AccessForm::GetterInSlot;
        plan.operation = Opcode::GetSlotGetter;
        requireFilledSlot(state, plan);
        return;
    }
    if (canCallWithoutFieldRead(program, state) && (state.isAlwaysFilled || !query.usesOnlyPlansWithoutTests) && query.readIsOnlyCalled()) {
        plan.form = AccessForm::Method;
        plan.member.method = state.deferredFunction;
        plan.capturesReceiver = state.deferral == ClosureDeferral::CapturesObject;
        requireFilledSlot(state, plan);
        return;
    }
    bool makesClosure = state.hasDeferredMember && !query.isPrivate;
    Opcode readOfSlotThatMayBeEmpty = makesClosure ? Opcode::GetDeferredClosureSlotOrUndefined : Opcode::GetSlotOrUndefined;
    if (state.hasSlotNowhere) {
        TestsOfEmptySlot tests;
        if (!state.form.isEmpty() || (!state.isAbsentOnPrototypes && !addTestsThatNothingIsInherited(context, query, arm, Access::Read, tests)))
            return;
        plan.requiredProofs |= tests.proofs;
        addConditions(plan.conditions, tests.conditions.span());
        plan.form = AccessForm::Constant;
        plan.operation = Opcode::Undefined;
        return;
    }
    if (!state.isAlwaysFilled && state.isAbsentOnPrototypes && state.cannotMoveOut && !query.isPrivate) {
        setSlotOperation(plan, readOfSlotThatMayBeEmpty);
        return;
    }
    bool holdsUnfinishedObjects = !state.isInContainer && isInCodeThatHoldsUnfinishedObjects(context, query);
    bool isOptionAmongMany = query.expectsEmptySlot && !state.isRequired && state.isLeftOutAtConstruction && !state.hasConstructor && !state.isInContainer;
    bool isOftenEmpty = (state.isAddedLate && state.admitsUndefined && !state.isRequired && !state.isFilledByCreator) || holdsUnfinishedObjects || isOptionAmongMany;
    bool mayNeedNoTest = state.isAddedLate || state.cannotMoveOut;
    if ((isOftenEmpty || mayNeedNoTest) && !state.isAlwaysFilled && !query.isPrivate && state.form.isEmpty()) {
        std::optional<TestsOfEmptySlot> tests = testsOfEmptySlotFor(context, query, arm, state, Access::Read);
        bool testsSlotFirst = tests && !tests->isEmpty() && isStoredLateOrNever(state) && !isOptionAmongMany && !holdsUnfinishedObjects && query.mayTestSlotFirst;
        if (tests && (isOftenEmpty || tests->isEmpty()) && !testsSlotFirst) {
            plan.requiredProofs |= tests->proofs;
            addConditions(plan.conditions, tests->conditions.span());
            setSlotOperation(plan, readOfSlotThatMayBeEmpty);
            return;
        }
        if (!tests && holdsUnfinishedObjects && !state.isAddedLate)
            return;
    }
    setSlotOperation(plan, makesClosure ? Opcode::GetDeferredClosureSlot : Opcode::GetSlot);
    requireFilledSlot(state, plan);
    if (makesClosure)
        return;
    plan.testsDeclaredType = !state.isAlwaysFilled && (state.holdsRawNumberOrBoolean || (state.isRequired && state.holdsOnlyListedFunctions));
    plan.emptySlotMeansNoOwnProperty = !state.isAlwaysFilled && state.cannotMoveOut && !query.isPrivate;
}

static void planFieldTest(const PropertyAccessQuery& query, const FieldArmState& state, PropertyAccessPlan& plan)
{
    if (state.isAlwaysFilled || (state.hasSlotNowhere && state.isAbsentOnPrototypes && state.form.isEmpty())) {
        plan.form = AccessForm::Constant;
        plan.operation = Opcode::Boolean;
        plan.booleanConstant = state.isAlwaysFilled;
        return;
    }
    if (!state.isAbsentOnPrototypes && !query.isOwnLookup)
        return;
    if (!state.cannotMoveOut)
        plan.requiredProofs.add(Proof::NoFieldOutsideSlot);
    setSlotOperation(plan, Opcode::IsSlotFilled);
}

static void planFieldStore(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, const FieldArmState& state, PropertyAccessPlan& plan)
{
    if (query.access == Access::Store && !query.isPrivate && state.isAccessorInEveryMember && state.isExpectedToHaveSetterInEveryMember) {
        plan.form = AccessForm::SetterInSlot;
        plan.operation = Opcode::GetSlotSetter;
        requireFilledSlot(state, plan);
        return;
    }
    if (state.hasImmutableMember || state.form.containsAny({ FieldForm::IsReadOnly, FieldForm::IsAccessor }))
        return;

    plan.storedValueFits = storedValueFits(context.program, query, state);
    requireStoresAllowed(query, state, plan);
    setSlotOperation(plan, Opcode::PutSlot);
    if (!mayFindSlotEmpty(context, query, state) || (query.isPrivate && state.isAlwaysFilled))
        return;
    if (isAnsweredByTestOfCell(context, query, state)) {
        plan.requiredProofs.add(Proof::BirthFieldsFilled);
        return;
    }
    if (query.isPrivate || (query.isListedByAllocation && !query.usesOnlyPlansWithoutTests)) {
        requireFilledSlotForStore(state, plan);
        return;
    }

    std::optional<TestsOfEmptySlot> tests = state.form.isEmpty() ? testsOfEmptySlotFor(context, query, arm, state, Access::Store) : std::nullopt;
    bool hasFoundSlotFilled = query.storesWhatWasMadeOfField && !query.expectsEmptySlot && tests && !tests->isEmpty() && !testsOfEmptySlotFor(context, query, arm, state, Access::Read);
    if (tests && !hasFoundSlotFilled && (tests->isEmpty() || query.expectsEmptySlot || state.isAddedLateInSomeMember || mayStoreIntoUnfinishedObject(context, query) || isInBodyOfModuleThatOwnsLayouts(context, query, arm))) {
        plan.requiredProofs |= tests->proofs;
        addConditions(plan.conditions, tests->conditions.span());
        setSlotOperation(plan, creatingStoreFor(state));
        return;
    }
    if (isInCodeThatHoldsUnfinishedObjects(context, query)) {
        plan.form = AccessForm::ByName;
        plan.operation = PropertyAccessPlan().operation;
        return;
    }
    requireFilledSlotForStore(state, plan);
}

static void planFieldDefinition(const PropertyAccessContext& context, const PropertyAccessQuery& query, const FieldArmState& state, PropertyAccessPlan& plan)
{
    if (!state.form.isEmpty())
        return;
    plan.storedValueFits = storedValueFits(context.program, query, state);
    requireStoresAllowed(query, state, plan);
    if (!query.isPrivate && !query.isLeftOutByUnfinishedAllocation && !mayFindSlotEmpty(context, query, state)) {
        setSlotOperation(plan, Opcode::PutSlot);
        return;
    }
    if (query.isPrivate && !state.createdOnlyAtConstruction && context.program.at(query.declaringLayout).has(LayoutFlags::HasHeritage))
        return;
    if (!query.isPrivate && !state.cannotMoveOut)
        plan.requiredProofs.add(Proof::NoFieldOutsideSlot);
    setSlotOperation(plan, creatingStoreFor(state));
}

void planFieldArm(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, PropertyAccessPlan& plan)
{
    const Program& program = context.program;
    std::optional<FieldArmState> computed;
    const FieldArmState* known = nullptr;
    if (PropertyAccessCache* cache = cacheFor(program, arm.layouts, query.key)) {
        PropertyAccessCache::FieldArmKey key { cache->lists.idOf(arm.layouts.span()), query.key, query.isPrivate };
        known = &findOrCompute(cache->fieldArms, key, computeHash(key.layouts, key.key, key.isPrivate), [&] {
            return PropertyAccessCache::FieldArm { key, computeStateOfFieldArm(program, arm.layouts.span(), query.key, query.isPrivate) };
        }).value;
    } else
        known = &computed.emplace(computeStateOfFieldArm(program, arm.layouts.span(), query.key, query.isPrivate));
    const FieldArmState& state = *known;

    if (query.isPrivate && program.at(query.declaringLayout).classFlow.evaluations != ExecutionCount::AtMostOnce)
        addCondition(plan.conditions, { MemberCondition::Kind::ClassEvaluatedOnce, query.declaringLayout, ClassSide::Prototype, { } });

    switch (query.access) {
    case Access::Read: {
        if (!isAnsweredByTestOfCell(context, query, state)) {
            planFieldRead(context, query, arm, state, plan);
            return;
        }
        FieldArmState whileFilled = state;
        whileFilled.isAlwaysFilled = true;
        whileFilled.isAbsentOnPrototypes = true;
        planFieldRead(context, query, arm, whileFilled, plan);
        if (plan.form != AccessForm::ByName)
            plan.requiredProofs.add(Proof::BirthFieldsFilled);
        if (plan.form == AccessForm::Slot && plan.operation == Opcode::GetSlot)
            plan.testsDeclaredType = state.holdsRawNumberOrBoolean;
        return;
    }
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

void planVariableArm(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, PropertyAccessPlan& plan)
{
    const Program& program = context.program;
    const Variable& variable = program.at(arm.answer.variable);
    bool isStaticField = !!variable.owner;

    switch (query.access) {
    case Access::Read:
        if (isStaticField && variable.flow.initialization == InitializationState::Unknown) {
            plan.cause = UnprovenCause::PropertyAdded;
            return;
        }
        plan.form = AccessForm::Variable;
        plan.operation = Opcode::GetModuleVariable;
        return;
    case Access::Store:
    case Access::Define: {
        bool valueFits = query.stored && (variable.declared.isExpected() || query.stored->isSubsetOf(program.at(*variable.declared.established()), program));
        if (!isStaticField || variable.flags.contains(VariableFlags::IsImmutable) || !valueFits)
            return;
        plan.form = AccessForm::Variable;
        plan.operation = Opcode::PutModuleVariable;
        plan.storedValueFits = true;
        return;
    }
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
        result = result.intersectWith(program.at(slotTypeOf(program, fieldAt(program, *reference))), arena);
    }
    if (!hasField)
        return Type::never();
    return result;
}

std::optional<Vector<FunctionID, 2>> gettersInSlots(const Program& program, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    Vector<FunctionID, 2> getters;
    for (LayoutID receiver : receivers) {
        std::optional<FieldReference> reference = findField(program, receiver, key, BaseChain::Proven);
        if (!reference)
            return std::nullopt;
        const Type& getter = program.at(slotTypeOf(program, fieldAt(program, *reference)));
        if (!listsOnlyFunctions(getter) || getter.primitives)
            return std::nullopt;
        for (FunctionID function : getter.functions)
            insertInUniquedSortedVector(getters, function);
    }
    return getters;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
