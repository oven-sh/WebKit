/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperationsBuiltins.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTTypedCompactTable.h"
#include "ArrayPrototypeInlines.h"
#include "DateInstance.h"
#include "FrameTracers.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "JSWeakMap.h"
#include "JSWeakSet.h"
#include "MathCommon.h"
#include "WeakMapImplInlines.h"

namespace JSC { namespace AOT {

// A key that is a part of another string, as `slice` yields it, and has not been made a string of its own, which takes an allocation and a
// copy. It is looked for by its characters where they are. If the table has the key, this one comes to share the characters of that one, and
// is found by comparing pointers from then on. Nothing: it is no such key. InvalidTableIndex: the table does not have it.
template<typename Table>
static std::optional<typename Table::TableIndex> findSubstring(JSGlobalObject* globalObject, Table* table, JSValue key)
{
    using Helper = typename Table::Helper;
    if (!key.isString() || !asString(key)->isSubstring())
        return std::nullopt;
    if (!table->m_storage) {
        if (table->m_compactTable)
            return std::nullopt;
        return Helper::InvalidTableIndex;
    }
    JSString* string = asString(key);
    auto& storage = table->storageRef();
    auto characters = string->view(globalObject);
    unsigned hash = characters->hash();
    for (JSValue next = Helper::get(storage, Helper::bucketIndex(Helper::capacity(storage), hash)); !next.isEmpty();) {
        typename Table::TableIndex index = Helper::toNumber(next);
        next = Helper::get(storage, index + Helper::ChainOffset);
        JSValue entryKey = Helper::get(storage, index);
        if (!entryKey.isString())
            continue;
        StringImpl* impl = asString(entryKey)->tryGetValueImpl();
        if (!impl)
            return std::nullopt;
        if (impl->hash() != hash || StringView { *impl } != characters.data)
            continue;
        if (impl->is8Bit() || !string->is8Bit())
            uncheckedDowncast<JSRopeString>(string)->resolveSubstringBySharing(*impl);
        return index;
    }
    return Helper::InvalidTableIndex;
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapGet, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSMap* table = uncheckedDowncast<JSMap>(map);
    if (auto found = findSubstring(globalObject, table, JSValue::decode(key)))
        OPERATION_RETURN(scope, JSValue::encode(JSMap::Helper::isValidTableIndex(*found) ? table->get(*found) : jsUndefined()));
    OPERATION_RETURN(scope, JSValue::encode(table->get(globalObject, JSValue::decode(key))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapHas, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSMap* table = uncheckedDowncast<JSMap>(map);
    if (auto found = findSubstring(globalObject, table, JSValue::decode(key)))
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(JSMap::Helper::isValidTableIndex(*found))));
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(table->has(globalObject, JSValue::decode(key)))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetHas, EncodedJSValue, (Instance* instance, JSCell* set, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSSet* table = uncheckedDowncast<JSSet>(set);
    if (auto found = findSubstring(globalObject, table, JSValue::decode(key)))
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(JSSet::Helper::isValidTableIndex(*found))));
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(table->has(globalObject, JSValue::decode(key)))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapSetGeneric, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue key, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSMap* table = uncheckedDowncast<JSMap>(map);
    if (auto found = findSubstring(globalObject, table, JSValue::decode(key)); found && JSMap::Helper::isValidTableIndex(*found)) {
        JSMap::Helper::setKeyOrValueData(vm, table->storageRef(), *found + 1, JSValue::decode(value));
        OPERATION_RETURN(scope, JSValue::encode(map));
    }
    table->set(globalObject, JSValue::decode(key), JSValue::decode(value));
    OPERATION_RETURN(scope, JSValue::encode(map));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetAddGeneric, EncodedJSValue, (Instance* instance, JSCell* set, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSSet* table = uncheckedDowncast<JSSet>(set);
    if (auto found = findSubstring(globalObject, table, JSValue::decode(key)); found && JSSet::Helper::isValidTableIndex(*found))
        OPERATION_RETURN(scope, JSValue::encode(set));
    table->add(globalObject, JSValue::decode(key));
    OPERATION_RETURN(scope, JSValue::encode(set));
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapSetWithStringKey, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue key, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSMap* table = uncheckedDowncast<JSMap>(map);
    if (!table->m_storage) {
        bool isAdded = Typed::CompactTable::tryAdd(globalObject, table, JSValue::decode(key), JSValue::decode(value), Typed::CompactTable::CreateIfNeeded::Yes);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (isAdded)
            OPERATION_RETURN(scope, JSValue::encode(map));
    }
    OPERATION_RETURN(scope, operationAOTMapSetGeneric(instance, map, key, value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetAddWithStringKey, EncodedJSValue, (Instance* instance, JSCell* set, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSSet* table = uncheckedDowncast<JSSet>(set);
    if (!table->m_storage) {
        bool isAdded = Typed::CompactTable::tryAdd(globalObject, table, JSValue::decode(key), JSValue(), Typed::CompactTable::CreateIfNeeded::Yes);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (isAdded)
            OPERATION_RETURN(scope, JSValue::encode(set));
    }
    OPERATION_RETURN(scope, operationAOTSetAddGeneric(instance, set, key));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
