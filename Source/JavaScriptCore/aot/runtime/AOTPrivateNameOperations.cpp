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
#include <algorithm>

namespace JSC { namespace AOT {

struct PrivateFieldName {
    const FieldData* declared { nullptr };
    UniquedStringImpl* uid { nullptr };
    UniquedStringImpl* brandUID { nullptr };
};

struct PrivateProperty {
    const FieldData* field { nullptr };
    JSValue value;
    bool isInSlot { false };
};

struct PrivateLocation {
    enum class Kind : uint8_t { Absent, Slot, Property };
    Kind kind { Kind::Absent };
    Object* compiledObject { nullptr };
    const FieldData* field { nullptr };
    UniquedStringImpl* name { nullptr };
    PrivateFieldName declaredName;
    PrivateProperty underDeclaredName;
    bool belongsToEvaluation { false };
};

static bool hasFieldNamed(const LayoutData& layout, uint32_t name)
{
    std::span<const FieldData> fields = layout.fields();
    return std::ranges::find(fields, name, &FieldData::name) != fields.end();
}

static const FieldData* findBrand(const ClassTable& classes, const LayoutData& declaring)
{
    const LayoutData* base = classes.baseOf(declaring);
    for (const FieldData& field : declaring.fields()) {
        if (field.isBrand() && !(base && hasFieldNamed(*base, field.name)))
            return &field;
    }
    return nullptr;
}

static PrivateFieldName resolveFieldName(VM& vm, State& state, JSCell* token, uint32_t fieldIndex)
{
    const LayoutData& declaring = ClassToken::classLayoutOf(state, token);
    RELEASE_ASSERT(fieldIndex < declaring.numberOfFields);
    const FieldData& declared = declaring.fields()[fieldIndex];
    RELEASE_ASSERT(declared.isPrivate());
    RuntimeLayoutTable& layouts = state.objects->layouts;
    const FieldData* brand = findBrand(state.objects->classes, declaring);
    return { &declared, layouts.nameOf(vm, declared), brand ? layouts.nameOf(vm, *brand) : nullptr };
}

static PrivateProperty findPrivateProperty(VM& vm, State& state, JSObject* object, Object* compiledObject, UniquedStringImpl* uid)
{
    if (!compiledObject)
        return { nullptr, object->getDirect(vm, uid), false };
    if (const FieldData* field = compiledObject->fieldNamed(state, uid)) {
        FieldProperty property = findFieldProperty(vm, *compiledObject, *field);
        return { field, property.valueOrAccessor, property.isInSlot };
    }
    JSObject* extra = compiledObject->extraProperties();
    return { nullptr, extra ? extra->getDirect(vm, uid) : JSValue(), false };
}

static PrivateLocation locate(VM& vm, State& state, JSObject* object, JSCell* token, uint32_t fieldIndex)
{
    PrivateLocation location;
    location.compiledObject = compiledObjectOrNull(object);
    location.declaredName = resolveFieldName(vm, state, token, fieldIndex);
    const PrivateFieldName& name = location.declaredName;
    auto find = [&](UniquedStringImpl* uid) {
        return findPrivateProperty(vm, state, object, location.compiledObject, uid);
    };

    location.underDeclaredName = find(name.uid);
    const PrivateProperty& declared = location.underDeclaredName;
    JSValue brand = name.brandUID == name.uid ? declared.value : name.brandUID ? find(name.brandUID).value : JSValue();
    location.belongsToEvaluation = !name.brandUID || brand == JSValue(token);
    if (location.belongsToEvaluation && declared.value) {
        location.kind = declared.isInSlot ? PrivateLocation::Kind::Slot : PrivateLocation::Kind::Property;
        location.field = declared.isInSlot ? declared.field : nullptr;
        location.name = name.uid;
        return location;
    }
    if (Symbol* evaluationName = ClassToken::privateNameIfCreated(token, fieldIndex)) {
        if (find(&evaluationName->uid()).value) {
            location.kind = PrivateLocation::Kind::Property;
            location.name = &evaluationName->uid();
        }
    }
    return location;
}

static JSObject* createAbsentNameError(JSGlobalObject* globalObject, State& state, JSCell* token, uint32_t fieldIndex)
{
    if (ClassToken::classLayoutOf(state, token).fields()[fieldIndex].isBrand())
        return createPrivateMethodAccessError(globalObject);
    return createInvalidPrivateNameError(globalObject);
}

static void setPrivateProperty(JSGlobalObject* globalObject, JSObject* object, UniquedStringImpl* uid, JSValue value)
{
    if (Object* compiledObject = compiledObjectOrNull(object)) {
        compiledObject->putDirectFromNativeCode(globalObject->vm(), uid, value, static_cast<unsigned>(PropertyAttribute::None));
        return;
    }
    PutPropertySlot slot(object, true);
    object->setPrivateField(globalObject, uid, value, slot);
}

static void definePrivateProperty(JSGlobalObject* globalObject, JSObject* object, const PrivateLocation& absent, UniquedStringImpl* uid, JSValue value)
{
    Object* compiledObject = absent.compiledObject;
    if (!compiledObject) {
        PutPropertySlot slot(object, true);
        object->definePrivateField(globalObject, uid, value, slot);
        return;
    }
    VM& vm = globalObject->vm();
    const FieldData* own = uid == absent.declaredName.uid ? absent.underDeclaredName.field : nullptr;
    if (own && tryCreateField(vm, compiledObject->state(), *compiledObject, *own, value))
        return;
    compiledObject->putDirectFromNativeCode(vm, uid, value, static_cast<unsigned>(PropertyAttribute::None));
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
    State& state = *instance->state;
    JSCell* classToken = cellArgument<JSCellButterfly>(token, entry);
    JSObject* base = JSValue::decode(object).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    PrivateLocation location = locate(vm, state, base, classToken, field);
    switch (location.kind) {
    case PrivateLocation::Kind::Absent:
        throwException(globalObject, scope, createAbsentNameError(globalObject, state, classToken, field));
        OPERATION_RETURN(scope, encodedJSValue());
    case PrivateLocation::Kind::Slot:
        OPERATION_RETURN(scope, JSValue::encode(location.compiledObject->read(*location.field)));
    case PrivateLocation::Kind::Property:
        OPERATION_RETURN(scope, JSValue::encode(findPrivateProperty(vm, state, base, location.compiledObject, location.name).value));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTPrivateHas, EncodedJSValue, (Instance* instance, EncodedJSValue object, JSCell* token, uint32_t field))
{
    constexpr Entry entry = Entry::operationAOTPrivateHas;
    AOT_OPERATION_BEGIN(instance, entry);
    JSCell* classToken = cellArgument<JSCellButterfly>(token, entry);
    JSValue base = JSValue::decode(object);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    PrivateLocation location = locate(vm, *instance->state, asObject(base), classToken, field);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(location.kind != PrivateLocation::Kind::Absent)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPrivateSet, void, (Instance* instance, EncodedJSValue object, JSCell* token, uint32_t field, EncodedJSValue value))
{
    constexpr Entry entry = Entry::operationAOTPrivateSet;
    AOT_OPERATION_BEGIN(instance, entry);
    State& state = *instance->state;
    JSCell* classToken = cellArgument<JSCellButterfly>(token, entry);
    JSObject* base = JSValue::decode(object).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    JSValue stored = JSValue::decode(value);
    PrivateLocation location = locate(vm, state, base, classToken, field);
    switch (location.kind) {
    case PrivateLocation::Kind::Absent:
        throwException(globalObject, scope, createAbsentNameError(globalObject, state, classToken, field));
        OPERATION_RETURN(scope);
    case PrivateLocation::Kind::Slot:
        if (!location.compiledObject->tryStore(vm, *location.field, stored))
            (void)location.compiledObject->moveFieldToExtraProperties(vm, *location.field, { stored, static_cast<unsigned>(PropertyAttribute::None) }, SlotExitReason::StoreOfOtherType);
        OPERATION_RETURN(scope);
    case PrivateLocation::Kind::Property:
        setPrivateProperty(globalObject, base, location.name, stored);
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
    PrivateLocation location = locate(vm, state, base, classToken, field);
    if (location.kind != PrivateLocation::Kind::Absent) {
        throwException(globalObject, scope, createRedefinedPrivateNameError(globalObject));
        OPERATION_RETURN(scope);
    }
    const PrivateFieldName& name = location.declaredName;
    JSValue defined = JSValue::decode(value);
    if (name.declared->isBrand() && defined != JSValue(classToken)) [[unlikely]]
        crashOnWrongOperand(entry);
    bool canUseDeclaredName = !location.underDeclaredName.value && (name.declared->isBrand() || location.belongsToEvaluation);
    UniquedStringImpl* uid = canUseDeclaredName ? name.uid : &ClassToken::privateNameOf(vm, state, classToken, field)->uid();
    definePrivateProperty(globalObject, base, location, uid, defined);
    ensureStillAliveHere(classToken);
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
