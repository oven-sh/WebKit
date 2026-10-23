/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTClassToken.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "Symbol.h"

namespace JSC { namespace AOT {

struct PrivateFieldName {
    const LayoutData* declaringLayout { nullptr };
    const FieldData* declared { nullptr };
    const FieldData* brand { nullptr };
    UniquedStringImpl* uid { nullptr };
    UniquedStringImpl* brandUID { nullptr };
};

struct PrivateProperty {
    const FieldData* field { nullptr };
    JSValue value;
    bool isInSlot { false };
    // Only for a property of an ordinary object.
    PropertyOffset offset { invalidOffset };
    unsigned attributes { 0 };
};

struct PrivateLocation {
    enum class Kind : uint8_t { Absent, Slot, Property };
    Kind kind { Kind::Absent };
    bool belongsToEvaluation { false };
    Object* compiledObject { nullptr };
    UniquedStringImpl* name { nullptr };
    PrivateProperty property;
    PrivateFieldName declaredName;
    PrivateProperty underDeclaredName;
    PrivateProperty brand;
};

static PrivateFieldName resolveFieldName(VM& vm, State& state, JSCell* token, uint32_t fieldIndex)
{
    const LayoutData& declaring = ClassToken::classLayoutOf(state, token);
    RELEASE_ASSERT(fieldIndex < declaring.numberOfFields);
    const FieldData& declared = declaring.fields()[fieldIndex];
    RELEASE_ASSERT(declared.isPrivate());
    const FieldData* brand = ClassToken::brandOf(state, declaring, token);
    RuntimeLayoutTable& layouts = state.objects->layouts;
    UniquedStringImpl* uid = layouts.nameOf(vm, declared);
    UniquedStringImpl* brandUID = brand == &declared ? uid : brand ? layouts.nameOf(vm, *brand) : nullptr;
    return { &declaring, &declared, brand, uid, brandUID };
}

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
        return property;
    }
    if (JSObject* extra = compiledObject->extraProperties())
        property.value = extra->getDirect(vm, uid);
    return property;
}

static PrivateLocation locate(VM& vm, State& state, JSObject* object, JSCell* token, uint32_t fieldIndex)
{
    PrivateLocation location;
    location.compiledObject = compiledObjectOrNull(object);
    location.declaredName = resolveFieldName(vm, state, token, fieldIndex);
    const PrivateFieldName& name = location.declaredName;
    bool hasDeclaringLayout = location.compiledObject && &location.compiledObject->layout() == name.declaringLayout;
    auto find = [&](UniquedStringImpl* uid, const FieldData* fieldOfDeclaringLayout) {
        return findPrivateProperty(vm, state, object, location.compiledObject, uid, hasDeclaringLayout ? fieldOfDeclaringLayout : nullptr);
    };
    auto setFound = [&](UniquedStringImpl* uid, const PrivateProperty& property) {
        location.kind = property.isInSlot ? PrivateLocation::Kind::Slot : PrivateLocation::Kind::Property;
        location.name = uid;
        location.property = property;
    };

    location.underDeclaredName = find(name.uid, name.declared);
    const PrivateProperty& declared = location.underDeclaredName;
    if (name.brand)
        location.brand = name.brand == name.declared ? declared : find(name.brandUID, name.brand);
    location.belongsToEvaluation = !name.brand || location.brand.value == JSValue(token);
    if (location.belongsToEvaluation && declared.value) {
        setFound(name.uid, declared);
        return location;
    }
    if (Symbol* evaluationName = ClassToken::privateNameIfCreated(token, fieldIndex)) {
        PrivateProperty property = find(&evaluationName->uid(), nullptr);
        if (property.value)
            setFound(&evaluationName->uid(), property);
    }
    return location;
}

