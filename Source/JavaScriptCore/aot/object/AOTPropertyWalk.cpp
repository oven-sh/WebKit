/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyWalk.h"

#if ENABLE(AOT)

#include "AOTAssignmentPlans.h"
#include "AOTChainDependents.h"
#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyListCache.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "AOTVector.h"
#include "DeferGC.h"
#include "DeferTermination.h"
#include "JSCInlines.h"
#include "TopExceptionScope.h"
#include <algorithm>
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

static bool extraPropertiesAreWalkedDirectly(JSObject* extra, Elements elements)
{
    Structure* structure = extra->structure();
    return extra->type() == FinalObjectType
        && (elements == Elements::AreLeftOut ? !extra->canHaveExistingOwnIndexedGetterSetterProperties() : !JSC::hasIndexedProperties(extra->indexingType()))
        && !structure->hasAnyKindOfGetterSetterProperties()
        && !extra->hasNonReifiedStaticProperties();
}

static bool extraPropertiesNeedGenericPath(JSObject* extra, Elements elements = Elements::NeedGenericPath)
{
    return !extraPropertiesAreWalkedDirectly(extra, elements) || extra->structure()->isUncacheableDictionary();
}

// A walk hands out values, a store that asks nobody writes them, and neither runs code. So they are not for an object that is asked to prepare itself first.
static bool hasFieldPreparedBeforeLookups(VM& vm, const Object& typed)
{
    const LayoutData& layout = typed.layout();
    if (!layout.hasFieldsWithSymbolKeys)
        return false;
    RuntimeLayoutTable& layouts = typed.state().objects->layouts;
    return std::ranges::any_of(layout.fields(), [&](const FieldData& field) {
        return layouts.hasSymbolKey(field) && typed.isInSlot(field) && isPreparedBeforeLookups(vm, layouts.nameOf(vm, field));
    });
}

