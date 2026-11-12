/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTClassTable.h"
#include "AOTClassToken.h"
#include "AOTClosure.h"
#include "AOTErrorMessages.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTPendingStars.h"
#include "AOTPropertyWalk.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "AOTThisAllocationInlines.h"
#include "AOTUnwinding.h"
#include "DeferGCInlines.h"
#include "ExceptionHelpers.h"
#include "InternalFunction.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "Symbol.h"
#include <algorithm>
#include <ranges>
#include <wtf/text/SymbolImpl.h>

namespace JSC { namespace AOT {

static const LayoutData& classLayoutAt(const ClassTable& classes, uint32_t classIndex)
{
    const LayoutData* classLayout = classes.classWithIndex(classIndex);
    RELEASE_ASSERT(classLayout);
    return *classLayout;
}

JSC_DEFINE_JIT_OPERATION(operationAOTResolveHeritage, EncodedJSValue, (Instance* instance, EncodedJSValue heritage))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTResolveHeritage);
    OPERATION_RETURN(scope, JSValue::encode(instance->state->objects->classes.resolveHeritage(globalObject, JSValue::decode(heritage))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewClass, JSCell*, (Instance* instance, uint32_t request, EncodedJSValue constructor, EncodedJSValue heritage, EncodedJSValue prototypeParent))
{
    constexpr Entry entry = Entry::operationAOTNewClass;
    AOT_OPERATION_BEGIN(instance, entry);
    ClassTable& classes = instance->state->objects->classes;
    Closure* closure = cellArgument<Closure>(objectArgument(constructor, entry), entry);
    const LayoutData& classLayout = classLayoutAt(classes, requestAt<NewClassRequest>(instance, request).classIndex);
    RELEASE_ASSERT(closure->function() == FunctionID(classLayout.classData().classConstructor));
    closure = classes.defineClassWhereItIsEvaluated(globalObject, closure, classLayout, JSValue::decode(heritage), JSValue::decode(prototypeParent));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, closure);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetClassPrototype, JSCell*, (Instance* instance, JSCell* classObject))
{
    constexpr Entry entry = Entry::operationAOTGetClassPrototype;
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, entry);
    JSCell* result = instance->state->objects->classes.prototypeOfClassObject(vm, cellArgument<Closure>(classObject, entry));
    if (!result) [[unlikely]]
        crashOnWrongOperand(entry);
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBeginMemberGuards, void, (Instance* instance, uint32_t classIndex, uint32_t endsTopLevelOfModule))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTBeginMemberGuards);
    ClassTable& classes = instance->state->objects->classes;
    const LayoutData& classLayout = classLayoutAt(classes, classIndex);
    if (endsTopLevelOfModule)
        classes.beginGuardsAtEndOfModule(vm, classLayout);
    else
        classes.beginGuardsAheadOfFirstObject(vm, classLayout);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRememberStructureOfClass, size_t, (Instance* instance, JSCell* cell, uint32_t classIndex, uint32_t offsetOfStructures))
{
    VM& vm = *instance->vm;
    ClassTable& classes = instance->state->objects->classes;
    bool asksElements = offsetOfStructures & StructuresOfHeir::asksElements;
    offsetOfStructures &= ~StructuresOfHeir::asksElements;
    bool isRemembered = classes.rememberStructureOfInstances(vm, cell, classLayoutAt(classes, classIndex), offsetOfStructures);
    if (asksElements)
        isRemembered = isRemembered && classes.rememberPrototypeWithoutElements(cell, offsetOfStructures);
    countEvent(vm, isRemembered ? RuntimeCounterID::BuiltinSubclassStructuresAccepted : RuntimeCounterID::BuiltinSubclassStructuresRejected);
    return isRemembered;
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES getConstructPrototypeByLookup(Instance*, EncodedJSValue newTarget);

JSC_DEFINE_JIT_OPERATION(operationAOTGetConstructPrototype, JSCell*, (Instance* instance, EncodedJSValue newTarget))
{
    if (JSValue target = JSValue::decode(newTarget); target.isCell()) [[likely]] {
        if (JSObject* prototype = instance->state->objects->classes.tryReadPrototypePropertyAtRememberedPlace(target.asCell())) [[likely]]
            return { prototype, nullptr };
    }
    [[clang::musttail]] return getConstructPrototypeByLookup(instance, newTarget);
}

static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES getConstructPrototypeByLookup(Instance* instance, EncodedJSValue newTarget)
{
    constexpr Entry entry = Entry::operationAOTGetConstructPrototype;
    AOT_OPERATION_BEGIN(instance, entry);
    countEvent(vm, RuntimeCounterID::ConstructPrototypeLookups);
    JSObject* target = objectArgument(newTarget, entry);
    if (auto* closure = dynamicDowncast<Closure>(target); closure && Instance::owning(closure) == instance && FunctionTables::hasPrototypePropertyAtCreation(closure->kind())) {
        ClassTable& classes = instance->state->objects->classes;
        JSObject* prototype = classes.prototypeForInstancesOf(vm, closure);
        classes.rememberPlaceOfPrototypeProperty(vm, closure, prototype);
        if (const LayoutData* classLayout = classes.classConstructedBy(classes.functionOfOwnClosure(*closure)); classLayout && !classes.isDefinedByClass(*classLayout)) {
            DeferGCForAWhile deferGC(vm);
            classes.classPrototypeOfInstances(vm, *classLayout);
        }
        OPERATION_RETURN(scope, prototype);
    }
    JSValue prototype = target->get(globalObject, vm.propertyNames->prototype);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (prototype.isObject())
        OPERATION_RETURN(scope, asObject(prototype));
    JSGlobalObject* realm = getFunctionRealm(globalObject, target);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, realm->objectPrototype());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSetConstructPrototype, JSCell*, (Instance* instance, JSCell* object, JSCell* prototype, uint32_t allocation, uint32_t holdsNoField))
{
    constexpr Entry entry = Entry::operationAOTSetConstructPrototype;
    AOT_OPERATION_BEGIN(instance, entry);
    if (vm.traps().needsOutOfMemoryError()) [[unlikely]]
        vm.heap.cage().tryTakeBackOutOfMemoryError(vm);
    ObjectModel& objects = *instance->state->objects;
    Object* allocated = object ? cellArgument<Object>(object, entry) : uncheckedDowncast<Object>(objects.structures.allocate(vm, allocation));
    size_t numberOfStructuresBefore = objects.structures.numberOfStructuresMade();
    JSObject* result = objects.classes.instanceWithPrototype(vm, allocated, cellArgument<JSObject>(prototype, entry), !!holdsNoField);
    if (result == allocated)
        objects.structures.predictStructure(vm, allocation, uncheckedDowncast<CompiledStructure>(result->structure()));
    if (Object* made = !object && holdsNoField ? compiledObjectOrNull(result) : nullptr; made && !made->compiledStructure().hasSpareWord() && objects.structures.structureIDOf(allocation) != made->structureID() && objects.structures.numberOfStructuresMade() == numberOfStructuresBefore)
        objects.classes.rememberInstanceBelow(vm, allocation, cellArgument<JSObject>(prototype, entry), *made, result == allocated ? instance->state->tables.allocations()[allocation].size() : made->layout().size);
    vm.writeBarrier(result);
    OPERATION_RETURN(scope, newCellForCompiledCode(vm, result));
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES createThisSlow(Instance*, JSCell* constructor, JSCell* newTarget);
static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES createThisOfOtherNewTarget(Instance*, JSCell* constructor, JSCell* newTarget);

static ALWAYS_INLINE JSObject* tryCreateThisInline(VM& vm, Structure* structure)
{
    const CompiledStructure* compiled = compiledObjectStructureOrNull(structure);
    if (!compiled)
        return tryCreateFinalObjectInline(vm, structure);
    const uint64_t* words = compiled->wordsOfObjectWithoutFields();
    return words ? tryCreateObjectWithoutFieldsInline(vm, structure, compiled->layout().size, words) : nullptr;
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateThis, JSCell*, (Instance* instance, JSCell* constructor, JSCell* newTarget))
{
    if (constructor != newTarget) [[unlikely]]
        [[clang::musttail]] return createThisOfOtherNewTarget(instance, constructor, newTarget);
    VM& vm = *instance->vm;
    Structure* structure = rememberedStructureOfThis(vm, newTarget, newTarget);
    if (!structure)
        structure = instance->state->objects->classes.structureOfThisRememberedBesideCallee(newTarget);
    if (structure) {
        if (JSObject* object = tryCreateThisInline(vm, structure)) [[likely]]
            return { object, nullptr };
    }
    [[clang::musttail]] return createThisSlow(instance, constructor, newTarget);
}

static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES createThisOfOtherNewTarget(Instance* instance, JSCell* constructor, JSCell* newTarget)
{
    VM& vm = *instance->vm;
    if (Structure* structure = rememberedStructureOfThis(vm, constructor, newTarget)) {
        if (JSObject* object = tryCreateThisInline(vm, structure)) [[likely]]
            return { object, nullptr };
    }
    [[clang::musttail]] return createThisSlow(instance, constructor, newTarget);
}

static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES createThisSlow(Instance* instance, JSCell* constructor, JSCell* newTarget)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCreateThis);
    countEvent(vm, RuntimeCounterID::CreateThisSlowPaths);
    JSObject* constructorObject = cellArgument<JSObject>(constructor, Entry::operationAOTCreateThis);
    JSObject* newTargetObject = cellArgument<JSObject>(newTarget, Entry::operationAOTCreateThis);
    OPERATION_RETURN(scope, instance->state->objects->classes.createThis(globalObject, constructorObject, newTargetObject));
}

