/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperandSnapshot.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTCompactTable.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTSort.h"
#include "AOTVector.h"
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
#include <wtf/CommaPrinter.h>
#include <wtf/StringPrintStream.h>
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

String describe(AbstractHeaps heaps)
{
    StringPrintStream out;
    CommaPrinter comma("|"_s);
    for (AbstractHeap heap : heaps)
        out.print(comma, nameOf(heap));
    return out.toString();
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
        uint64_t bits = representation == SlotRepresentation::DeferredClosure && object.isInSlot(field);
        if (slot)
            memcpy(&bits, slot, widthOf(representation));
        state.fields.append(bits);
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
    if (!storage)
        return;
    std::span bytes(reinterpret_cast<const uint8_t*>(storage), storage->cellSize());
    // Whoever walks a table notes in the table that it has been walked, or how far the walk has come. No program sees the note.
    if (JSMap::Helper::isCompact(*storage)) {
        constexpr size_t offsetOfFlags = CompactTable::offsetOfKeysShift();
        state.internalState.append(hashBytes(bytes.subspan(sizeof(JSCell), offsetOfFlags - sizeof(JSCell))));
        state.internalState.append(bytes[offsetOfFlags] & ~CompactTable::wasWalked);
        state.internalState.append(hashBytes(bytes.subspan(offsetOfFlags + 1)));
        return;
    }
    static_assert(JSMap::Helper::iterationEntryIndex() == JSSet::Helper::iterationEntryIndex());
    constexpr size_t offsetOfIterationEntry = JSMap::Helper::offsetInStorageForIndex(JSMap::Helper::iterationEntryIndex());
    state.internalState.append(hashBytes(bytes.subspan(sizeof(JSCell), offsetOfIterationEntry - sizeof(JSCell))));
    state.internalState.append(hashBytes(bytes.subspan(offsetOfIterationEntry + sizeof(EncodedJSValue))));
}

// Another thread writes shared memory while the call runs: that the bytes differ behind the call says nothing about the call.
static void captureBytes(const void* bytes, size_t byteLength, bool isShared, OperandSnapshot& state)
{
    state.bytes = bytes;
    state.byteLength = byteLength;
    state.hashOfBytes = bytes && !isShared ? hashBytes(std::span(static_cast<const uint8_t*>(bytes), byteLength)) : 0;
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
        if (JSObject* extra = compiled->extraProperties()) {
            captureNamedProperties(extra, state);
            captureElements(extra, state);
        }
    } else {
        // The Structure of a class that overrides isExtensible() says no for every cell.
        state.isExtensible = state.type == SlimClosureType || structure->isStructureExtensible();
        // An array overrides the lookup for its length alone.
        state.createsPropertiesLazily |= !isJSArray(object) && (structure->typeInfo().overridesGetOwnPropertySlot() || structure->typeInfo().hasStaticPropertyTable());
        if (hasButterfly(state.type)) {
            captureNamedProperties(object, state);
            captureElements(object, state);
        }
        captureInternalState(object, state);
        state.isClassPrototypeWithoutMembers = object->classInfo() == ClassPrototype::info();
    }
    AOT::sort(state.properties, { }, [](const CapturedProperty& property) {
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
            if (!before.createsPropertiesLazily && !after.createsPropertiesLazily)
                differences.append({ OperandChange::PropertyAdded, describeKey(current->key.get()), current->key, current->value });
            ++current;
            continue;
        }
        // A value that native code makes at the first read and puts in the place of its getter is the same data property to a script.
        bool wasCreatedByFirstRead = old->attributes == (current->attributes | PropertyAttribute::CustomValue);
        if (wasCreatedByFirstRead && old->attributes != current->attributes) {
            ++old;
            ++current;
            continue;
        }
        if (old->attributes != current->attributes)
            differences.append({ OperandChange::AttributesChanged, describeKey(old->key.get()), old->key, old->value != current->value ? current->value : 0 });
        else if (old->value != current->value)
            differences.append({ OperandChange::PropertyStored, describeKey(old->key.get()), old->key, current->value });
        ++old;
        ++current;
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
        differences.append({ isGone ? OperandChange::PropertyRemoved : OperandChange::PropertyStored, describeKey(propertyNameOf(vm, object, fields[index])), propertyNameOf(vm, object, fields[index]) });
    }
}

// A deletion takes a key out of the order, and whatever else changes the order puts one in.
static void compareKeyOrder(const OperandSnapshot& before, const OperandSnapshot& after, Vector<OperandDifference>& differences)
{
    if (before.shapeWord != after.shapeWord || !((before.orderWord ^ after.orderWord) & orderAndCountMask))
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
    if (compiledObjectOrNull(object))
        compareKeyOrder(before, after, differences);

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
