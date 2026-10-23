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
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static_assert(accessOf(Opcode::PutSlot) == Access::Store && accessOf(Opcode::PutSlotOrAdd) == Access::Store);
static_assert(accessOf(Opcode::PutModuleVariable) == Access::None);

bool canBeEmpty(const Program& program, LayoutID layout, PropertyKeyID key)
{
    std::optional<FieldReference> reference = findField(program, layout, key, BaseChain::Proven);
    RELEASE_ASSERT(reference);
    return !staysFilled(program, *reference);
}

// The slot of an accessor says something of the getter only where the flow analysis has seen the definitions. An accessor may have no getter at all.
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
        state.isAddedLateInSomeMember |= field.flags.contains(FieldFlags::IsAddedLate);
        state.isLeftOutAtConstruction |= reference->layout == layout && std::ranges::any_of(program.at(layout).flow.constructorOrders, [&](const FieldOrder& order) {
            return !order.fields.contains(reference->field);
        });
        state.admitsUndefined |= !!(program.at(field.declared.type).primitives & Primitive::Undefined);
        state.hasConstructor |= !!program.at(declaredLayoutOf(program, layout)).constructor;
        state.isInContainer |= program.at(layout).kind == LayoutKind::Container;
        state.cannotMoveOut &= staysInSlot(program, *reference);
        state.mayLieOutOfLine |= program.at(layout).mayKeepFieldsOutOfLine() && flow.location.initialization != InitializationState::SetAtCreation;
        // A lookup that always finds the slot filled never goes on to the prototypes.
        bool inheritsNothingUnderName = isPrivate || flow.prototypeName == PrototypeNameState::AbsentOnPrototypes;
        state.isAbsentOnPrototypes &= memberIsAlwaysFilled || inheritsNothingUnderName;
        state.inheritsNothingUnderName &= inheritsNothingUnderName;
        state.createdOnlyAtConstruction &= program.at(layout).flow.fieldCreation == FieldCreation::OnlyAtConstruction;
        state.hasImmutableMember |= field.flags.contains(FieldFlags::IsImmutable);
        state.form.add(field.form);
        state.isAccessorInEveryMember &= field.form.contains(FieldForm::IsAccessor) && holdsGetterThatIsFunction(program, field);

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

