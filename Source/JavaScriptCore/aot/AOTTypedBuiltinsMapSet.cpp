/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedCompactTable.h"
#include "AOTTypedObject.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSMapIterator.h"
#include "JSSetInlines.h"
#include "JSSetIterator.h"
#include "JSWeakMap.h"
#include "JSWeakSet.h"
#include <wtf/text/IntegerToStringConversion.h>
#include <wtf/text/RapidHash.h>

namespace JSC { namespace AOT {

using Typed::CompactTable;

static JSArray* asContiguousArray(JSValue value, unsigned minLength)
{
    JSArray* array = value.isCell() ? dynamicDowncast<JSArray>(value.asCell()) : nullptr;
    if (!array || (array->indexingType() & IndexingShapeMask) != ContiguousShape || array->butterfly()->publicLength() < minLength)
        return nullptr;
    return array;
}

static JSCell* tryNewSetFromSideEffectFreeIterable(JSGlobalObject* globalObject, Structure* structure, JSValue given)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!given.isCell() || given.asCell()->type() == JSSetType || !JSSet::isAddFastAndNonObservable(structure))
        return nullptr;
    JSArray* walked = constructEmptyArray(globalObject, nullptr);
    RETURN_IF_EXCEPTION(scope, nullptr);
    bool isWalked = Typed::CollectionIteration::tryAppendTo(globalObject, walked, given);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!isWalked)
        return nullptr;
    JSSet* set = JSSet::create(vm, structure);
    unsigned length = walked->length();
    bool isCompact = length <= CompactTable::maxCapacity;
    for (unsigned i = 0; i < length; ++i) {
        JSValue key = walked->getIndexQuickly(i);
        if (isCompact) {
            isCompact = CompactTable::tryAdd(globalObject, set, key, JSValue(), CompactTable::CreateIfNeeded::Yes, length);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (isCompact)
                continue;
        }
        set->add(globalObject, key);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return set;
}

template<typename Table>
static JSCell* tryNewTableWithStringKeys(JSGlobalObject* globalObject, Structure* structure, JSValue given)
{
    constexpr bool isMap = std::is_same_v<Table, JSMap>;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asContiguousArray(given, 1);
    if constexpr (!isMap) {
        if (!list)
            RELEASE_AND_RETURN(scope, tryNewSetFromSideEffectFreeIterable(globalObject, structure, given));
    }
    if (!list || !list->isIteratorProtocolFastAndNonObservable())
        return nullptr;
    unsigned length = list->butterfly()->publicLength();
    if (length > CompactTable::maxCapacity)
        return nullptr;
    auto entryAt = [&](unsigned i) -> std::pair<JSValue, JSValue> {
        JSValue element = list->butterfly()->contiguous().at(list, i).get();
        if constexpr (!isMap)
            return { element, JSValue() };
        JSArray* pair = element ? asContiguousArray(element, 2) : nullptr;
        if (!pair || !globalObject->isOriginalArrayStructure(pair->structure()))
            return { };
        return { pair->butterfly()->contiguous().at(pair, 0).get(), pair->butterfly()->contiguous().at(pair, 1).get() };
    };
    for (unsigned i = 0; i < length; ++i) {
        auto [key, value] = entryAt(i);
        if (!key || !key.isString() || (isMap && !value))
            return nullptr;
    }
    Table* table = Table::create(vm, structure);
    for (unsigned i = 0; i < length; ++i) {
        auto [key, value] = entryAt(i);
        bool isAdded = CompactTable::tryAdd(globalObject, table, key, value, CompactTable::CreateIfNeeded::Yes, length);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!isAdded)
            return nullptr;
    }
    return table;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedTryNewStringSet, JSCell*, (Instance* instance, EncodedJSValue list))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, tryNewTableWithStringKeys<JSSet>(globalObject, globalObject->setStructure(), JSValue::decode(list)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedTryNewMapWithStringKeys, JSCell*, (Instance* instance, EncodedJSValue list))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, tryNewTableWithStringKeys<JSMap>(globalObject, globalObject->mapStructure(), JSValue::decode(list)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewSetInImage, JSCell*, (Instance* instance, uint32_t table))
{
    auto& data = instance->typed->data;
    return CompactTable::createSetInImage(instance->globalObject, *data.at<Typed::TableInImage>(data.at<uint32_t>(data.tablesInImageOffset)[table]));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewMapInImage, JSCell*, (Instance* instance, uint32_t table))
{
    auto& data = instance->typed->data;
    return CompactTable::createMapInImage(instance->globalObject, *data.at<Typed::TableInImage>(data.at<uint32_t>(data.tablesInImageOffset)[table]));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedMoveTableToHeap, JSCell*, (Instance*, JSCell* collection))
{
    if (collection->type() == JSMapType)
        return CompactTable::heapTableOf(uncheckedDowncast<JSMap>(collection));
    return CompactTable::heapTableOf(uncheckedDowncast<JSSet>(collection));
}

template<typename Traits>
static size_t nextEntry(Instance* instance, JSCell* collection, EncodedJSValue encodedTable, uint32_t entry)
{
    JSValue table = JSValue::decode(encodedTable);
    auto position = CompactTable::next(*instance->vm, uncheckedDowncast<JSOrderedHashTable<Traits>>(collection), table.isCell() ? table.asCell() : nullptr, entry);
    return position.table ? position.entry : UINT32_MAX;
}

