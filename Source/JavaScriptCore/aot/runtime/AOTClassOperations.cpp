/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTCheckedFrameCells.h"
#include "AOTClassTable.h"
#include "AOTClassToken.h"
#include "AOTClosure.h"
#include "AOTHeapHooks.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "AOTUnwinding.h"
#include "DeferGCInlines.h"
#include "ExceptionHelpers.h"
#include "InternalFunction.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "Symbol.h"
#include <algorithm>
#include <wtf/text/SymbolImpl.h>

namespace JSC { namespace AOT {

static const LayoutData& classLayoutAt(const ClassTable& classes, uint32_t classIndex)
{
    const LayoutData* classLayout = classes.classWithIndex(classIndex);
    RELEASE_ASSERT(classLayout);
    return *classLayout;
}

// What these operations make on demand stays on the heap and names the function: the prototype object as its constructor, the remembered allocation as its owner.
static ALWAYS_INLINE void validateFunctionIsNotInFrame(VM& vm, const JSCell* functionCell)
{
    if (validatesRuntime(vm) || CheckedFrameCells::isInUse()) [[unlikely]]
        validateFunctionCellIsNotInFrame(vm, functionCell);
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
    validateFunctionIsNotInFrame(vm, closure);
    const LayoutData& classLayout = classLayoutAt(classes, requestAt<NewClassRequest>(instance, request).classIndex);
    RELEASE_ASSERT(closure->function() == FunctionID(classLayout.classData().classConstructor));
    closure = classes.classObjectForEvaluation(closure, classLayout);
    classes.defineClass(globalObject, closure, classLayout, JSValue::decode(heritage), JSValue::decode(prototypeParent));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, closure);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetClassPrototype, JSCell*, (Instance* instance, JSCell* classObject))
{
    constexpr Entry entry = Entry::operationAOTGetClassPrototype;
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, entry);
    validateFunctionIsNotInFrame(vm, classObject);
    JSValue prototype = cellArgument<Closure>(classObject, entry)->prototypeProperty(vm);
    if (!prototype || !prototype.isObject()) [[unlikely]]
        crashOnWrongOperand(entry);
    JSCell* result = asObject(prototype);
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBeginMemberGuards, void, (Instance* instance, uint32_t classIndex))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTBeginMemberGuards);
    ClassTable& classes = instance->state->objects->classes;
    classes.classPrototypeOfInstances(vm, classLayoutAt(classes, classIndex));
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
    JSObject* target = objectArgument(newTarget, entry);
    validateFunctionIsNotInFrame(vm, target);
    if (auto* closure = dynamicDowncast<Closure>(target); closure && Instance::of(closure->structure()->realm()) == instance && FunctionTables::hasPrototypePropertyAtCreation(closure->kind())) {
        ClassTable& classes = instance->state->objects->classes;
        JSObject* prototype = classes.prototypeForInstancesOf(vm, closure);
        classes.rememberPlaceOfPrototypeProperty(vm, closure, prototype);
        // The first construction defines the class of a constructor function, which arms the guards that its stores are compiled under.
        if (const LayoutData* classLayout = classes.classConstructedBy(closure->function()); classLayout && !ClassTable::isDefinedByClass(*classLayout)) {
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
    DeferGC deferGC(vm);
    ObjectModel& objects = *instance->state->objects;
    // The stub that looks at the prototype first has allocated nothing.
    Object* allocated = object ? cellArgument<Object>(object, entry) : uncheckedDowncast<Object>(objects.structures.allocate(vm, allocation));
    JSObject* result = objects.classes.instanceWithPrototype(vm, allocated, cellArgument<JSObject>(prototype, entry), !!holdsNoField, allocation);
    if (result == allocated)
        objects.structures.predictStructure(vm, allocation, uncheckedDowncast<CompiledStructure>(result->structure()));
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateThis, JSCell*, (Instance* instance, JSCell* constructor, JSCell* newTarget))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCreateThis);
    JSObject* constructorObject = cellArgument<JSObject>(constructor, Entry::operationAOTCreateThis);
    JSObject* newTargetObject = cellArgument<JSObject>(newTarget, Entry::operationAOTCreateThis);
    validateFunctionIsNotInFrame(vm, constructorObject);
    validateFunctionIsNotInFrame(vm, newTargetObject);
    OPERATION_RETURN(scope, instance->state->objects->classes.createThis(globalObject, constructorObject, newTargetObject));
}