JSCellButterfly* ClassToken::create(VM& vm, const LayoutData& classLayout)
{
    JSCellButterfly* butterfly = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, firstNameSlot + classLayout.numberOfFields);
    butterfly->setIndex(vm, classIndexSlot, jsNumber(classLayout.index));
    vm.aotState().someClassHasPrivateNames = true;
    return butterfly;
}

static int32_t locationOf(PropertyOffset offset)
{
    if (isInlineOffset(offset))
        return static_cast<int32_t>(JSObject::offsetOfInlineStorage() + offsetInInlineStorage(offset) * sizeof(EncodedJSValue));
    return static_cast<int32_t>(offsetInButterfly(offset) * static_cast<intptr_t>(sizeof(EncodedJSValue)));
}

static PropertyOffset offsetAt(int32_t location)
{
    if (location > 0)
        return static_cast<PropertyOffset>((location - JSObject::offsetOfInlineStorage()) / sizeof(EncodedJSValue));
    return static_cast<PropertyOffset>(firstOutOfLineOffset + offsetInButterfly(firstOutOfLineOffset) - location / static_cast<int32_t>(sizeof(EncodedJSValue)));
}

static JSObject* holderOfNamesWithoutField(JSObject* object)
{
    Object* compiledObject = compiledObjectOrNull(object);
    return compiledObject ? extraPropertiesIfCreated(*compiledObject) : object;
}

static std::optional<PropertyOffset> findInKnownStructure(JSCellButterfly* token, uint32_t field, JSObject* object)
{
    JSValue records = token->get(ClassToken::knownStructuresSlot);
    if (!records) [[unlikely]]
        return std::nullopt;
    JSCellButterfly* butterfly = uncheckedDowncast<JSCellButterfly>(records.asCell());
    JSValue number = jsNumber(static_cast<int32_t>(object->structureID().bits()));
    for (unsigned record = 0; record < ClassToken::recordsPerField; ++record) {
        unsigned first = ClassToken::slotOfRecord(field, record);
        if (butterfly->get(first + ClassToken::StructureNumber) != number)
            continue;
        int32_t brand = butterfly->get(first + ClassToken::LocationOfBrand).asInt32();
        if (brand && object->getDirect(offsetAt(brand)) != JSValue(token))
            return std::nullopt;
        return offsetAt(butterfly->get(first + ClassToken::LocationOfValue).asInt32());
    }
    return std::nullopt;
}

static void addKnownStructure(VM& vm, JSCellButterfly* token, uint32_t field, Structure* structure, PropertyOffset value, std::optional<PropertyOffset> brand)
{
    JSValue records = token->get(ClassToken::knownStructuresSlot);
    if (!records)
        return;
    JSCellButterfly* butterfly = uncheckedDowncast<JSCellButterfly>(records.asCell());
    for (unsigned record = 0; record < ClassToken::recordsPerField; ++record) {
        unsigned first = ClassToken::slotOfRecord(field, record);
        if (butterfly->get(first + ClassToken::StructureNumber))
            continue;
        butterfly->setIndex(vm, first + ClassToken::KeptStructure, structure);
        butterfly->setIndex(vm, first + ClassToken::LocationOfValue, jsNumber(locationOf(value)));
        butterfly->setIndex(vm, first + ClassToken::LocationOfBrand, jsNumber(brand ? locationOf(*brand) : 0));
        butterfly->setIndex(vm, first + ClassToken::StructureNumber, jsNumber(static_cast<int32_t>(structure->id().bits())));
        return;
    }
}

