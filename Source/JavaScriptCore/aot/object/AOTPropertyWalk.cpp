/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyWalk.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTPropertyListCache.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "DeferGC.h"
#include "JSCInlines.h"
#include "TopExceptionScope.h"
#include <optional>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

JSValue Object::tryGetOwnDataProperty(VM& vm, JSCell* cell, PropertyName name)
{
    if (!isCompiledObject(cell))
        return { };
    Object* object = uncheckedDowncast<Object>(cell);
    if (const FieldData* field = object->fieldNamed(object->state(), name)) {
        FieldProperty property = findFieldProperty(vm, *object, *field);
        return property.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue ? JSValue() : property.valueOrAccessor;
    }
    object->state().objects->layouts.didReadAbsentField(vm, object->layout(), name.uid());
    JSObject* extra = object->extraProperties();
    if (!extra)
        return { };
    unsigned attributes;
    JSValue value = extra->getDirect(vm, name, attributes);
    return value && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue) ? value : JSValue();
}

bool Object::structureGuaranteesAbsence(Structure* structure, UniquedStringImpl* name)
{
    const CompiledStructure* compiled = compiledObjectStructureOrNull(structure);
    if (!compiled)
        return false;
    if (parseIndex(*name) || PropertyName(name).isPrivateName())
        return false;
    State& state = *Instance::of(structure->realm())->state;
    if (state.objects->layouts.field(structure->vm(), compiled->layout(), name))
        return false;
    if (!compiled->hasExtraProperties())
        return true;
    if (!compiled->listsEveryExtraProperty())
        return false;
    unsigned attributes;
    std::optional<PropertyOffset> offset = compiled->extraPropertiesStructure()->getWithoutMaterializing(name, attributes);
    return offset && !isValidOffset(*offset);
}