template<typename Traits>
static EncodedJSValue entryPart(JSCell* collection, uint32_t entry, bool isKey)
{
    using Helper = JSOrderedHashTableHelper<Traits>;
    auto* owner = uncheckedDowncast<JSOrderedHashTable<Traits>>(collection);
    if (owner->m_compactTable) {
        auto* table = CompactTable::heapTableOf(owner);
        return JSValue::encode(isKey ? JSValue(table->keyAt(entry)) : table->valueAt(entry));
    }
    if constexpr (Traits::hasValueData) {
        if (!isKey)
            return JSValue::encode(Helper::getValue(owner->storageRef(), entry));
    }
    return JSValue::encode(Helper::getKey(owner->storageRef(), entry));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedMapDelete, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(uncheckedDowncast<JSMap>(map)->remove(globalObject, JSValue::decode(key)))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetDelete, EncodedJSValue, (Instance* instance, JSCell* set, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(uncheckedDowncast<JSSet>(set)->remove(globalObject, JSValue::decode(key)))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewMapIterator, JSCell*, (Instance* instance, JSCell* map, uint32_t kind))
{
    return JSMapIterator::create(*instance->vm, instance->globalObject->mapIteratorStructure(), uncheckedDowncast<JSMap>(map), static_cast<IterationKind>(kind));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewSetIterator, JSCell*, (Instance* instance, JSCell* set, uint32_t kind))
{
    return JSSetIterator::create(*instance->vm, instance->globalObject->setIteratorStructure(), uncheckedDowncast<JSSet>(set), static_cast<IterationKind>(kind));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedMapClear, void, (Instance* instance, JSCell* map))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    uncheckedDowncast<JSMap>(map)->clear(globalObject);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetClear, void, (Instance* instance, JSCell* set))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    uncheckedDowncast<JSSet>(set)->clear(globalObject);
    OPERATION_RETURN(scope);
}

static ALWAYS_INLINE unsigned lengthAsDecimal(uint32_t value)
{
    static constexpr std::array<uint32_t, 10> powersOfTen { 0, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000 };
    unsigned approximate = ((32 - clz(value | 1)) * 1233) >> 12;
    return approximate - (value < powersOfTen[approximate]) + 1;
}

static ALWAYS_INLINE void writeAsDecimal(uint32_t value, Latin1Character* end)
{
    static constexpr char pairs[] = "0001020304050607080910111213141516171819202122232425262728293031323334353637383940414243444546474849"
        "5051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";
    while (value >= 100) {
        unsigned pair = value % 100 * 2;
        value /= 100;
        end -= 2;
        memcpy(end, pairs + pair, 2);
    }
    if (value >= 10)
        memcpy(end - 2, pairs + value * 2, 2);
    else
        end[-1] = '0' + value;
}

static ALWAYS_INLINE void copyCharacters(Latin1Character* to, const Latin1Character* from, size_t length)
{
    if (length == 1) {
        *to = *from;
        return;
    }
    if (length > 16) {
        memcpy(to, from, length);
        return;
    }
    if (length >= 8) {
        memcpy(to, from, 8);
        memcpy(to + length - 8, from + length - 8, 8);
        return;
    }
    if (length >= 4) {
        memcpy(to, from, 4);
        memcpy(to + length - 4, from + length - 4, 4);
        return;
    }
    if (length >= 2) {
        memcpy(to, from, 2);
        memcpy(to + length - 2, from + length - 2, 2);
    }
}

struct KeyPieces {
    unsigned count { 0 };
    JSString* lastString { nullptr };
};

static ALWAYS_INLINE std::optional<std::span<const Latin1Character>> flattenKeyParts(uint32_t count, const EncodedJSValue* parts, std::span<Latin1Character> buffer, KeyPieces& pieces)
{
    Latin1Character* cursor = buffer.data();
    size_t room = buffer.size();
    for (uint32_t i = 0; i < count; ++i) {
        JSValue part = JSValue::decode(parts[i]);
        size_t length;
        if (part.isInt32()) {
            int32_t integer = part.asInt32();
            uint32_t magnitude = integer < 0 ? 0u - static_cast<uint32_t>(integer) : integer;
            length = lengthAsDecimal(magnitude) + (integer < 0);
            if (length > room)
                return std::nullopt;
            if (integer < 0)
                *cursor = '-';
            writeAsDecimal(magnitude, cursor + length);
            ++pieces.count;
        } else {
            if (!part.isString() || !asString(part)->is8Bit() || asString(part)->length() > room)
                return std::nullopt;
            length = asString(part)->length();
            if (const StringImpl* impl = asString(part)->tryGetValueImpl()) [[likely]]
                copyCharacters(cursor, impl->span8().data(), length);
            else
                asString(part)->resolveToBuffer(std::span { cursor, length });
            if (length) {
                ++pieces.count;
                pieces.lastString = asString(part);
            }
        }
        cursor += length;
        room -= length;
    }
    return std::span<const Latin1Character> { buffer.data(), cursor };
}

static ALWAYS_INLINE std::optional<std::span<const Latin1Character>> flattenKeyParts(uint32_t count, const EncodedJSValue* parts, std::span<Latin1Character> buffer)
{
    KeyPieces pieces;
    return flattenKeyParts(count, parts, buffer, pieces);
}