static void rememberDefinition(VM& vm, JSCellButterfly* token, uint32_t field, Structure* before, Structure* after, std::optional<PropertyOffset> brand)
{
    JSValue records = token->get(ClassToken::knownStructuresSlot);
    if (!records || after->previousID() != before || after->transitionKind() != TransitionKind::PropertyAddition)
        return;
    PropertyOffset offset = after->transitionOffset();
    std::optional<uint32_t> newCapacity = capacityOfStorageGrownByStub(vm, before, after, offset);
    bool isStoredWithElements = !newCapacity && before->typeInfo().type() == ArrayType && offset == firstOutOfLineOffset && !before->outOfLineCapacity() && after->outOfLineCapacity() == initialOutOfLineCapacity
        && !before->hasElementsStartOffset() && !before->mayBePrototype() && !before->mayHaveImmutableProperties() && after->propertyAccessesAreCacheable();
    if (!newCapacity && !isStoredWithElements)
        return;
    int32_t placeOfValue = isStoredWithElements ? 1 : *newCapacity ? static_cast<int32_t>(*newCapacity << 1 | 1) : locationOf(offset);
    int32_t placeOfBrand = brand ? locationOf(*brand) : 0;
    if (placeOfValue != static_cast<int16_t>(placeOfValue) || placeOfBrand != static_cast<int16_t>(placeOfBrand))
        return;
    JSCellButterfly* butterfly = uncheckedDowncast<JSCellButterfly>(records.asCell());
    JSValue number = jsNumber(static_cast<int32_t>(before->id().bits()));
    for (unsigned definition = 0; definition < ClassToken::definitionsPerField; ++definition) {
        unsigned first = ClassToken::slotOfDefinition(field, definition);
        JSValue taken = butterfly->get(first + ClassToken::NumberOfStructureBefore);
        if (taken == number)
            return;
        if (taken)
            continue;
        butterfly->setIndex(vm, first + ClassToken::KeptStructureBefore, before);
        butterfly->setIndex(vm, first + ClassToken::StructureAfter, after);
        butterfly->setIndex(vm, first + ClassToken::Places, jsNumber(static_cast<int32_t>(static_cast<uint32_t>(placeOfBrand) << 16 | static_cast<uint16_t>(placeOfValue))));
        butterfly->setIndex(vm, first + ClassToken::NumberOfStructureBefore, number);
        return;
    }
}

static void rememberDefinitionAmongExtraProperties(VM& vm, JSCellButterfly* token, uint32_t field, Object& object, const CompiledStructure& before, JSObject* extraBefore, Structure* extraStructureBefore, std::optional<PropertyOffset> brand)
{
    JSObject* extra = extraPropertiesIfCreated(object);
    if (!extra || (extraBefore && extra != extraBefore) || before.mayBePrototype() || object.isNotExtensible())
        return;
    if (extraBefore && before.extraPropertiesStructureID() != extraStructureBefore->id())
        return;
    object.ensureStructureForExtraProperties(vm);
    const CompiledStructure& after = object.compiledStructure();
    if (!after.extraPropertiesSlot() || after.extraPropertiesStructureID() != extra->structureID())
        return;
    if (!extraBefore && (extra->structure()->transitionOffset() != 0 || brand))
        return;
    rememberDefinition(vm, token, field, extraStructureBefore, extra->structure(), brand);
    const_cast<CompiledStructure&>(before).setSuccessorByPrivateName(vm, const_cast<CompiledStructure*>(&after));
}

static unsigned placeOfStructureWithoutName(StructureID structure, uint32_t field)
{
    return ((structure.bits() >> ClassToken::shiftOfStructureWithoutName) + field) % ClassToken::numberOfStructuresWithoutNames * 2;
}

static bool isKnownToLackName(JSCellButterfly* token, uint32_t field, JSObject* object)
{
    JSValue places = token->get(ClassToken::structuresWithoutNamesSlot);
    if (!places)
        return false;
    const WriteBarrier<Unknown>* place = uncheckedDowncast<JSCellButterfly>(places.asCell())->toButterfly()->contiguous().data() + placeOfStructureWithoutName(object->structureID(), field);
    return place[0].get() == JSValue(object->structure()) && place[1].get() == jsNumber(field);
}

static void rememberStructureWithoutName(VM& vm, JSCellButterfly* token, uint32_t field, Structure* structure)
{
    if (!token->get(ClassToken::structuresWithoutNamesSlot))
        token->setIndex(vm, ClassToken::structuresWithoutNamesSlot, JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, ClassToken::numberOfStructuresWithoutNames * 2));
    JSCellButterfly* places = uncheckedDowncast<JSCellButterfly>(token->get(ClassToken::structuresWithoutNamesSlot).asCell());
    unsigned place = placeOfStructureWithoutName(structure->id(), field);
    places->setIndex(vm, place, structure);
    places->setIndex(vm, place + 1, jsNumber(field));
}

static const LayoutData& declaringLayoutOf(State& state, JSCellButterfly* token)
{
    return classLayoutAt(state.objects->classes, token->get(ClassToken::classIndexSlot).asUInt32());
}

static const LayoutData& declaringLayoutOf(State& state, const Object& object, JSCellButterfly* token)
{
    const LayoutData& layout = object.layout();
    return layout.index == token->get(ClassToken::classIndexSlot).asUInt32() ? layout : declaringLayoutOf(state, token);
}

static void ensureRecordsOfKnownStructures(VM& vm, State& state, JSCellButterfly* token)
{
    if (!token->get(ClassToken::knownStructuresSlot)) [[unlikely]]
        token->setIndex(vm, ClassToken::knownStructuresSlot, JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, ClassToken::slotOfRecord(declaringLayoutOf(state, token).numberOfFields, 0)));
}

static bool objectsWithoutLayoutTakeNamesOfEvaluation(const LayoutData& declaringLayout)
{
    return declaringLayout.classData().privateNamesAreKnownToTexts && declaringLayout.evaluation() != EvaluationState::ProvenOnce;
}

static const FieldData& declaredFieldOf(const LayoutData& declaringLayout, uint32_t fieldIndex)
{
    RELEASE_ASSERT(fieldIndex < declaringLayout.numberOfFields);
    const FieldData& declared = declaringLayout.fields()[fieldIndex];
    RELEASE_ASSERT(declared.isPrivate());
    return declared;
}

