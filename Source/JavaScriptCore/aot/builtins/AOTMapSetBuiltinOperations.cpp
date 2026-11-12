/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTArraysMadeInARow.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinOperationSupport.h"
#include "AOTCompactTable.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeHooks.h"
#include "CommonSlowPathsInlines.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "ExceptionHelpers.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSMapInlines.h"
#include "JSMapIterator.h"
#include "MapIteratorPrototypeInlines.h"
#include "JSSetInlines.h"
#include "JSSetIterator.h"
#include "SetIteratorPrototypeInlines.h"
#include "TopExceptionScope.h"
#include <algorithm>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static JSArray* asContiguousArray(JSValue value, unsigned minLength)
{
    JSArray* array = value.isCell() ? dynamicDowncast<JSArray>(value.asCell()) : nullptr;
    if (!array || (array->indexingType() & IndexingShapeMask) != ContiguousShape || array->butterfly()->publicLength() < minLength)
        return nullptr;
    return array;
}

template<typename Collection>
static ALWAYS_INLINE void reserveForAdditionsIfFull(JSGlobalObject* globalObject, Collection* collection, uint64_t remaining, uint64_t finished)
{
    if (collection->hasCompactStorage() && uncheckedDowncast<CompactTable>(collection->storage())->isFull())
        CompactTable::reserveForAdditions(globalObject, collection, remaining, finished, 0);
}

static JSCell* tryNewSetFromSideEffectFreeIterable(JSGlobalObject* globalObject, Structure* structure, JSValue iterable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!iterable.isCell() || iterable.asCell()->type() == JSSetType)
        return nullptr;
    JSCellButterfly* walked = nullptr;
    {
        auto listScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        walked = CommonSlowPaths::trySpreadFast(globalObject, iterable.asCell());
        if (listScope.exception()) [[unlikely]] {
            listScope.clearExceptionExceptTermination();
            return nullptr;
        }
    }
    if (!walked)
        return nullptr;
    JSSet* set = JSSet::create(vm, structure);
    unsigned length = walked->length();
    for (unsigned i = 0; i < length; ++i) {
        reserveForAdditionsIfFull(globalObject, set, length - i, i);
        set->add(globalObject, walked->get(i));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    ensureStillAliveHere(walked);
    return set;
}

template<typename Iterator>
static bool hasUnseenSteps(VM& vm, Iterator* iterator)
{
    if constexpr (std::is_same_v<Iterator, JSMapIterator>)
        return mapIteratorProtocolIsFastAndNonObservable(vm, iterator);
    else
        return setIteratorProtocolIsFastAndNonObservable(vm, iterator);
}

template<typename Iterator>
static JSCell* newSetFromIterator(JSGlobalObject* globalObject, Structure* structure, Iterator* iterator)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSSet* set = JSSet::create(vm, structure);
    uint64_t size = iterator->iteratedObject()->size();
    JSValue element;
    for (uint64_t i = 0; iterator->next(globalObject, element); ++i) {
        RETURN_IF_EXCEPTION(scope, nullptr);
        reserveForAdditionsIfFull(globalObject, set, size > i ? size - i : 1, i);
        set->add(globalObject, element);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    RETURN_IF_EXCEPTION(scope, nullptr);
    ensureStillAliveHere(iterator);
    return set;
}

template<typename Iterator>
static JSCell* newMapFromIterator(JSGlobalObject* globalObject, Structure* structure, Iterator* iterator)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSMap* map = JSMap::create(vm, structure);
    uint64_t size = iterator->iteratedObject()->size();
    for (uint64_t i = 0;; ++i) {
        JSValue key;
        JSValue value;
        if constexpr (std::is_same_v<Iterator, JSMapIterator>) {
            auto entry = iterator->nextWithAdvance(vm);
            key = entry.key;
            value = entry.value;
        } else
            key = value = iterator->nextWithAdvance(vm);
        if (!key)
            break;
        reserveForAdditionsIfFull(globalObject, map, size > i ? size - i : 1, i);
        map->set(globalObject, key, value);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    ensureStillAliveHere(iterator);
    return map;
}