static constexpr size_t maxSunkKeyLength = 64;

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTryGetByValWithKeyParts, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint32_t count, const EncodedJSValue* parts))
{
    VM& vm = *instance->vm;
    JSValue base = JSValue::decode(encodedBase);
    auto isOrdinary = [](JSObject* object) {
        Structure* structure = object->structure();
        return !TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags()) && !structure->compiledLayout();
    };
    if (!base.isObject() || !isOrdinary(asObject(base)))
        return encodedJSValue();
    std::array<Latin1Character, maxSunkKeyLength> buffer;
    auto text = flattenKeyParts(count, parts, buffer);
    if (!text || (!text->empty() && isASCIIDigit(text->front()) && parseIndex(*text)))
        return encodedJSValue();
    if (RefPtr name = AtomStringImpl::lookUp(*text)) {
        unsigned attributes;
        PropertyOffset offset = asObject(base)->structure()->get(vm, name.get(), attributes);
        if (offset == invalidOffset || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessorOrValue))
            return encodedJSValue();
        return JSValue::encode(asObject(base)->getDirect(offset));
    }
    for (JSObject* holder = asObject(base); isOrdinary(holder) && !TypeInfo::hasStaticPropertyTable(holder->inlineTypeFlags());) {
        JSValue prototype = holder->getPrototypeDirect();
        if (!prototype.isObject())
            return JSValue::encode(jsUndefined());
        holder = asObject(prototype);
    }
    return encodedJSValue();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTryMapOrSetLookupWithKeyParts, EncodedJSValue, (Instance*, JSCell* collection, uint32_t returnsValue, uint32_t count, const EncodedJSValue* parts))
{
    JSCell* table = collection->type() == JSMapType ? uncheckedDowncast<JSMap>(collection)->m_compactTable.get() : uncheckedDowncast<JSSet>(collection)->m_compactTable.get();
    if (!table)
        return encodedJSValue();
    std::array<Latin1Character, maxSunkKeyLength> buffer;
    auto text = flattenKeyParts(count, parts, buffer);
    if (!text)
        return encodedJSValue();
    auto entry = uncheckedDowncast<CompactTable>(table)->find(*text);
    if (!returnsValue)
        return JSValue::encode(jsBoolean(!!entry));
    return JSValue::encode(entry ? uncheckedDowncast<CompactTable>(table)->valueAt(*entry) : jsUndefined());
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateWithIntegersGeneric(Instance*, uint32_t count, const EncodedJSValue* parts);

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateByContents(Instance*, uint32_t count, const EncodedJSValue* parts);

JSC_DEFINE_JIT_OPERATION(operationAOTTypedConcatenateWithIntegers, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* parts))
{
    Typed::State::KeyByParts byParts;
    if (!Options::useAOTOperationCounters() && byParts.tryTake(count, parts)) [[likely]] {
        if (JSString* key = instance->typed->tryFindKeyByParts(byParts)) [[likely]]
            return { key, nullptr };
        instance->typed->lastKeyPartsLookup = byParts;
    } else
        instance->typed->lastKeyPartsLookup.countAndKinds = 0;
    [[clang::musttail]] return concatenateByContents(instance, count, parts);
}

static NEVER_INLINE OperationReturnType<JSCell*> makeKey(Instance* instance, CallFrame* callFrame, std::span<const Latin1Character> characters, unsigned hash)
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return { instance->typed->makeKey(vm, characters, hash), nullptr };
}

static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateByContents(Instance* instance, uint32_t count, const EncodedJSValue* parts)
{
    std::array<Latin1Character, Typed::State::maxCachedKeyLength> characters;
    KeyPieces pieces;
    if (auto text = flattenKeyParts(count, parts, characters, pieces); text && text->size() >= 2 && !Options::useAOTOperationCounters()) [[likely]] {
        if (pieces.count == 1 && pieces.lastString)
            return { pieces.lastString, nullptr };
        unsigned hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(*text);
        JSString* key = instance->typed->tryFindCachedKey(*text, hash);
        if (!key)
            key = uncheckedDowncast<JSString>(makeKey(instance, DECLARE_CALL_FRAME(*instance->vm), *text, hash).value);
        if (instance->typed->lastKeyPartsLookup.countAndKinds)
            instance->typed->cacheKeyByParts(instance->typed->lastKeyPartsLookup, key);
        return { key, nullptr };
    }
    [[clang::musttail]] return concatenateWithIntegersGeneric(instance, count, parts);
}

static NEVER_INLINE OperationReturnType<JSCell*> makeFlatString(Instance* instance, CallFrame* callFrame, std::span<const Latin1Character> characters)
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return { jsNontrivialString(vm, String { characters }), nullptr };
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedConcatenateFlat, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* parts))
{
    std::array<Latin1Character, maxSunkKeyLength> characters;
    KeyPieces pieces;
    if (auto text = flattenKeyParts(count, parts, characters, pieces); text && text->size() >= 2 && !Options::useAOTOperationCounters()) [[likely]] {
        if (pieces.count == 1 && pieces.lastString)
            return { pieces.lastString, nullptr };
        return makeFlatString(instance, DECLARE_CALL_FRAME(*instance->vm), *text);
    }
    [[clang::musttail]] return concatenateWithIntegersGeneric(instance, count, parts);
}