static const FieldData* brandOf(State& state, const LayoutData& classLayout, JSCellButterfly* token)
{
    constexpr int32_t noBrandField = -1;
    JSValue brandField = token->get(ClassToken::brandFieldSlot);
    if (!brandField) [[unlikely]] {
        const LayoutData* base = state.objects->classes.baseOf(classLayout);
        auto isInherited = [&](const FieldData& field) {
            return base && std::ranges::any_of(base->fields(), [&](const FieldData& inherited) { return inherited.name == field.name; });
        };
        brandField = jsNumber(noBrandField);
        for (const FieldData& field : classLayout.fields()) {
            if (field.isBrand() && field.isRequired() && !isInherited(field)) {
                brandField = jsNumber(static_cast<int32_t>(classLayout.indexOf(field)));
                break;
            }
        }
        token->setIndex(*state.instance.vm, ClassToken::brandFieldSlot, brandField);
    }
    return brandField.asInt32() == noBrandField ? nullptr : &classLayout.fields()[brandField.asInt32()];
}

static ALWAYS_INLINE unsigned nameSlotOf(JSCellButterfly* token, uint32_t field)
{
    RELEASE_ASSERT(static_cast<uint64_t>(ClassToken::firstNameSlot) + field < token->length());
    return ClassToken::firstNameSlot + field;
}

static Symbol* privateNameIfCreated(JSCellButterfly* token, uint32_t field)
{
    JSValue name = token->get(nameSlotOf(token, field));
    return name ? asSymbol(name) : nullptr;
}

static Symbol* privateNameOf(VM& vm, State& state, JSCellButterfly* token, uint32_t field)
{
    if (Symbol* name = privateNameIfCreated(token, field))
        return name;
    const ImageText& text = state.instance.program->text();
    String description = text.createString(text.identifier(declaringLayoutOf(state, token).fields()[field].name).text());
    Symbol* name = Symbol::create(vm, PrivateSymbolImpl::create(*description.impl()).get());
    token->setIndex(vm, nameSlotOf(token, field), name);
    return name;
}

static constexpr unsigned attributesOfNameWithoutField = static_cast<unsigned>(PropertyAttribute::DontEnum);

static bool isOfRealmOf(const State& state, const Object& object)
{
    return object.structure()->realm() == state.instance.globalObject;
}

struct PrivateProperty {
    const FieldData* field { nullptr };
    JSValue value;
    bool isInSlot { false };
    PropertyOffset offset { invalidOffset };
    unsigned attributes { 0 };
};

struct PrivateLocation {
    Object* compiledObject;
    const FieldData* declared;
    UniquedStringImpl* declaredName;
    bool hasBrand;
    bool isOfOnlyEvaluation;
    bool belongsToEvaluation { false };
    PrivateProperty underDeclaredName { };
    PrivateProperty brand { };
    UniquedStringImpl* name { nullptr };
    PrivateProperty property { };
};

static PrivateProperty findPrivateProperty(VM& vm, State& state, JSObject* object, Object* compiledObject, UniquedStringImpl* uid, const FieldData* knownField)
{
    PrivateProperty property;
    if (!compiledObject) {
        property.offset = object->getDirectOffset(vm, uid, property.attributes);
        if (isValidOffset(property.offset))
            property.value = object->getDirect(property.offset);
        return property;
    }
    property.field = knownField ? knownField : compiledObject->fieldNamed(state, uid);
    if (property.field) {
        FieldProperty found = findFieldProperty(vm, *compiledObject, *property.field);
        property.value = found.valueOrAccessor;
        property.isInSlot = found.isInSlot;
    } else if (JSObject* extra = compiledObject->extraProperties()) {
        property.offset = extra->getDirectOffset(vm, uid, property.attributes);
        if (isValidOffset(property.offset))
            property.value = extra->getDirect(property.offset);
    }
    return property;
}

static PrivateLocation locatePrivateName(VM& vm, State& state, JSObject* object, JSCellButterfly* token, uint32_t fieldIndex)
{
    const LayoutData& declaringLayout = declaringLayoutOf(state, token);
    const FieldData& declared = declaredFieldOf(declaringLayout, fieldIndex);
    const FieldData* brand = brandOf(state, declaringLayout, token);
    RuntimeLayoutTable& layouts = state.objects->layouts;
    PrivateLocation location { compiledObjectOrNull(object), &declared, layouts.nameOf(vm, declared), !!brand, declaringLayout.evaluation() == EvaluationState::ProvenOnce };
    bool hasDeclaringLayout = location.compiledObject && &location.compiledObject->layout() == &declaringLayout;
    auto find = [&](UniquedStringImpl* uid, const FieldData* fieldOfDeclaringLayout) {
        return findPrivateProperty(vm, state, object, location.compiledObject, uid, hasDeclaringLayout ? fieldOfDeclaringLayout : nullptr);
    };

    UniquedStringImpl* brandName = brand && brand != &declared ? layouts.nameOf(vm, *brand) : nullptr;
    location.underDeclaredName = find(location.declaredName, &declared);
    if (brand)
        location.brand = brandName ? find(brandName, brand) : location.underDeclaredName;
    location.belongsToEvaluation = (!location.compiledObject || isOfRealmOf(state, *location.compiledObject)) && (!brand || location.brand.value == JSValue(token));
    if (location.belongsToEvaluation && location.underDeclaredName.value) {
        location.name = location.declaredName;
        location.property = location.underDeclaredName;
    } else if (Symbol* evaluationName = privateNameIfCreated(token, fieldIndex)) {
        location.property = find(&evaluationName->uid(), nullptr);
        if (location.property.value)
            location.name = &evaluationName->uid();
    }
    return location;
}

static void rememberLocation(VM& vm, JSObject* object, JSCellButterfly* token, uint32_t fieldIndex, const PrivateLocation& found)
{
    JSObject* holder = holderOfNamesWithoutField(object);
    if (!holder || !found.name || (found.property.attributes & ~attributesOfNameWithoutField) || !isValidOffset(found.property.offset) || holder->structure()->isDictionary())
        return;
    bool needsBrand = found.name == found.declaredName && found.hasBrand && !found.isOfOnlyEvaluation;
    if (needsBrand && !isValidOffset(found.brand.offset))
        return;
    if (holder->structure()->mayHaveImmutableProperties())
        return;
    holder->structure()->didReplaceProperty(found.property.offset);
    addKnownStructure(vm, token, fieldIndex, holder->structure(), found.property.offset, needsBrand ? std::optional(found.brand.offset) : std::nullopt);
}

static JSObject* createAbsentNameError(JSGlobalObject* globalObject, const PrivateLocation& absent)
{
    return createAbsentPrivateNameError(globalObject, absent.declared->isBrand() ? createPrivateMethodAccessError : createInvalidPrivateNameError);
}