// What the next access to an object of the same Structure need not look for again.
static void rememberLocation(VM& vm, JSObject* object, JSCell* token, uint32_t fieldIndex, const PrivateLocation& found)
{
    if (found.compiledObject || found.kind != PrivateLocation::Kind::Property || found.property.attributes || !isValidOffset(found.property.offset))
        return;
    Structure* structure = object->structure();
    if (structure->isDictionary())
        return;
    ClassToken::KnownLocation known { found.property.offset, std::nullopt };
    // The declared name is shared by all evaluations of the class, and the brand tells them apart. A name that this evaluation has made for itself needs no brand.
    if (found.name == found.declaredName.uid && found.declaredName.brand) {
        if (!isValidOffset(found.brand.offset))
            return;
        known.brand = found.brand.offset;
    }
    ClassToken::addKnownStructure(vm, token, fieldIndex, structure, known);
}

static JSObject* createAbsentNameError(JSGlobalObject* globalObject, const PrivateLocation& absent)
{
    if (absent.declaredName.declared->isBrand())
        return createPrivateMethodAccessError(globalObject);
    return createInvalidPrivateNameError(globalObject);
}

static JSObject* createPresentNameError(JSGlobalObject* globalObject, const PrivateLocation& present)
{
    if (present.declaredName.declared->isBrand())
        return createReinstallPrivateMethodError(globalObject);
    return createRedefinedPrivateNameError(globalObject);
}

static void setPrivateProperty(JSGlobalObject* globalObject, JSObject* object, const PrivateLocation& present, JSValue value)
{
    VM& vm = globalObject->vm();
    if (Object* compiledObject = present.compiledObject) {
        compiledObject->putDirectFromNativeCode(vm, present.name, value, static_cast<unsigned>(PropertyAttribute::None));
        return;
    }
    Structure* structure = object->structure();
    if (!present.property.attributes && !structure->mayHaveImmutableProperties()) [[likely]] {
        structure->didReplaceProperty(present.property.offset);
        object->putDirectOffset(vm, present.property.offset, value);
        return;
    }
    PutPropertySlot slot(object, true);
    object->setPrivateField(globalObject, present.name, value, slot);
}