// Every writer checks a value against the field of the cell it stores into, so a store without a test needs a value that the slot of every receiver holds.
static bool storedValueFits(const Program& program, const PropertyAccessQuery& query, const FieldArmState& state)
{
    if (!query.stored)
        return false;
    std::optional<Type> storedWithoutMarkers;
    const Type* stored = query.stored;
    // Only the definition of a class field, which finds an object that its constructor has not finished, puts a marker where the mark of the cell answers for a raw value.
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
    // The call has the receiver at hand, not the environment that the object keeps in a hidden slot.
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

// A store asks for no more than a filled slot.
static void requireFilledSlotForStore(const FieldArmState& state, PropertyAccessPlan& plan)
{
    plan.requiredProofs.add(Proof::SlotFilled);
    plan.testsDeclaredType = state.holdsRawNumberOrBoolean;
}

// An empty slot says that the object has no such property only where the property has not left the slot.
static void requirePropertyInSlotOrNowhere(const FieldArmState& state, PropertyAccessPlan& plan)
{
    if (!state.cannotMoveOut)
        plan.requiredProofs.add(Proof::NoFieldOutsideSlot);
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

// The body of a module fills the objects of the module's own layouts, its exports object before all others, as a constructor fills `this`: most of its stores find the slot empty.
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

// No read sees the field of a class empty when the constructor fills it before the object leaks. The store that fills it does.
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
    if (state.isAlwaysFilled || !state.isFilledAtBirth || query.isPrivate || state.hasDeferredMember || !state.form.isEmpty())
        return false;
    if ((state.holdsRawNumberOrBoolean && !state.holdsRawValuesWhileCellIsUnmarked) || (state.isRequired && state.holdsOnlyListedFunctions))
        return false;
    return !mayStoreIntoUnfinishedObject(context, query);
}

static std::optional<CreationTests> computeTestsThatNothingIsInherited(const PropertyAccessContext& context, std::span<const LayoutID> layouts, PropertyKeyID key, Access access)
{
    CreationTests tests;
    for (LayoutID layout : layouts) {
        MemberAnswer inherited = findMemberOnPrototypes(context, { MemberStart::Instance, layout, key, access, false });
        if (inherited.kind != MemberKind::Absent)
            return std::nullopt;
        tests.proofs |= inherited.receiverProofs;
        addConditions(tests.conditions, inherited.conditions.span());
    }
    return tests;
}

// A store passes a method of the class by and creates the property. A read finds the method. So each asks for itself what lies above the object.
static bool addTestsThatNothingIsInherited(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, Access access, CreationTests& tests)
{
    std::optional<CreationTests> computed;
    const std::optional<CreationTests>* inherited = &computed;
    if (PropertyAccessCache* cache = cacheFor(context.program, arm.layouts, query.key)) {
        PropertyAccessCache::CreationKey key { cache->lists.idOf(arm.layouts.span()), query.key, access, context.flow };
        inherited = &findOrCompute(cache->creations, key, computeHash(key.layouts, key.key, key.access, key.flow), [&] {
            return PropertyAccessCache::Creation { key, computeTestsThatNothingIsInherited(context, arm.layouts.span(), query.key, access) };
        }).tests;
    } else
        computed = computeTestsThatNothingIsInherited(context, arm.layouts.span(), query.key, access);
    if (!*inherited)
        return false;
    tests.proofs |= (*inherited)->proofs;
    addConditions(tests.conditions, (*inherited)->conditions.span());
    return true;
}

// A store that finds the slot empty creates the property only where the object has it nowhere else and inherits nothing of that name.
static std::optional<CreationTests> testsToCreateField(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, const FieldArmState& state)
{
    CreationTests tests;
    if (!state.cannotMoveOut)
        tests.proofs.add(Proof::NoFieldOutsideSlot);
    if (state.inheritsNothingUnderName)
        return tests;
    if (!addTestsThatNothingIsInherited(context, query, arm, Access::Store, tests))
        return std::nullopt;
    return tests;
}

static std::optional<CreationTests> testsThatEmptySlotReadsAsUndefined(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, const FieldArmState& state)
{
    CreationTests tests;
    if (!state.cannotMoveOut)
        tests.proofs.add(Proof::NoFieldOutsideSlot);
    if (!addTestsThatNothingIsInherited(context, query, arm, Access::Read, tests))
        return std::nullopt;
    return tests;
}

// `type.members`, `links.type`: a reference that is stored once the object is out in the world, or never.
static bool isStoredLateOrNever(const FieldArmState& state)
{
    return state.isAddedLate && state.admitsUndefined && !state.isRequired && !state.holdsRawNumberOrBoolean;
}

static bool isLazilyInitialized(const FieldArmState& state)
{
    return state.hasConstructor && !state.isRequired && !state.isInContainer && state.admitsUndefined && !state.holdsRawNumberOrBoolean;
}

// `child.style.position`, where the style is one of two records that leave `position` out and of two other layouts that always have it: the objects that come with an empty slot are those of the records.
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
    std::optional<CreationTests> inherited = computeTestsThatNothingIsInherited(context, layoutsWithEmptySlots.span(), key, Access::Read);
    if (!inherited)
        return std::nullopt;
    TestsOfEmptySlot tests { inherited->proofs, WTF::move(inherited->conditions) };
    if (!cannotMoveOut)
        tests.proofs.add(Proof::NoFieldOutsideSlot);
    return tests;
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
        std::optional<CreationTests> inherited = computeTestsThatNothingIsInherited(context, receivers, key, Access::Store);
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
    // Every writer of the slot of an accessor tests the getter against the type of the slot, so a filled slot says which functions the getter may be a closure of.
    if (state.form.contains(FieldForm::IsAccessor)) {
        if (!state.isAccessorInEveryMember) {
            plan.form = AccessForm::ByName;
            return;
        }
        plan.form = AccessForm::GetterInSlot;
        plan.operation = Opcode::GetSlotGetter;
        requireFilledSlot(state, plan);
        return;
    }
    // A call tests a slot that may be empty, and goes to the function without making the closure.
    if (canCallWithoutFieldRead(program, state) && (state.isAlwaysFilled || !query.usesOnlyPlansWithoutTests) && query.readIsOnlyCalled()) {
        plan.form = AccessForm::Method;
        plan.member.method = state.deferredFunction;
        plan.capturesReceiver = state.deferral == ClosureDeferral::CapturesObject;
        requireFilledSlot(state, plan);
        return;
    }
    // The slot of a private field never defers its closure.
    bool makesClosure = state.hasDeferredMember && !query.isPrivate;
    Opcode readOfSlotThatMayBeEmpty = makesClosure ? Opcode::GetDeferredClosureSlotOrUndefined : Opcode::GetSlotOrUndefined;
    // Where the read needs a test, it tests for what is expected: the slot of an optional field is often empty, that of a required field hardly ever.
    bool findsNothingBehindEmptySlot = !state.isAlwaysFilled && state.isAbsentOnPrototypes && !query.isPrivate;
    if (findsNothingBehindEmptySlot && (state.cannotMoveOut || !state.isRequired)) {
        requirePropertyInSlotOrNowhere(state, plan);
        setSlotOperation(plan, readOfSlotThatMayBeEmpty);
        return;
    }
    // Nobody stores it, so no object has it, and the read finds what lies above the object.
    if (state.hasSlotNowhere) {
        CreationTests tests;
        if (state.form.isEmpty() && addTestsThatNothingIsInherited(context, query, arm, Access::Read, tests)) {
            plan.requiredProofs |= tests.proofs;
            addConditions(plan.conditions, tests.conditions.span());
            plan.form = AccessForm::Constant;
            plan.operation = Opcode::Undefined;
            return;
        }
        plan.form = AccessForm::ByName;
        return;
    }
    // `if (this[kPending])` asks for a name that is stored late or never.
    bool holdsUnfinishedObjects = !state.isInContainer && isInCodeThatHoldsUnfinishedObjects(context, query);
    // `(type as ObjectFlagsType).objectFlags` asks for a name that is stored late too, but for a number: undefined is what its type rules out, so its slot is expected to be filled like that of a required field.
    bool isOptionAmongMany = query.expectsEmptySlot && !state.isRequired && state.isLeftOutAtConstruction && !state.hasConstructor && !state.isInContainer;
    bool isOftenEmpty = (state.isAddedLate && state.admitsUndefined && !state.isRequired) || holdsUnfinishedObjects || isOptionAmongMany;
    // Where the property cannot be elsewhere in the object and nothing has to be tested above it, the state of the slot is the whole answer: the read needs no test and has no side that fails.
    bool mayNeedNoTest = state.isAddedLate || state.cannotMoveOut;
    if ((isOftenEmpty || mayNeedNoTest) && !state.isAlwaysFilled && !query.isPrivate && state.form.isEmpty()) {
        std::optional<CreationTests> tests = testsThatEmptySlotReadsAsUndefined(context, query, arm, state);
        // What a filled slot holds is the answer whatever lies above the object or beside the slot.
        bool testsSlotFirst = tests && !tests->isEmpty() && isStoredLateOrNever(state) && !isOptionAmongMany && !holdsUnfinishedObjects && query.mayTestSlotFirst;
        if (tests && (isOftenEmpty || tests->isEmpty()) && !testsSlotFirst) {
            plan.requiredProofs |= tests->proofs;
            addConditions(plan.conditions, tests->conditions.span());
            setSlotOperation(plan, readOfSlotThatMayBeEmpty);
            return;
        }
        if (!tests && holdsUnfinishedObjects && !state.isAddedLate) {
            plan.form = AccessForm::ByName;
            return;
        }
    }
    if (makesClosure) {
        setSlotOperation(plan, Opcode::GetDeferredClosureSlot);
        requireFilledSlot(state, plan);
        return;
    }
    setSlotOperation(plan, Opcode::GetSlot);
    requireFilledSlot(state, plan);
    // The slot has markers for undefined and null whatever the type of the field says.
    plan.testsDeclaredType = !state.isAlwaysFilled && (state.holdsRawNumberOrBoolean || (state.isRequired && state.holdsOnlyListedFunctions));
    plan.emptySlotMeansNoOwnProperty = !state.isAlwaysFilled && state.cannotMoveOut && !query.isPrivate;
}

