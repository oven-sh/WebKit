/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTKeyOrderTable.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTState.h"
#include "DeferGC.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSPropertyNameEnumerator.h"
#include "StructureChain.h"
#include "VMInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static bool hasNumberedKeyOrder(const Object& object)
{
    return !object.hasOutOfLineKeyOrder() && !object.layout().keysAreNeverEnumerated;
}

static bool holdsValue(const Object& object, const FieldData& listedField)
{
    SlotRepresentation representation = listedField.slot.representation();
    if (representation == SlotRepresentation::DeferredClosure)
        return true;
    const void* slot = object.slotOf(listedField);
    if (!slot)
        return listedField.flags & fieldMayExistWithoutStorage;
    return !slotHoldsAbsent(representation, slot);
}

std::optional<std::span<const FieldAsProperty>> Object::fieldsInKeyOrderSlow() const
{
    validateKeyListing(vm(), *this);
    if (hasExtraProperties() || !hasNumberedKeyOrder(*this))
        return std::nullopt;
    State& state = this->state();
    uint32_t orderAndCount = orderWord() & orderAndCountMask;
    auto fields = state.objects->keyOrders.propertiesInOrder(*state.instance.vm, layout(), orderAndCount);
    if (fields)
        compiledStructure().setListedFields({ orderAndCount, static_cast<uint32_t>(fields->size()), fields->data() });
    return fields;
}

bool Object::forEachFieldAsProperty(const ScopedLambda<bool(UniquedStringImpl&, JSValue)>& functor, SymbolKeys symbolKeys) const
{
    if (hasExtraProperties())
        return false;
    State& state = this->state();
    VM& vm = *state.instance.vm;
    validateKeyListing(vm, *this);
    std::optional<std::span<const FieldAsProperty>> properties;
    if (symbolKeys == SymbolKeys::Skip || !layout().hasFieldsWithSymbolKeys)
        properties = fieldsInKeyOrder();
    else if (hasNumberedKeyOrder(*this))
        properties = state.objects->keyOrders.propertiesInOrder(vm, layout(), orderWord() & orderAndCountMask, symbolKeys);
    if (properties) {
        for (auto& property : *properties) {
            JSValue value = readListedField(property);
            if (value && !functor(*property.name, value))
                break;
        }
        return true;
    }
    RuntimeLayoutTable& layouts = state.objects->layouts;
    std::span<const FieldData> fields = layout().fields();
    bool shouldContinue = true;
    forEachFieldInKeyOrder(state, [&](uint16_t index) {
        const FieldData& field = fields[index];
        if (!shouldContinue || !(layouts.hasStringKey(field) || (symbolKeys == SymbolKeys::Include && layouts.hasSymbolKey(field))))
            return;
        if (JSValue value = read(field))
            shouldContinue = functor(*layouts.nameOf(vm, field), value);
    });
    return true;
}