static ALWAYS_INLINE bool isIteratorOfTable(JSValue iterable)
{
    return iterable.isCell() && (iterable.asCell()->type() == JSMapIteratorType || iterable.asCell()->type() == JSSetIteratorType);
}

static JSCell* tryNewSetFromIterator(JSGlobalObject* globalObject, Structure* structure, JSValue iterable)
{
    if (!JSSet::isAddFastAndNonObservable(structure))
        return nullptr;
    VM& vm = globalObject->vm();
    if (auto* iterator = dynamicDowncast<JSMapIterator>(iterable.asCell()); iterator && hasUnseenSteps(vm, iterator))
        return newSetFromIterator(globalObject, structure, iterator);
    if (auto* iterator = dynamicDowncast<JSSetIterator>(iterable.asCell()); iterator && hasUnseenSteps(vm, iterator))
        return newSetFromIterator(globalObject, structure, iterator);
    return nullptr;
}

static JSCell* tryNewMapFromIterator(JSGlobalObject* globalObject, Structure* structure, JSValue iterable)
{
    if (!JSMap::isSetFastAndNonObservable(structure))
        return nullptr;
    VM& vm = globalObject->vm();
    if (auto* iterator = dynamicDowncast<JSMapIterator>(iterable.asCell()); iterator && iterator->kind() == IterationKind::Entries && hasUnseenSteps(vm, iterator))
        return newMapFromIterator(globalObject, structure, iterator);
    if (auto* iterator = dynamicDowncast<JSSetIterator>(iterable.asCell()); iterator && iterator->kind() == IterationKind::Entries && hasUnseenSteps(vm, iterator))
        return newMapFromIterator(globalObject, structure, iterator);
    return nullptr;
}

static JSCell* tryNewSetFromIterable(JSGlobalObject* globalObject, Structure* structure, JSValue iterable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!JSSet::isAddFastAndNonObservable(structure))
        return nullptr;
    if (JSArray* list = asContiguousArray(iterable, 1); list && list->isIteratorProtocolFastAndNonObservable()) {
        const Butterfly* butterfly = list->butterfly();
        JSSet* set = CompactTable::tryCreateSetOfStrings(globalObject, { butterfly->contiguous().data(), butterfly->publicLength() });
        ensureStillAliveHere(list);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (set)
            return set;
    }
    if (JSArray* list = iterable.isCell() ? dynamicDowncast<JSArray>(iterable.asCell()) : nullptr; list && structure == globalObject->setStructure() && list->isIteratorProtocolFastAndNonObservable()) {
        JSSet* set = CompactTable::tryCreateSetOfNumbers(globalObject, list);
        ensureStillAliveHere(list);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (set)
            return set;
    }
    RELEASE_AND_RETURN(scope, tryNewSetFromSideEffectFreeIterable(globalObject, structure, iterable));
}

static NEVER_INLINE std::pair<JSValue, JSValue> pairOfDoubles(JSGlobalObject* globalObject, JSArray* pair)
{
    if ((pair->indexingType() & IndexingShapeMask) != DoubleShape || pair->butterfly()->publicLength() < 2 || !globalObject->isOriginalArrayStructure(pair->structure()))
        return { };
    double key = pair->butterfly()->contiguousDouble().at(pair, 0);
    double value = pair->butterfly()->contiguousDouble().at(pair, 1);
    if (key != key || value != value)
        return { };
    return { jsNumber(key), jsNumber(value) };
}