static void planReadBehindTestOfCell(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, const FieldArmState& state, PropertyAccessPlan& plan)
{
    FieldArmState whileFilled = state;
    whileFilled.isAlwaysFilled = true;
    whileFilled.isAbsentOnPrototypes = true;
    planFieldRead(context, query, arm, whileFilled, plan);
    if (plan.form != AccessForm::ByName)
        plan.requiredProofs.add(Proof::BirthFieldsFilled);
    if (plan.form == AccessForm::Slot && plan.operation == Opcode::GetSlot)
        plan.testsDeclaredType = state.holdsRawNumberOrBoolean;
}

static void planFieldTest(const PropertyAccessQuery& query, const FieldArmState& state, PropertyAccessPlan& plan)
{
    if (state.isAlwaysFilled) {
        plan.form = AccessForm::Constant;
        plan.operation = Opcode::Boolean;
        plan.booleanConstant = true;
        return;
    }
    if (state.isAbsentOnPrototypes || query.isOwnLookup) {
        requirePropertyInSlotOrNowhere(state, plan);
        setSlotOperation(plan, Opcode::IsSlotFilled);
        return;
    }
    plan.form = AccessForm::ByName;
}

static void planFieldStore(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, const FieldArmState& state, PropertyAccessPlan& plan)
{
    if (state.hasImmutableMember || state.form.containsAny({ FieldForm::IsReadOnly, FieldForm::IsAccessor })) {
        plan.form = AccessForm::ByName;
        return;
    }

    plan.storedValueFits = storedValueFits(context.program, query, state);
    requireStoresAllowed(query, state, plan);
    setSlotOperation(plan, Opcode::PutSlot);
    if (query.isPrivate || !mayFindSlotEmpty(context, query, state)) {
        if (!state.isAlwaysFilled)
            requireFilledSlotForStore(state, plan);
        return;
    }
    if (isAnsweredByTestOfCell(context, query, state)) {
        plan.requiredProofs.add(Proof::BirthFieldsFilled);
        return;
    }
    // The allocation has filled the slot and listed the key, so the store creates nothing.
    if (query.isListedByAllocation) {
        requireFilledSlotForStore(state, plan);
        return;
    }

    // Where a creating store has to be paid for with tests, the stores of a constructor and of what it calls pay.
    std::optional<CreationTests> tests = state.form.isEmpty() ? testsToCreateField(context, query, arm, state) : std::nullopt;
    // Nothing but a store creates a field that was added late, so one of the stores outside the constructor finds the slot empty in every object.
    bool hasFoundSlotFilled = query.storesWhatWasMadeOfField && !query.expectsEmptySlot && tests && !tests->isEmpty() && !testsThatEmptySlotReadsAsUndefined(context, query, arm, state);
    if (tests && !hasFoundSlotFilled && (tests->isEmpty() || query.expectsEmptySlot || state.isAddedLateInSomeMember || mayStoreIntoUnfinishedObject(context, query) || isInBodyOfModuleThatOwnsLayouts(context, query, arm))) {
        plan.requiredProofs |= tests->proofs;
        addConditions(plan.conditions, tests->conditions.span());
        setSlotOperation(plan, creatingStoreFor(state));
        return;
    }
    // The store of a base constructor under a name that a subclass defines later finds the slot empty and goes to what the object inherits: a setter, a property that is read-only.
    if (isInCodeThatHoldsUnfinishedObjects(context, query)) {
        plan.form = AccessForm::ByName;
        plan.operation = PropertyAccessPlan().operation;
        return;
    }
    requireFilledSlotForStore(state, plan);
}