JSCellButterfly* ClassToken::create(VM& vm, const LayoutData& classLayout)
{
    JSCellButterfly* butterfly = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, firstNameSlot + classLayout.numberOfFields);
    butterfly->setIndex(vm, classIndexSlot, jsNumber(classLayout.index));
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

// The offset of the value, if a record for the object's Structure says that the object has the name and belongs to this evaluation.
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

// What compiled code does by itself for the next object of the Structure. The name is the declared one, and the Structure before lacks it and every name that this evaluation has made for itself so far.
static void rememberDefinition(VM& vm, JSCellButterfly* token, uint32_t field, Structure* before, Structure* after, std::optional<PropertyOffset> brand)
{
    JSValue records = token->get(ClassToken::knownStructuresSlot);
    if (!records || after->previousID() != before || after->transitionKind() != TransitionKind::PropertyAddition)
        return;
    PropertyOffset offset = after->transitionOffset();
    std::optional<uint32_t> newCapacity = capacityOfStorageGrownByStub(vm, before, after, offset);
    if (!newCapacity)
        return;
    int32_t placeOfValue = *newCapacity ? static_cast<int32_t>(*newCapacity << 1 | 1) : locationOf(offset);
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

static constexpr unsigned numberOfStructuresWithoutNames = 8;

static unsigned placeOfStructureWithoutName(StructureID structure, uint32_t field)
{
    return ((structure.bits() >> 4) + field) % numberOfStructuresWithoutNames * 2;
}

// An object that is no compiled object has the properties that its Structure lists, unless that is a dictionary: a Structure that lacks a name lacks it for ever. The token keeps the Structure, so its number is given to no other.
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
        token->setIndex(vm, ClassToken::structuresWithoutNamesSlot, JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, numberOfStructuresWithoutNames * 2));
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

static const FieldData& declaredFieldOf(const LayoutData& declaringLayout, uint32_t fieldIndex)
{
    RELEASE_ASSERT(fieldIndex < declaringLayout.numberOfFields);
    const FieldData& declared = declaringLayout.fields()[fieldIndex];
    RELEASE_ASSERT(declared.isPrivate());
    return declared;
}

// Null for a class that declares no brand of its own. A brand that the layout inherits from a base class does not count.
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
            if (field.isBrand() && !isInherited(field)) {
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
    const ImageText& text = Image::current()->text();
    String description = text.createString(text.identifier(declaringLayoutOf(state, token).fields()[field].name).text());
    Symbol* name = Symbol::create(vm, PrivateSymbolImpl::create(*description.impl()).get());
    token->setIndex(vm, nameSlotOf(token, field), name);
    return name;
}

struct PrivateProperty {
    const FieldData* field { nullptr };
    JSValue value;
    bool isInSlot { false };
    // Only for a property of an ordinary object.
    PropertyOffset offset { invalidOffset };
    unsigned attributes { 0 };
};

struct PrivateLocation {
    Object* compiledObject;
    const FieldData* declared;
    UniquedStringImpl* declaredName;
    bool hasBrand;
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
    } else if (JSObject* extra = compiledObject->extraProperties())
        property.value = extra->getDirect(vm, uid);
    return property;
}

