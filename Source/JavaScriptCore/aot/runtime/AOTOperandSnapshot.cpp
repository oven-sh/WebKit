/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeCallValidation.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTCompactTable.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTPendingStars.h"
#include "DateInstance.h"
#include "GetterSetter.h"
#include "JSArrayBuffer.h"
#include "JSArrayBufferView.h"
#include "JSArrayIterator.h"
#include "JSCInlines.h"
#include "JSMap.h"
#include "JSMapIterator.h"
#include "JSPromise.h"
#include "JSSet.h"
#include "JSSetIterator.h"
#include "RegExpObject.h"
#include <algorithm>
#include <bit>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT {

// A loop that calls a native function once per element of a large operand stays linear.
static constexpr unsigned maxCheckedProperties = 1024;
static constexpr size_t maxCheckedWordsAtEachEnd = 64;
static constexpr size_t maxCheckedBytesAtEachEnd = 2 * KB;

const OperandChangeRow& rowOf(OperandChange change)
{
    using enum OperandUse;
    using enum AbstractHeap;
    static constexpr OperandUses storesProperties { StoresByKey, AssignsFrom, DefinesName, TakesDescriptor };
    static constexpr OperandUses writesElements = storesProperties | OperandUses { WritesContents, MovesElements, DeletesByKey };
    static constexpr AbstractHeaps properties { NamedProperties, ObjectFields };
    static constexpr AbstractHeaps shape { NamedProperties, ObjectFields, OrderWords, ShapeWords };
    static constexpr OperandChangeRow rows[] {
        { "property added"_s, storesProperties, properties },
        { "property removed"_s, { DeletesByKey, MovesElements }, properties },
        { "property stored"_s, storesProperties, properties },
        { "attributes changed"_s, { DefinesName, TakesDescriptor, ChangesIntegrity }, properties },
        { "integrity changed"_s, { ChangesIntegrity }, shape },
        { "prototype changed"_s, { ChangesPrototype }, { PrototypeLinks } },
        { "shape lost"_s, storesProperties | OperandUses { DeletesByKey, ChangesIntegrity, ChangesPrototype }, shape },
        { "pristine state lost"_s, { }, { PrototypeLinks, NamedProperties, ArrayStorage } },
        { "elements changed"_s, writesElements, { Elements } },
        // An array that is frozen in place shares its elements from then on, and one whose prototype may hold indexed accessors asks it at every store.
        { "array storage changed"_s, writesElements | OperandUses { ChangesIntegrity, ChangesPrototype }, { ArrayStorage } },
        { "internal state changed"_s, { WritesContents, MovesElements }, { } },
        { "bytes changed"_s, { WritesContents, MovesElements }, { TypedArrayElements } },
        { "byte storage changed"_s, { }, { TypedArrayStorage } },
    };
    static_assert(std::size(rows) == static_cast<size_t>(OperandChange::ByteStorageChanged) + 1);
    return rows[static_cast<size_t>(change)];
}

String describe(const OperandDifference& difference)
{
    return makeString(rowOf(difference.change).name, difference.detail.isEmpty() ? ""_s : " "_s, difference.detail);
}

AbstractHeaps heapsOfClassOf(JSObject* object)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    AbstractHeaps heaps;
    for (unsigned index = 0; index < table.classCount(); ++index) {
        const BuiltinClass& builtinClass = table.at(BuiltinClassID(index));
        if (builtinClass.instanceTest != BuiltinInstanceTest::JSTypeEquals || builtinClass.type != object->type())
            continue;
        for (BuiltinSlotID slot : builtinClass.slots)
            heaps.add(table.at(slot).heap);
        if (builtinClass.stateHeap)
            heaps.add(*builtinClass.stateHeap);
    }
    return heaps;
}

template<typename T>
static void forEachAtEnds(std::span<const T> values, size_t maxAtEachEnd, const Invocable<void(T)> auto& functor)
{
    for (size_t index = 0; index < values.size(); ++index) {
        if (index == maxAtEachEnd && values.size() > 2 * maxAtEachEnd)
            index = values.size() - maxAtEachEnd;
        functor(values[index]);
    }
}

static uint64_t hashSnapshotBytes(std::span<const uint8_t> bytes)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    auto mix = [&](uint64_t bits) {
        hash = (hash ^ bits) * 0x100000001b3ull;
    };
    auto mixAll = [&](std::span<const uint8_t> part) {
        for (; part.size() >= sizeof(uint64_t); part = part.subspan(sizeof(uint64_t)))
            mix(WTF::unalignedLoad<uint64_t>(part.data()));
        for (uint8_t byte : part)
            mix(byte);
    };
    if (bytes.size() <= 2 * maxCheckedBytesAtEachEnd)
        mixAll(bytes);
    else {
        mixAll(bytes.first(maxCheckedBytesAtEachEnd));
        mixAll(bytes.last(maxCheckedBytesAtEachEnd));
    }
    return hash;
}