static JSCell* tryNewMapFromPairs(JSGlobalObject* globalObject, Structure* structure, JSValue iterable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asContiguousArray(iterable, 1);
    if (!list || !list->isIteratorProtocolFastAndNonObservable() || !JSMap::isSetFastAndNonObservable(structure))
        return nullptr;
    if (structure == globalObject->mapStructure()) {
        JSMap* map = CompactTable::tryCreateMapOfStringKeys(globalObject, list);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (map)
            return map;
    }
    unsigned length = list->butterfly()->publicLength();
    auto entryAt = [&](unsigned i) -> std::pair<JSValue, JSValue> {
        JSValue element = list->butterfly()->contiguous().at(list, i).get();
        JSArray* pair = element && element.isCell() ? dynamicDowncast<JSArray>(element.asCell()) : nullptr;
        if (!pair)
            return { };
        IndexingType shape = pair->indexingType() & IndexingShapeMask;
        if (shape != ContiguousShape && shape != Int32Shape)
            return pairOfDoubles(globalObject, pair);
        if (pair->butterfly()->publicLength() < 2 || !globalObject->isOriginalArrayStructure(pair->structure()))
            return { };
        return { pair->butterfly()->contiguous().at(pair, 0).get(), pair->butterfly()->contiguous().at(pair, 1).get() };
    };
    for (unsigned i = 0; i < length; ++i) {
        auto [key, value] = entryAt(i);
        if (!key || !value)
            return nullptr;
    }
    JSMap* map = JSMap::create(vm, structure);
    for (unsigned i = 0; i < length; ++i) {
        auto [key, value] = entryAt(i);
        reserveForAdditionsIfFull(globalObject, map, length - i, i);
        map->set(globalObject, key, value);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return map;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTryNewSetFromIterable, JSCell*, (Instance* instance, EncodedJSValue iterable))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTryNewSetFromIterable);
    OPERATION_RETURN(scope, tryNewSetFromIterable(globalObject, globalObject->setStructure(), JSValue::decode(iterable)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTryNewMapFromEntries, JSCell*, (Instance* instance, EncodedJSValue iterable))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTryNewMapFromEntries);
    OPERATION_RETURN(scope, tryNewMapFromPairs(globalObject, globalObject->mapStructure(), JSValue::decode(iterable)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewSetFromIterable, JSCell*, (Instance* instance, EncodedJSValue encodedIterable))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewSetFromIterable);
    JSValue iterable = JSValue::decode(encodedIterable);
    JSCell* set = tryNewSetFromIterable(globalObject, globalObject->setStructure(), iterable);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (!set && isIteratorOfTable(iterable)) {
        set = tryNewSetFromIterator(globalObject, globalObject->setStructure(), iterable);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    if (set)
        OPERATION_RETURN(scope, set);
    JSValue constructed = constructInEngine(instance, Builtins::Set, iterable);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, constructed.asCell());
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewMapFromEntries, JSCell*, (Instance* instance, EncodedJSValue encodedIterable))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewMapFromEntries);
    JSValue iterable = JSValue::decode(encodedIterable);
    JSCell* map = tryNewMapFromPairs(globalObject, globalObject->mapStructure(), iterable);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (!map && isIteratorOfTable(iterable)) {
        map = tryNewMapFromIterator(globalObject, globalObject->mapStructure(), iterable);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    if (map)
        OPERATION_RETURN(scope, map);
    JSValue constructed = constructInEngine(instance, Builtins::Map, iterable);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, constructed.asCell());
}

static ALWAYS_INLINE UGPRPair hasEntryAndIndex(bool hasEntry, uint32_t entry)
{
    return makeUGPRPair(hasEntry, entry);
}

template<typename Collection>
static void moveWalkedTableToHeap(JSGlobalObject* globalObject, Collection* collection, JSCell* storage)
{
    if (!storage || !collection->hasCompactStorage() || storage != collection->storage())
        return;
    auto* table = uncheckedDowncast<CompactTable>(storage);
    if (!table->isLive() || !table->isInImage())
        return;
    DeferTerminationForAWhile deferTermination(globalObject->vm());
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    CompactTable::tryMoveToHeap(globalObject, collection);
    scope.clearException();
}