static void definePrivateProperty(JSGlobalObject* globalObject, JSObject* object, const PrivateLocation& absent, UniquedStringImpl* uid, JSValue value)
{
    VM& vm = globalObject->vm();
    Object* compiledObject = absent.compiledObject;
    if (!compiledObject) {
        // A private name is no property, so a frozen array takes one. The structure of an array whose elements were frozen in place admits nothing more, that of the ordinary frozen form does.
        if (object->hasFrozenElementsInPlace()) [[unlikely]]
            object->moveFrozenElementsToSparseMap(vm);
        if (object->tryPutDirectByExistingTransition(vm, uid, value, static_cast<unsigned>(PropertyAttribute::None))) [[likely]]
            return;
        PutPropertySlot slot(object, true);
        object->definePrivateField(globalObject, uid, value, slot);
        return;
    }
    const FieldData* own = uid == absent.declaredName.uid ? absent.underDeclaredName.field : nullptr;
    if (own && tryCreateField(vm, compiledObject->state(), *compiledObject, *own, value))
        return;
    compiledObject->putDirectFromNativeCode(vm, uid, value, static_cast<unsigned>(PropertyAttribute::None));
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

static const LayoutData& declaringLayoutOf(State& state, const Object& object, JSCell* token)
{
    const LayoutData& layout = object.layout();
    return layout.index == ClassToken::classIndexOf(token) ? layout : ClassToken::classLayoutOf(state, token);
}

static const FieldData& declaredFieldOf(const LayoutData& declaringLayout, uint32_t fieldIndex)
{
    RELEASE_ASSERT(fieldIndex < declaringLayout.numberOfFields);
    const FieldData& declared = declaringLayout.fields()[fieldIndex];
    RELEASE_ASSERT(declared.isPrivate());
    return declared;
}

// The declared name is shared by all evaluations of the class, and the brand tells them apart. A class that is evaluated once has nothing to tell apart.
static bool wasMadeByEvaluation(VM& vm, State& state, const Object& object, const LayoutData& declaringLayout, JSCell* token)
{
    if (declaringLayout.evaluation() == EvaluationState::ProvenOnce)
        return true;
    const FieldData* brand = ClassToken::brandOf(state, declaringLayout, token);
    if (!brand)
        return true;
    const FieldData* brandField = fieldUnderDeclaredName(vm, state, object, declaringLayout, *brand);
    return brandField && object.readInline(*brandField) == JSValue(token);
}

// The usual access: an object of compiled code that keeps the name in the slot of its field, in the evaluation of the class that made the object.
static const FieldData* fieldOfEvaluation(VM& vm, State& state, const Object& object, JSCell* token, uint32_t fieldIndex)
{
    const LayoutData& declaringLayout = declaringLayoutOf(state, object, token);
    const FieldData* field = fieldUnderDeclaredName(vm, state, object, declaringLayout, declaredFieldOf(declaringLayout, fieldIndex));
    if (!field || !wasMadeByEvaluation(vm, state, object, declaringLayout, token))
        return nullptr;
    return field;
}

static JSValue tryReadFromSlot(VM& vm, State& state, JSObject* base, JSCell* token, uint32_t fieldIndex)
{
    Object* object = compiledObjectOrNull(base);
    if (!object)
        return { };
    const FieldData* field = fieldOfEvaluation(vm, state, *object, token, fieldIndex);
    return field ? object->readInline(*field) : JSValue();
}

static bool tryStoreIntoFilledSlot(VM& vm, State& state, JSObject* base, JSCell* token, uint32_t fieldIndex, JSValue stored)
{
    Object* object = compiledObjectOrNull(base);
    if (!object)
        return false;
    const FieldData* field = fieldOfEvaluation(vm, state, *object, token, fieldIndex);
    return field && object->isInSlot(*field) && object->tryStore(vm, *field, stored);
}

// The usual definition: an object of compiled code whose slot for the field is still empty, in the evaluation of the class that made the object. Everything else, and every error, is left to the caller.
static bool tryDefineInEmptySlot(VM& vm, State& state, JSObject* base, JSCell* token, uint32_t fieldIndex, JSValue defined)
{
    Object* object = compiledObjectOrNull(base);
    if (!object || ClassToken::privateNameIfCreated(token, fieldIndex))
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewClassToken, JSCell*, (Instance* instance, uint32_t classIndex))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewClassToken);
    const LayoutData* classLayout = instance->state->objects->classes.classWithIndex(classIndex);
    RELEASE_ASSERT(classLayout);
    JSCell* token = ClassToken::create(vm, *classLayout);
    OPERATION_RETURN(scope, token);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPrivateGet, EncodedJSValue, (Instance* instance, EncodedJSValue object, JSCell* token, uint32_t field))
{
    constexpr Entry entry = Entry::operationAOTPrivateGet;
    AOT_OPERATION_BEGIN(instance, entry);
    // A lookup may make the property table of a Structure, which ends in a place where the collector may stop this thread. Compiled code counts on no collection here.
    DeferGCForAWhile deferGC(vm);
    JSCell* classToken = cellArgument<JSCellButterfly>(token, entry);
    JSObject* base = JSValue::decode(object).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (JSValue value = tryReadFromSlot(vm, *instance->state, base, classToken, field))
        OPERATION_RETURN(scope, JSValue::encode(value));
    if (std::optional<PropertyOffset> offset = ClassToken::findInKnownStructure(classToken, field, base))
        OPERATION_RETURN(scope, JSValue::encode(base->getDirect(*offset)));
    PrivateLocation location = locate(vm, *instance->state, base, classToken, field);
    if (location.kind == PrivateLocation::Kind::Absent) [[unlikely]] {
        throwException(globalObject, scope, createAbsentNameError(globalObject, location));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    rememberLocation(vm, base, classToken, field, location);
    OPERATION_RETURN(scope, JSValue::encode(location.property.value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPrivateHas, EncodedJSValue, (Instance* instance, EncodedJSValue object, JSCell* token, uint32_t field))
{
    constexpr Entry entry = Entry::operationAOTPrivateHas;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferGCForAWhile deferGC(vm);
    JSCell* classToken = cellArgument<JSCellButterfly>(token, entry);
    JSValue base = JSValue::decode(object);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    if (tryReadFromSlot(vm, *instance->state, asObject(base), classToken, field))
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(true)));
    PrivateLocation location = locate(vm, *instance->state, asObject(base), classToken, field);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(location.kind != PrivateLocation::Kind::Absent)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPrivateSet, void, (Instance* instance, EncodedJSValue object, JSCell* token, uint32_t field, EncodedJSValue value))
{
    constexpr Entry entry = Entry::operationAOTPrivateSet;
    AOT_OPERATION_BEGIN(instance, entry);
    JSCell* classToken = cellArgument<JSCellButterfly>(token, entry);
    JSObject* base = JSValue::decode(object).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    JSValue stored = JSValue::decode(value);
    if (tryStoreIntoFilledSlot(vm, *instance->state, base, classToken, field, stored))
        OPERATION_RETURN(scope);
    if (std::optional<PropertyOffset> offset = ClassToken::findInKnownStructure(classToken, field, base)) {
        Structure* structure = base->structure();
        if (!structure->mayHaveImmutableProperties()) [[likely]] {
            structure->didReplaceProperty(*offset);
            base->putDirectOffset(vm, *offset, stored);
            OPERATION_RETURN(scope);
        }
    }
    PrivateLocation location = locate(vm, *instance->state, base, classToken, field);
    rememberLocation(vm, base, classToken, field, location);
    switch (location.kind) {
    case PrivateLocation::Kind::Absent:
        throwException(globalObject, scope, createAbsentNameError(globalObject, location));
        OPERATION_RETURN(scope);
    case PrivateLocation::Kind::Slot:
        if (!location.compiledObject->tryStore(vm, *location.property.field, stored))
            location.compiledObject->moveFieldToExtraProperties(vm, *location.property.field, { stored, static_cast<unsigned>(PropertyAttribute::None) }, SlotExitReason::StoreOfOtherType);
        OPERATION_RETURN(scope);
    case PrivateLocation::Kind::Property:
        setPrivateProperty(globalObject, base, location, stored);
        OPERATION_RETURN(scope);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTPrivateDefine, void, (Instance* instance, EncodedJSValue object, JSCell* token, uint32_t field, EncodedJSValue value))
{
    constexpr Entry entry = Entry::operationAOTPrivateDefine;
    AOT_OPERATION_BEGIN(instance, entry);
    State& state = *instance->state;
    JSCell* classToken = cellArgument<JSCellButterfly>(token, entry);
    JSObject* base = objectArgument(object, entry);
    if (tryDefineInEmptySlot(vm, state, base, classToken, field, JSValue::decode(value))) {
        ensureStillAliveHere(classToken);
        OPERATION_RETURN(scope);
    }
    PrivateLocation location = locate(vm, state, base, classToken, field);
    if (location.kind != PrivateLocation::Kind::Absent) {
        throwException(globalObject, scope, createPresentNameError(globalObject, location));
        OPERATION_RETURN(scope);
    }
    const PrivateFieldName& name = location.declaredName;
    JSValue defined = JSValue::decode(value);
    if (name.declared->isBrand() && defined != JSValue(classToken)) [[unlikely]]
        crashOnWrongOperand(entry);
    bool canUseDeclaredName = !location.underDeclaredName.value && (name.declared->isBrand() || location.belongsToEvaluation);
    UniquedStringImpl* uid = canUseDeclaredName ? name.uid : &ClassToken::privateNameOf(vm, state, classToken, field)->uid();
    definePrivateProperty(globalObject, base, location, uid, defined);
    if (!location.compiledObject)
        ClassToken::makeRoomForKnownStructures(vm, state, classToken);
    ensureStillAliveHere(classToken);
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