static const FieldData* fieldUnderDeclaredName(VM& vm, State& state, const Object& object, const LayoutData& declaringLayout, const FieldData& declared)
{
    const LayoutData& layout = object.layout();
    if (&layout == &declaringLayout)
        return &declared;
    if (!state.fieldsUnderDeclaredNames) [[unlikely]]
        state.fieldsUnderDeclaredNames = state.tableArena.allocate<FieldUnderDeclaredName>(State::numberOfFieldsUnderDeclaredNames);
    FieldUnderDeclaredName& place = state.fieldsUnderDeclaredNames[((std::bit_cast<uintptr_t>(&declared) >> 3) ^ (std::bit_cast<uintptr_t>(&layout) >> 4)) % State::numberOfFieldsUnderDeclaredNames];
    if (place.layoutOfObject != &layout || place.declared != &declared)
        place = { &layout, &declared, object.fieldNamed(state, state.objects->layouts.nameOf(vm, declared)) };
    return place.field;
}

static bool wasMadeByEvaluation(VM& vm, State& state, const Object& object, const LayoutData& declaringLayout, JSCellButterfly* token)
{
    if (!isOfRealmOf(state, object)) [[unlikely]]
        return false;
    if (declaringLayout.evaluation() == EvaluationState::ProvenOnce)
        return true;
    const FieldData* brand = brandOf(state, declaringLayout, token);
    if (!brand)
        return true;
    const FieldData* brandField = fieldUnderDeclaredName(vm, state, object, declaringLayout, *brand);
    return brandField && object.readInline(*brandField) == JSValue(token);
}

static const FieldData* fieldOfEvaluation(VM& vm, State& state, const Object& object, JSCellButterfly* token, uint32_t fieldIndex)
{
    const LayoutData& declaringLayout = declaringLayoutOf(state, object, token);
    const FieldData* field = fieldUnderDeclaredName(vm, state, object, declaringLayout, declaredFieldOf(declaringLayout, fieldIndex));
    if (!field || !wasMadeByEvaluation(vm, state, object, declaringLayout, token))
        return nullptr;
    return field;
}

static void rememberLayoutWithoutPlace(State& state, const Object& object, JSCellButterfly* token, uint32_t fieldIndex)
{
    if (!isOfRealmOf(state, object) || (fieldIndex & layoutHasNoPlaceForPrivateField))
        return;
    uint32_t layoutOfObject = object.layout().index;
    uint32_t declaringClass = token->get(ClassToken::classIndexSlot).asUInt32();
    state.instance.classes.privateFieldPlaces[indexOfPrivateFieldPlace(layoutOfObject, declaringClass, fieldIndex)] = { static_cast<uint64_t>(layoutOfObject) << 32 | declaringClass, fieldIndex | layoutHasNoPlaceForPrivateField, 0, nullptr, 0 };
}

static bool hasNoPlaceForName(VM& vm, State& state, JSObject* holder, JSCellButterfly* token, uint32_t fieldIndex)
{
    Object* object = compiledObjectOrNull(holder);
    if (!object || object->hasExtraProperties())
        return false;
    const LayoutData& declaringLayout = declaringLayoutOf(state, *object, token);
    if (fieldUnderDeclaredName(vm, state, *object, declaringLayout, declaredFieldOf(declaringLayout, fieldIndex)))
        return false;
    rememberLayoutWithoutPlace(state, *object, token, fieldIndex);
    return true;
}

static void rememberFieldOfOtherLayout(State& state, const Object& object, JSCellButterfly* token, uint32_t fieldIndex, const FieldData& field)
{
    uint32_t layoutOfObject = object.layout().index;
    uint32_t declaringClass = token->get(ClassToken::classIndexSlot).asUInt32();
    if (layoutOfObject == declaringClass || declaringLayoutOf(state, token).evaluation() != EvaluationState::ProvenOnce)
        return;
    state.instance.classes.privateFieldPlaces[indexOfPrivateFieldPlace(layoutOfObject, declaringClass, fieldIndex)] = { static_cast<uint64_t>(layoutOfObject) << 32 | declaringClass, fieldIndex, 0, &field, 0 };
}

static JSValue tryReadFromSlot(VM& vm, State& state, JSObject* base, JSCellButterfly* token, uint32_t fieldIndex)
{
    Object* object = compiledObjectOrNull(base);
    if (!object)
        return { };
    const FieldData* field = fieldOfEvaluation(vm, state, *object, token, fieldIndex);
    if (!field)
        return { };
    JSValue value = object->readInline(*field);
    if (value)
        rememberFieldOfOtherLayout(state, *object, token, fieldIndex, *field);
    return value;
}

static bool tryStoreIntoFilledSlot(VM& vm, State& state, JSObject* base, JSCellButterfly* token, uint32_t fieldIndex, JSValue stored)
{
    Object* object = compiledObjectOrNull(base);
    if (!object)
        return false;
    const FieldData* field = fieldOfEvaluation(vm, state, *object, token, fieldIndex);
    return field && object->isInSlot(*field) && object->tryStore(vm, *field, stored);
}

static bool tryDefineInEmptySlot(VM& vm, State& state, JSObject* base, JSCellButterfly* token, uint32_t fieldIndex, JSValue defined)
{
    Object* object = compiledObjectOrNull(base);
    if (!object || privateNameIfCreated(token, fieldIndex))
        return false;
    const LayoutData& declaringLayout = declaringLayoutOf(state, *object, token);
    const FieldData& declared = declaredFieldOf(declaringLayout, fieldIndex);
    const FieldData* field = fieldUnderDeclaredName(vm, state, *object, declaringLayout, declared);
    if (!field || findFieldProperty(vm, *object, *field))
        return false;
    if (declared.isBrand() ? defined != JSValue(token) : !wasMadeByEvaluation(vm, state, *object, declaringLayout, token))
        return false;
    return tryCreateField(vm, state, *object, *field, defined);
}