template<typename Collection>
static UGPRPair nextEntry(JSGlobalObject* globalObject, Collection* collection, JSValue table, uint32_t entry, EncodedJSValue* results)
{
    VM& vm = globalObject->vm();
    constexpr bool hasValues = std::is_same_v<Collection, JSMap>;
    results[0] = JSValue::encode(jsUndefined());
    if constexpr (hasValues)
        results[1] = JSValue::encode(jsUndefined());
    JSCell* storage = table.isCell() ? table.asCell() : nullptr;
    if (!storage) {
        storage = collection->storage();
        entry = 0;
    }
    if (!storage)
        return hasEntryAndIndex(false, 0);
    moveWalkedTableToHeap(globalObject, collection, storage);
    uint32_t& deletedPrefix = CompactTable::deletedPrefixOf(collection);
    bool beginsInDeletedPrefix = Collection::Helper::isCompact(*storage) && storage == collection->storage() && entry <= deletedPrefix;
    if (beginsInDeletedPrefix)
        entry = deletedPrefix;
    DeferGCForAWhile deferGC(vm);
    auto next = Collection::Helper::transitAndNext(vm, *storage, entry);
    if (beginsInDeletedPrefix && next.storage == storage)
        deletedPrefix = next.entry;
    if (!next.storage)
        return hasEntryAndIndex(false, 0);
    results[0] = JSValue::encode(next.key);
    if constexpr (hasValues)
        results[1] = JSValue::encode(next.value);
    return hasEntryAndIndex(true, next.entry);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMapNext, UGPRPair, (Instance* instance, JSCell* collection, EncodedJSValue table, uint32_t entry, EncodedJSValue* results))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTMapNext);
    OPERATION_RETURN(scope, nextEntry(globalObject, cellArgument<JSMap>(collection, Entry::operationAOTMapNext), JSValue::decode(table), entry, results));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSetNext, UGPRPair, (Instance* instance, JSCell* collection, EncodedJSValue table, uint32_t entry, EncodedJSValue* results))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTSetNext);
    OPERATION_RETURN(scope, nextEntry(globalObject, cellArgument<JSSet>(collection, Entry::operationAOTSetNext), JSValue::decode(table), entry, results));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNextSetElement, EncodedJSValue, (Instance* instance, EncodedJSValue encodedIterator))
{
    constexpr Entry entry = Entry::operationAOTNextSetElement;
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, entry);
    JSValue iterator = JSValue::decode(encodedIterator);
    JSValue element = cellArgument<JSSetIterator>(iterator.isCell() ? iterator.asCell() : nullptr, entry)->nextWithAdvance(vm);
    OPERATION_RETURN(scope, JSValue::encode(element ? element : JSValue(vm.orderedHashTableSentinel())));
}

template<typename Functor>
static void forEachMemberOfSet(VM& vm, JSSet* set, NOESCAPE const Functor& functor)
{
    JSCell* storage = set->storage();
    if (!storage)
        return;
    if (JSSet::Helper::isCompact(*storage)) {
        CompactTable::forEachInColumn(vm, storage, 0, true, functor);
        return;
    }
    for (auto next = JSSet::Helper::transitAndNext(vm, *storage, 0); next.storage; next = JSSet::Helper::transitAndNext(vm, *next.storage, next.entry + 1))
        functor(next.key);
}

template<typename Functor>
static void forEachInColumnOfMap(VM& vm, JSMap* map, bool wantsKeys, NOESCAPE const Functor& functor)
{
    JSCell* storage = map->storage();
    if (!storage)
        return;
    if (JSMap::Helper::isCompact(*storage)) {
        CompactTable::forEachEntry(vm, storage, [&](JSValue key, JSValue value) ALWAYS_INLINE_LAMBDA { functor(wantsKeys ? key : value); });
        return;
    }
    for (auto next = JSMap::Helper::transitAndNext(vm, *storage, 0); next.storage; next = JSMap::Helper::transitAndNext(vm, *next.storage, next.entry + 1))
        functor(wantsKeys ? next.key : next.value);
}

