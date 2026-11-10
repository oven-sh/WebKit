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
#include "AOTClassToken.h"
#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTPendingStars.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyListCache.h"
#include "AOTPropertyWalkInlines.h"
#include "AOTRuntimeFunctions.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "AOTVector.h"
#include "DeferGC.h"
#include "DeferTermination.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <optional>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

bool Object::structureGuaranteesAbsence(Structure* structure, UniquedStringImpl* name)
{
    const CompiledStructure* compiled = compiledObjectStructureOrNull(structure);
    if (!compiled)
        return false;
    if (parseIndex(*name) || PropertyName(name).isPrivateName())
        return false;
    State& state = *structure->realm()->aotInstance()->state;
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
        && (elements == Elements::Skip ? !extra->canHaveExistingOwnIndexedGetterSetterProperties() : !JSC::hasIndexedProperties(extra->indexingType()))
        && !structure->hasAnyKindOfGetterSetterProperties()
        && !extra->hasNonReifiedStaticProperties();
}

static bool extraPropertiesNeedGenericPath(JSObject* extra, Elements elements = Elements::NeedGenericPath)
{
    return !extraPropertiesAreWalkedDirectly(extra, elements) || extra->structure()->isUncacheableDictionary();
}

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

static bool extraPropertiesAreWritableAndConfigurable(JSObject* extra)
{
    Structure* structure = extra->structure();
    return !structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() && !structure->hasNonConfigurableProperties();
}

// The visitor may collect, which forgets the list.
static std::optional<Result> tryVisitCachedList(VM& vm, const Object& typed, JSObject* extra, const Visitor& visitor)
{
    if (!extraPropertiesAreWritableAndConfigurable(extra))
        return std::nullopt;
    Vector<PropertyListEntry, 16> entries;
    {
        DeferGC deferGC(vm);
        if (!findCachedPropertyList(typed, extra) && !typed.state().objects->caches->propertyLists.isSecondWalk(extra->structureID(), typed.shapeWord(), typed.orderWord() & orderAndCountMask))
            return std::nullopt;
        CachedPropertyList* list = cachedPropertyList(vm, typed);
        if (!list)
            return std::nullopt;
        entries.append(list->properties.span());
    }
    unsigned attributes = typed.attributesOfPublicFields();
    for (const PropertyListEntry& entry : entries) {
        JSValue value = typed.readPropertyListEntry(entry, extra);
        if (value && visitor(OwnProperty { entry.name, value, entry.field ? attributes : 0 }) == IterationStatus::Done)
            return Result::Stopped;
    }
    return Result::Completed;
}

const CachedPropertyList* tryGetOwnPropertyList(VM& vm, JSObject* object, JSObject*& extra)
{
    Object* typed = compiledObjectOrNull(object);
    extra = typed ? typed->extraProperties() : nullptr;
    if (!extra)
        return nullptr;
    if (CachedPropertyList* found = findCachedPropertyList(*typed, extra); found && found->holdsOnlyPlainData) [[likely]]
        return found;
    if (extraPropertiesNeedGenericPath(extra) || !extraPropertiesAreWritableAndConfigurable(extra))
        return nullptr;
    ASSERT(vm.heap.isDeferred());
    CachedPropertyList* list = cachedPropertyList(vm, *typed);
    if (list && !list->holdsOnlyPlainData) {
        writeJSONKeys(vm, *list);
        list->holdsOnlyPlainData = true;
    }
    return list;
}