CachedPropertyList* cachedPropertyList(VM& vm, const Object& object)
{
    validateKeyListing(vm, object);
    JSObject* extra = object.extraProperties();
    if (!extra || !hasNumberedKeyOrder(object))
        return nullptr;
    State& state = object.state();
    const LayoutData& layout = object.layout();
    if (layout.hasFieldsWithSymbolKeys)
        return nullptr;
    PropertyListCache& propertyLists = state.objects->caches->propertyLists;
    Structure* structure = extra->structure();
    uint32_t shapeWord = object.shapeWord();
    uint32_t orderAndCount = object.orderWord() & orderAndCountMask;
    auto& cachedEntry = propertyLists.entryFor(structure->id(), shapeWord, orderAndCount);
    if (cachedEntry.extraPropertiesStructureID == structure->id() && cachedEntry.shapeWord == shapeWord && cachedEntry.orderAndCount == orderAndCount) {
        for (auto& property : cachedEntry.properties) {
            if (property.field && !holdsValue(object, *property.field))
                return nullptr;
        }
        return &cachedEntry;
    }
    if (extra->type() != FinalObjectType || structure->isDictionary() || structure->hasNonEnumerableProperties() || !structure->canPerformFastPropertyEnumeration() || structure->hasNonReifiedStaticProperties())
        return nullptr;
    RuntimeLayoutTable& layouts = state.objects->layouts;
    std::span<const FieldData> fields = layout.fields();
    Vector<PropertyListEntry, 16> extraKeys;
    bool hasSymbolKey = false;
    structure->forEachProperty(vm, [&](const auto& entry) {
        hasSymbolKey = entry.key()->isSymbol();
        if (!hasSymbolKey && !object.fieldNamed(state, entry.key()))
            extraKeys.append(PropertyListEntry { entry.key(), nullptr, entry.offset() });
        return !hasSymbolKey;
    });
    if (hasSymbolKey)
        return nullptr;
    Vector<PropertyListEntry> properties;
    bool hasAllFields = true;
    Object::KeyOrderStorage storage;
    size_t nextExtraKey = Object::forEachPositionedKey(object.keyOrder(state, storage), layout.numberOfFields, extraKeys.size(), [&](uint16_t index) {
        const FieldData& field = fields[index];
        if (!layouts.hasStringKey(field))
            return;
        hasAllFields &= holdsValue(object, field);
        properties.append(PropertyListEntry { layouts.nameOf(vm, field), &field, invalidOffset });
    }, [&](size_t ordinal) {
        properties.append(extraKeys[ordinal]);
    });
    if (!hasAllFields)
        return nullptr;
    for (; nextExtraKey < extraKeys.size(); ++nextExtraKey)
        properties.append(extraKeys[nextExtraKey]);
    // A collection since the first lookup has forgotten that the cache was written, so this records it again.
    auto& newEntry = propertyLists.entryFor(structure->id(), shapeWord, orderAndCount);
    newEntry = { structure->id(), shapeWord, orderAndCount, nullptr, WTF::move(properties) };
    return &newEntry;
}

JSValue Object::readPropertyListEntry(const PropertyListEntry& property, JSObject* extra) const
{
    if (!property.field)
        return extra->getDirect(property.offsetInExtraProperties);
    JSValue value = readInline(*property.field);
    return value ? value : read(*property.field);
}

bool Object::keysDependOnlyOnStructureAndOrderWord() const
{
    return hasNumberedKeyOrder(*this) && (!hasExtraProperties() || compiledStructure().listsEveryExtraProperty());
}

JSCellButterfly* Object::keysWithExtraProperties(VM& vm) const
{
    DeferGC deferGC(vm);
    auto* listing = cachedPropertyList(vm, *this);
    if (!listing || listing->keys)
        return listing ? listing->keys : nullptr;
    JSCellButterfly* list = JSCellButterfly::tryCreate(vm, CopyOnWriteArrayWithContiguous, listing->properties.size());
    if (!list)
        return nullptr;
    for (unsigned i = 0; i < listing->properties.size(); ++i)
        list->setIndex(vm, i, jsOwnedString(vm, listing->properties[i].name));
    listing->keys = list;
    return list;
}

JSArray* Object::tryCreateKeysArrayWithExtraProperties(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    DeferGC deferGC(vm);
    JSCellButterfly* keys = keysWithExtraProperties(vm);
    if (!keys)
        return nullptr;
    if (keysDependOnlyOnStructureAndOrderWord())
        return JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(keys->indexingMode()), keys->toButterfly());
    unsigned length = keys->length();
    JSArray* list = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), length);
    if (!list)
        return nullptr;
    auto values = list->butterfly()->contiguous();
    for (unsigned i = 0; i < length; ++i)
        values.at(list, i).setWithoutWriteBarrier(keys->get(i));
    return list;
}

const Vector<PropertyListEntry>* Object::fieldsAndExtraPropertiesInKeyOrder(VM& vm) const
{
    auto* listing = cachedPropertyList(vm, *this);
    return listing ? &listing->properties : nullptr;
}

JSCellButterfly* Object::keys(VM& vm) const
{
    validateKeyListing(vm, *this);
    if (hasExtraProperties() || !hasNumberedKeyOrder(*this))
        return nullptr;
    State& state = this->state();
    uint32_t shapeWord = this->shapeWord();
    auto& cacheEntry = state.instance.objects.keyLists[indexOfKeyList(shapeWord, orderWord())];
    if (cacheEntry.shapeWord == shapeWord && cacheEntry.orderWord == orderWord())
        return uncheckedDowncast<JSCellButterfly>(cacheEntry.list);
    JSCellButterfly* list = state.objects->keyOrders.keysInOrder(vm, layout(), orderWord() & orderAndCountMask);
    if (list)
        cacheEntry = { orderWord(), shapeWord, list };
    return list;
}