static bool tryDefineByExistingTransition(VM& vm, State& state, JSObject* base, JSCellButterfly* token, uint32_t fieldIndex, JSValue defined)
{
    if (compiledObjectOrNull(base) || privateNameIfCreated(token, fieldIndex) || !token->get(ClassToken::knownStructuresSlot) || base->hasFrozenElementsInPlace())
        return false;
    const LayoutData& declaringLayout = declaringLayoutOf(state, token);
    const FieldData& declared = declaredFieldOf(declaringLayout, fieldIndex);
    if (objectsWithoutLayoutTakeNamesOfEvaluation(declaringLayout))
        return false;
    if (declared.isBrand() ? defined != JSValue(token) : !!brandOf(state, declaringLayout, token))
        return false;
    Structure* before = base->structure();
    if (!base->tryPutDirectByExistingTransition(vm, state.objects->layouts.nameOf(vm, declared), defined, attributesOfNameWithoutField))
        return false;
    rememberDefinition(vm, token, fieldIndex, before, base->structure(), std::nullopt);
    return true;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewClassToken, JSCell*, (Instance* instance, uint32_t classIndex))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewClassToken);
    JSCell* token = ClassToken::create(vm, classLayoutAt(instance->state->objects->classes, classIndex));
    OPERATION_RETURN(scope, token);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPrivateGet, EncodedJSValue, (Instance* instance, EncodedJSValue object, JSCell* token, uint32_t field))
{
    constexpr Entry entry = Entry::operationAOTPrivateGet;
    // A lookup may make the property table of a Structure, which ends in a place where the collector may stop this thread. Compiled code counts on no collection here.
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, entry);
    JSCellButterfly* classToken = cellArgument<JSCellButterfly>(token, entry);
    if (JSValue::decode(object).isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createPrivateNameOfNullishError(globalObject, JSValue::decode(object)));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    JSObject* base = JSValue::decode(object).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (JSObject* holder = holderOfNamesWithoutField(base)) {
        if (std::optional<PropertyOffset> offset = findInKnownStructure(classToken, field, holder))
            OPERATION_RETURN(scope, JSValue::encode(holder->getDirect(*offset)));
    }
    if (JSValue value = tryReadFromSlot(vm, *instance->state, base, classToken, field))
        OPERATION_RETURN(scope, JSValue::encode(value));
    PrivateLocation location = locatePrivateName(vm, *instance->state, base, classToken, field);
    if (!location.name) [[unlikely]] {
        throwException(globalObject, scope, createAbsentNameError(globalObject, location));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    rememberLocation(vm, base, classToken, field, location);
    OPERATION_RETURN(scope, JSValue::encode(location.property.value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPrivateHas, EncodedJSValue, (Instance* instance, EncodedJSValue object, JSCell* token, uint32_t field))
{
    constexpr Entry entry = Entry::operationAOTPrivateHas;
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, entry);
    JSCellButterfly* classToken = cellArgument<JSCellButterfly>(token, entry);
    JSValue base = JSValue::decode(object);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    JSObject* holder = asObject(base);
    if (hasNoPlaceForName(vm, *instance->state, holder, classToken, field))
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(false)));
    JSObject* holderWithoutField = holderOfNamesWithoutField(holder);
    if ((holderWithoutField && findInKnownStructure(classToken, field, holderWithoutField)) || tryReadFromSlot(vm, *instance->state, holder, classToken, field))
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(true)));
    if (isKnownToLackName(classToken, field, holder) || hasNoPlaceForName(vm, *instance->state, holder, classToken, field))
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(false)));
    PrivateLocation location = locatePrivateName(vm, *instance->state, holder, classToken, field);
    rememberLocation(vm, holder, classToken, field, location);
    if (!location.name && !location.compiledObject && !location.underDeclaredName.value && !holder->structure()->isDictionary())
        rememberStructureWithoutName(vm, classToken, field, holder->structure());
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(!!location.name)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPrivateSet, void, (Instance* instance, EncodedJSValue object, JSCell* token, uint32_t field, EncodedJSValue value))
{
    constexpr Entry entry = Entry::operationAOTPrivateSet;
    AOT_OPERATION_BEGIN(instance, entry);
    JSCellButterfly* classToken = cellArgument<JSCellButterfly>(token, entry);
    if (JSValue::decode(object).isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createPrivateNameOfNullishError(globalObject, JSValue::decode(object)));
        OPERATION_RETURN(scope);
    }
    JSObject* base = JSValue::decode(object).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    JSValue stored = JSValue::decode(value);
    if (JSObject* holder = holderOfNamesWithoutField(base)) {
        if (std::optional<PropertyOffset> offset = findInKnownStructure(classToken, field, holder)) {
            Structure* structure = holder->structure();
            if (!structure->mayHaveImmutableProperties()) [[likely]] {
                structure->didReplaceProperty(*offset);
                holder->putDirectOffset(vm, *offset, stored);
                OPERATION_RETURN(scope);
            }
        }
    }
    if (tryStoreIntoFilledSlot(vm, *instance->state, base, classToken, field, stored))
        OPERATION_RETURN(scope);
    PrivateLocation location = locatePrivateName(vm, *instance->state, base, classToken, field);
    rememberLocation(vm, base, classToken, field, location);
    if (!location.name) {
        throwException(globalObject, scope, createAbsentNameError(globalObject, location));
        OPERATION_RETURN(scope);
    }
    Object* compiledObject = location.compiledObject;
    if (!compiledObject) {
        base->structure()->didReplaceProperty(location.property.offset);
        base->putDirectOffset(vm, location.property.offset, stored);
    } else if (!location.property.isInSlot)
        compiledObject->putDirectFromNativeCode(vm, location.name, stored, attributesOfNameWithoutField);
    else if (!compiledObject->tryStore(vm, *location.property.field, stored))
        compiledObject->moveFieldToExtraProperties(vm, *location.property.field, { stored, attributesOfNameWithoutField }, SlotExitReason::StoreOfOtherType);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPrivateDefine, void, (Instance* instance, EncodedJSValue object, JSCell* token, uint32_t field, EncodedJSValue value))
{
    constexpr Entry entry = Entry::operationAOTPrivateDefine;
    AOT_OPERATION_BEGIN(instance, entry);
    State& state = *instance->state;
    JSCellButterfly* classToken = cellArgument<JSCellButterfly>(token, entry);
    if (!JSValue::decode(object).isObject()) [[unlikely]] {
        throwException(globalObject, scope, JSC::createNotAnObjectError(globalObject, JSValue::decode(object)));
        OPERATION_RETURN(scope);
    }
    JSObject* base = objectArgument(object, entry);
    JSValue defined = JSValue::decode(value);
    if (tryDefineInEmptySlot(vm, state, base, classToken, field, defined) || tryDefineByExistingTransition(vm, state, base, classToken, field, defined)) {
        ensureStillAliveHere(classToken);
        OPERATION_RETURN(scope);
    }
    PrivateLocation location = locatePrivateName(vm, state, base, classToken, field);
    bool isBrand = location.declared->isBrand();
    if (location.name) {
        throwException(globalObject, scope, isBrand ? createReinstallPrivateMethodError(globalObject) : createRedefinedPrivateNameError(globalObject));
        OPERATION_RETURN(scope);
    }
    if (isBrand && defined != JSValue(classToken)) [[unlikely]]
        crashOnWrongOperand(entry);
    bool takesNameOfEvaluation = !location.compiledObject && objectsWithoutLayoutTakeNamesOfEvaluation(declaringLayoutOf(state, classToken));
    bool canUseDeclaredName = !takesNameOfEvaluation && !location.underDeclaredName.value && (isBrand || location.belongsToEvaluation);
    UniquedStringImpl* uid = canUseDeclaredName ? location.declaredName : &privateNameOf(vm, state, classToken, field)->uid();
    if (Object* compiledObject = location.compiledObject) {
        const FieldData* own = canUseDeclaredName ? location.underDeclaredName.field : nullptr;
        if (!own || !tryCreateField(vm, compiledObject->state(), *compiledObject, *own, defined)) {
            if (own && !state.objects->layouts.accept(*own, defined))
                validateMutation(vm, *compiledObject, *own, ObjectMutation::StoreOfOtherType);
            const CompiledStructure& before = compiledObject->compiledStructure();
            bool hadExtraProperties = compiledObject->hasExtraProperties();
            JSObject* extraBefore = extraPropertiesIfCreated(*compiledObject);
            Structure* extraStructureBefore = extraBefore ? extraBefore->structure() : instance->realm.emptyExtraPropertiesStructure;
            vm.aotState().somePrivateNameLiesBesideFields = true;
            compiledObject->putDirectFromNativeCode(vm, uid, defined, attributesOfNameWithoutField);
            ensureRecordsOfKnownStructures(vm, state, classToken);
            bool needsBrand = !isBrand && location.hasBrand;
            if (!own && canUseDeclaredName && hadExtraProperties == !!extraBefore && (!needsBrand || isValidOffset(location.brand.offset)))
                rememberDefinitionAmongExtraProperties(vm, classToken, field, *compiledObject, before, extraBefore, extraStructureBefore, needsBrand ? std::optional(location.brand.offset) : std::nullopt);
        }
    } else {
        if (base->hasFrozenElementsInPlace()) [[unlikely]]
            base->moveFrozenElementsToSparseMap(vm);
        Structure* before = base->structure();
        vm.aotState().somePrivateNameLiesBesideFields = true;
        if (!base->tryPutDirectByExistingTransition(vm, uid, defined, attributesOfNameWithoutField)) [[unlikely]] {
            PutPropertySlot slot(base, true);
            base->definePrivateField(globalObject, uid, defined, slot, attributesOfNameWithoutField);
        }
        ensureRecordsOfKnownStructures(vm, state, classToken);
        bool needsBrand = !isBrand && location.hasBrand;
        if (takesNameOfEvaluation)
            rememberDefinition(vm, classToken, field, before, base->structure(), std::nullopt);
        if (canUseDeclaredName && (!needsBrand || isValidOffset(location.brand.offset)))
            rememberDefinition(vm, classToken, field, before, base->structure(), needsBrand ? std::optional(location.brand.offset) : std::nullopt);
    }
    ensureStillAliveHere(classToken);
    OPERATION_RETURN(scope);
}