static void appendWords(Vector<uint64_t>& captured, std::span<const uint64_t> words)
{
    forEachAtEnds(words, maxCheckedWordsAtEachEnd, [&](uint64_t word) {
        captured.append(word);
    });
}

static void capturePropertiesAndElements(JSObject* holder, OperandSnapshot& state)
{
    Structure* structure = holder->structure();
    state.hasTooManyProperties = structure->maxOffset() != invalidOffset && static_cast<unsigned>(structure->maxOffset()) > maxCheckedProperties;
    structure->forEachPropertyConcurrently([&](const PropertyTableEntry& entry) {
        if (state.hasTooManyProperties)
            return false;
        // A getter of native code may keep what it has made on the object, under the name or under another.
        state.createsPropertiesLazily |= !!(entry.attributes() & (PropertyAttribute::CustomValue | PropertyAttribute::CustomAccessor));
        JSValue value = holder->getDirect(entry.offset());
        EncodedJSValue captured = JSValue::encode(value);
        // Whoever defines an accessor anew with the functions that it has puts another cell in the place of the pair.
        if (auto* accessor = dynamicDowncast<GetterSetter>(value))
            captured = static_cast<EncodedJSValue>(std::bit_cast<uint64_t>(accessor->getter()) ^ std::rotl(std::bit_cast<uint64_t>(accessor->setter()), 17));
        state.properties.append({ entry.key(), entry.attributes(), captured });
        return true;
    });

    state.indexingMode = holder->indexingMode();
    Butterfly* butterfly = holder->butterfly();
    if (!butterfly || !hasIndexedProperties(holder->indexingType()))
        return;

    state.length = butterfly->publicLength();
    if (hasAnyArrayStorage(holder->indexingType())) {
        ArrayStorage* storage = butterfly->arrayStorage();
        state.elements.append(storage->m_numValuesInVector);
        appendWords(state.elements, std::span(reinterpret_cast<const uint64_t*>(storage->m_vector), storage->vectorLength()));
    } else if (!hasUndecided(holder->indexingType()))
        appendWords(state.elements, std::span<const uint64_t>(butterfly->indexingPayload<uint64_t>(), state.length));
}

static void captureFields(const Object& object, OperandSnapshot& state)
{
    state.orderWord = object.orderWord();
    state.shapeWord = object.shapeWord();
    for (const FieldData& field : object.layout().fields()) {
        SlotRepresentation representation = field.slot.representation();
        const void* slot = representation == SlotRepresentation::DeferredClosure ? nullptr : object.slotOf(field);
        uint64_t bits = representation == SlotRepresentation::DeferredClosure && object.isInSlot(field);
        if (slot)
            memcpy(&bits, slot, widthOf(representation));
        state.fields.append(bits);
    }
}

// A photograph is no look: the stars go on waiting, as they do in a run that takes none.
static void capturePendingStars(const Object& object, OperandSnapshot& state)
{
    state.hasPendingStars = true;
    forEachKeyOfPendingStars(object.vm(), object, [&](UniquedStringImpl* key, unsigned attributes) {
        state.hasTooManyProperties |= state.properties.size() >= maxCheckedProperties;
        if (!state.hasTooManyProperties)
            state.properties.append({ key, attributes, 0 });
    });
}

template<typename Iterator>
static void captureInternalFields(JSObject* object, OperandSnapshot& state)
{
    auto* iterator = uncheckedDowncast<Iterator>(object);
    for (unsigned index = 0; index < Iterator::numberOfInternalFields; ++index)
        state.internalState.append(JSValue::encode(iterator->JSInternalFieldObjectImpl<Iterator::numberOfInternalFields>::internalField(index).get()));
}