template<typename ForEachMember>
static bool tryAppendMembersInPlace(VM& vm, JSArray* target, unsigned numberOfMembers, JSCell* storageOfKeys, NOESCAPE const ForEachMember& forEachMember)
{
    IndexingType mode = target->indexingMode();
    if (mode != ArrayWithInt32 && mode != ArrayWithDouble && mode != ArrayWithContiguous)
        return false;
    unsigned length = target->butterfly()->publicLength();
    uint64_t newLength = static_cast<uint64_t>(length) + numberOfMembers;
    if (newLength > MAX_STORAGE_VECTOR_LENGTH || !target->ensureLength(vm, static_cast<unsigned>(newLength)))
        return false;

    DeferGCForAWhile deferGC(vm);
    Butterfly* butterfly = target->butterfly();
    unsigned index = length;
    auto keysCopied = CompactTable::KeysCopied::None;
    if (mode != ArrayWithDouble && storageOfKeys && JSSet::Helper::isCompact(*storageOfKeys))
        keysCopied = CompactTable::tryCopyKeys(storageOfKeys, std::span { std::bit_cast<EncodedJSValue*>(butterfly->contiguous().data() + length), numberOfMembers });
    if (keysCopied != CompactTable::KeysCopied::None && mode == ArrayWithContiguous) {
        vm.writeBarrier(target);
        return true;
    }
    if (mode == ArrayWithContiguous) {
        forEachMember([&](JSValue member) {
            RELEASE_ASSERT(index < newLength);
            butterfly->contiguous().atUnsafe(index++).setWithoutWriteBarrier(member);
        });
        RELEASE_ASSERT(index == newLength);
        vm.writeBarrier(target);
        return true;
    }

    bool takesAll = true;
    if (keysCopied != CompactTable::KeysCopied::None) {
        takesAll = keysCopied == CompactTable::KeysCopied::Int32s;
        index = static_cast<unsigned>(newLength);
    } else if (mode == ArrayWithInt32) {
        forEachMember([&](JSValue member) {
            RELEASE_ASSERT(index < newLength);
            takesAll &= member.isInt32();
            butterfly->contiguous().atUnsafe(index++).setWithoutWriteBarrier(member);
        });
    } else {
        forEachMember([&](JSValue member) {
            RELEASE_ASSERT(index < newLength);
            double number = member.isNumber() ? member.asNumber() : PNaN;
            takesAll &= number == number;
            butterfly->contiguousDouble().atUnsafe(index++) = number;
        });
    }
    RELEASE_ASSERT(index == newLength);
    if (takesAll)
        return true;
    while (index > length) {
        if (mode == ArrayWithDouble)
            butterfly->contiguousDouble().atUnsafe(--index) = PNaN;
        else
            butterfly->contiguous().atUnsafe(--index).clear();
    }
    butterfly->setPublicLength(length);
    return false;
}