static PrivateLocation locatePrivateName(VM& vm, State& state, JSObject* object, JSCellButterfly* token, uint32_t fieldIndex)
{
    const LayoutData& declaringLayout = declaringLayoutOf(state, token);
    const FieldData& declared = declaredFieldOf(declaringLayout, fieldIndex);
    const FieldData* brand = brandOf(state, declaringLayout, token);
    RuntimeLayoutTable& layouts = state.objects->layouts;
    PrivateLocation location { compiledObjectOrNull(object), &declared, layouts.nameOf(vm, declared), !!brand };
    bool hasDeclaringLayout = location.compiledObject && &location.compiledObject->layout() == &declaringLayout;
    auto find = [&](UniquedStringImpl* uid, const FieldData* fieldOfDeclaringLayout) {
        return findPrivateProperty(vm, state, object, location.compiledObject, uid, hasDeclaringLayout ? fieldOfDeclaringLayout : nullptr);
    };

    UniquedStringImpl* brandName = brand && brand != &declared ? layouts.nameOf(vm, *brand) : nullptr;
    location.underDeclaredName = find(location.declaredName, &declared);
    if (brand)
        location.brand = brandName ? find(brandName, brand) : location.underDeclaredName;
    location.belongsToEvaluation = !brand || location.brand.value == JSValue(token);
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

// What the next access to an object of the same Structure need not look for again.
static void rememberLocation(VM& vm, JSObject* object, JSCellButterfly* token, uint32_t fieldIndex, const PrivateLocation& found)
{
    if (found.compiledObject || !found.name || found.property.attributes || !isValidOffset(found.property.offset) || object->structure()->isDictionary())
        return;
    // The declared name is shared by all evaluations of the class, and the brand tells them apart. A name that this evaluation has made for itself needs no brand.
    bool needsBrand = found.name == found.declaredName && found.hasBrand;
    if (needsBrand && !isValidOffset(found.brand.offset))
        return;
    addKnownStructure(vm, token, fieldIndex, object->structure(), found.property.offset, needsBrand ? std::optional(found.brand.offset) : std::nullopt);
}

static JSObject* createAbsentNameError(JSGlobalObject* globalObject, const PrivateLocation& absent)
{
    return absent.declared->isBrand() ? createPrivateMethodAccessError(globalObject) : createInvalidPrivateNameError(globalObject);
}

// The field of the object's layout that has the name of a field that a class declares. Layouts and their fields are data of the image, so what is remembered never goes stale.
static const FieldData* fieldUnderDeclaredName(VM& vm, State& state, const Object& object, const LayoutData& declaringLayout, const FieldData& declared)
{
    struct Remembered {
        const LayoutData* layoutOfObject;
        const FieldData* declared;
        const FieldData* field;
    };
    constexpr size_t numberOfPlaces = 64;
    static thread_local Remembered places[numberOfPlaces];

    const LayoutData& layout = object.layout();
    if (&layout == &declaringLayout)
        return &declared;
    Remembered& place = places[((std::bit_cast<uintptr_t>(&declared) >> 3) ^ (std::bit_cast<uintptr_t>(&layout) >> 4)) % numberOfPlaces];
    if (place.layoutOfObject != &layout || place.declared != &declared)
        place = { &layout, &declared, object.fieldNamed(state, state.objects->layouts.nameOf(vm, declared)) };
    return place.field;
}

// The declared name is shared by all evaluations of the class, and the brand tells them apart. A class that is evaluated once has nothing to tell apart.
static bool wasMadeByEvaluation(VM& vm, State& state, const Object& object, const LayoutData& declaringLayout, JSCellButterfly* token)
{
    if (declaringLayout.evaluation() == EvaluationState::ProvenOnce)
        return true;
    const FieldData* brand = brandOf(state, declaringLayout, token);
    if (!brand)
        return true;
    const FieldData* brandField = fieldUnderDeclaredName(vm, state, object, declaringLayout, *brand);
    return brandField && object.readInline(*brandField) == JSValue(token);
}

// The usual access: an object of compiled code that keeps the name in the slot of its field, in the evaluation of the class that made the object.
static const FieldData* fieldOfEvaluation(VM& vm, State& state, const Object& object, JSCellButterfly* token, uint32_t fieldIndex)
{
    const LayoutData& declaringLayout = declaringLayoutOf(state, object, token);
    const FieldData* field = fieldUnderDeclaredName(vm, state, object, declaringLayout, declaredFieldOf(declaringLayout, fieldIndex));
    if (!field || !wasMadeByEvaluation(vm, state, object, declaringLayout, token))
        return nullptr;
    return field;
}

static JSValue tryReadFromSlot(VM& vm, State& state, JSObject* base, JSCellButterfly* token, uint32_t fieldIndex)
{
    Object* object = compiledObjectOrNull(base);
    if (!object)
        return { };
    const FieldData* field = fieldOfEvaluation(vm, state, *object, token, fieldIndex);
    return field ? object->readInline(*field) : JSValue();
}

static bool tryStoreIntoFilledSlot(VM& vm, State& state, JSObject* base, JSCellButterfly* token, uint32_t fieldIndex, JSValue stored)
{
    Object* object = compiledObjectOrNull(base);
    if (!object)
        return false;
    const FieldData* field = fieldOfEvaluation(vm, state, *object, token, fieldIndex);
    return field && object->isInSlot(*field) && object->tryStore(vm, *field, stored);
}

// The usual definition: an object of compiled code whose slot for the field is still empty, in the evaluation of the class that made the object. Everything else, and every error, is left to the caller.
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
    // The brand is what makes the object one of the evaluation.
    if (declared.isBrand() ? defined != JSValue(token) : !wasMadeByEvaluation(vm, state, *object, declaringLayout, token))
        return false;
    return tryCreateField(vm, state, *object, *field, defined);
}