static void captureHashTable(JSObject* object, ptrdiff_t offsetOfStorage, OperandSnapshot& state)
{
    JSCell* storage = *reinterpret_cast<JSCell**>(reinterpret_cast<uint8_t*>(object) + offsetOfStorage);
    state.internalState.append(std::bit_cast<uint64_t>(storage));
    if (!storage)
        return;
    std::span bytes(reinterpret_cast<const uint8_t*>(storage), storage->cellSize());
    // Whoever walks a table notes in the table that it has been walked, or how far the walk has come. No program sees the note.
    if (JSMap::Helper::isCompact(*storage)) {
        constexpr size_t offsetOfFlags = CompactTable::offsetOfKeysShift();
        state.internalState.append(hashSnapshotBytes(bytes.subspan(sizeof(JSCell), offsetOfFlags - sizeof(JSCell))));
        state.internalState.append(bytes[offsetOfFlags] & ~CompactTable::wasWalked);
        state.internalState.append(hashSnapshotBytes(bytes.subspan(offsetOfFlags + 1)));
        return;
    }
    static_assert(JSMap::Helper::iterationEntryIndex() == JSSet::Helper::iterationEntryIndex());
    constexpr size_t offsetOfIterationEntry = JSMap::Helper::offsetInStorageForIndex(JSMap::Helper::iterationEntryIndex());
    state.internalState.append(hashSnapshotBytes(bytes.subspan(sizeof(JSCell), offsetOfIterationEntry - sizeof(JSCell))));
    state.internalState.append(hashSnapshotBytes(bytes.subspan(offsetOfIterationEntry + sizeof(EncodedJSValue))));
}

// Another thread may write and grow shared memory during the call, so a difference in the bytes or their count after the call says nothing about the call.
static void captureBytes(const void* bytes, size_t byteLength, bool isShared, OperandSnapshot& state)
{
    state.bytes = bytes;
    state.byteLength = isShared ? 0 : byteLength;
    state.hashOfBytes = bytes && !isShared ? hashSnapshotBytes(std::span(static_cast<const uint8_t*>(bytes), byteLength)) : 0;
}

static void captureInternalState(JSObject* object, OperandSnapshot& state)
{
    static_assert(sizeof(JSPromise) == sizeof(JSNonFinalObject) + 2 * sizeof(uint64_t));
    JSType type = object->type();
    if (isTypedArrayTypeIncludingDataView(type)) {
        auto* view = uncheckedDowncast<JSArrayBufferView>(object);
        captureBytes(view->vector(), view->byteLength(), view->isShared(), state);
        return;
    }
    switch (type) {
    case ArrayBufferType: {
        ArrayBuffer* buffer = uncheckedDowncast<JSArrayBuffer>(object)->impl();
        captureBytes(buffer->data(), buffer->byteLength(), buffer->isShared(), state);
        return;
    }
    case JSMapType:
    case JSSetType:
        captureHashTable(object, type == JSMapType ? JSMap::offsetOfStorage() : JSSet::offsetOfStorage(), state);
        return;
    case JSDateType:
        state.internalState.append(std::bit_cast<uint64_t>(uncheckedDowncast<DateInstance>(object)->internalNumber()));
        return;
    case RegExpObjectType:
        state.internalState.append(JSValue::encode(uncheckedDowncast<RegExpObject>(object)->getLastIndex()));
        return;
    case JSPromiseType: {
        // Whether somebody handles a rejection is nothing that a program can ask a promise for.
        using Packed = CompactPointerTuple<JSCell*, uint16_t>;
        static const uint64_t handledBit = std::bit_cast<uint64_t>(Packed(nullptr, JSPromise::isHandledFlag)) ^ std::bit_cast<uint64_t>(Packed(nullptr, 0));
        auto wordAt = [&](ptrdiff_t offset) { return *reinterpret_cast<const uint64_t*>(reinterpret_cast<const char*>(object) + offset); };
        state.internalState.append(wordAt(JSPromise::offsetOfPacked()) & ~handledBit);
        state.internalState.append(wordAt(JSPromise::offsetOfSlot()));
        return;
    }
    case JSArrayIteratorType:
        captureInternalFields<JSArrayIterator>(object, state);
        return;
    case JSMapIteratorType:
        captureInternalFields<JSMapIterator>(object, state);
        return;
    case JSSetIteratorType:
        captureInternalFields<JSSetIterator>(object, state);
        return;
    default:
        return;
    }
}

OperandSnapshot capture(JSObject* object)
{
    OperandSnapshot state;
    Structure* structure = object->structure();
    state.type = object->type();
    state.isPristine = TypeInfo::isPristine(object->inlineTypeFlags());
    state.prototype = JSValue::encode(structure->storedPrototype(object));

    if (Object* compiled = compiledObjectOrNull(object)) {
        state.isExtensible = !compiled->isNotExtensible();
        captureFields(*compiled, state);
        if (hasPendingStars(*compiled))
            capturePendingStars(*compiled, state);
        else if (JSObject* extra = compiled->extraProperties())
            capturePropertiesAndElements(extra, state);
    } else {
        // The Structure of a class that overrides isExtensible() says no for every cell.
        state.isExtensible = state.type == SlimClosureType || structure->isStructureExtensible();
        // An array overrides the lookup for its length alone.
        state.createsPropertiesLazily |= !isJSArray(object) && (structure->typeInfo().overridesGetOwnPropertySlot() || structure->typeInfo().hasStaticPropertyTable());
        if (!objectTypesWithoutButterfly.contains(state.type))
            capturePropertiesAndElements(object, state);
        captureInternalState(object, state);
        state.isClassPrototypeWithoutMembers = object->classInfo() == ClassPrototype::info();
    }
    return state;
}