void appendMembersOfSet(JSGlobalObject* globalObject, JSArray* target, JSSet* source)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    moveWalkedTableToHeap(globalObject, source, source->storage());
    if (static_cast<uint64_t>(target->length()) + source->size() > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return;
    }
    if (tryAppendMembersInPlace(vm, target, source->size(), source->storage(), [&](const auto& take) { forEachMemberOfSet(vm, source, take); }))
        return;

    MarkedArgumentBuffer members;
    {
        DeferGCForAWhile deferGC(vm);
        forEachMemberOfSet(vm, source, [&](JSValue member) { members.append(member); });
    }
    if (members.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return;
    }
    for (size_t index = 0; index < members.size(); ++index) {
        target->push(globalObject, members.at(index));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

static void appendColumnOfMap(JSGlobalObject* globalObject, JSArray* target, JSMap* source, bool wantsKeys)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    moveWalkedTableToHeap(globalObject, source, source->storage());
    if (static_cast<uint64_t>(target->length()) + source->size() > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return;
    }
    if (tryAppendMembersInPlace(vm, target, source->size(), wantsKeys ? source->storage() : nullptr, [&](const auto& take) { forEachInColumnOfMap(vm, source, wantsKeys, take); }))
        return;

    MarkedArgumentBuffer members;
    {
        DeferGCForAWhile deferGC(vm);
        forEachInColumnOfMap(vm, source, wantsKeys, [&](JSValue member) { members.append(member); });
    }
    if (members.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return;
    }
    for (size_t index = 0; index < members.size(); ++index) {
        target->push(globalObject, members.at(index));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

bool tryAppendPairsOfMap(JSGlobalObject* globalObject, JSArray* target, JSMap* source)
{
    VM& vm = globalObject->vm();
    if (source->realm() != globalObject) [[unlikely]]
        return false;
    if (target->indexingMode() != ArrayWithContiguous || globalObject->isHavingABadTime()) [[unlikely]] {
        if (globalObject->isHavingABadTime() || !source->size() || !target->tryMakeWritableContiguous(vm).data() || target->indexingMode() != ArrayWithContiguous)
            return false;
    }
    JSCell* storage = source->storage();
    unsigned size = source->size();
    if (!storage || !size)
        return true;
    unsigned length = target->butterfly()->publicLength();
    uint64_t newLength = static_cast<uint64_t>(length) + size;
    if (newLength > MAX_STORAGE_VECTOR_LENGTH || !target->ensureLength(vm, static_cast<unsigned>(newLength)))
        return false;

    DeferGCForAWhile deferGC(vm);
    ArraysMadeInARow pairs(vm, globalObject->originalArrayStructureForIndexingType(ArrayWithContiguous), 2, 3);
    auto* elements = target->butterfly()->contiguous().data();
    unsigned index = length;
    auto append = [&](JSValue key, JSValue value) ALWAYS_INLINE_LAMBDA {
        RELEASE_ASSERT(index < newLength);
        JSArray* pair = pairs.make([&](EncodedJSValue* both) ALWAYS_INLINE_LAMBDA {
            both[0] = JSValue::encode(key);
            both[1] = JSValue::encode(value);
        });
        elements[index++].setWithoutWriteBarrier(pair);
    };
    if (JSMap::Helper::isCompact(*storage))
        CompactTable::forEachEntry(vm, storage, append);
    else {
        for (auto next = JSMap::Helper::transitAndNext(vm, *storage, 0); next.storage; next = JSMap::Helper::transitAndNext(vm, *next.storage, next.entry + 1))
            append(next.key, next.value);
    }
    vm.writeBarrier(target);
    RELEASE_ASSERT(index == newLength);
    return true;
}

bool tryAppendMembersOfIteratorNobodyStepped(JSGlobalObject* globalObject, JSArray* target, JSMapIterator* members)
{
    VM& vm = globalObject->vm();
    if (members->realm() != globalObject || members->kind() == IterationKind::Entries || members->entry() || members->tryGetStorage() == vm.orderedHashTableSentinel() || !mapIteratorProtocolIsFastAndNonObservable(vm, members))
        return false;
    JSMap* map = members->iteratedObject();
    if (!tryAppendMembersInPlace(vm, target, map->size(), members->kind() == IterationKind::Keys ? map->storage() : nullptr, [&](const auto& take) { forEachInColumnOfMap(vm, map, members->kind() == IterationKind::Keys, take); }))
        return false;
    members->close(vm);
    return true;
}

bool tryAppendMembersOfIteratorNobodyStepped(JSGlobalObject* globalObject, JSArray* target, JSSetIterator* members)
{
    VM& vm = globalObject->vm();
    if (members->realm() != globalObject || members->kind() == IterationKind::Entries || members->entry() || members->tryGetStorage() == vm.orderedHashTableSentinel() || !setIteratorProtocolIsFastAndNonObservable(vm, members))
        return false;
    JSSet* set = members->iteratedObject();
    moveWalkedTableToHeap(globalObject, set, set->storage());
    if (!tryAppendMembersInPlace(vm, target, set->size(), set->storage(), [&](const auto& take) { forEachMemberOfSet(vm, set, take); }))
        return false;
    members->close(vm);
    return true;
}

bool tryAppendPairsLeftIn(JSGlobalObject* globalObject, JSArray* target, JSMapIterator* entries)
{
    VM& vm = globalObject->vm();
    if (entries->realm() != globalObject || entries->kind() != IterationKind::Entries || entries->entry() || entries->tryGetStorage() == vm.orderedHashTableSentinel() || !mapIteratorProtocolIsFastAndNonObservable(vm, entries))
        return false;
    if (!tryAppendPairsOfMap(globalObject, target, entries->iteratedObject()))
        return false;
    entries->close(vm);
    return true;
}

bool tryAppendKeysOfArray(VM& vm, JSArray* target, JSArray* source)
{
    uint32_t size = source->length();
    if (target->indexingMode() == ArrayWithInt32) {
        unsigned length = target->butterfly()->publicLength();
        uint64_t newLength = static_cast<uint64_t>(length) + size;
        if (newLength > MAX_STORAGE_VECTOR_LENGTH || !target->ensureLength(vm, static_cast<unsigned>(newLength)))
            return false;
        auto* keys = target->butterfly()->contiguousInt32().data() + length;
        for (uint32_t index = 0; index < size; ++index)
            keys[index].setWithoutWriteBarrier(jsNumber(static_cast<int32_t>(index)));
        return true;
    }
    return tryAppendMembersInPlace(vm, target, size, nullptr, [&](const auto& take) {
        for (uint32_t index = 0; index < size; ++index)
            take(jsNumber(index));
    });
}

bool tryAppendEntriesOfArray(JSGlobalObject* globalObject, JSArray* target, JSArray* source)
{
    VM& vm = globalObject->vm();
    uint32_t size = source->length();
    if (globalObject->isHavingABadTime() || source == target) [[unlikely]]
        return false;
    if (!size)
        return true;
    if (target->indexingMode() != ArrayWithContiguous && (!target->tryMakeWritableContiguous(vm).data() || target->indexingMode() != ArrayWithContiguous))
        return false;
    unsigned length = target->butterfly()->publicLength();
    uint64_t newLength = static_cast<uint64_t>(length) + size;
    if (newLength > MAX_STORAGE_VECTOR_LENGTH || !target->ensureLength(vm, static_cast<unsigned>(newLength)))
        return false;

    DeferGCForAWhile deferGC(vm);
    ArraysMadeInARow pairs(vm, globalObject->originalArrayStructureForIndexingType(ArrayWithContiguous), 2, 3);
    auto* elements = target->butterfly()->contiguous().data();
    for (uint32_t index = 0; index < size; ++index) {
        JSArray* pair = pairs.make([&](EncodedJSValue* both) ALWAYS_INLINE_LAMBDA {
            both[0] = JSValue::encode(jsNumber(index));
            both[1] = JSValue::encode(source->canGetIndexQuickly(index) ? source->getIndexQuickly(index) : jsUndefined());
        });
        elements[length + index].setWithoutWriteBarrier(pair);
    }
    vm.writeBarrier(target);
    return true;
}

JSArray* RuntimeHooks::tryCreateArrayOfPairs(JSGlobalObject* globalObject, JSCell* items)
{
    JSMap* map = items->type() == JSMapType ? uncheckedDowncast<JSMap>(items) : nullptr;
    JSMapIterator* entries = items->type() == JSMapIteratorType ? uncheckedDowncast<JSMapIterator>(items) : nullptr;
    if (map ? !map->isIteratorProtocolFastAndNonObservable() : !entries || entries->kind() != IterationKind::Entries)
        return nullptr;
    JSArray* list = JSArray::tryCreate(globalObject->vm(), globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), 0);
    if (!list)
        return nullptr;
    return (map ? tryAppendPairsOfMap(globalObject, list, map) : tryAppendPairsLeftIn(globalObject, list, entries)) ? list : nullptr;
}

JSC_DEFINE_JIT_OPERATION(operationAOTAppendMembers, void, (Instance* instance, JSCell* cell, JSCell* sourceCell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAppendMembers);
    if (sourceCell->type() == JSMapType)
        appendColumnOfMap(globalObject, cellArgument<JSArray>(cell, Entry::operationAOTAppendMembers), cellArgument<JSMap>(sourceCell, Entry::operationAOTAppendMembers), true);
    else
        appendMembersOfSet(globalObject, cellArgument<JSArray>(cell, Entry::operationAOTAppendMembers), cellArgument<JSSet>(sourceCell, Entry::operationAOTAppendMembers));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTAppendValuesOfMap, void, (Instance* instance, JSCell* cell, JSCell* sourceCell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAppendValuesOfMap);
    appendColumnOfMap(globalObject, cellArgument<JSArray>(cell, Entry::operationAOTAppendValuesOfMap), cellArgument<JSMap>(sourceCell, Entry::operationAOTAppendValuesOfMap), false);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapDelete, EncodedJSValue, (Instance* instance, JSCell* collection, EncodedJSValue lookupKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMapDelete);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(cellArgument<JSMap>(collection, Entry::operationAOTMapDelete)->remove(globalObject, JSValue::decode(lookupKey)))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetDelete, EncodedJSValue, (Instance* instance, JSCell* collection, EncodedJSValue lookupKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetDelete);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(cellArgument<JSSet>(collection, Entry::operationAOTSetDelete)->remove(globalObject, JSValue::decode(lookupKey)))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewMapIterator, JSCell*, (Instance* instance, JSCell* collection, uint32_t kind))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewMapIterator);
    DeferTerminationForAWhile deferTermination(vm);
    prepareInlineAllocation<JSMapIterator>(instance, InlineAllocation::MapIterator, globalObject->mapIteratorStructure());
    OPERATION_RETURN(scope, JSMapIterator::create(vm, globalObject->mapIteratorStructure(), cellArgument<JSMap>(collection, Entry::operationAOTNewMapIterator), static_cast<IterationKind>(kind)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewSetIterator, JSCell*, (Instance* instance, JSCell* collection, uint32_t kind))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewSetIterator);
    DeferTerminationForAWhile deferTermination(vm);
    prepareInlineAllocation<JSSetIterator>(instance, InlineAllocation::SetIterator, globalObject->setIteratorStructure());
    OPERATION_RETURN(scope, JSSetIterator::create(vm, globalObject->setIteratorStructure(), cellArgument<JSSet>(collection, Entry::operationAOTNewSetIterator), static_cast<IterationKind>(kind)));
}

