/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperandSnapshot.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "DateInstance.h"
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
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// A loop that calls a native function once per element of a large operand stays linear.
static constexpr unsigned maxCheckedProperties = 1024;
static constexpr size_t maxCheckedWordsAtEachEnd = 64;
static constexpr size_t maxCheckedBytesAtEachEnd = 2 * KB;

ASCIILiteral nameOf(OperandChange change)
{
    switch (change) {
    case OperandChange::PropertyAdded:
        return "property added"_s;
    case OperandChange::PropertyRemoved:
        return "property removed"_s;
    case OperandChange::PropertyStored:
        return "property stored"_s;
    case OperandChange::AttributesChanged:
        return "attributes changed"_s;
    case OperandChange::IntegrityChanged:
        return "integrity changed"_s;
    case OperandChange::PrototypeChanged:
        return "prototype changed"_s;
    case OperandChange::ShapeLost:
        return "shape lost"_s;
    case OperandChange::PristineStateLost:
        return "pristine state lost"_s;
    case OperandChange::ElementsChanged:
        return "elements changed"_s;
    case OperandChange::ArrayStorageChanged:
        return "array storage changed"_s;
    case OperandChange::InternalStateChanged:
        return "internal state changed"_s;
    case OperandChange::BytesChanged:
        return "bytes changed"_s;
    case OperandChange::ByteStorageChanged:
        return "byte storage changed"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename T>
static void forEachAtEnds(std::span<const T> values, size_t maxAtEachEnd, const Invocable<void(T)> auto& functor)
{
    size_t sizeOfStart = std::min(values.size(), maxAtEachEnd);
    size_t sizeOfEnd = std::min(values.size() - sizeOfStart, maxAtEachEnd);
    for (T value : values.first(sizeOfStart))
        functor(value);
    for (T value : values.last(sizeOfEnd))
        functor(value);
}

static uint64_t hashBytes(std::span<const uint8_t> bytes)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    forEachAtEnds(bytes, maxCheckedBytesAtEachEnd, [&](uint8_t byte) {
        hash = (hash ^ byte) * 0x100000001b3ull;
    });
    return hash;
}

static void appendWords(Vector<uint64_t>& captured, std::span<const uint64_t> words)
{
    forEachAtEnds(words, maxCheckedWordsAtEachEnd, [&](uint64_t word) {
        captured.append(word);
    });
}

static bool hasButterfly(JSType type)
{
    return type < objectTypesWithoutButterfly.first || type > objectTypesWithoutButterfly.last;
}

static void captureNamedProperties(JSObject* holder, OperandSnapshot& state)
{
    Structure* structure = holder->structure();
    if (structure->maxOffset() != invalidOffset && static_cast<unsigned>(structure->maxOffset()) > maxCheckedProperties) {
        state.hasTooManyProperties = true;
        return;
    }
    structure->forEachPropertyConcurrently([&](const PropertyTableEntry& entry) {
        state.properties.append({ entry.key(), entry.attributes(), JSValue::encode(holder->getDirect(entry.offset())) });
        return true;
    });
}

static void captureElements(JSObject* holder, OperandSnapshot& state)
{
    state.indexingMode = holder->indexingMode();
    Butterfly* butterfly = holder->butterfly();
    if (!butterfly || !hasIndexedProperties(holder->indexingType()))
        return;

    switch (holder->indexingType() & IndexingShapeMask) {
    case Int32Shape:
    case DoubleShape:
    case ContiguousShape:
        state.length = butterfly->publicLength();
        appendWords(state.elements, std::span<const uint64_t>(butterfly->indexingPayload<uint64_t>(), state.length));
        return;
    case ArrayStorageShape:
    case SlowPutArrayStorageShape: {
        ArrayStorage* storage = butterfly->arrayStorage();
        state.length = storage->length();
        state.elements.append(storage->m_numValuesInVector);
        appendWords(state.elements, std::span(reinterpret_cast<const uint64_t*>(storage->m_vector), storage->vectorLength()));
        return;
    }
    default:
        state.length = butterfly->publicLength();
        return;
    }
}

static void captureFields(const Object& object, OperandSnapshot& state)
{
    state.orderWord = object.orderWord();
    state.shapeWord = object.shapeWord();
    for (const FieldData& field : object.layout().fields()) {
        SlotRepresentation representation = field.slot.representation();
        const void* slot = representation == SlotRepresentation::DeferredClosure ? nullptr : object.slotOf(field);
        uint64_t bits = 0;
        if (slot)
            memcpy(&bits, slot, widthOf(representation));
        state.fields.append(bits);
        bool hasDoubleInExtension = slot && representation == SlotRepresentation::Int32OrDouble && static_cast<int32_t>(bits) == doubleInExtensionInt32;
        state.fields.append(hasDoubleInExtension ? *static_cast<const uint64_t*>(object.extension()->slot(object.layout().overflowOffsetOf(field))) : 0);
    }
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
    if (storage)
        state.internalState.append(hashBytes(std::span(reinterpret_cast<const uint8_t*>(storage), storage->cellSize()).subspan(sizeof(JSCell))));
}

static void captureBytes(const void* bytes, size_t byteLength, OperandSnapshot& state)
{
    state.bytes = bytes;
    state.byteLength = byteLength;
    state.hashOfBytes = bytes ? hashBytes(std::span(static_cast<const uint8_t*>(bytes), byteLength)) : 0;
}

static void captureInternalState(JSObject* object, OperandSnapshot& state)
{
    static_assert(sizeof(JSPromise) == sizeof(JSNonFinalObject) + 2 * sizeof(uint64_t));
    JSType type = object->type();
    if (isTypedArrayTypeIncludingDataView(type)) {
        auto* view = uncheckedDowncast<JSArrayBufferView>(object);
        captureBytes(view->vector(), view->byteLength(), state);
        return;
    }
    switch (type) {
    case ArrayBufferType: {
        ArrayBuffer* buffer = uncheckedDowncast<JSArrayBuffer>(object)->impl();
        captureBytes(buffer->data(), buffer->byteLength(), state);
        return;
    }
    case JSMapType:
        captureHashTable(object, JSMap::offsetOfStorage(), state);
        return;
    case JSSetType:
        captureHashTable(object, JSSet::offsetOfStorage(), state);
        return;
    case JSDateType:
        state.internalState.append(std::bit_cast<uint64_t>(uncheckedDowncast<DateInstance>(object)->internalNumber()));
        return;
    case RegExpObjectType:
        state.internalState.append(JSValue::encode(uncheckedDowncast<RegExpObject>(object)->getLastIndex()));
        return;
    case JSPromiseType:
        state.internalState.append(std::span(reinterpret_cast<const uint64_t*>(object) + sizeof(JSNonFinalObject) / sizeof(uint64_t), 2));
        return;
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
        if (JSObject* extra = compiled->extraProperties()) {
            captureNamedProperties(extra, state);
            captureElements(extra, state);
        }
    } else {
        // The Structure of a class that overrides isExtensible() says no for every cell.
        state.isExtensible = state.type == SlimClosureType || structure->isStructureExtensible();
        // An array overrides the lookup for its length alone.
        state.createsPropertiesLazily = !isJSArray(object) && (structure->typeInfo().overridesGetOwnPropertySlot() || structure->typeInfo().hasStaticPropertyTable());
        if (hasButterfly(state.type)) {
            captureNamedProperties(object, state);
            captureElements(object, state);
        }
        captureInternalState(object, state);
        state.isClassPrototypeWithoutMembers = object->classInfo() == ClassPrototype::info();
    }
    std::ranges::sort(state.properties, { }, [](const CapturedProperty& property) {
        return property.key.get();
    });
    return state;
}

bool canCapture(JSValue operand)
{
    if (!operand.isObject())
        return false;
    switch (asObject(operand)->type()) {
    case ProxyObjectType:
    case GlobalProxyType:
    case GlobalObjectType:
        return false;
    default:
        return true;
    }
}

static String describeKey(UniquedStringImpl* key)
{
    if (key->isSymbol())
        return makeString("[Symbol("_s, StringView(key), ")]"_s);
    return String(key);
}

static void compareNamedProperties(const OperandSnapshot& before, const OperandSnapshot& after, Vector<OperandDifference>& differences)
{
    if (before.hasTooManyProperties || after.hasTooManyProperties)
        return;
    auto old = before.properties.begin();
    auto current = after.properties.begin();
    while (old != before.properties.end() || current != after.properties.end()) {
        bool isRemoved = current == after.properties.end() || (old != before.properties.end() && old->key.get() < current->key.get());
        if (isRemoved) {
            differences.append({ OperandChange::PropertyRemoved, describeKey(old->key.get()), old->key });
            ++old;
            continue;
        }
        bool isAdded = old == before.properties.end() || current->key.get() < old->key.get();
        if (isAdded) {
            if (!after.createsPropertiesLazily)
                differences.append({ OperandChange::PropertyAdded, describeKey(current->key.get()), current->key, current->value });
            ++current;
            continue;
        }
        bool wasCreatedByFirstRead = after.type == SlimClosureWithPropertiesType && old->attributes == (current->attributes | PropertyAttribute::CustomValue);
        if (wasCreatedByFirstRead && old->attributes != current->attributes) {
            ++old;
            ++current;
            continue;
        }
        if (old->attributes != current->attributes)
            differences.append({ OperandChange::AttributesChanged, describeKey(old->key.get()), old->key, current->value });
        else if (old->value != current->value)
            differences.append({ OperandChange::PropertyStored, describeKey(old->key.get()), old->key, current->value });
        ++old;
        ++current;
    }
}

static void compareFields(VM& vm, const Object& object, const OperandSnapshot& before, const OperandSnapshot& after, Vector<OperandDifference>& differences)
{
    if (before.shapeWord != after.shapeWord) {
        differences.append({ OperandChange::ShapeLost, makeString("shape word "_s, before.shapeWord, " -> "_s, after.shapeWord) });
        return;
    }
    if ((before.orderWord ^ after.orderWord) & orderWordIntegrityBits)
        differences.append({ OperandChange::IntegrityChanged, "order word"_s });
    if ((before.orderWord ^ after.orderWord) & orderAndCountMask)
        differences.append({ OperandChange::PropertyAdded, makeString("key order "_s, before.orderWord & orderAndCountMask, " -> "_s, after.orderWord & orderAndCountMask) });

    std::span fields = object.layout().fields();
    RELEASE_ASSERT(before.fields.size() == 2 * fields.size() && after.fields.size() == before.fields.size());
    for (size_t index = 0; index < fields.size(); ++index) {
        if (before.fields[2 * index] != after.fields[2 * index] || before.fields[2 * index + 1] != after.fields[2 * index + 1])
            differences.append({ OperandChange::PropertyStored, describeKey(propertyNameOf(vm, object, fields[index])), propertyNameOf(vm, object, fields[index]) });
    }
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
    size_t numberOfFieldDifferences = differences.size();
    compareNamedProperties(before, after, differences);
    // A property that moves from the extra properties of a compiled object to a field is stored, not removed.
    auto isStoredInField = [&](const OperandDifference& difference) {
        return std::ranges::any_of(differences.span().first(numberOfFieldDifferences), [&](const OperandDifference& field) {
            return field.change == OperandChange::PropertyStored && field.key == difference.key;
        });
    };
    for (size_t index = differences.size(); index-- > numberOfFieldDifferences;) {
        if (differences[index].change == OperandChange::PropertyRemoved && isStoredInField(differences[index]))
            differences.removeAt(index);
    }

    if (before.length != after.length)
        differences.append({ OperandChange::ArrayStorageChanged, makeString("length "_s, before.length, " -> "_s, after.length) });
    else if (before.indexingMode != after.indexingMode)
        differences.append({ OperandChange::ArrayStorageChanged, makeString("indexing mode "_s, static_cast<unsigned>(before.indexingMode), " -> "_s, static_cast<unsigned>(after.indexingMode)) });
    else if (before.elements != after.elements && hasAnyArrayStorage(before.indexingMode)) {
        // A change of integrity moves the elements of a sparse array from the vector to the map.
        differences.append({ OperandChange::ArrayStorageChanged, "vector"_s });
    } else if (before.elements != after.elements) {
        // Another storage of the same length may hold fewer words.
        size_t index = std::ranges::mismatch(before.elements, after.elements).in1 - before.elements.begin();
        differences.append({ OperandChange::ElementsChanged, { }, nullptr, index < after.elements.size() ? static_cast<EncodedJSValue>(after.elements[index]) : 0 });
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

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