bool canCapture(JSValue operand)
{
    if (!operand.isObject())
        return false;
    JSType type = asObject(operand)->type();
    return type != ProxyObjectType && type != GlobalProxyType && type != GlobalObjectType;
}

static OperandDifference differenceAt(OperandChange change, UniquedStringImpl* key, EncodedJSValue arrived = 0)
{
    return { change, key->isSymbol() ? makeString("[Symbol("_s, StringView(key), ")]"_s) : String(key), key, arrived };
}

// A field that holds undefined may exist without storage.
static bool liesInField(VM& vm, const Object& object, const FieldData& field)
{
    if (object.isInSlot(field))
        return true;
    return field.flags & fieldMayExistWithoutStorage && object.isInKeyOrder(object.state(), field) && !(object.hasExtraProperties() && livesInExtraProperties(vm, object, field));
}

// `compiled` is as `after` has it.
static void compareNamedProperties(VM& vm, const Object* compiled, const OperandSnapshot& before, const OperandSnapshot& after, Vector<OperandDifference>& differences)
{
    if (before.hasTooManyProperties || after.hasTooManyProperties)
        return;
    auto find = [](const OperandSnapshot& state, const CapturedProperty& property) {
        size_t index = state.properties.findIf([&](const CapturedProperty& other) {
            return other.key == property.key;
        });
        return index == notFound ? nullptr : &state.properties[index];
    };
    for (const CapturedProperty& old : before.properties) {
        UniquedStringImpl* key = old.key.get();
        const CapturedProperty* current = find(after, old);
        if (!current) {
            // A property that moves from the extra properties of a compiled object to a field is stored, not removed.
            bool isStoredInField = differences.containsIf([&](const OperandDifference& field) {
                return field.change == OperandChange::PropertyStored && field.key == key;
            });
            // A slot that its field has left keeps its bits, and the value that returns may have the same bits. If the shape word changed, the fields were not compared.
            bool hasMovedToField = compiled && std::ranges::any_of(compiled->layout().fields(), [&](const FieldData& field) {
                return propertyNameOf(vm, *compiled, field) == key && liesInField(vm, *compiled, field);
            });
            if (!isStoredInField)
                differences.append(differenceAt(hasMovedToField ? OperandChange::PropertyStored : OperandChange::PropertyRemoved, key));
        } else if (old.attributes != current->attributes) {
            // A value that native code makes at the first read and puts in the place of its getter is the same data property to a script.
            if (old.attributes != (current->attributes | PropertyAttribute::CustomValue))
                differences.append(differenceAt(OperandChange::AttributesChanged, key, old.value != current->value ? current->value : 0));
        } else if (old.value && old.value != current->value) {
            // A native accessor may store what it was given on the object under a private name.
            bool isKeptByNativeAccessor = before.createsPropertiesLazily && key->isSymbol() && static_cast<SymbolImpl*>(key)->isPrivate();
            if (!isKeptByNativeAccessor)
                differences.append(differenceAt(OperandChange::PropertyStored, key, current->value));
        }
    }
    if (before.createsPropertiesLazily || after.createsPropertiesLazily)
        return;
    for (const CapturedProperty& current : after.properties) {
        if (!find(before, current))
            differences.append(differenceAt(OperandChange::PropertyAdded, current.key.get(), current.value));
    }
}

// `object` is as `after` has it.
static void compareFields(VM& vm, const Object& object, const OperandSnapshot& before, const OperandSnapshot& after, Vector<OperandDifference>& differences)
{
    if (before.shapeWord != after.shapeWord) {
        differences.append({ OperandChange::ShapeLost, makeString("shape word "_s, before.shapeWord, " -> "_s, after.shapeWord) });
        return;
    }
    if ((before.orderWord ^ after.orderWord) & orderWordIntegrityBits)
        differences.append({ OperandChange::IntegrityChanged, "order word"_s });

    std::span fields = object.layout().fields();
    RELEASE_ASSERT(before.fields.size() == fields.size() && after.fields.size() == before.fields.size());
    for (size_t index = 0; index < fields.size(); ++index) {
        if (before.fields[index] == after.fields[index])
            continue;
        // A field that has left its slot and lies nowhere else is deleted.
        bool isGone = !object.isInSlot(fields[index]) && !(object.hasExtraProperties() && livesInExtraProperties(vm, object, fields[index]));
        differences.append(differenceAt(isGone ? OperandChange::PropertyRemoved : OperandChange::PropertyStored, propertyNameOf(vm, object, fields[index])));
    }
}