static NEVER_INLINE OperationReturnType<void> JIT_OPERATION_ATTRIBUTES clearMapGeneric(Instance*, JSCell* collection);
static NEVER_INLINE OperationReturnType<void> JIT_OPERATION_ATTRIBUTES clearSetGeneric(Instance*, JSCell* collection);

JSC_DEFINE_JIT_OPERATION(operationAOTMapClear, void, (Instance* instance, JSCell* collection))
{
    if (CompactTable::tryClearInPlace(asObject(collection))) [[likely]]
        return { nullptr };
    [[clang::musttail]] return clearMapGeneric(instance, collection);
}

static OperationReturnType<void> JIT_OPERATION_ATTRIBUTES clearMapGeneric(Instance* instance, JSCell* collection)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMapClear);
    cellArgument<JSMap>(collection, Entry::operationAOTMapClear)->clear(globalObject);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetClear, void, (Instance* instance, JSCell* collection))
{
    if (CompactTable::tryClearInPlace(asObject(collection))) [[likely]]
        return { nullptr };
    [[clang::musttail]] return clearSetGeneric(instance, collection);
}

static OperationReturnType<void> JIT_OPERATION_ATTRIBUTES clearSetGeneric(Instance* instance, JSCell* collection)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetClear);
    cellArgument<JSSet>(collection, Entry::operationAOTSetClear)->clear(globalObject);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapClearGeneric, EncodedJSValue, (Instance* instance, JSCell* collection))
{
    if (CompactTable::tryClearInPlace(asObject(collection))) [[likely]]
        return { JSValue::encode(jsUndefined()), nullptr };
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMapClearGeneric);
    cellArgument<JSMap>(collection, Entry::operationAOTMapClearGeneric)->clear(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetClearGeneric, EncodedJSValue, (Instance* instance, JSCell* collection))
{
    if (CompactTable::tryClearInPlace(asObject(collection))) [[likely]]
        return { JSValue::encode(jsUndefined()), nullptr };
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetClearGeneric);
    cellArgument<JSSet>(collection, Entry::operationAOTSetClearGeneric)->clear(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