static Result visitFields(const Object& typed, Filter filter, const Visitor& visitor)
{
    unsigned attributes = typed.attributesOfPublicFields();
    bool isStopped = false;
    auto visitKeys = [&](Object::SymbolKeys symbolKeys) {
        return typed.forEachFieldAsProperty([&](UniquedStringImpl& name, JSValue value) {
            if (symbolKeys == Object::SymbolKeys::Include && !name.isSymbol())
                return true;
            isStopped = visitor(OwnProperty { &name, value, attributes }) == IterationStatus::Done;
            return !isStopped;
        }, symbolKeys);
    };
    if (!visitKeys(Object::SymbolKeys::Skip))
        return Result::NeedsGenericPath;
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

const CachedPropertyList* tryGetListOfOwnProperties(VM& vm, JSObject* object, JSObject*& extra)
{
    Object* typed = compiledObjectOrNull(object);
    extra = typed ? typed->extraProperties() : nullptr;
    if (!extra)
        return nullptr;
    if (CachedPropertyList* found = findCachedPropertyList(vm, *typed, extra); found && found->holdsOnlyPlainData) [[likely]]
        return found;
    if (extraPropertiesNeedGenericPath(extra))
        return nullptr;
    Structure* structure = extra->structure();
    if (structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || structure->hasNonConfigurableProperties())
        return nullptr;
    ASSERT(vm.heap.isDeferred());
    CachedPropertyList* list = cachedPropertyList(vm, *typed);
    if (list)
        list->holdsOnlyPlainData = true;
    return list;
}

static Vector<OrderedKey, 32> collectKeysInOrder(VM& vm, const Object& typed, JSObject* extra)
{
    validateKeyListing(vm, typed);
    State& state = typed.state();
    Structure* extraStructure = extra->structure();
    std::span fields = typed.layout().fields();
    Vector<OrderedKey, 32> ordered;
    forEachKeyAsCreated(vm, state, typed, [&](uint16_t index) {
        const FieldData& field = fields[index];
        if (!state.objects->layouts.hasStringKey(field) && !state.objects->layouts.hasSymbolKey(field))
            return;
        UniquedStringImpl* name = state.objects->layouts.nameOf(vm, field);
        if (typed.read(field)) {
            ordered.append(OrderedKey { name, &field, invalidOffset, typed.attributesOf(field) });
            return;
        }
        unsigned extraAttributes;
        PropertyOffset offset = extraStructure->get(vm, name, extraAttributes);
        if (isValidOffset(offset))
            ordered.append(OrderedKey { name, nullptr, offset, extraAttributes });
    }, [&](const auto& entry) {
        ordered.append(OrderedKey { entry.key(), nullptr, entry.offset(), entry.attributes() });
    });
    return ordered;
}

static Result visitKeysInOrder(VM& vm, const Object& typed, JSObject* extra, std::optional<Filter> filter, const Visitor& visitor)
{
    auto ordered = collectKeysInOrder(vm, typed, extra);
    if (std::ranges::any_of(ordered, [](const OrderedKey& key) { return key.attributes & PropertyAttribute::Accessor; }))
        return Result::NeedsGenericPath;
    auto visitKeys = [&](std::optional<bool> visitsSymbols) {
        for (auto& key : ordered) {
            if ((visitsSymbols && key.name->isSymbol() != *visitsSymbols) || (filter == Filter::EnumerableStrings && key.attributes & PropertyAttribute::DontEnum))
                continue;
            JSValue value = key.field ? typed.read(*key.field) : extra->getDirect(key.offsetInExtraProperties);
            if (value && visitor(OwnProperty { key.name, value, key.attributes }) == IterationStatus::Done)
                return false;
        }
        return true;
    };
    bool isCompleted = filter ? visitKeys(false) && (filter != Filter::StringsAndSymbols || visitKeys(true)) : visitKeys(std::nullopt);
    return isCompleted ? Result::Completed : Result::Stopped;
}

static Presence compiledObjectHasProperty(const Object& typed, UniquedStringImpl* name)
{
    if (const FieldData* field = typed.fieldNamed(typed.state(), name)) {
        bool defersClosure = field->slot.representation() == SlotRepresentation::DeferredClosure;
        if (defersClosure ? typed.isInSlot(*field) : typed.readInline(*field) || typed.read(*field))
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
    return (from == ClassPrototype::info() && to == JSFinalObject::info()) || (from == SlimClosure::info() && to == ReifiedSlimClosure::info());
}

Result forEachOwnProperty(VM& vm, JSObject* object, Filter filter, const Visitor& visitor)
{
    Object* typed = compiledObjectOrNull(object);
    if (!typed || (filter == Filter::StringsAndSymbols && hasFieldPreparedBeforeLookups(vm, *typed)))
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

Result forEachOwnPropertyAsCreated(VM& vm, JSObject* object, const Visitor& visitor, Elements elements)
{
    Object* typed = compiledObjectOrNull(object);
    if (!typed || hasFieldPreparedBeforeLookups(vm, *typed))
        return Result::NeedsGenericPath;
    bool isStopped = false;
    JSObject* extra = typed->extraProperties();
    // A second object that holds nothing but elements has no name for a walk that leaves them out.
    if (extra && elements == Elements::AreLeftOut && extra->structure() == typed->instance().realm.extraPropertiesWithElementsStructure)
        extra = nullptr;
    if (!extra) {
        unsigned attributes = typed->attributesOfPublicFields();
        bool isListed = typed->forEachFieldAsProperty([&](UniquedStringImpl& name, JSValue value) {
            isStopped = visitor(OwnProperty { &name, value, attributes }) == IterationStatus::Done;
            return !isStopped;
        }, Object::SymbolKeys::Include);
        if (isListed)
            return isStopped ? Result::Stopped : Result::Completed;
        State& state = typed->state();
        RuntimeLayoutTable& layouts = state.objects->layouts;
        std::span<const FieldData> fields = typed->layout().fields();
        bool hasAccessor = false;
        typed->forEachFieldInKeyOrder(state, [&](uint16_t index) {
            hasAccessor |= fields[index].isAccessor();
        });
        if (hasAccessor)
            return Result::NeedsGenericPath;
        typed->forEachFieldInKeyOrder(state, [&](uint16_t index) {
            const FieldData& field = fields[index];
            if (isStopped || field.isPrivate() || !(layouts.hasStringKey(field) || layouts.hasSymbolKey(field)))
                return;
            if (JSValue value = typed->read(field))
                isStopped = visitor(OwnProperty { layouts.nameOf(vm, field), value, typed->attributesOf(field) }) == IterationStatus::Done;
        });
        return isStopped ? Result::Stopped : Result::Completed;
    }
    // A walk runs no code, so a dictionary that has lost a name is as good as any. Only a list kept for its Structure does not last.
    if (!extraPropertiesAreWalkedDirectly(extra, elements))
        return Result::NeedsGenericPath;
    if (!extra->structure()->isUncacheableDictionary()) {
        if (auto result = tryVisitCachedList(vm, *typed, extra, visitor))
            return *result;
    }
    return visitKeysInOrder(vm, *typed, extra, std::nullopt, visitor);
}

Result readOwnPropertiesCallingGetters(JSGlobalObject* globalObject, JSObject* object, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Object* typed = compiledObjectOrNull(object);
    if (!typed || hasFieldPreparedBeforeLookups(vm, *typed))
        return Result::NeedsGenericPath;
    JSObject* extra = typed->extraProperties();
    if (extra && !extraPropertiesAreWalkedDirectly(extra, Elements::NeedGenericPath))
        return Result::NeedsGenericPath;
    State& state = typed->state();
    RuntimeLayoutTable& layouts = state.objects->layouts;
    Vector<OrderedKey, 32> ordered;
    if (extra)
        ordered = collectKeysInOrder(vm, *typed, extra);
    else {
        std::span<const FieldData> fields = typed->layout().fields();
        typed->forEachFieldInKeyOrder(state, [&](uint16_t index) {
            const FieldData& field = fields[index];
            if (!field.isPrivate() && (layouts.hasStringKey(field) || layouts.hasSymbolKey(field)) && typed->read(field))
                ordered.append(OrderedKey { layouts.nameOf(vm, field), &field, invalidOffset, typed->attributesOf(field) });
        });
    }
    // A getter may change the object in any way: what follows it is asked for by name.
    bool isAsListed = true;
    for (bool wantsSymbols : { false, true }) {
        for (const OrderedKey& key : ordered) {
            if (key.name->isSymbol() != wantsSymbols || PropertyName(key.name).isPrivateName())
                continue;
            JSValue value;
            if (isAsListed && !(key.attributes & PropertyAttribute::Accessor)) {
                if (key.attributes & PropertyAttribute::DontEnum)
                    continue;
                value = key.field ? typed->read(*key.field) : extra->getDirect(key.offsetInExtraProperties);
            } else {
                PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
                bool has = object->methodTable()->getOwnPropertySlot(object, globalObject, PropertyName(key.name), slot);
                RETURN_IF_EXCEPTION(scope, Result::Stopped);
                if (!has || slot.attributes() & PropertyAttribute::DontEnum)
                    continue;
                value = slot.isTaintedByOpaqueObject() ? object->get(globalObject, PropertyName(key.name)) : slot.getValue(globalObject, PropertyName(key.name));
                RETURN_IF_EXCEPTION(scope, Result::Stopped);
                isAsListed = false;
            }
            names.append(key.name);
            values.appendWithCrashOnOverflow(value);
        }
    }
    return Result::Completed;
}

bool hasOwnEnumerableProperty(JSGlobalObject* globalObject, JSObject* object)
{
    KeyListingThatShowsNoOrder listing(object);
    PropertyNameArrayBuilder keys(globalObject->vm(), PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, globalObject, keys, DontEnumPropertiesMode::Exclude);
    return keys.size();
}

void prepareHoldersBeforeLookupsUnder(JSGlobalObject* globalObject, Symbol* symbol, JSObject* function)
{
    if (Instance* instance = Instance::of(globalObject))
        instance->state->propertyCaches->prepareHoldersBeforeLookupsUnder(symbol, function);
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

bool listExtraPropertiesInStructure(VM& vm, JSObject* object)
{
    DeferGCForAWhile deferGC(vm);
    return Object::ensureStructureForExtraPropertiesIfNeeded(vm, object);
}

JSValue getDirect(VM& vm, const JSObject* object, PropertyName name, unsigned* attributesOrNull)
{
    if (!isCompiledObject(object))
        return { };
    return uncheckedDowncast<Object>(const_cast<JSObject*>(object))->getDirectFromNativeCode(vm, name, attributesOrNull);
}

bool ordinaryGetOwnPropertySlot(JSGlobalObject* globalObject, JSObject* object, PropertyName name, PropertySlot& slot)
{
    ASSERT(slot.isVMInquiry());
    VM& vm = globalObject->vm();
    if (isCompiledObject(object)) {
        DeferTerminationForAWhile deferTermination(vm);
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        bool isFound = Object::getOwnPropertySlot(object, globalObject, name, slot);
        scope.assertNoException();
        return isFound;
    }
    if (isCompiledClosure(object)) {
        Closure& closure = *uncheckedDowncast<Closure>(object);
        if (const FieldData* field = StaticFields::findOwnField(closure, name.uid())) {
            if (JSValue value = closure.state().objects->classes.readStaticField(*field)) {
                slot.setValue(object, static_cast<unsigned>(PropertyAttribute::None), value);
                return true;
            }
        }
        if (declaresStaticMethod(closure, name.uid()))
            closure.ensureClassObject(vm);
    } else if (object->classInfo() == ClassPrototype::info()) {
        if (name == vm.propertyNames->constructor) {
            slot.setValue(object, static_cast<unsigned>(PropertyAttribute::DontEnum), uncheckedDowncast<ClassPrototype>(object)->classObject());
            return true;
        }
        if (ClassPrototype::hasMember(object, name))
            ClassPrototype::ensureMembers(object);
    }
    return JSObject::getOwnPropertySlot(object, globalObject, name, slot);
}

Presence tryGetOwnValue(JSObject* object, UniquedStringImpl* name, JSValue& value)
{
    ASSERT(isCompiledObject(object));
    const Object& typed = *uncheckedDowncast<Object>(object);
    State& state = typed.state();
    if (const FieldData* field = typed.fieldNamed(state, name)) {
        if (field->isPrivate() || field->isAccessor() || isPreparedBeforeLookups(*state.instance.vm, name)) [[unlikely]]
            return Presence::Unknown;
        value = typed.readInline(*field);
        if (!value) [[unlikely]]
            value = typed.read(*field);
        if (value) [[likely]]
            return Presence::Present;
    } else
        state.objects->layouts.didReadAbsentField(*state.instance.vm, typed.layout(), name);
    return typed.hasExtraProperties() ? Presence::Unknown : Presence::Absent;
}

static void findDescriptorFields(VM& vm, RuntimeLayoutTable& layouts, const LayoutData& layout, DescriptorFields& found)
{
    const CommonIdentifiers& names = *vm.propertyNames;
    std::array<UniquedStringImpl*, 6> keys { names.enumerable.impl(), names.configurable.impl(), names.value.impl(), names.writable.impl(), names.get.impl(), names.set.impl() };
    found.layout = &layout;
    for (size_t index = 0; index < keys.size(); ++index) {
        found.fields[index] = layouts.field(vm, layout, keys[index]);
        if (!found.fields[index])
            layouts.didReadAbsentField(vm, layout, keys[index]);
    }
}

bool tryReadDescriptorMembers(JSObject* object, DescriptorMembers& members)
{
    ASSERT(isCompiledObject(object));
    const Object& typed = *uncheckedDowncast<Object>(object);
    State& state = typed.state();
    const LayoutData& layout = typed.layout();
    DescriptorFields& known = state.objects->caches->descriptorFields[layout.shape % ObjectModelCaches::numberOfDescriptorFields];
    if (known.layout != &layout) [[unlikely]]
        findDescriptorFields(*state.instance.vm, state.objects->layouts, layout, known);
    bool hasExtraProperties = typed.hasExtraProperties();
    for (size_t index = 0; index < members.size(); ++index) {
        JSValue value;
        if (const FieldData* field = known.fields[index]) {
            if (field->isAccessor()) [[unlikely]]
                return false;
            value = typed.readInline(*field);
        }
        // The property may lie among the extra properties.
        if (!value && hasExtraProperties) [[unlikely]]
            return false;
        members[index] = value;
    }
    return true;
}

void putDirect(VM& vm, JSObject* object, PropertyName name, JSValue value, unsigned attributes)
{
    ASSERT(isCompiledObject(object));
    uncheckedDowncast<Object>(object)->putDirectFromNativeCode(vm, name, value, attributes);
}

void prepareForFreeze(JSObject* object)
{
    ASSERT(isCompiledObject(object));
    prepareForWalk(object->structure()->realm(), object);
}

void setIntegrity(VM& vm, JSObject* object, Integrity level)
{
    ASSERT(isCompiledObject(object));
    DeferTerminationForAWhile deferTermination(vm);
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

static bool hasOnlyWritableDataProperties(VM& vm, const Object& object)
{
    if (hasFieldPreparedBeforeLookups(vm, object))
        return false;
    for (const FieldData& field : object.layout().fields()) {
        bool interceptsStores = field.isAccessor() || object.attributesOf(field) & PropertyAttribute::ReadOnly;
        if (interceptsStores && object.isInSlot(field))
            return false;
    }
    JSObject* extra = object.extraProperties();
    return !extra || !extra->structure()->hasReadOnlyOrGetterSetterPropertiesExcludingProto();
}

bool wouldPerformFastPut(VM& vm, JSObject* target)
{
    if (!Instance::of(target->realm()))
        return false;
    for (JSObject* object = target;;) {
        if (object != target)
            materializeLazyProperties(vm, object);
        Structure* structure = object->structure();
        if (Object* compiled = compiledObjectOrNull(object)) {
            if (!hasOnlyWritableDataProperties(vm, *compiled))
                return false;
        } else if (structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || structure->typeInfo().overridesGetPrototype() || (object == target ? structure->mayHaveImmutableProperties() : structure->typeInfo().overridesPut()))
            return false;
        JSValue above = object->getPrototypeDirect();
        if (above.isNull())
            break;
        object = asObject(above);
    }
    return isCompiledObject(target) ? testIntegrity(target, Integrity::NonExtensible) == Presence::Absent : target->isStructureExtensible();
}

static bool tryAssignInOrderOfCreation(VM& vm, Object& target, JSObject* source)
{
    if (source == &target || !isCompiledObject(source))
        return false;
    Vector<UniquedStringImpl*, 8> names;
    MarkedArgumentBuffer values;
    EnsureStillAliveScope sourceScope(source);
    Result result = forEachOwnPropertyAsCreated(vm, source, [&](const OwnProperty& own) {
        if (own.attributes & PropertyAttribute::DontEnum)
            return IterationStatus::Continue;
        if (own.name == vm.propertyNames->underscoreProto.impl() || parseIndex(*own.name))
            return IterationStatus::Done;
        names.append(own.name);
        values.appendWithCrashOnOverflow(own.value);
        return IterationStatus::Continue;
    }, Elements::AreLeftOut);
    if (result != Result::Completed || !wouldPerformFastPut(vm, &target))
        return false;
    JSObject* holder = objectHoldingIndexedProperties(source);
    if (holder && holder->canHaveExistingOwnIndexedProperties()) {
        JSObject* elementsOfTarget = objectHoldingIndexedProperties(&target);
        if (target.anyObjectInChainMayInterceptIndexedAccesses() || (elementsOfTarget && elementsOfTarget->canHaveExistingOwnIndexedGetterSetterProperties()))
            return false;
        DeferTerminationForAWhile deferTermination(vm);
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        JSGlobalObject* realm = target.structure()->realm();
        holder->forEachOwnIndexedProperty<>([&](unsigned index, JSValue value) {
            Object::putByIndex(&target, realm, index, value, true);
            scope.assertNoException();
            return IterationStatus::Continue;
        });
    }
    return storePropertiesInOrder(vm, target, names.mutableSpan(), { values.data(), values.size() });
}

bool tryAssign(VM& vm, JSObject* compiledTarget, JSObject* source)
{
    Object* typed = compiledObjectOrNull(compiledTarget);
    return typed && (typed->tryAssignPropertiesOf(vm, source) || tryAssignInOrderOfCreation(vm, *typed, source));
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

bool wouldHaveArrayStorage(JSObject* object)
{
    JSObject* extra = objectHoldingIndexedProperties(object);
    if (!extra || !JSC::hasIndexedProperties(extra->indexingType()))
        return false;
    return hasAnyArrayStorage(extra->indexingType()) || object->anyObjectInChainMayInterceptIndexedAccesses();
}

} // namespace PropertyWalk

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