// The constructor of a class without a parent allocates the object, and the definitions of the class run on it once: none finds its name there, whatever happens to the object later.
static bool definesPrivateNameOnceInEachObject(const Program& program, const PropertyAccessQuery& query)
{
    return !program.at(query.declaringLayout).has(LayoutFlags::HasHeritage);
}

static void planFieldDefinition(const PropertyAccessContext& context, const PropertyAccessQuery& query, const FieldArmState& state, PropertyAccessPlan& plan)
{
    if (!state.form.isEmpty()) {
        plan.form = AccessForm::ByName;
        return;
    }
    plan.storedValueFits = storedValueFits(context.program, query, state);
    requireStoresAllowed(query, state, plan);
    // Every finished object may have the field, and the definition that gives it to an unfinished one still creates it: it puts the name among the keys.
    if (!query.isPrivate && !query.isLeftOutByUnfinishedAllocation && !mayFindSlotEmpty(context, query, state)) {
        setSlotOperation(plan, Opcode::PutSlot);
        return;
    }
    if (query.isPrivate && !state.createdOnlyAtConstruction && !definesPrivateNameOnceInEachObject(context.program, query)) {
        plan.form = AccessForm::ByName;
        return;
    }
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
        }).state;
    } else
        known = &computed.emplace(computeStateOfFieldArm(program, arm.layouts.span(), query.key, query.isPrivate));
    const FieldArmState& state = *known;

    if (query.isPrivate && program.at(query.declaringLayout).classFlow.evaluations != ExecutionCount::AtMostOnce)
        addCondition(plan.conditions, { MemberCondition::Kind::ClassEvaluatedOnce, query.declaringLayout, ClassSide::Prototype, { } });

    switch (query.access) {
    case Access::Read:
        if (isAnsweredByTestOfCell(context, query, state))
            planReadBehindTestOfCell(context, query, arm, state, plan);
        else
            planFieldRead(context, query, arm, state, plan);
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

// A variable whose type is only expected is boxed.
static bool fitsVariable(const Program& program, const Type& stored, const Variable& variable)
{
    return variable.declared.flags.contains(PositionFlags::IsExpected) || stored.isSubsetOf(program.at(variable.declared.type), program);
}

static void planVariableStore(const Program& program, const PropertyAccessQuery& query, const Variable& variable, PropertyAccessPlan& plan)
{
    bool isStaticField = !!variable.owner;
    bool valueFits = query.stored && fitsVariable(program, *query.stored, variable);
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