namespace PropertyWalk {

using Visitor = ScopedLambda<IterationStatus(const OwnProperty&)>;

struct OrderedKey {
    UniquedStringImpl* name;
    const FieldData* field;
    PropertyOffset offsetInExtraProperties;
    unsigned attributes;
};

static bool extraPropertiesNeedGenericPath(JSObject* extra)
{
    Structure* structure = extra->structure();
    return extra->type() != FinalObjectType
        || JSC::hasIndexedProperties(extra->indexingType())
        || structure->hasAnyKindOfGetterSetterProperties()
        || structure->isUncacheableDictionary()
        || extra->hasNonReifiedStaticProperties();
}

static Result visitFields(const Object& typed, Filter filter, const Visitor& visitor)
{
    unsigned attributes = typed.attributesOfPublicFields();
    bool isStopped = false;
    auto visitKeys = [&](Object::SymbolKeys symbolKeys) {
        typed.forEachFieldAsProperty([&](UniquedStringImpl& name, JSValue value) {
            if (symbolKeys == Object::SymbolKeys::Include && !name.isSymbol())
                return true;
            isStopped = visitor(OwnProperty { &name, value, attributes }) == IterationStatus::Done;
            return !isStopped;
        }, symbolKeys);
    };
    visitKeys(Object::SymbolKeys::Skip);
    if (!isStopped && filter == Filter::StringsAndSymbols && typed.layout().hasFieldsWithSymbolKeys)
        visitKeys(Object::SymbolKeys::Include);
    return isStopped ? Result::Stopped : Result::Completed;
}

static std::optional<Result> tryVisitCachedList(VM& vm, const Object& typed, JSObject* extra, const Visitor& visitor)
{
    Structure* structure = extra->structure();
    if (structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || structure->hasNonConfigurableProperties())
        return std::nullopt;
    Vector<PropertyListEntry, 16> entries;
    {
        DeferGC deferGC(vm);
        CachedPropertyList* listing = cachedPropertyList(vm, typed);
        if (!listing)
            return std::nullopt;
        entries.append(listing->properties.span());
    }
    unsigned attributes = typed.attributesOfPublicFields();
    for (auto& entry : entries) {
        JSValue value = typed.readPropertyListEntry(entry, extra);
        if (value && visitor(OwnProperty { entry.name, value, entry.field ? attributes : 0 }) == IterationStatus::Done)
            return Result::Stopped;
    }
    return Result::Completed;
}

static Vector<OrderedKey, 32> collectKeysInOrder(VM& vm, const Object& typed, JSObject* extra)
{
    validateKeyListing(vm, typed);
    State& state = typed.state();
    Structure* extraStructure = extra->structure();
    Vector<OrderedKey, 16> positioned;
    Vector<OrderedKey, 16> unpositioned;
    extraStructure->forEachProperty(vm, [&](const auto& entry) {
        UniquedStringImpl* name = entry.key();
        if (!PropertyName(name).isPrivateName() && !typed.fieldNamed(state, name))
            (typed.hasPositionAmongKeys(name) ? positioned : unpositioned).append(OrderedKey { name, nullptr, entry.offset(), entry.attributes() });
        return true;
    });

    const LayoutData& layout = typed.layout();
    std::span fields = layout.fields();
    unsigned attributes = typed.attributesOfPublicFields();
    Object::KeyOrderStorage storage;
    Vector<OrderedKey, 32> ordered;
    size_t nextPositioned = Object::forEachPositionedKey(typed.keyOrder(state, storage), layout.numberOfFields, positioned.size(), [&](uint16_t index) {
        const FieldData& field = fields[index];
        if (!state.objects->layouts.hasStringKey(field) && !state.objects->layouts.hasSymbolKey(field))
            return;
        UniquedStringImpl* name = state.objects->layouts.nameOf(vm, field);
        if (typed.read(field)) {
            ordered.append(OrderedKey { name, &field, invalidOffset, attributes });
            return;
        }
        unsigned extraAttributes;
        PropertyOffset offset = extraStructure->get(vm, name, extraAttributes);
        if (isValidOffset(offset))
            ordered.append(OrderedKey { name, nullptr, offset, extraAttributes });
    }, [&](size_t ordinal) {
        ordered.append(positioned[ordinal]);
    });
    ordered.append(positioned.span().subspan(nextPositioned));
    ordered.appendVector(unpositioned);
    return ordered;
}

static Result visitKeysInOrder(VM& vm, const Object& typed, JSObject* extra, Filter filter, const Visitor& visitor)
{
    auto ordered = collectKeysInOrder(vm, typed, extra);
    auto visitKeys = [&](bool visitsSymbols) {
        for (auto& key : ordered) {
            if (key.name->isSymbol() != visitsSymbols || (filter == Filter::EnumerableStrings && key.attributes & PropertyAttribute::DontEnum))
                continue;
            JSValue value = key.field ? typed.read(*key.field) : extra->getDirect(key.offsetInExtraProperties);
            if (value && visitor(OwnProperty { key.name, value, key.attributes }) == IterationStatus::Done)
                return false;
        }
        return true;
    };
    if (!visitKeys(false) || (filter == Filter::StringsAndSymbols && !visitKeys(true)))
        return Result::Stopped;
    return Result::Completed;
}

static Presence compiledObjectHasProperty(const Object& typed, UniquedStringImpl* name)
{
    if (const FieldData* field = typed.fieldNamed(typed.state(), name)) {
        if (field->slot.representation() == SlotRepresentation::DeferredClosure || typed.readInline(*field) || typed.read(*field))
            return Presence::Present;
    } else
        typed.state().objects->layouts.didReadAbsentField(typed.vm(), typed.layout(), name);
    JSObject* extra = typed.extraProperties();
    if (!extra)
        return Presence::Absent;
    if (parseIndex(*name) || extra->type() != FinalObjectType)
        return Presence::Unknown;
    return isValidOffset(extra->structure()->get(typed.vm(), name)) ? Presence::Present : Presence::Absent;
}

static Presence classPrototypeHasProperty(JSObject* classPrototype, UniquedStringImpl* name)
{
    if (ClassPrototype::hasMember(classPrototype, name))
        return Presence::Present;
    return classPrototype->butterfly() ? Presence::Unknown : Presence::Absent;
}

static bool isLazyFunctionProperty(VM& vm, UniquedStringImpl* name)
{
    const CommonIdentifiers& names = *vm.propertyNames;
    return name == names.prototype || name == names.length || name == names.name || name == names.hasInstanceSymbol;
}

static bool declaresStaticMethod(const Closure& closure, UniquedStringImpl* name)
{
    const LayoutData* classLayout = closure.classData();
    return classLayout && closure.isLazyClassObject() && closure.state().objects->classes.hasStaticMethodNamed(*classLayout, name);
}

static Presence functionHasProperty(JSObject* function, UniquedStringImpl* name)
{
    VM& vm = function->vm();
    if (isLazyFunctionProperty(vm, name))
        return Presence::Unknown;
    if (function->type() == SlimClosureType)
        return Presence::Absent;
    const Closure* closure = isCompiledClosure(function) ? uncheckedDowncast<Closure>(function) : nullptr;
    if (closure) {
        if (const FieldData* field = StaticFields::findOwnField(*closure, name); field && StaticFields::isInVariable(*closure, *field))
            return Presence::Present;
    }
    if (isValidOffset(function->structure()->get(vm, name)))
        return Presence::Present;
    if (closure)
        return declaresStaticMethod(*closure, name) ? Presence::Present : Presence::Absent;
    return function->methodTable()->getOwnPropertySlot == JSFunction::info()->methodTable.getOwnPropertySlot ? Presence::Absent : Presence::Unknown;
}

static Object::Integrity objectIntegrity(Integrity level)
{
    switch (level) {
    case Integrity::NonExtensible:
        return Object::Integrity::NotExtensible;
    case Integrity::Sealed:
        return Object::Integrity::Sealed;
    case Integrity::Frozen:
        return Object::Integrity::Frozen;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool convertsToOrdinaryObject(const ClassInfo* from, const ClassInfo* to)
{
    return ((from == ClassPrototype::info() || from == Object::info()) && to == JSFinalObject::info()) || (from == SlimClosure::info() && to == ReifiedSlimClosure::info());
}

Result forEachOwnProperty(VM& vm, JSObject* object, Filter filter, const Visitor& visitor)
{
    Object* typed = compiledObjectOrNull(object);
    if (!typed)
        return Result::NeedsGenericPath;
    JSObject* extra = typed->extraProperties();
    if (!extra)
        return visitFields(*typed, filter, visitor);
    if (extraPropertiesNeedGenericPath(extra))
        return Result::NeedsGenericPath;
    if (auto result = tryVisitCachedList(vm, *typed, extra, visitor))
        return *result;
    return visitKeysInOrder(vm, *typed, extra, filter, visitor);
}

void materializeLazyProperties(VM& vm, JSObject* object)
{
    if (object->classInfo() == ClassPrototype::info()) {
        ClassPrototype::ensureMembers(object);
        return;
    }
    if (object->type() == SlimClosureType) {
        CompiledCallee::reifyProperties(vm, object);
        return;
    }
    if (!isCompiledClosure(object))
        return;
    Closure* closure = uncheckedDowncast<Closure>(object);
    if (!closure->classData())
        return;
    closure->ensureClassObject(vm);
    StaticFields::ensureProperties(vm, *closure);
}

JSCellButterfly* tryGetCachedOwnKeys(VM& vm, JSObject* object, Filter filter)
{
    Object* typed = compiledObjectOrNull(object);
    if (!typed)
        return nullptr;
    if (!typed->hasExtraProperties()) {
        if (filter == Filter::StringsAndSymbols && typed->layout().hasFieldsWithSymbolKeys)
            return nullptr;
        return typed->keys(vm);
    }
    typed->ensureStructureForExtraProperties(vm);
    if (!typed->keysDependOnlyOnStructureAndOrderWord())
        return nullptr;
    return typed->keysWithExtraProperties(vm);
}

Presence hasOwnProperty(JSObject* object, UniquedStringImpl* name)
{
    if (PropertyName(name).isPrivateName())
        return Presence::Unknown;
    if (Object* typed = compiledObjectOrNull(object))
        return compiledObjectHasProperty(*typed, name);
    if (object->classInfo() == ClassPrototype::info())
        return classPrototypeHasProperty(object, name);
    if (isJSFunctionType(object->type()) || object->type() == SlimClosureType)
        return functionHasProperty(object, name);
    return Presence::Unknown;
}

Presence hasPropertyOnChain(Structure* structure, UniquedStringImpl* name)
{
    return presenceForStructure(structure->vm(), structure, name);
}

JSValue getDirect(VM& vm, const JSObject* object, PropertyName name, unsigned* attributesOrNull)
{
    if (!isCompiledObject(object))
        return { };
    return uncheckedDowncast<Object>(const_cast<JSObject*>(object))->getDirectFromNativeCode(vm, name, attributesOrNull);
}

void putDirect(VM& vm, JSObject* object, PropertyName name, JSValue value, unsigned attributes)
{
    ASSERT(isCompiledObject(object));
    uncheckedDowncast<Object>(object)->putDirectFromNativeCode(vm, name, value, attributes);
}

void convertToOrdinaryObject(VM& vm, JSObject* object)
{
    if (CompiledCallee::reifyProperties(vm, object))
        return;
    RELEASE_ASSERT(isCompiledObject(object));
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    uncheckedDowncast<Object>(object)->convertToOrdinaryObject(vm, ShapeLossReason::NativeDefinition);
    scope.assertNoException();
}

void setIntegrity(VM& vm, JSObject* object, Integrity level)
{
    ASSERT(isCompiledObject(object));
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    uncheckedDowncast<Object>(object)->setIntegrity(object->structure()->realm(), objectIntegrity(level));
    scope.assertNoException();
}

Presence testIntegrity(JSObject* object, Integrity level)
{
    Object* typed = compiledObjectOrNull(object);
    if (!typed)
        return Presence::Unknown;
    return typed->testIntegrity(typed->vm(), objectIntegrity(level)) ? Presence::Present : Presence::Absent;
}

bool tryAssign(VM& vm, JSObject* compiledTarget, JSObject* source)
{
    Object* typed = compiledObjectOrNull(compiledTarget);
    return typed && typed->tryAssignPropertiesOf(vm, source);
}

void didBecomePrototype(VM& vm, JSObject* object)
{
    Object* typed = uncheckedDowncast<Object>(object);
    if (typed->structure()->mayBePrototype())
        return;
    validateMutation(vm, *typed, ObjectMutation::BecomePrototype);
    typed->JSCell::setStructure(vm, typed->state().objects->structures.asPrototype(vm, typed->compiledStructure()));
}

bool hasIndexedProperties(JSObject* object)
{
    JSObject* holder = objectHoldingIndexedProperties(object);
    return holder && JSC::hasIndexedProperties(holder->indexingType());
}

JSObject* objectHoldingIndexedProperties(const JSObject* object)
{
    ASSERT(isCompiledObject(object));
    return uncheckedDowncast<Object>(object)->extraProperties();
}

} // namespace PropertyWalk

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