// A deletion takes a key out of the order, and whatever else changes the order puts one in.
static void compareKeyOrder(const OperandSnapshot& before, const OperandSnapshot& after, Vector<OperandDifference>& differences)
{
    if (before.shapeWord != after.shapeWord || !((before.orderWord ^ after.orderWord) & orderAndCountMask))
        return;
    // Stars that are carried out list the fields whose names they bind.
    if (before.hasPendingStars && !after.hasPendingStars)
        return;
    auto has = [&](OperandChange change) {
        return std::ranges::contains(differences, change, &OperandDifference::change);
    };
    bool onlyRemoves = has(OperandChange::PropertyRemoved) && !has(OperandChange::PropertyAdded) && !has(OperandChange::PropertyStored);
    differences.append({ onlyRemoves ? OperandChange::PropertyRemoved : OperandChange::PropertyAdded, makeString("key order "_s, before.orderWord & orderAndCountMask, " -> "_s, after.orderWord & orderAndCountMask) });
}

Vector<OperandDifference> differencesBetween(VM& vm, JSObject* object, const OperandSnapshot& before, const OperandSnapshot& after)
{
    Vector<OperandDifference> differences;
    if (before.prototype != after.prototype)
        differences.append({ OperandChange::PrototypeChanged, { } });
    if (before.isExtensible != after.isExtensible)
        differences.append({ OperandChange::IntegrityChanged, "extensibility"_s });
    if (before.isPristine && !after.isPristine)
        differences.append({ OperandChange::PristineStateLost, { } });

    if (before.type != after.type) {
        // At the first lookup of a property a slim closure gets room for properties, and the prototype of a class its members.
        if (!isFunctionType(before.type) && !before.isClassPrototypeWithoutMembers)
            differences.append({ OperandChange::ShapeLost, makeString("cell type "_s, static_cast<unsigned>(before.type), " -> "_s, static_cast<unsigned>(after.type)) });
        return differences;
    }

    if (Object* compiled = compiledObjectOrNull(object))
        compareFields(vm, *compiled, before, after, differences);
    compareNamedProperties(vm, compiledObjectOrNull(object), before, after, differences);
    if (compiledObjectOrNull(object))
        compareKeyOrder(before, after, differences);

    if (before.length != after.length)
        differences.append({ OperandChange::ArrayStorageChanged, makeString("length "_s, before.length, " -> "_s, after.length) });
    else if (before.indexingMode != after.indexingMode) {
        differences.append({ OperandChange::ArrayStorageChanged, makeString("indexing mode "_s, static_cast<unsigned>(before.indexingMode), " -> "_s, static_cast<unsigned>(after.indexingMode)) });
        // Once an indexed accessor is on the prototype chain of an object with elements, untouched objects check their chain on every store. Their contents are unchanged.
        differences.last().isRepresentationOnly = hasSlowPutArrayStorage(after.indexingMode);
    }
    else if (before.elements != after.elements && hasAnyArrayStorage(before.indexingMode)) {
        // A change of integrity moves the elements of a sparse array from the vector to the map.
        differences.append({ OperandChange::ArrayStorageChanged, "vector"_s });
    } else if (before.elements != after.elements) {
        // Another storage of the same length may hold fewer words.
        size_t index = std::ranges::mismatch(before.elements, after.elements).in1 - before.elements.begin();
        differences.append({ OperandChange::ElementsChanged, { }, nullptr, index < after.elements.size() ? static_cast<EncodedJSValue>(after.elements[index]) : 0, static_cast<uint32_t>(index) });
    }

    if (before.internalState != after.internalState)
        differences.append({ OperandChange::InternalStateChanged, { } });
    if (before.byteLength != after.byteLength)
        differences.append({ OperandChange::InternalStateChanged, makeString("byte length "_s, before.byteLength, " -> "_s, after.byteLength) });
    else if (before.bytes != after.bytes)
        differences.append({ OperandChange::ByteStorageChanged, { } });
    else if (before.hashOfBytes != after.hashOfBytes)
        differences.append({ OperandChange::BytesChanged, { } });
    return differences;
}

} } // namespace JSC::AOT

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