// An ordinary object whose Structure has taken the name before. A Structure that leads on by adding a name has not got the name, so nothing is looked up.
static bool tryDefineByExistingTransition(VM& vm, State& state, JSObject* base, JSCellButterfly* token, uint32_t fieldIndex, JSValue defined)
{
    if (compiledObjectOrNull(base) || privateNameIfCreated(token, fieldIndex) || !token->get(ClassToken::knownStructuresSlot) || base->hasFrozenElementsInPlace())
        return false;
    const LayoutData& declaringLayout = declaringLayoutOf(state, token);
    const FieldData& declared = declaredFieldOf(declaringLayout, fieldIndex);
    // The brand is what makes the object one of the evaluation. Whether an object that has a brand is one is left to the caller.
    if (declared.isBrand() ? defined != JSValue(token) : !!brandOf(state, declaringLayout, token))
        return false;
    Structure* before = base->structure();
    if (!base->tryPutDirectByExistingTransition(vm, state.objects->layouts.nameOf(vm, declared), defined, static_cast<unsigned>(PropertyAttribute::None)))
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
    JSObject* base = JSValue::decode(object).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (JSValue value = tryReadFromSlot(vm, *instance->state, base, classToken, field))
        OPERATION_RETURN(scope, JSValue::encode(value));
    if (std::optional<PropertyOffset> offset = findInKnownStructure(classToken, field, base))
        OPERATION_RETURN(scope, JSValue::encode(base->getDirect(*offset)));
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
    if (tryReadFromSlot(vm, *instance->state, holder, classToken, field) || findInKnownStructure(classToken, field, holder))
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(true)));
    if (isKnownToLackName(classToken, field, holder))
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(false)));
    PrivateLocation location = locatePrivateName(vm, *instance->state, holder, classToken, field);
    rememberLocation(vm, holder, classToken, field, location);
    // Under the declared name the brand says which evaluation of the class an object belongs to, and the brand is in the object.
    if (!location.name && !location.compiledObject && !location.underDeclaredName.value && !holder->structure()->isDictionary())
        rememberStructureWithoutName(vm, classToken, field, holder->structure());
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(!!location.name)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPrivateSet, void, (Instance* instance, EncodedJSValue object, JSCell* token, uint32_t field, EncodedJSValue value))
{
    constexpr Entry entry = Entry::operationAOTPrivateSet;
    AOT_OPERATION_BEGIN(instance, entry);
    JSCellButterfly* classToken = cellArgument<JSCellButterfly>(token, entry);
    JSObject* base = JSValue::decode(object).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    JSValue stored = JSValue::decode(value);
    if (tryStoreIntoFilledSlot(vm, *instance->state, base, classToken, field, stored))
        OPERATION_RETURN(scope);
    if (std::optional<PropertyOffset> offset = findInKnownStructure(classToken, field, base)) {
        Structure* structure = base->structure();
        if (!structure->mayHaveImmutableProperties()) [[likely]] {
            structure->didReplaceProperty(*offset);
            base->putDirectOffset(vm, *offset, stored);
            OPERATION_RETURN(scope);
        }
    }
    PrivateLocation location = locatePrivateName(vm, *instance->state, base, classToken, field);
    rememberLocation(vm, base, classToken, field, location);
    if (!location.name) {
        throwException(globalObject, scope, createAbsentNameError(globalObject, location));
        OPERATION_RETURN(scope);
    }
    Object* compiledObject = location.compiledObject;
    if (!compiledObject) {
        Structure* structure = base->structure();
        if (!location.property.attributes && !structure->mayHaveImmutableProperties()) [[likely]] {
            structure->didReplaceProperty(location.property.offset);
            base->putDirectOffset(vm, location.property.offset, stored);
            OPERATION_RETURN(scope);
        }
        PutPropertySlot slot(base, true);
        base->setPrivateField(globalObject, location.name, stored, slot);
    } else if (!location.property.isInSlot)
        compiledObject->putDirectFromNativeCode(vm, location.name, stored, static_cast<unsigned>(PropertyAttribute::None));
    else if (!compiledObject->tryStore(vm, *location.property.field, stored))
        compiledObject->moveFieldToExtraProperties(vm, *location.property.field, { stored, static_cast<unsigned>(PropertyAttribute::None) }, SlotExitReason::StoreOfOtherType);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPrivateDefine, void, (Instance* instance, EncodedJSValue object, JSCell* token, uint32_t field, EncodedJSValue value))
{
    constexpr Entry entry = Entry::operationAOTPrivateDefine;
    AOT_OPERATION_BEGIN(instance, entry);
    State& state = *instance->state;
    JSCellButterfly* classToken = cellArgument<JSCellButterfly>(token, entry);
    // A constructor of the host may answer `new` with a primitive, which a class below it then has as `this`.
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
    bool canUseDeclaredName = !location.underDeclaredName.value && (isBrand || location.belongsToEvaluation);
    UniquedStringImpl* uid = canUseDeclaredName ? location.declaredName : &privateNameOf(vm, state, classToken, field)->uid();
    if (Object* compiledObject = location.compiledObject) {
        const FieldData* own = canUseDeclaredName ? location.underDeclaredName.field : nullptr;
        if (!own || !tryCreateField(vm, compiledObject->state(), *compiledObject, *own, defined))
            compiledObject->putDirectFromNativeCode(vm, uid, defined, static_cast<unsigned>(PropertyAttribute::None));
    } else {
        // A private name is no property, so a frozen array takes one. The structure of an array whose elements were frozen in place admits nothing more, that of the ordinary frozen form does.
        if (base->hasFrozenElementsInPlace()) [[unlikely]]
            base->moveFrozenElementsToSparseMap(vm);
        Structure* before = base->structure();
        if (!base->tryPutDirectByExistingTransition(vm, uid, defined, static_cast<unsigned>(PropertyAttribute::None))) [[unlikely]] {
            PutPropertySlot slot(base, true);
            base->definePrivateField(globalObject, uid, defined, slot);
        }
        if (!classToken->get(ClassToken::knownStructuresSlot)) [[unlikely]]
            classToken->setIndex(vm, ClassToken::knownStructuresSlot, JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, ClassToken::slotOfRecord(declaringLayoutOf(state, classToken).numberOfFields, 0)));
        // A field of a class that has a brand goes under the declared name in the objects of this evaluation alone, which the brand in the object tells.
        bool needsBrand = !isBrand && location.hasBrand;
        if (canUseDeclaredName && (!needsBrand || isValidOffset(location.brand.offset)))
            rememberDefinition(vm, classToken, field, before, base->structure(), needsBrand ? std::optional(location.brand.offset) : std::nullopt);
    }
    ensureStillAliveHere(classToken);
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