static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateWithIntegersGeneric(Instance* instance, uint32_t count, const EncodedJSValue* parts)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    countOperationNamed(instance, "operationAOTTypedConcatenateWithIntegers");
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::array<Latin1Character, Typed::State::maxCachedKeyLength> characters;
    KeyPieces pieces;
    if (auto text = flattenKeyParts(count, parts, characters, pieces)) [[likely]] {
        if (pieces.count == 1 && pieces.lastString)
            OPERATION_RETURN(scope, pieces.lastString);
        OPERATION_RETURN(scope, instance->typed->cachedKeyFor(vm, *text));
    }

    JSString* result = jsEmptyString(vm);
    for (uint32_t i = 0; i < count; ++i) {
        JSString* part = JSValue::decode(parts[i]).toString(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        result = jsString(globalObject, result, part);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    OPERATION_RETURN(scope, result);
}

static constexpr uint32_t findReturnsValue = 1 << 8;

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES findMakingKeyGeneric(Instance* instance, JSCell* collection, uint32_t countAndKind, const EncodedJSValue* parts, EncodedJSValue* found)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    countOperationNamed(instance, "operationAOTTypedFindMakingKey");
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t count = countAndKind & 0xff;
    JSString* key = nullptr;
    std::array<Latin1Character, Typed::State::maxCachedKeyLength> characters;
    KeyPieces pieces;
    if (auto text = flattenKeyParts(count, parts, characters, pieces))
        key = pieces.count == 1 && pieces.lastString ? pieces.lastString : instance->typed->cachedKeyFor(vm, *text);
    else {
        key = jsEmptyString(vm);
        for (uint32_t i = 0; i < count; ++i) {
            JSString* part = JSValue::decode(parts[i]).toString(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
            key = jsString(globalObject, key, part);
            OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        }
    }
    JSValue answer;
    if (collection->type() != JSMapType)
        answer = jsBoolean(uncheckedDowncast<JSSet>(collection)->has(globalObject, key));
    else if (countAndKind & findReturnsValue)
        answer = uncheckedDowncast<JSMap>(collection)->get(globalObject, key);
    else
        answer = jsBoolean(uncheckedDowncast<JSMap>(collection)->has(globalObject, key));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    found[0] = JSValue::encode(answer);
    found[1] = CompactTable::placeIsUnknown;
    OPERATION_RETURN(scope, key);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedFindMakingKey, JSCell*, (Instance* instance, JSCell* collection, uint32_t countAndKind, const EncodedJSValue* parts, EncodedJSValue* found))
{
    JSCell* cell = collection->type() == JSMapType ? uncheckedDowncast<JSMap>(collection)->m_compactTable.get() : uncheckedDowncast<JSSet>(collection)->m_compactTable.get();
    std::array<Latin1Character, Typed::State::maxCachedKeyLength> characters;
    KeyPieces pieces;
    if (auto text = cell ? flattenKeyParts(countAndKind & 0xff, parts, characters, pieces) : std::nullopt; text && text->size() >= 2 && !Options::useAOTOperationCounters()) [[likely]] {
        auto* table = uncheckedDowncast<CompactTable>(cell);
        unsigned hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(*text);
        bool returnsValue = countAndKind & findReturnsValue;
        uint32_t entry;
        switch (table->tryFindCharactersQuickly(*text, hash, entry)) {
        case CompactTable::Quickly::Found:
            found[0] = JSValue::encode(returnsValue ? table->valueAt(entry) : jsBoolean(true));
            found[1] = table->placeOfValue(entry);
            return { table->keyAt(entry), nullptr };
        case CompactTable::Quickly::Absent: {
            found[0] = JSValue::encode(returnsValue ? jsUndefined() : jsBoolean(false));
            found[1] = CompactTable::keyIsAbsent;
            if (pieces.count == 1 && pieces.lastString)
                return { pieces.lastString, nullptr };
            if (JSString* key = instance->typed->tryFindCachedKey(*text, hash))
                return { key, nullptr };
            return makeKey(instance, DECLARE_CALL_FRAME(*instance->vm), *text, hash);
        }
        case CompactTable::Quickly::Unknown:
            break;
        }
    }
    [[clang::musttail]] return findMakingKeyGeneric(instance, collection, countAndKind, parts, found);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedMapSetAfterLookup, void, (Instance* instance, JSCell* map, JSCell* key, EncodedJSValue value, uint64_t place))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSMap* table = uncheckedDowncast<JSMap>(map);
    if (!table->m_storage) {
        bool isAdded = place == CompactTable::keyIsAbsent
            ? CompactTable::tryAddAbsent(globalObject, table, uncheckedDowncast<JSString>(key), JSValue::decode(value))
            : CompactTable::tryAdd(globalObject, table, key, JSValue::decode(value), CompactTable::CreateIfNeeded::Yes);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        if (isAdded)
            OPERATION_RETURN(scope);
    }
    table->set(globalObject, key, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetAddAfterLookup, void, (Instance* instance, JSCell* set, JSCell* key, uint64_t place))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSSet* table = uncheckedDowncast<JSSet>(set);
    if (!table->m_storage) {
        bool isAdded = place == CompactTable::keyIsAbsent
            ? CompactTable::tryAddAbsent(globalObject, table, uncheckedDowncast<JSString>(key), JSValue())
            : CompactTable::tryAdd(globalObject, table, key, JSValue(), CompactTable::CreateIfNeeded::Yes);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        if (isAdded)
            OPERATION_RETURN(scope);
    }
    table->add(globalObject, key);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedMapNext, size_t, (Instance* instance, JSCell* map, EncodedJSValue table, uint32_t entry))
{
    return nextEntry<MapTraits>(instance, map, table, entry);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedSetNext, size_t, (Instance* instance, JSCell* set, EncodedJSValue table, uint32_t entry))
{
    return nextEntry<SetTraits>(instance, set, table, entry);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedMapKeyAt, EncodedJSValue, (Instance*, JSCell* map, uint32_t entry))
{
    return entryPart<MapTraits>(map, entry, true);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedMapValueAt, EncodedJSValue, (Instance*, JSCell* map, uint32_t entry))
{
    return entryPart<MapTraits>(map, entry, false);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedSetKeyAt, EncodedJSValue, (Instance*, JSCell* set, uint32_t entry))
{
    return entryPart<SetTraits>(set, entry, true);
}