static std::optional<uint32_t> countKeysInOrderWord(const Object& object)
{
    auto properties = object.fieldsInKeyOrder();
    if (!properties)
        return std::nullopt;
    for (auto& property : *properties) {
        if (!holdsValue(object, *property.field))
            return std::nullopt;
    }
    return static_cast<uint32_t>(properties->size());
}

static bool canCacheEnumeratorFor(StructureChain* prototypeChain)
{
    for (StructureID* current = prototypeChain->head(); *current; ++current) {
        Structure* structure = current->decode();
        if (!structure->canCacheOwnPropertyNames() && structure->classInfoForCells() != ClassPrototype::info())
            return false;
    }
    return true;
}

JSPropertyNameEnumerator* Object::propertyNameEnumerator(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = this->state();
    KeyOrderTable& keyOrders = state.objects->keyOrders;
    const LayoutData& layout = this->layout();
    Structure* structureBefore = structure();
    uint32_t orderWordBefore = orderWord();

    std::optional<uint32_t> numberOfOwnNames = countKeysInOrderWord(*this);
    bool isCacheable = numberOfOwnNames && !compiledStructure().hasOtherPrototype();
    StructureChain* prototypeChainBefore = nullptr;
    if (isCacheable) {
        prototypeChainBefore = structureBefore->prototypeChain(vm, globalObject, this);
        if (JSPropertyNameEnumerator* cached = keyOrders.enumerator(layout, orderWordBefore, prototypeChainBefore)) {
            keyOrders.publishEnumerator(*this, cached);
            return cached;
        }
    }

    uint32_t indexedLength = 0;
    uint32_t numberOfStructureProperties = 0;
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    getEnumerablePropertyNames(globalObject, this, names, indexedLength, numberOfStructureProperties);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ASSERT(!indexedLength && !numberOfStructureProperties);

    // A Proxy on the chain may have run code that turned this cell into an ordinary object.
    bool isUnchanged = structure() == structureBefore && orderWord() == orderWordBefore;
    JSPropertyNameEnumerator* enumerator = nullptr;
    if (!names.size())
        enumerator = vm.emptyPropertyNameEnumerator();
    else if (numberOfOwnNames && isUnchanged)
        enumerator = JSPropertyNameEnumerator::tryCreate(vm, structureBefore, 0, *numberOfOwnNames, WTF::move(names), orderWordBefore & orderAndCountMask, orderAndCountMask | orderWordHasExtraProperties);
    else
        enumerator = JSPropertyNameEnumerator::tryCreate(vm, structure(), 0, 0, WTF::move(names));
    if (!enumerator) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }

    bool sawPolyProto;
    if (isCacheable && isUnchanged && normalizePrototypeChain(globalObject, this, sawPolyProto) != InvalidPrototypeChain) {
        if (structureBefore->prototypeChain(vm, globalObject, this) == prototypeChainBefore && canCacheEnumeratorFor(prototypeChainBefore)) {
            keyOrders.setEnumerator(vm, layout, orderWordBefore, enumerator, prototypeChainBefore);
            keyOrders.publishEnumerator(*this, enumerator);
        }
    }
    return enumerator;
}

PropertyListCache::PropertyListCache(State& state)
    : m_state(state)
{
    state.caches->add(CacheKind::ClearedEachCollection, "property lists"_s, *this);
}

PropertyListCache::~PropertyListCache() = default;

CachedPropertyList& PropertyListCache::entryFor(StructureID extraPropertiesStructureID, uint32_t shapeWord, uint32_t orderAndCount)
{
    if (!m_entries)
        m_entries = makeUniqueWithoutFastMallocCheck<Entries>();
    m_writes.recordAll();
    return (*m_entries)[((extraPropertiesStructureID.bits() >> 4) + shapeWord + orderAndCount * 31) % numberOfEntries];
}

void PropertyListCache::clear()
{
    m_writes.forEachRegionToClear([&](size_t, size_t) {
        for (auto& listing : *m_entries)
            listing = { };
        for (auto& cacheEntry : std::span { m_state.instance.objects.keyLists, numberOfKeyLists }) {
            if (cacheEntry.orderWord & orderWordHasExtraProperties)
                cacheEntry = { };
        }
    });
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
