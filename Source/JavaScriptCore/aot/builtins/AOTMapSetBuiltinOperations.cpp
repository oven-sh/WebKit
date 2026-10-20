/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "CommonSlowPathsInlines.h"
#include "DeferGCInlines.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSMapInlines.h"
#include "JSMapIterator.h"
#include "JSSetInlines.h"
#include "JSSetIterator.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static JSArray* asContiguousArray(JSValue value, unsigned minLength)
{
    JSArray* array = value.isCell() ? dynamicDowncast<JSArray>(value.asCell()) : nullptr;
    if (!array || (array->indexingType() & IndexingShapeMask) != ContiguousShape || array->butterfly()->publicLength() < minLength)
        return nullptr;
    return array;
}

static JSCell* tryNewSetFromSideEffectFreeIterable(JSGlobalObject* globalObject, Structure* structure, JSValue iterable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!iterable.isCell() || iterable.asCell()->type() == JSSetType)
        return nullptr;
    JSCellButterfly* walked = CommonSlowPaths::trySpreadFast(globalObject, iterable.asCell());
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!walked)
        return nullptr;
    JSSet* set = JSSet::create(vm, structure);
    unsigned length = walked->length();
    for (unsigned i = 0; i < length; ++i) {
        set->add(globalObject, walked->get(i));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    ensureStillAliveHere(walked);
    return set;
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
    RELEASE_AND_RETURN(scope, tryNewSetFromSideEffectFreeIterable(globalObject, structure, iterable));
}

static JSCell* tryNewMapFromPairs(JSGlobalObject* globalObject, Structure* structure, JSValue iterable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* list = asContiguousArray(iterable, 1);
    if (!list || !list->isIteratorProtocolFastAndNonObservable() || !JSMap::isSetFastAndNonObservable(structure))
        return nullptr;
    unsigned length = list->butterfly()->publicLength();
    auto entryAt = [&](unsigned i) -> std::pair<JSValue, JSValue> {
        JSValue element = list->butterfly()->contiguous().at(list, i).get();
        JSArray* pair = element ? asContiguousArray(element, 2) : nullptr;
        if (!pair || !globalObject->isOriginalArrayStructure(pair->structure()))
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

static ALWAYS_INLINE UGPRPair hasEntryAndIndex(bool hasEntry, uint32_t entry)
{
    return makeUGPRPair(hasEntry, entry);
}

template<typename Collection>
static UGPRPair nextEntry(VM& vm, Collection* collection, JSValue table, uint32_t entry, EncodedJSValue* results)
{
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
    DeferGCForAWhile deferGC(vm);
    auto next = Collection::Helper::transitAndNext(vm, *storage, entry);
    if (!next.storage)
        return hasEntryAndIndex(false, 0);
    results[0] = JSValue::encode(next.key);
    if constexpr (hasValues)
        results[1] = JSValue::encode(next.value);
    return hasEntryAndIndex(true, next.entry);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMapNext, UGPRPair, (Instance* instance, JSCell* collection, EncodedJSValue table, uint32_t entry, EncodedJSValue* results))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMapNext);
    OPERATION_RETURN(scope, nextEntry(vm, cellArgument<JSMap>(collection, Entry::operationAOTMapNext), JSValue::decode(table), entry, results));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSetNext, UGPRPair, (Instance* instance, JSCell* collection, EncodedJSValue table, uint32_t entry, EncodedJSValue* results))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetNext);
    OPERATION_RETURN(scope, nextEntry(vm, cellArgument<JSSet>(collection, Entry::operationAOTSetNext), JSValue::decode(table), entry, results));
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
    OPERATION_RETURN(scope, JSMapIterator::create(vm, globalObject->mapIteratorStructure(), cellArgument<JSMap>(collection, Entry::operationAOTNewMapIterator), static_cast<IterationKind>(kind)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewSetIterator, JSCell*, (Instance* instance, JSCell* collection, uint32_t kind))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewSetIterator);
    OPERATION_RETURN(scope, JSSetIterator::create(vm, globalObject->setIteratorStructure(), cellArgument<JSSet>(collection, Entry::operationAOTNewSetIterator), static_cast<IterationKind>(kind)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapClear, void, (Instance* instance, JSCell* collection))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMapClear);
    cellArgument<JSMap>(collection, Entry::operationAOTMapClear)->clear(globalObject);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetClear, void, (Instance* instance, JSCell* collection))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetClear);
    cellArgument<JSSet>(collection, Entry::operationAOTSetClear)->clear(globalObject);
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