static JSCellButterfly* classTokenOrNull(State& state, JSValue value)
{
    JSCellButterfly* token = value && value.isCell() ? dynamicDowncast<JSCellButterfly>(value.asCell()) : nullptr;
    if (!token || token->length() < ClassToken::firstNameSlot || !token->get(ClassToken::classIndexSlot).isUInt32())
        return nullptr;
    uint32_t index = token->get(ClassToken::classIndexSlot).asUInt32();
    const LayoutData* layout = index < state.tables.numberOfLayoutIndices ? state.objects->classes.classWithIndex(index) : nullptr;
    return layout && token->length() == ClassToken::firstNameSlot + layout->numberOfFields ? token : nullptr;
}

struct NameOfEvaluation {
    JSCellButterfly* token { nullptr };
    uint32_t field { 0 };
};

static void findNameAmongThoseOf(State& state, JSValue value, UniquedStringImpl* name, NameOfEvaluation& found)
{
    JSCellButterfly* token = found.token ? nullptr : classTokenOrNull(state, value);
    uint32_t numberOfFields = token ? declaringLayoutOf(state, token).numberOfFields : 0;
    for (uint32_t field = 0; field < numberOfFields; ++field) {
        Symbol* symbol = privateNameIfCreated(token, field);
        if (symbol && &symbol->uid() == name)
            found = { token, field };
    }
}

Symbol* ClassToken::tryNameForText(VM& vm, State& state, JSValue value, UniquedStringImpl* declaredName)
{
    JSCellButterfly* token = classTokenOrNull(state, value);
    if (!token)
        return nullptr;
    const LayoutData& declaringLayout = declaringLayoutOf(state, token);
    if (declaringLayout.evaluation() == EvaluationState::ProvenOnce)
        return nullptr;
    for (const FieldData& field : declaringLayout.fields()) {
        if (!field.isPrivate() || state.objects->layouts.nameOf(vm, field) != declaredName)
            continue;
        vm.aotState().someTextKnowsPrivateNames = true;
        return privateNameOf(vm, state, token, declaringLayout.indexOf(field));
    }
    return nullptr;
}

UniquedStringImpl* ClassToken::declaredNameUnder(VM& vm, Object& holder, UniquedStringImpl* nameOfEvaluation)
{
    State& state = holder.state();
    NameOfEvaluation found;
    for (const FieldData& field : holder.layout().fields()) {
        if (field.isBrand())
            findNameAmongThoseOf(state, findFieldProperty(vm, holder, field).valueOrAccessor, nameOfEvaluation, found);
    }
    JSObject* extra = !found.token && vm.aotState().somePrivateNameLiesBesideFields ? extraPropertiesIfCreated(holder) : nullptr;
    if (extra && !extra->structure()->isQuickPropertyAccessAllowedForEnumeration()) {
        extra->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            if (PropertyName(entry.key()).isPrivateName())
                findNameAmongThoseOf(state, extra->getDirect(entry.offset()), nameOfEvaluation, found);
            return !found.token;
        });
    }
    if (!found.token)
        return nullptr;
    PrivateLocation location = locatePrivateName(vm, state, &holder, found.token, found.field);
    return location.name == location.declaredName ? location.name : nullptr;
}

UniquedStringImpl* ClassToken::nameBesideFields(VM& vm, const Object& allocated, const FieldData& field, bool holderIsCompiled)
{
    State& state = allocated.state();
    vm.aotState().somePrivateNameLiesBesideFields = true;
    UniquedStringImpl* declaredName = state.objects->layouts.nameOf(vm, field);
    for (const FieldData& brand : allocated.layout().fields()) {
        JSCellButterfly* token = !holderIsCompiled && brand.isBrand() ? classTokenOrNull(state, findFieldProperty(vm, allocated, brand).valueOrAccessor) : nullptr;
        const LayoutData* declaringLayout = token ? &declaringLayoutOf(state, token) : nullptr;
        if (!declaringLayout || !objectsWithoutLayoutTakeNamesOfEvaluation(*declaringLayout))
            continue;
        for (const FieldData& declared : declaringLayout->fields()) {
            if (declared.isPrivate() && state.objects->layouts.nameOf(vm, declared) == declaredName)
                return &privateNameOf(vm, state, token, declaringLayout->indexOf(declared))->uid();
        }
    }
    return declaredName;
}

