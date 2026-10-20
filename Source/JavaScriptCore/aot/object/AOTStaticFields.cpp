/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStaticFields.h"

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTImmortalCells.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramStatics.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "CustomGetterSetter.h"
#include "FunctionRareData.h"
#include "JSCInlines.h"
#include <bit>
#include <ranges>
#include <span>

namespace JSC { namespace AOT { namespace StaticFields {

static JSC_DECLARE_CUSTOM_GETTER(staticFieldGetter);
static JSC_DECLARE_CUSTOM_SETTER(staticFieldSetter);

bool ownsVariables(const Closure& classObject)
{
    return classObject.kind().isConstructor && classObject.state().objects->closures.closureIfCreated(classObject.function()) == &classObject;
}

const FieldData* findOwnField(const Closure& classObject, UniquedStringImpl* uid)
{
    if (PropertyName(uid).isPrivateName())
        return nullptr;
    const LayoutData* layout = classObject.classData();
    if (!layout || layout->staticFields().empty() || !ownsVariables(classObject))
        return nullptr;
    const FieldData* field = classObject.state().objects->classes.staticField(*layout, uid);
    if (!field)
        return nullptr;
    std::span<const FieldData> ownFields = layout->staticFields();
    bool isOwn = std::bit_cast<uintptr_t>(field) - std::bit_cast<uintptr_t>(ownFields.data()) < ownFields.size_bytes();
    return isOwn ? field : nullptr;
}

bool isInVariable(const Closure& classObject, const FieldData& field)
{
    ProgramStatics& statics = *classObject.state().statics;
    statics.ensureProgramStaticsInitialized();
    return !slotHoldsAbsent(field.slot.representation(), &statics.at<uint8_t>(field.slot.offset));
}

bool canEnterVariable(VM& vm, const Closure& classObject, const FieldData& field)
{
    ASSERT(!isInVariable(classObject, field));
    State& state = classObject.state();
    const LayoutData& layout = *classObject.classData();
    UniquedStringImpl* uid = state.objects->layouts.nameOf(vm, field);
    Structure* structure = classObject.structure();
    if (!structure->isStructureExtensible() || isValidOffset(structure->get(vm, uid)) || state.objects->classes.hasStaticMethodNamed(layout, uid))
        return false;
    bool isDeclaredLater = false;
    for (const FieldData& other : layout.staticFields()) {
        if (isDeclaredLater && isInVariable(classObject, other))
            return false;
        isDeclaredLater |= &other == &field;
    }
    return true;
}

static CustomGetterSetter* accessor(VM& vm, State& state)
{
    CustomGetterSetter*& created = state.objects->caches->staticFieldAccessor;
    if (!created) {
        created = CustomGetterSetter::create(vm, staticFieldGetter, staticFieldSetter);
        state.immortalCells->add(created);
    }
    return created;
}

// A field enters its variable only while no later field is in its own, so the last one in its variable answers for all before it.
static bool hasEveryProperty(VM& vm, const Closure& classObject, std::span<const FieldData> fields)
{
    RuntimeLayoutTable& layouts = classObject.state().objects->layouts;
    for (const FieldData& field : fields | std::views::reverse) {
        if (!isInVariable(classObject, field))
            continue;
        UniquedStringImpl* uid = layouts.nameOf(vm, field);
        if (PropertyName(uid).isPrivateName())
            continue;
        unsigned attributes;
        return isValidOffset(classObject.structure()->get(vm, uid, attributes)) && (attributes & PropertyAttribute::CustomValue);
    }
    return true;
}

static void setHasReifiedProperty(VM& vm, Closure& classObject, UniquedStringImpl* uid)
{
    const CommonIdentifiers& names = *vm.propertyNames;
    if (uid == names.name.impl()) {
        FunctionRareData* rareData = classObject.ensureRareData(vm);
        rareData->setHasReifiedName();
        rareData->setHasModifiedNameForBoundOrNonHostFunction();
    } else if (uid == names.length.impl()) {
        FunctionRareData* rareData = classObject.ensureRareData(vm);
        rareData->setHasReifiedLength();
        rareData->setHasModifiedLengthForBoundOrNonHostFunction();
    }
}

void ensureProperties(VM& vm, Closure& classObject)
{
    const LayoutData* layout = classObject.classData();
    if (!layout || layout->staticFields().empty() || !ownsVariables(classObject))
        return;
    if (hasEveryProperty(vm, classObject, layout->staticFields()))
        return;
    classObject.ensureClassObject(vm);
    State& state = classObject.state();
    for (const FieldData& field : layout->staticFields()) {
        UniquedStringImpl* uid = state.objects->layouts.nameOf(vm, field);
        if (PropertyName(uid).isPrivateName() || !isInVariable(classObject, field) || isValidOffset(classObject.structure()->get(vm, uid)))
            continue;
        classObject.putDirectCustomAccessor(vm, uid, accessor(vm, state), static_cast<unsigned>(PropertyAttribute::CustomValue));
        setHasReifiedProperty(vm, classObject, uid);
    }
}

void moveToProperty(VM& vm, Closure& classObject, const FieldData& field)
{
    RELEASE_ASSERT(field.canLeaveSlot());
    State& state = classObject.state();
    ClassTable& classes = state.objects->classes;
    UniquedStringImpl* uid = state.objects->layouts.nameOf(vm, field);
    JSValue value = classes.readStaticField(field);
    ASSERT(value);
    ensureProperties(vm, classObject);
    classObject.putDirect(vm, uid, value, 0);
    ProgramStatics::writeVariable(state.instance, field.slot, JSValue());
    classes.didChangeMember(*classObject.classData(), ClassSide::Static, uid);
}

void copyToProperties(VM& vm, Closure& classObject)
{
    const LayoutData* layout = classObject.classData();
    if (!layout || layout->staticFields().empty() || !ownsVariables(classObject))
        return;
    ensureProperties(vm, classObject);
    State& state = classObject.state();
    for (const FieldData& field : layout->staticFields()) {
        UniquedStringImpl* uid = state.objects->layouts.nameOf(vm, field);
        if (!PropertyName(uid).isPrivateName() && isInVariable(classObject, field))
            classObject.putDirect(vm, uid, state.objects->classes.readStaticField(field), 0);
    }
}

void didDeleteProperty(VM& vm, Closure& classObject, const FieldData& field)
{
    RELEASE_ASSERT(field.canLeaveSlot());
    State& state = classObject.state();
    ProgramStatics::writeVariable(state.instance, field.slot, JSValue());
    state.objects->classes.didChangeMember(*classObject.classData(), ClassSide::Static, state.objects->layouts.nameOf(vm, field));
}

const FieldData* findFieldForCache(VM& vm, JSCell* cell, UniquedStringImpl* uid)
{
    if (!isCompiledClosure(cell))
        return nullptr;
    Closure& classObject = *uncheckedDowncast<Closure>(cell);
    const FieldData* field = findOwnField(classObject, uid);
    if (!field || !isInVariable(classObject, *field))
        return nullptr;
    ensureProperties(vm, classObject);
    return field;
}

JSC_DEFINE_CUSTOM_GETTER(staticFieldGetter, (JSGlobalObject*, EncodedJSValue thisValue, PropertyName name))
{
    Closure& classObject = *uncheckedDowncast<Closure>(JSValue::decode(thisValue).asCell());
    const FieldData* field = findOwnField(classObject, name.uid());
    JSValue value = field ? classObject.state().objects->classes.readStaticField(*field) : JSValue();
    return JSValue::encode(value ? value : jsUndefined());
}

JSC_DEFINE_CUSTOM_SETTER(staticFieldSetter, (JSGlobalObject* globalObject, EncodedJSValue thisValue, EncodedJSValue encodedValue, PropertyName name))
{
    VM& vm = globalObject->vm();
    Closure& classObject = *uncheckedDowncast<Closure>(JSValue::decode(thisValue).asCell());
    JSValue value = JSValue::decode(encodedValue);
    const FieldData* field = findOwnField(classObject, name.uid());
    RELEASE_ASSERT(field);
    if (!classObject.state().objects->classes.tryStoreStaticField(vm, *field, value)) {
        moveToProperty(vm, classObject, *field);
        classObject.putDirect(vm, name, value);
    }
    return true;
}

} } } // namespace JSC::AOT::StaticFields

#endif // ENABLE(AOT)