static Vector<OrderedKey, 32> collectKeysInOrder(VM& vm, const Object& typed, JSObject* extra)
{
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

static Vector<OrderedKey, 32> collectFieldsInOrder(VM& vm, const Object& typed)
{
    State& state = typed.state();
    RuntimeLayoutTable& layouts = state.objects->layouts;
    std::span<const FieldData> fields = typed.layout().fields();
    Vector<OrderedKey, 32> ordered;
    typed.forEachFieldInKeyOrder(state, [&](uint16_t index) {
        const FieldData& field = fields[index];
        if (!field.isPrivate() && (layouts.hasStringKey(field) || layouts.hasSymbolKey(field)))
            ordered.append(OrderedKey { layouts.nameOf(vm, field), &field, invalidOffset, typed.attributesOf(field) });
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
    State& state = typed.state();
    if (const FieldData* field = typed.fieldNamed(state, name)) {
        bool defersClosure = field->slot.representation() == SlotRepresentation::DeferredClosure;
        if (defersClosure ? typed.isInSlot(*field) : typed.readInline(*field) || typed.read(*field))
            return Presence::Present;
    }
    if (std::optional<Object*> source = sourceOfPendingBinding(typed.vm(), typed, name))
        return *source ? Presence::Present : Presence::Absent;
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

static Presence presenceOfStaticFieldInVariable(const Closure& closure, UniquedStringImpl* name)
{
    const LayoutData* classLayout = closure.classData();
    if (!classLayout || !closure.isLazyClassObject() || !closure.state().objects->classes.staticField(*classLayout, name))
        return Presence::Absent;
    return StaticFields::tryRead(closure, name) ? Presence::Present : Presence::Unknown;
}

static Presence functionHasProperty(JSObject* function, UniquedStringImpl* name)
{
    VM& vm = function->vm();
    if (isLazyFunctionProperty(vm, name))
        return Presence::Unknown;
    if (function->type() == SlimClosureType)
        return Presence::Absent;
    const Closure* closure = isCompiledClosure(function) ? uncheckedDowncast<Closure>(function) : nullptr;
    if (isValidOffset(function->structure()->get(vm, name)))
        return Presence::Present;
    if (closure)
        return declaresStaticMethod(*closure, name) ? Presence::Present : presenceOfStaticFieldInVariable(*closure, name);
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

Result forEachOwnPropertyAsCreated(VM& vm, JSObject* object, const Visitor& visitor, Elements elements, Preparations preparations)
{
    Object* typed = compiledObjectOrNull(object);
    if (!typed || (preparations == Preparations::NeedGenericPath && hasFieldPreparedBeforeLookups(vm, *typed)))
        return Result::NeedsGenericPath;
    bool isStopped = false;
    JSObject* extra = typed->extraProperties();
    if (extra && elements == Elements::Skip && extra->structure() == typed->instance().realm.extraPropertiesWithElementsStructure)
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
    if (!extraPropertiesAreWalkedDirectly(extra, elements))
        return Result::NeedsGenericPath;
    if (!extra->structure()->isUncacheableDictionary()) {
        if (auto result = tryVisitCachedList(vm, *typed, extra, visitor))
            return *result;
    }
    return visitKeysInOrder(vm, *typed, extra, std::nullopt, visitor);
}

Result forEachOwnPropertyAsCreatedAfterPreparing(JSGlobalObject* globalObject, JSObject* object, const Visitor& visitor, Elements elements)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    prepareForWalk(globalObject, object);
    RETURN_IF_EXCEPTION(scope, Result::NeedsGenericPath);
    return forEachOwnPropertyAsCreated(vm, object, visitor, elements, Preparations::AreDone);
}

bool hasGetterToCall(JSObject* object)
{
    const Object& typed = *uncheckedDowncast<Object>(object);
    if (!typed.compiledStructure().isCopiedByWords() && std::ranges::any_of(typed.layout().fields(), &FieldData::isAccessor))
        return true;
    return typed.hasExtraProperties() && typed.extraProperties()->structure()->hasAnyKindOfGetterSetterProperties();
}

namespace {

struct PlacesOfProperties {
    explicit PlacesOfProperties(const Object& object)
        : structure(object.structureID())
        , orderWord(object.orderWord())
        , extra(object.extraProperties())
        , structureOfExtra(extra ? extra->structureID() : StructureID())
    {
    }

    bool areStill(const Object& object) const
    {
        JSObject* now = object.extraProperties();
        return object.structureID() == structure && object.orderWord() == orderWord && !object.hasKeyOrderOfItsOwn() && now == extra && (!now || (now->structureID() == structureOfExtra && !now->structure()->isDictionary()));
    }

    StructureID structure;
    uint32_t orderWord;
    JSObject* extra;
    StructureID structureOfExtra;
};

}

static Result readFieldsCallingGetters(JSGlobalObject* globalObject, Object& typed, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values, const GetterWalkHooks& hooks)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = typed.state();
    RuntimeLayoutTable& layouts = state.objects->layouts;
    const LayoutData& layout = typed.layout();
    if (hooks.refusesProtoName && layouts.field(vm, layout, vm.propertyNames->underscoreProto.impl()))
        return Result::NeedsGenericPath;
    std::span<const FieldData> fields = layout.fields();
    Vector<uint16_t, 32> order;
    typed.forEachFieldInKeyOrder(state, [&](uint16_t index) {
        order.append(index);
    });
    PlacesOfProperties places(typed);
    bool isAsListed = true;
    for (bool wantsSymbols : { false, true }) {
        if (wantsSymbols && !layout.hasFieldsWithSymbolKeys)
            break;
        for (uint16_t index : order) {
            const FieldData& field = fields[index];
            if (field.isPrivate() || !(wantsSymbols ? layouts.hasSymbolKey(field) : layouts.hasStringKey(field)))
                continue;
            UniquedStringImpl* name = layouts.nameOf(vm, field);
            if (hooks.isLeftOut && (*hooks.isLeftOut)(name))
                continue;
            JSValue value;
            if (isAsListed) {
                if (typed.attributesOf(field) & PropertyAttribute::DontEnum)
                    continue;
                value = typed.read(field);
                ASSERT(value);
                if (value && field.isAccessor()) {
                    if (hooks.willRunCode)
                        (*hooks.willRunCode)();
                    RETURN_IF_EXCEPTION(scope, Result::Stopped);
                    value = uncheckedDowncast<GetterSetter>(value)->callGetter(globalObject, &typed);
                    RETURN_IF_EXCEPTION(scope, Result::Stopped);
                    isAsListed = places.areStill(typed) && (!hooks.mayReadAhead || (*hooks.mayReadAhead)());
                }
            }
            if (!value) {
                isAsListed = false;
                if (hooks.willRunCode)
                    (*hooks.willRunCode)();
                RETURN_IF_EXCEPTION(scope, Result::Stopped);
                PropertySlot slot(&typed, PropertySlot::InternalMethodType::GetOwnProperty);
                bool has = Object::getOwnPropertySlot(&typed, globalObject, PropertyName(name), slot);
                RETURN_IF_EXCEPTION(scope, Result::Stopped);
                if (!has || slot.attributes() & PropertyAttribute::DontEnum)
                    continue;
                value = slot.getValue(globalObject, PropertyName(name));
                RETURN_IF_EXCEPTION(scope, Result::Stopped);
            }
            names.append(name);
            values.appendWithCrashOnOverflow(value);
        }
    }
    return Result::Completed;
}

Result readOwnPropertiesCallingGetters(JSGlobalObject* globalObject, JSObject* object, Vector<RefPtr<UniquedStringImpl>, 32>& heldNames, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values, GetterWalkHooks hooks)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Object* typed = compiledObjectOrNull(object);
    if (!typed || hasFieldPreparedBeforeLookups(vm, *typed))
        return Result::NeedsGenericPath;
    if (!typed->hasExtraProperties())
        RELEASE_AND_RETURN(scope, readFieldsCallingGetters(globalObject, *typed, names, values, hooks));
    JSObject* extra = typed->extraProperties();
    if (extra && (extra->type() != FinalObjectType || JSC::hasIndexedProperties(extra->indexingType()) || extra->hasNonReifiedStaticProperties()))
        return Result::NeedsGenericPath;
    Vector<OrderedKey, 32> ordered = extra ? collectKeysInOrder(vm, *typed, extra) : collectFieldsInOrder(vm, *typed);
    if (hooks.refusesProtoName && std::ranges::contains(ordered, vm.propertyNames->underscoreProto.impl(), &OrderedKey::name))
        return Result::NeedsGenericPath;
    for (const OrderedKey& key : ordered) {
        if (!key.field)
            heldNames.append(key.name);
    }
    PlacesOfProperties places(*typed);
    bool isAsListed = true;
    for (bool wantsSymbols : { false, true }) {
        if (wantsSymbols && !extra && !typed->layout().hasFieldsWithSymbolKeys)
            break;
        for (const OrderedKey& key : ordered) {
            if (key.name->isSymbol() != wantsSymbols || (wantsSymbols && PropertyName(key.name).isPrivateName()) || (hooks.isLeftOut && (*hooks.isLeftOut)(key.name)))
                continue;
            JSValue value;
            if (isAsListed && !(key.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)) {
                if (key.attributes & PropertyAttribute::DontEnum)
                    continue;
                value = key.field ? typed->read(*key.field) : extra->getDirect(key.offsetInExtraProperties);
                ASSERT(value);
            } else if (isAsListed && key.attributes & PropertyAttribute::Accessor) {
                if (key.attributes & PropertyAttribute::DontEnum)
                    continue;
                if (hooks.willRunCode)
                    (*hooks.willRunCode)();
                RETURN_IF_EXCEPTION(scope, Result::Stopped);
                JSValue accessor = key.field ? typed->read(*key.field) : extra->getDirect(key.offsetInExtraProperties);
                ASSERT(accessor);
                if (accessor) {
                    value = uncheckedDowncast<GetterSetter>(accessor)->callGetter(globalObject, object);
                    RETURN_IF_EXCEPTION(scope, Result::Stopped);
                    isAsListed = places.areStill(*typed) && (!hooks.mayReadAhead || (*hooks.mayReadAhead)());
                }
            }
            if (!value) {
                if (hooks.willRunCode)
                    (*hooks.willRunCode)();
                RETURN_IF_EXCEPTION(scope, Result::Stopped);
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

static bool newObjectsTakeStoresAsDefinitions(JSGlobalObject* globalObject)
{
    return !globalObject->objectPrototype()->structure()->hasReadOnlyOrGetterSetterPropertiesExcludingProto();
}

static NEVER_INLINE void fillGetterSourceList(JSGlobalObject* globalObject, GetterSourceList& list, const Object& typed, JSObject* extra)
{
    VM& vm = globalObject->vm();
    list.structure = typed.structureID();
    list.orderWord = typed.orderWord();
    list.extraPropertiesStructure = extra ? extra->structureID() : StructureID();
    list.structureOfCopy = nullptr;
    list.properties.shrink(0);
    if (extra && (extra->type() != FinalObjectType || extra->structure()->isDictionary() || JSC::hasIndexedProperties(extra->indexingType()) || extra->hasNonReifiedStaticProperties()))
        return;
    Vector<OrderedKey, 32> ordered = extra ? collectKeysInOrder(vm, typed, extra) : collectFieldsInOrder(vm, typed);
    Vector<UniquedStringImpl*, 8> names;
    for (bool wantsSymbols : { false, true }) {
        for (const OrderedKey& key : ordered) {
            if (key.name->isSymbol() != wantsSymbols || PropertyName(key.name).isPrivateName())
                continue;
            if (key.attributes & PropertyAttribute::CustomAccessorOrValue || key.name == vm.propertyNames->underscoreProto.impl() || parseIndex(PropertyName(key.name)))
                return;
            bool isEnumerable = !(key.attributes & PropertyAttribute::DontEnum);
            list.properties.append({ key.name, key.field, key.offsetInExtraProperties, isEnumerable ? static_cast<PropertyOffset>(names.size()) : invalidOffset, !!(key.attributes & PropertyAttribute::Accessor) });
            if (isEnumerable)
                names.append(key.name);
        }
    }
    if (names.isEmpty() || names.size() > JSFinalObject::maxInlineCapacity)
        return;
    MarkedArgumentBuffer values;
    for (size_t index = 0; index < names.size(); ++index)
        values.appendWithCrashOnOverflow(jsUndefined());
    JSFinalObject* model = constructEmptyObject(globalObject, globalObject->objectPrototype(), std::max<unsigned>(names.size(), JSFinalObject::defaultInlineCapacity));
    model->putOwnDataPropertyBatching(vm, names.mutableSpan().data(), values.data(), names.size());
    Structure* structure = model->structure();
    if (structure->isDictionary())
        return;
    for (size_t index = 0; index < names.size(); ++index) {
        if (structure->get(vm, names[index]) != static_cast<PropertyOffset>(index))
            return;
    }
    list.structureOfCopy = structure;
}

static NEVER_INLINE JSObject* finishCopyByName(JSGlobalObject* globalObject, JSObject* object, std::span<const GetterSourceList::Property> properties, size_t numberRead, JSFinalObject* begun, JSValue valueOfNext, NewObjectTakes takes, const ScopedLambda<bool(UniquedStringImpl*)>* isLeftOut, JSObject* unseenTarget)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* copy = unseenTarget ? unseenTarget : constructEmptyObject(globalObject);
    ASSERT(takes == NewObjectTakes::Stores || !isCompiledObject(copy));
    Vector<UniquedStringImpl*, 8> names;
    MarkedArgumentBuffer values;
    for (const GetterSourceList::Property& property : properties.first(numberRead)) {
        JSValue value = isValidOffset(property.offsetInCopy) ? begun->getDirect(property.offsetInCopy) : JSValue();
        if (!value)
            continue;
        names.append(property.name);
        values.appendWithCrashOnOverflow(value);
    }
    if (!names.isEmpty()) {
        Object* record = compiledObjectOrNull(copy);
        if (!record)
            copy->putOwnDataPropertyBatching(vm, names.mutableSpan().data(), values.data(), names.size());
        else if (!storePropertiesInOrder(vm, *record, names.mutableSpan(), { values.data(), values.size() })) {
            for (size_t index = 0; index < names.size(); ++index) {
                PutPropertySlot slot(copy, true);
                copy->putInline(globalObject, PropertyName(names[index]), values.at(index), slot);
                RETURN_IF_EXCEPTION(scope, nullptr);
            }
        }
    }
    for (size_t index = numberRead; index < properties.size(); ++index) {
        PropertyName name(properties[index].name);
        if (isLeftOut && (*isLeftOut)(name.uid()))
            continue;
        JSValue value = std::exchange(valueOfNext, JSValue());
        if (!value) {
            PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
            bool has = object->methodTable()->getOwnPropertySlot(object, globalObject, name, slot);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (!has || slot.attributes() & PropertyAttribute::DontEnum)
                continue;
            value = slot.isTaintedByOpaqueObject() ? object->get(globalObject, name) : slot.getValue(globalObject, name);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
        if (takes == NewObjectTakes::Stores) {
            PutPropertySlot slot(copy, true);
            copy->putInline(globalObject, name, value, slot);
            RETURN_IF_EXCEPTION(scope, nullptr);
        } else
            copy->putDirect(vm, name, value);
    }
    return copy;
}

JSObject* tryCopyCallingGetters(JSGlobalObject* globalObject, JSObject* object, NewObjectTakes takes, const ScopedLambda<bool(UniquedStringImpl*)>* isLeftOut, JSObject* unseenTarget)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Object& typed = *uncheckedDowncast<Object>(object);
    if (typed.structure()->realm() != globalObject || (takes == NewObjectTakes::Stores && !newObjectsTakeStoresAsDefinitions(globalObject)))
        return nullptr;
    if (typed.hasKeyOrderOfItsOwn() || hasFieldPreparedBeforeLookups(vm, typed))
        return nullptr;
    JSObject* extra = typed.extraProperties();
    PlacesOfProperties places(typed);
    GetterSourceList& list = typed.state().objects->caches->getterSources.placeFor(places.structure, places.orderWord);
    if (!list.isFor(places.structure, places.orderWord, places.structureOfExtra)) [[unlikely]]
        fillGetterSourceList(globalObject, list, typed, extra);
    if (!list.structureOfCopy)
        return nullptr;
    // A getter may copy another object, which may take the place of the list, and may collect, which empties it. The Structure of the second object holds the names.
    Vector<GetterSourceList::Property, 8> properties(list.properties);
    EnsureStillAliveScope namesScope(extra ? extra->structure() : nullptr);
    JSFinalObject* copy = JSFinalObject::create(vm, list.structureOfCopy);
    for (size_t index = 0; index < properties.size(); ++index) {
        const GetterSourceList::Property& property = properties[index];
        if (!isValidOffset(property.offsetInCopy) || (isLeftOut && (*isLeftOut)(property.name)))
            continue;
        JSValue value = property.field ? typed.read(*property.field) : extra->getDirect(property.offsetInExtraProperties);
        if (!value) [[unlikely]]
            RELEASE_AND_RETURN(scope, finishCopyByName(globalObject, object, properties.span(), index, copy, JSValue(), takes, isLeftOut, unseenTarget));
        if (property.isGetter) {
            value = uncheckedDowncast<GetterSetter>(value)->callGetter(globalObject, object);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (!places.areStill(typed) || (takes == NewObjectTakes::Stores && !newObjectsTakeStoresAsDefinitions(globalObject))) [[unlikely]]
                RELEASE_AND_RETURN(scope, finishCopyByName(globalObject, object, properties.span(), index, copy, value, takes, isLeftOut, unseenTarget));
        }
        copy->putDirectOffset(vm, property.offsetInCopy, value);
    }
    if (isLeftOut)
        RELEASE_AND_RETURN(scope, finishCopyByName(globalObject, object, properties.span(), properties.size(), copy, JSValue(), takes, isLeftOut, unseenTarget));
    return copy;
}

bool hasOwnEnumerableProperty(JSGlobalObject* globalObject, JSObject* object)
{
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
}

JSCellButterfly* tryGetCachedOwnKeys(VM& vm, JSObject* object, Filter filter)
{
    Object* typed = compiledObjectOrNull(object);
    if (!typed)
        return nullptr;
    if (typed->hasExtraProperties())
        typed->ensureStructureForExtraProperties(vm);
    else if (filter == Filter::StringsAndSymbols && typed->layout().hasFieldsWithSymbolKeys)
        return nullptr;
    return typed->keys(vm);
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

bool ensureExtraPropertiesStructure(VM& vm, JSObject* object)
{
    DeferGCForAWhile deferGC(vm);
    return Object::ensureStructureForExtraPropertiesIfNeeded(vm, object);
}

JSValue getDirect(VM& vm, const JSObject* object, PropertyName name, unsigned* attributesOrNull)
{
    if (!isCompiledObject(object))
        return { };
    Object& typed = *uncheckedDowncast<Object>(const_cast<JSObject*>(object));
    JSValue value = typed.getDirectFromNativeCode(vm, name, attributesOrNull);
    if (value || !vm.aotState().someTextKnowsPrivateNames || !name.isPrivateName()) [[likely]]
        return value;
    UniquedStringImpl* declaredName = ClassToken::declaredNameUnder(vm, typed, name.uid());
    return declaredName ? typed.getDirectFromNativeCode(vm, declaredName, attributesOrNull) : JSValue();
}

bool isOrdinaryPrototypeChain(JSObject* first, JSObject* last)
{
    if (first->type() != CompiledObjectType)
        return false;
    for (JSObject* object = first; object; object = object->getPrototypeDirect().getObject()) {
        if (TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags()) && object->type() != CompiledObjectType)
            return false;
        if (object == last)
            return true;
    }
    return false;
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
        if (declaresStaticMethod(closure, name.uid()) || StaticFields::tryRead(closure, name.uid()))
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
    }
    return typed.hasExtraProperties() ? Presence::Unknown : Presence::Absent;
}

static void findDescriptorFields(VM& vm, RuntimeLayoutTable& layouts, const LayoutData& layout, DescriptorFields& found)
{
    const CommonIdentifiers& names = *vm.propertyNames;
    std::array<UniquedStringImpl*, 6> keys { names.enumerable.impl(), names.configurable.impl(), names.value.impl(), names.writable.impl(), names.get.impl(), names.set.impl() };
    found.layout = &layout;
    for (size_t index = 0; index < keys.size(); ++index)
        found.fields[index] = layouts.field(vm, layout, keys[index]);
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
        if (!value && hasExtraProperties) [[unlikely]]
            return false;
        members[index] = value;
    }
    return true;
}

void putDirect(VM& vm, JSObject* object, PropertyName name, JSValue value, unsigned attributes)
{
    ASSERT(isCompiledObject(object));
    Object& typed = *uncheckedDowncast<Object>(object);
    UniquedStringImpl* uid = name.uid();
    if (vm.aotState().someTextKnowsPrivateNames && name.isPrivateName() && !typed.getDirectFromNativeCode(vm, name, nullptr)) [[unlikely]] {
        if (UniquedStringImpl* declaredName = ClassToken::declaredNameUnder(vm, typed, uid))
            uid = declaredName;
    }
    typed.putDirectFromNativeCode(vm, uid, value, attributes);
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
    uncheckedDowncast<Object>(object)->setIntegrity(object->structure()->realm(), objectIntegrity(level));
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

static bool readPropertiesInOrderOfCreation(JSGlobalObject* realm, JSObject* source, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values)
{
    VM& vm = realm->vm();
    if (isCompiledObject(source)) {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        Result result = forEachOwnPropertyAsCreatedAfterPreparing(realm, source, [&](const OwnProperty& own) {
            if (own.attributes & PropertyAttribute::DontEnum)
                return IterationStatus::Continue;
            if (own.name == vm.propertyNames->underscoreProto.impl() || parseIndex(*own.name))
                return IterationStatus::Done;
            names.append(own.name);
            values.appendWithCrashOnOverflow(own.value);
            return IterationStatus::Continue;
        }, Elements::Skip);
        return !scope.exception() && result == Result::Completed;
    }
    Structure* structure = source->structure();
    if (source->hasNonReifiedStaticProperties() || !structure->canPerformFastPropertyEnumerationCommon() || source->canHaveExistingOwnIndexedGetterSetterProperties())
        return false;
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        if (entry.attributes() & PropertyAttribute::DontEnum || PropertyName(entry.key()).isPrivateName())
            return true;
        names.append(entry.key());
        values.appendWithCrashOnOverflow(source->getDirect(entry.offset()));
        return true;
    });
    return true;
}

static bool tryAssignInOrderOfCreation(VM& vm, Object& target, JSObject* source, bool& goesOnKeyByKey)
{
    goesOnKeyByKey = true;
    if (isCompiledObject(source) && hasGetterToCall(source))
        return tryAssignCallingGetters(target.structure()->realm(), &target, source);
    Vector<UniquedStringImpl*, 8> names;
    MarkedArgumentBuffer values;
    EnsureStillAliveScope sourceScope(source);
    if (!readPropertiesInOrderOfCreation(target.structure()->realm(), source, names, values) || !wouldPerformFastPut(vm, &target))
        return false;
    JSObject* holder = isCompiledObject(source) ? objectHoldingIndexedProperties(source) : source;
    bool hasElements = holder && holder->canHaveExistingOwnIndexedProperties();
    JSObject* elementsOfTarget = objectHoldingIndexedProperties(&target);
    if (hasElements && (target.anyObjectInChainMayInterceptIndexedAccesses() || (elementsOfTarget && elementsOfTarget->canHaveExistingOwnIndexedGetterSetterProperties())))
        return false;
    goesOnKeyByKey = false;
    if (source == &target)
        return true;
    if (hasElements) {
        DeferTerminationForAWhile deferTermination(vm);
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        JSGlobalObject* realm = target.structure()->realm();
        Object* from = compiledObjectOrNull(source);
        bool didAssignFields = from && holder->structure() == from->instance().realm.extraPropertiesWithElementsStructure && tryAssignFieldsLeavingElementsOut(vm, target, *from);
        Instance& instance = target.instance();
        if (didAssignFields && !target.hasExtraProperties() && target.layout().extraPropertiesSlot() && !target.structure()->mayBePrototype() && !target.layout().isClass()) {
            if (JSFinalObject* elements = tryCopyWithElements(vm, holder)) {
                target.setExtraProperties(vm, elements, instance.state->objects->structures.withExtraProperties(vm, target.compiledStructure(), nullptr));
                return true;
            }
        }
        bool mayDefine = !elementsOfTarget || !JSC::hasIndexedProperties(elementsOfTarget->indexingType());
        holder->forEachOwnElement<>([&](unsigned index, JSValue value) {
            if (!mayDefine || !tryDefineElementOfCompiledObject(&instance, &target, index, value))
                Object::putByIndex(&target, realm, index, value, true);
            scope.assertNoException();
            return IterationStatus::Continue;
        });
        if (didAssignFields)
            return true;
    }
    return storePropertiesInOrder(vm, target, names.mutableSpan(), { values.data(), values.size() });
}

static bool takesStoresWithoutCode(VM& vm, JSObject* target)
{
    if (isCompiledObject(target))
        return wouldPerformFastPut(vm, target);
    auto* ordinary = dynamicDowncast<JSFinalObject>(target);
    return ordinary && ordinary->canPerformFastPutInlineExcludingProto() && ordinary->isStructureExtensible();
}

static void storeInOrder(JSGlobalObject* globalObject, JSObject* target, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (takesStoresWithoutCode(vm, target)) {
        Object* record = compiledObjectOrNull(target);
        if (!record) {
            target->putOwnDataPropertyBatching(vm, names.mutableSpan().data(), values.data(), names.size());
            return;
        }
        if (storePropertiesInOrder(vm, *record, names.mutableSpan(), { values.data(), values.size() }))
            return;
    }
    for (size_t index = 0; index < names.size(); ++index) {
        PutPropertySlot slot(target, true);
        target->putInline(globalObject, PropertyName(names[index]), values.at(index), slot);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

bool tryAssignCallingGetters(JSGlobalObject* globalObject, JSObject* target, JSObject* source)
{
    VM& vm = globalObject->vm();
    if (source == target || !isCompiledObject(source) || !hasGetterToCall(source) || !takesStoresWithoutCode(vm, target))
        return false;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<RefPtr<UniquedStringImpl>, 32> heldNames;
    Vector<UniquedStringImpl*, 8> names;
    MarkedArgumentBuffer values;
    auto storeWhatIsRead = [&] {
        if (names.isEmpty())
            return;
        storeInOrder(globalObject, target, names, values);
        names.shrink(0);
        values.clear();
    };
    ScopedLambda<void()> willRunCode = storeWhatIsRead;
    auto takesStores = [&] {
        return takesStoresWithoutCode(vm, target);
    };
    ScopedLambda<bool()> mayReadAhead = takesStores;
    Result result = readOwnPropertiesCallingGetters(globalObject, source, heldNames, names, values, { .willRunCode = &willRunCode, .mayReadAhead = &mayReadAhead, .refusesProtoName = true });
    RETURN_IF_EXCEPTION(scope, true);
    if (result != Result::Completed)
        return false;
    scope.release();
    storeWhatIsRead();
    return true;
}

bool tryAssign(VM& vm, JSObject* compiledTarget, JSObject* source, bool& goesOnKeyByKey)
{
    Object* typed = compiledObjectOrNull(compiledTarget);
    if (typed && isAskedByCodeThatInspectorBrought(vm, *typed))
        return false;
    return typed && (tryAssignPropertiesOf(vm, *typed, source) || tryAssignInOrderOfCreation(vm, *typed, source, goesOnKeyByKey));
}

bool tryAssign(VM& vm, JSObject* compiledTarget, JSObject* source)
{
    bool goesOnKeyByKey = false;
    return tryAssign(vm, compiledTarget, source, goesOnKeyByKey);
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