enum class AboveProxies : bool { IsNotLookedAt, IsLookedAt };

static size_t collectObjectsThatLookupComesTo(JSGlobalObject* globalObject, JSObject* lowest, AboveProxies aboveProxies, MarkedArgumentBuffer& objects)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::optional<size_t> numberBelowProxies;
    for (JSObject* object = lowest; object;) {
        bool isProxy = object->type() == ProxyObjectType;
        if (isProxy && !numberBelowProxies)
            numberBelowProxies = objects.size();
        if (isProxy && aboveProxies == AboveProxies::IsNotLookedAt)
            break;
        if (!isProxy)
            objects.append(object);
        JSValue above = isProxy ? object->getPrototype(globalObject) : object->getPrototypeDirect();
        RETURN_IF_EXCEPTION(scope, 0);
        object = above.isObject() ? asObject(above) : nullptr;
    }
    return numberBelowProxies.value_or(objects.size());
}

static void collectClassTokens(VM& vm, State& state, const MarkedArgumentBuffer& objects, MarkedArgumentBuffer& tokens)
{
    auto add = [&](JSValue value) {
        JSCellButterfly* token = classTokenOrNull(state, value);
        for (size_t index = 0; token && index < tokens.size(); ++index) {
            if (tokens.at(index) == JSValue(token))
                token = nullptr;
        }
        if (token)
            tokens.append(token);
    };
    for (size_t index = 0; index < objects.size(); ++index) {
        JSObject* object = asObject(objects.at(index));
        if (Object* compiledObject = compiledObjectOrNull(object)) {
            for (const FieldData& field : compiledObject->layout().fields() | std::views::reverse) {
                if (field.isBrand())
                    add(findFieldProperty(vm, *compiledObject, field).valueOrAccessor);
            }
        }
        JSObject* holder = vm.aotState().somePrivateNameLiesBesideFields ? holderOfNamesWithoutField(object) : nullptr;
        if (!holder || holder->structure()->isQuickPropertyAccessAllowedForEnumeration())
            continue;
        holder->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            if (PropertyName(entry.key()).isPrivateName())
                add(holder->getDirect(entry.offset()));
            return true;
        });
    }
}

static JSValue findPrivateFieldAmong(VM& vm, State& state, const MarkedArgumentBuffer& objects, size_t numberBelowProxies, JSCellButterfly* token, uint32_t field)
{
    for (size_t index = 0; index < objects.size(); ++index) {
        if (PrivateLocation location = locatePrivateName(vm, state, asObject(objects.at(index)), token, field); location.name)
            return index < numberBelowProxies ? location.property.value : jsUndefined();
    }
    return { };
}

template<typename Functor>
static void forEachWrittenPrivateFieldOn(JSGlobalObject* globalObject, JSObject* lowest, const Functor& functor)
{
    VM& vm = globalObject->vm();
    if (!vm.aotState().someClassHasPrivateNames)
        return;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Instance* instance = Instance::of(globalObject);
    if (JSGlobalObject* realm = lowest->structure()->realm(); !instance && realm)
        instance = Instance::of(realm);
    if (!instance || !instance->state)
        return;
    State& state = *instance->state;
    MarkedArgumentBuffer objects;
    size_t numberBelowProxies = collectObjectsThatLookupComesTo(globalObject, lowest, AboveProxies::IsLookedAt, objects);
    RETURN_IF_EXCEPTION(scope, void());
    MarkedArgumentBuffer tokens;
    collectClassTokens(vm, state, objects, tokens);
    if (objects.hasOverflowed() || tokens.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return;
    }
    for (size_t index = 0; index < tokens.size(); ++index) {
        JSCellButterfly* token = uncheckedDowncast<JSCellButterfly>(tokens.at(index).asCell());
        const LayoutData& declaringLayout = declaringLayoutOf(state, token);
        for (uint32_t field = 0; field < declaringLayout.numberOfFields; ++field) {
            const FieldData& declared = declaringLayout.fields()[field];
            JSValue held = declared.isWrittenPrivateName ? findPrivateFieldAmong(vm, state, objects, numberBelowProxies, token, field) : JSValue();
            if (held && functor(state, token, field, declared, held) == IterationStatus::Done)
                return;
        }
    }
}

unsigned PropertyWalk::countPrivateFieldsOfCompiledClasses(JSGlobalObject* globalObject, JSObject* lowest)
{
    unsigned count = 0;
    forEachWrittenPrivateFieldOn(globalObject, lowest, [&](State&, JSCellButterfly*, uint32_t, const FieldData&, JSValue) {
        ++count;
        return IterationStatus::Continue;
    });
    return count;
}

bool PropertyWalk::matchPrivateFieldsOfCompiledClasses(JSGlobalObject* globalObject, JSObject* object, JSObject* pattern, const ScopedLambda<bool(JSValue ofObject, JSValue ofPattern)>& matches)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool everyFieldMatches = true;
    MarkedArgumentBuffer objects;
    forEachWrittenPrivateFieldOn(globalObject, pattern, [&](State& state, JSCellButterfly* token, uint32_t field, const FieldData& declared, JSValue ofPattern) {
        if (objects.isEmpty()) {
            collectObjectsThatLookupComesTo(globalObject, object, AboveProxies::IsNotLookedAt, objects);
            scope.assertNoException();
        }
        if (objects.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            everyFieldMatches = false;
            return IterationStatus::Done;
        }
        JSValue ofObject = findPrivateFieldAmong(vm, state, objects, objects.size(), token, field);
        auto letsNothingGo = [](JSValue value) { return !value.isCell() || value.isString() || value.isHeapBigInt(); };
        if (!ofObject)
            everyFieldMatches = false;
        else if (declared.isKeptFromComparisons && (!letsNothingGo(ofPattern) || !letsNothingGo(ofObject))) {
            const ImageText& text = state.instance.program->text();
            throwException(globalObject, scope, createComparisonOfPrivateFieldRefusedError(globalObject, text.createString(text.identifier(declared.name).text())));
            everyFieldMatches = false;
        } else {
            bool matched = matches(ofObject, ofPattern);
            everyFieldMatches = !scope.exception() && matched;
        }
        return everyFieldMatches ? IterationStatus::Continue : IterationStatus::Done;
    });
    RETURN_IF_EXCEPTION(scope, false);
    return everyFieldMatches;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