namespace Typed {

using namespace B3;

std::optional<Writes> writesOfInlineMapSet(InlineBuiltin which, std::span<const Type> arguments)
{
    switch (which) {
    case InlineBuiltin::MapSize:
    case InlineBuiltin::SetSize:
    case InlineBuiltin::MapNext:
    case InlineBuiltin::MapKeyAt:
    case InlineBuiltin::MapValueAt:
    case InlineBuiltin::SetNext:
    case InlineBuiltin::SetKeyAt:
    case InlineBuiltin::WeakMapGet:
    case InlineBuiltin::WeakMapHas:
    case InlineBuiltin::WeakSetHas:
        return Writes::Nothing;
    case InlineBuiltin::CollectionTable:
    case InlineBuiltin::MapNew:
    case InlineBuiltin::MapSet:
    case InlineBuiltin::SetAdd:
    case InlineBuiltin::MapClear:
    case InlineBuiltin::SetClear:
    case InlineBuiltin::MapDelete:
    case InlineBuiltin::SetDelete:
    case InlineBuiltin::MapIterator:
    case InlineBuiltin::SetIterator:
        return Writes::RuntimeState;
    case InlineBuiltin::MapGet:
    case InlineBuiltin::MapHas:
    case InlineBuiltin::SetHas:
        return arguments[0].primitives & Primitive::String ? Writes::RuntimeState : Writes::Nothing;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

std::optional<Operand> Lowering::lowerInlineMapSet(InlineBuiltin which, ValueID result, const Value& value, Writes inlinePath)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    auto collection = [&] {
        Operand operand = get(arguments[0]);
        return operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(in<Boxed>(arguments[0]));
    };
    auto entry = [&](unsigned argument) {
        return low32(in<Integer>(arguments[argument]));
    };
    bool readsInline = !Options::useAOTWideReferences();
    auto fieldOf = [&](LValue table, ptrdiff_t offset) {
        return m_out.address(m_heaps.AOTTyped_runtimeState, table, offset);
    };
    auto compactTableOf = [&](CellPointer owner) {
        return m_out.loadPtr(owner.value(), m_heaps.JSMap_compactTable);
    };
    auto keysOf = [&](LValue table) {
        LValue sizeOfIndex = m_out.shl(m_out.constInt32(2), m_out.load8ZeroExt32(fieldOf(table, CompactTable::offsetOfKeysShift())));
        return m_out.add(table, m_out.add(m_out.zeroExtPtr(sizeOfIndex), m_out.constIntPtr(CompactTable::offsetOfIndex())));
    };
    auto at = [&](LValue base, LValue index, unsigned shift) {
        return fieldOf(m_out.add(base, m_out.shl(index, m_out.constInt32(shift))), 0);
    };
    static_assert(JSMap::Helper::capacityIndex() == JSSet::Helper::capacityIndex() && JSMap::Helper::hashTableStartIndex() == JSSet::Helper::hashTableStartIndex());
    static_assert(JSMap::Helper::aliveEntryCountIndex() == JSSet::Helper::aliveEntryCountIndex() && JSMap::Helper::deletedEntryCountIndex() == JSSet::Helper::deletedEntryCountIndex());
    auto dataAt = [&](LValue base, ptrdiff_t index) {
        return m_out.address(m_heaps.OrderedHashTableData, base, index * sizeof(EncodedJSValue));
    };
    auto dataOf = [&](LValue storage) {
        return m_out.add(storage, m_out.constIntPtr(JSCellButterfly::offsetOfData()));
    };
    auto entriesOf = [&](LValue data) {
        LValue capacity = m_out.zeroExtPtr(m_out.load32(dataAt(data, JSMap::Helper::capacityIndex())));
        return m_out.add(data, m_out.shl(m_out.add(capacity, m_out.constIntPtr(JSMap::Helper::hashTableStartIndex())), m_out.constInt32(3)));
    };
    auto entryAt = [&](LValue entries, LValue number, bool isMap) {
        return m_out.add(entries, m_out.mul(number, m_out.constIntPtr((isMap ? JSMap::Helper::EntrySize : JSSet::Helper::EntrySize) * sizeof(EncodedJSValue))));
    };
    auto decoded = [&](LValue index, LValue table) {
        LValue cell = m_out.opaque(decodeCell32(index));
        ensureStillAliveHere(CellPointer(table));
        return cell;
    };
    auto next = [&](bool isMap) {
        CellPointer owner = collection();
        Boxed from = in<Boxed>(arguments[1]);
        Integer start = in<Integer>(arguments[2]);
        auto genericPath = [&] {
            return signExtended(isMap ? AOT_TYPED_OPERATION(MapNext, owner, from, low32(start)) : AOT_TYPED_OPERATION(SetNext, owner, from, low32(start)));
        };
        Type guarantee = Type::integer(Range { -1, INT32_MAX });
        if (!readsInline)
            return ensure(genericPath(), guarantee, type);
        LBasicBlock isUnchanged = m_out.newBlock();
        LBasicBlock loop = m_out.newBlock();
        LBasicBlock isUsed = m_out.newBlock();
        LBasicBlock isDeleted = m_out.newBlock();
        LBasicBlock isAlive = m_out.newBlock();
        LBasicBlock isBehind = m_out.newBlock();
        LBasicBlock isNotCompact = newUnlikelyBlock();
        LBasicBlock isUnchangedStorage = m_out.newBlock();
        LBasicBlock storageLoop = m_out.newBlock();
        LBasicBlock isUsedInStorage = m_out.newBlock();
        LBasicBlock isDeletedInStorage = m_out.newBlock();
        LBasicBlock isAliveInStorage = m_out.newBlock();
        LBasicBlock otherwise = newColdBlock();
        Join join(m_out, Rep::Int64, Int64);
        LValue table = compactTableOf(owner);
        m_out.branch(m_out.bitAnd(m_out.notNull(table), m_out.equal(table, from.value())), unsure(isUnchanged), unsure(isNotCompact));

        m_out.appendTo(isNotCompact);
        LValue storage = m_out.loadPtr(owner.value(), m_heaps.JSMap_storage);
        m_out.branch(m_out.bitAnd(m_out.notNull(storage), m_out.equal(storage, from.value())), usually(isUnchangedStorage), rarely(otherwise));

        m_out.appendTo(isUnchangedStorage);
        LValue data = dataOf(storage);
        LValue usedInStorage = m_out.zeroExtPtr(m_out.add(m_out.load32(dataAt(data, JSMap::Helper::aliveEntryCountIndex())), m_out.load32(dataAt(data, JSMap::Helper::deletedEntryCountIndex()))));
        LValue entries = entriesOf(data);
        LValue deletedValue = m_out.loadPtr(fieldOf(fixedPointer(Instance::offsetOfVM()), VM::offsetOfOrderedHashTableDeletedValue()));
        ValueFromBlock firstInStorage = m_out.anchor(start.value());
        m_out.jump(storageLoop);

        m_out.appendTo(storageLoop);
        LValue entryInStorage = m_out.phi(Int64, firstInStorage);
        m_out.branch(m_out.below(entryInStorage, usedInStorage), usually(isUsedInStorage), rarely(isBehind));

        m_out.appendTo(isUsedInStorage);
        m_out.branch(m_out.notEqual(m_out.load64(dataAt(entryAt(entries, entryInStorage, isMap), 0)), deletedValue), usually(isAliveInStorage), rarely(isDeletedInStorage));

        m_out.appendTo(isDeletedInStorage);
        m_out.addIncomingToPhi(entryInStorage, m_out.anchor(m_out.add(entryInStorage, m_out.constInt64(1))));
        m_out.jump(storageLoop);

        m_out.appendTo(isAliveInStorage);
        join.add(Integer(entryInStorage));

        m_out.appendTo(isUnchanged);
        LValue used = m_out.zeroExtPtr(m_out.load32(table, m_heaps.AOTTypedCompactTable_used));
        LValue keys = keysOf(table);
        ValueFromBlock first = m_out.anchor(start.value());
        m_out.jump(loop);

        m_out.appendTo(loop);
        LValue entry = m_out.phi(Int64, first);
        m_out.branch(m_out.below(entry, used), usually(isUsed), rarely(isBehind));

        m_out.appendTo(isUsed);
        m_out.branch(m_out.notZero32(m_out.load32(at(keys, entry, 2))), usually(isAlive), rarely(isDeleted));

        m_out.appendTo(isDeleted);
        m_out.addIncomingToPhi(entry, m_out.anchor(m_out.add(entry, m_out.constInt64(1))));
        m_out.jump(loop);

        m_out.appendTo(isAlive);
        join.add(Integer(entry));

        m_out.appendTo(isBehind);
        join.add(integer(-1));

        m_out.appendTo(otherwise);
        join.add(genericPath());
        Operand result = join.finish();
        ensureStillAliveHere(owner);
        return ensure(result, guarantee, type);
    };
    auto entryPart = [&](bool isKey, auto genericPath) {
        if (!readsInline)
            return ensure(genericPath(), type);
        CellPointer owner = collection();
        LValue number = in<Integer>(arguments[1]).value();
        LBasicBlock isCompact = m_out.newBlock();
        LBasicBlock otherwise = newUnlikelyBlock();
        Join join(m_out, Rep::JSValue, Int64);
        LValue table = compactTableOf(owner);
        m_out.branch(m_out.notNull(table), unsure(isCompact), unsure(otherwise));

        m_out.appendTo(isCompact);
        LValue keys = keysOf(table);
        if (isKey)
            join.add(Boxed(decoded(m_out.zeroExtPtr(m_out.load32(at(keys, number, 2))), table)));
        else {
            LBasicBlock isNarrow = m_out.newBlock();
            LBasicBlock isWide = newUnlikelyBlock();
            LValue values = m_out.add(keys, m_out.shl(m_out.zeroExtPtr(m_out.load32(fieldOf(table, CompactTable::offsetOfCapacity()))), m_out.constInt32(2)));
            LValue width = m_out.load8ZeroExt32(fieldOf(table, CompactTable::offsetOfValueWidth()));
            m_out.branch(m_out.equal(width, m_out.constInt32(static_cast<int32_t>(CompactTable::Width::Narrow))), unsure(isNarrow), unsure(isWide));
            m_out.appendTo(isNarrow);
            LValue index = m_out.zeroExtPtr(m_out.load32(at(values, number, 2)));
            join.add(Boxed(cell32IsPointer ? decoded(index, table) : m_out.select(m_out.belowOrEqual(index, m_out.constInt64(undefinedCell32)), index, decoded(index, table))));
            m_out.appendTo(isWide);
            join.add(Boxed(m_out.load64(at(values, number, 3))));
        }

        m_out.appendTo(otherwise);
        join.add(Boxed(m_out.load64(dataAt(entryAt(entriesOf(dataOf(m_out.loadPtr(owner.value(), m_heaps.JSMap_storage))), number, which != InlineBuiltin::SetKeyAt), isKey ? 0 : 1))));
        Operand result = join.finish();
        ensureStillAliveHere(owner);
        return ensure(result.as<Boxed>(), type);
    };
    switch (which) {
    case InlineBuiltin::MapDelete:
    case InlineBuiltin::SetDelete: {
        CellPointer owner = nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
        Vector<std::pair<LValue, GPRReg>, 4> operands { { owner.value(), thisGPR }, { mapKey(arguments[0]).value(), argumentGPR(0) } };
        Boxed isRemoved(callICStub(which == InlineBuiltin::MapDelete ? Stub::MapDelete : Stub::SetDelete, Int64, operands, std::nullopt, OperationEffects { inlinePath, true }));
        return booleanResult(unboxedBoolean(isRemoved), type);
    }
    case InlineBuiltin::MapIterator:
    case InlineBuiltin::SetIterator: {
        bool isMap = which == InlineBuiltin::MapIterator;
        auto& builtin = m_builtins.at(value.builtin());
        CellPointer owner = nonNullish(value.child(0), m_program.stringOf(builtin.nameName));
        IterationKind kind = builtin.name == "entries"_s || (isMap && builtin.name == "@@iterator"_s) ? IterationKind::Entries : isMap && builtin.name == "keys"_s ? IterationKind::Keys : IterationKind::Values;
        Word32 encodedKind = word32(static_cast<int32_t>(kind));
        return ensure(isMap ? AOT_TYPED_OPERATION(NewMapIterator, owner, encodedKind) : AOT_TYPED_OPERATION(NewSetIterator, owner, encodedKind), Type::engineClass(m_builtins.classes()[value.engineClass()]), type);
    }
    case InlineBuiltin::MapClear:
    case InlineBuiltin::SetClear: {
        bool isMap = which == InlineBuiltin::MapClear;
        CellPointer owner = nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
        LValue storage = m_out.loadPtr(owner.value(), isMap ? m_heaps.JSMap_storage : m_heaps.JSSet_storage);
        LValue compactTable = m_out.loadPtr(owner.value(), isMap ? m_heaps.JSMap_compactTable : m_heaps.JSSet_compactTable);
        LBasicBlock hasTable = m_out.newBlock();
        LBasicBlock hasNone = newUnlikelyBlock();
        LBasicBlock isCompact = m_out.newBlock();
        LBasicBlock isReset = m_out.newBlock();
        LBasicBlock genericPath = m_out.newBlock();
        Join join(m_out, Rep::Void, Void);
        m_out.branch(m_out.notZero64(m_out.bitOr(storage, compactTable)), unsure(hasTable), unsure(hasNone));
        m_out.appendTo(hasTable);
        m_out.branch(m_out.notNull(compactTable), usually(isCompact), rarely(genericPath));
        m_out.appendTo(isCompact);
        static_assert(CompactTable::offsetOfState() == CompactTable::offsetOfIndexBits() + 3 && !static_cast<uint8_t>(CompactTable::State::Live));
        LValue kinds = m_out.load32(fieldOf(compactTable, CompactTable::offsetOfIndexBits()));
        m_out.branch(m_out.testIsZero32(kinds, m_out.constInt32(static_cast<int32_t>(0xff0000ff))), usually(isReset), rarely(genericPath));
        m_out.appendTo(isReset);
        didEmitWrite(Writes::RuntimeState);
        static_assert(CompactTable::offsetOfAlive() == CompactTable::offsetOfUsed() + 4 && !(CompactTable::offsetOfUsed() % 8));
        m_out.store64(m_out.int64Zero, compactTable, m_heaps.AOTTypedCompactTable_used);
        m_out.store64(m_out.int64Zero, fieldOf(compactTable, CompactTable::offsetOfIndex()));
        join.add({ });
        m_out.appendTo(genericPath);
        if (isMap)
            AOT_TYPED_OPERATION(MapClear, owner);
        else
            AOT_TYPED_OPERATION(SetClear, owner);
        join.add({ });
        m_out.appendTo(hasNone);
        join.add({ });
        join.finish();
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    case InlineBuiltin::CollectionTable: {
        RELEASE_ASSERT(JSMap::offsetOfStorage() == JSSet::offsetOfStorage() && JSMap::offsetOfCompactTable() == JSSet::offsetOfCompactTable());
        CellPointer owner = collection();
        LValue compactTable = m_out.loadPtr(owner.value(), m_heaps.JSMap_compactTable);
        LValue storage = m_out.loadPtr(owner.value(), m_heaps.JSMap_storage);
        LBasicBlock isCompact = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        auto walksThisCollection = [&](ValueID id) {
            auto& other = m_function.at(id);
            if (other.opcode != Opcode::CallBuiltin || other.arguments.isEmpty() || other.arguments[0] != arguments[0])
                return false;
            auto& implementation = m_builtins.at(other.builtin()).implementation;
            if (implementation.kind != BuiltinImplementation::Inline)
                return false;
            switch (implementation.inlined) {
            case InlineBuiltin::CollectionTable:
            case InlineBuiltin::MapNext:
            case InlineBuiltin::SetNext:
                return true;
            default:
                return false;
            }
        };
        bool isAlreadyWalked = [&] {
            for (size_t i = m_currentIndexInBlock; i--;) {
                if (walksThisCollection(m_currentBlock->values[i]))
                    return true;
            }
            if (m_singlePredecessors.isEmpty()) {
                m_singlePredecessors.fill(UINT32_MAX, m_function.blocks.size());
                for (uint32_t i = 0; i < m_function.blocks.size(); ++i) {
                    for (auto& target : m_function.blocks[i].terminator.targets) {
                        uint32_t& only = m_singlePredecessors[target.block.index()];
                        only = only == UINT32_MAX ? i : UINT32_MAX - 1;
                    }
                }
            }
            uint32_t previous = m_singlePredecessors[m_currentBlockID.index()];
            return previous < UINT32_MAX - 1 && std::ranges::any_of(m_function.blocks[previous].values, walksThisCollection);
        }();
        if (isAlreadyWalked) {
            m_out.branch(m_out.notNull(compactTable), unsure(isCompact), unsure(continuation));
            m_out.appendTo(isCompact);
            didEmitWrite(Writes::RuntimeState);
            m_out.store32As8(m_out.constInt32(static_cast<int32_t>(CompactTable::State::Iterated)), fieldOf(compactTable, CompactTable::offsetOfState()));
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            LValue table = m_out.select(m_out.notNull(compactTable), compactTable, m_out.select(m_out.notNull(storage), storage, m_out.constInt64(JSValue::encode(jsNull()))));
            ensureStillAliveHere(owner);
            return ensure(Boxed(table), type);
        }
        LBasicBlock isInImage = newColdBlock();
        LBasicBlock isInHeap = m_out.newBlock();
        ValueFromBlock none = m_out.anchor(compactTable);
        m_out.branch(m_out.notNull(compactTable), unsure(isCompact), unsure(continuation));
        m_out.appendTo(isCompact);
        didEmitWrite(Writes::RuntimeState);
        ValueFromBlock unmoved = m_out.anchor(compactTable);
        m_out.branch(m_out.testNonZero32(m_out.load8ZeroExt32(fieldOf(compactTable, CompactTable::offsetOfKeysShift())), m_out.constInt32(CompactTable::keysAreInImage)), rarely(isInImage), usually(isInHeap));
        m_out.appendTo(isInImage);
        ValueFromBlock moved = m_out.anchor(AOT_TYPED_OPERATION(MoveTableToHeap, owner).value());
        m_out.jump(isInHeap);
        m_out.appendTo(isInHeap);
        LValue heapTable = m_out.phi(pointerType(), unmoved, moved);
        m_out.store32As8(m_out.constInt32(static_cast<int32_t>(CompactTable::State::Iterated)), fieldOf(heapTable, CompactTable::offsetOfState()));
        ValueFromBlock iterated = m_out.anchor(heapTable);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        compactTable = m_out.phi(pointerType(), none, iterated);
        LValue table = m_out.select(m_out.notNull(compactTable), compactTable, m_out.select(m_out.notNull(storage), storage, m_out.constInt64(JSValue::encode(jsNull()))));
        ensureStillAliveHere(owner);
        return ensure(Boxed(table), type);
    }
    case InlineBuiltin::MapNext:
        return next(true);
    case InlineBuiltin::SetNext:
        return next(false);
    case InlineBuiltin::MapKeyAt:
        return entryPart(true, [&] { return AOT_TYPED_OPERATION(MapKeyAt, collection(), entry(1)); });
    case InlineBuiltin::MapValueAt:
        return entryPart(false, [&] { return AOT_TYPED_OPERATION(MapValueAt, collection(), entry(1)); });
    case InlineBuiltin::SetKeyAt:
        return entryPart(true, [&] { return AOT_TYPED_OPERATION(SetKeyAt, collection(), entry(1)); });
    case InlineBuiltin::WeakMapGet:
    case InlineBuiltin::WeakMapHas:
    case InlineBuiltin::WeakSetHas: {
        using MapBucket = WeakMapBucket<WeakMapBucketDataKeyValue>;
        using SetBucket = WeakMapBucket<WeakMapBucketDataKey>;
        static_assert(hasOneBitSet(sizeof(MapBucket)) && hasOneBitSet(sizeof(SetBucket)));
        static_assert(JSWeakMap::offsetOfBuffer() == JSWeakSet::offsetOfBuffer() && JSWeakMap::offsetOfCapacity() == JSWeakSet::offsetOfCapacity());
        static_assert(MapBucket::offsetOfKey() == SetBucket::offsetOfKey());
        bool isMap = which != InlineBuiltin::WeakSetHas;
        auto withoutReadBarrier = [&](LValue base, ptrdiff_t offset) {
            return m_out.address(m_heaps.AOTTyped_collectorState, base, offset);
        };
        CellPointer table = nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
        Boxed given = in<Boxed>(arguments[0]);
        LBasicBlock mayBeKey = m_out.newBlock();
        LBasicBlock loop = m_out.newBlock();
        LBasicBlock isAnother = m_out.newBlock();
        LBasicBlock next = m_out.newBlock();
        LBasicBlock found = m_out.newBlock();
        LBasicBlock absent = m_out.newBlock();
        m_out.branch(isCell(given, typeOf(arguments[0])).value(), usually(mayBeKey), rarely(absent));

        m_out.appendTo(mayBeKey);
        CellPointer key = asCell(given);
        ValueFromBlock first = m_out.anchor(mapKeyHash(box(key)).value());
        LValue buffer = m_out.loadPtr(withoutReadBarrier(table.value(), JSWeakMap::offsetOfBuffer()));
        LValue mask = m_out.sub(m_out.load32(withoutReadBarrier(table.value(), JSWeakMap::offsetOfCapacity())), m_out.int32One);
        m_out.jump(loop);

        m_out.appendTo(loop);
        LValue unmasked = m_out.phi(Int32, first);
        LValue index = m_out.bitAnd(unmasked, mask);
        LValue bucket = m_out.add(buffer, m_out.shl(m_out.zeroExtPtr(index), m_out.constInt32(getLSBSet(isMap ? sizeof(MapBucket) : sizeof(SetBucket)))));
        LValue bucketKey = m_out.loadPtr(withoutReadBarrier(bucket, MapBucket::offsetOfKey()));
        m_out.branch(m_out.equal(bucketKey, key.value()), unsure(found), unsure(isAnother));

        m_out.appendTo(isAnother);
        m_out.branch(m_out.isNull(bucketKey), unsure(absent), unsure(next));

        m_out.appendTo(next);
        m_out.addIncomingToPhi(unmasked, m_out.anchor(m_out.add(index, m_out.int32One)));
        m_out.jump(loop);

        m_out.appendTo(found);
        if (which == InlineBuiltin::WeakMapGet) {
            Rep rep = repOf(type);
            Join join(m_out, rep, b3TypeOf(rep));
            join.add(ensure(Boxed(m_out.load64(withoutReadBarrier(bucket, MapBucket::offsetOfValue()))), type));
            m_out.appendTo(absent);
            join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));
            Operand held = join.finish();
            ensureStillAliveHere(table);
            return held;
        }
        Join join(m_out, Rep::Int32, Int32);
        join.add(boolean(true));
        m_out.appendTo(absent);
        join.add(boolean(false));
        Operand has = join.finish();
        ensureStillAliveHere(table);
        return ensure(has, m_program.at(m_program.types.boolean), type);
    }
    default:
        return lowerMapOrSetBuiltin(which, result, value, inlinePath);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
