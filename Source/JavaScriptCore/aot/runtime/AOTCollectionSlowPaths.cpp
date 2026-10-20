/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTImage.h"
#include "AOTOperationScope.h"
#include "AOTTableInImage.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "JSWeakMapInlines.h"
#include "WeakMapImplInlines.h"
#include <wtf/text/StringHasherInlines.h>

namespace JSC { namespace AOT {

static ALWAYS_INLINE void resolveBySharing(JSRopeString* substring, StringImpl& impl)
{
    if (impl.is8Bit() || !substring->is8Bit())
        substring->resolveSubstringBySharing(impl);
}

static ALWAYS_INLINE CompactTable* compactTableOrNull(JSObject* collection)
{
    return CompactTable::tableOf(collection);
}

static ALWAYS_INLINE CompactTable::QuickLookup tryFindSubstringQuickly(JSObject* collection, JSValue key, JSValue& value)
{
    using QuickLookup = CompactTable::QuickLookup;
    if (!key.isString() || !asString(key)->isSubstring())
        return QuickLookup::Unknown;
    CompactTable* table = compactTableOrNull(collection);
    if (!table)
        return QuickLookup::Absent;
    if (!table->isHeapTable())
        return QuickLookup::Unknown;
    auto* rope = uncheckedDowncast<JSRopeString>(asString(key));
    size_t length = rope->length();
    size_t offset = rope->substringOffset();
    const StringImpl* baseImpl = rope->substringBase()->tryGetValueImpl();
    ASSERT(baseImpl);
    auto find = [&]<typename CharacterType>(std::span<const CharacterType> text) ALWAYS_INLINE_LAMBDA {
        unsigned hash = StringHasher::computeHashAndMaskTop8Bits(text);
        auto lookup = table->tryFindCharactersQuickly(text, hash);
        if (lookup.result == QuickLookup::Found) {
            value = table->valueAt(lookup.entry);
            resolveBySharing(rope, *asString(table->heapKeyAt(lookup.entry))->tryGetValueImpl());
        }
        return lookup.result;
    };
    return baseImpl->is8Bit() ? find(baseImpl->span8().subspan(offset, length)) : find(baseImpl->span16().subspan(offset, length));
}

static ALWAYS_INLINE CompactTable::QuickLookup tryFindInImage(JSObject* collection, JSValue key)
{
    using QuickLookup = CompactTable::QuickLookup;
    if (!key.isString())
        return QuickLookup::Unknown;
    CompactTable* table = compactTableOrNull(collection);
    const StringImpl* impl = asString(key)->tryGetValueImpl();
    if (!table || !impl || !impl->hasHash() || !table->isInImage())
        return QuickLookup::Unknown;
    return table->tableInImage().find(*Image::current(), StringView(*impl), impl->existingHash()) ? QuickLookup::Found : QuickLookup::Absent;
}

static ALWAYS_INLINE CompactTable::QuickLookup tryFindKeyQuickly(JSObject* collection, JSValue key)
{
    if (auto lookup = tryFindInImage(collection, key); lookup != CompactTable::QuickLookup::Unknown)
        return lookup;
    JSValue value;
    return tryFindSubstringQuickly(collection, key, value);
}

static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES mapGetGeneric(Instance*, JSCell*, EncodedJSValue);
static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES mapHasGeneric(Instance*, JSCell*, EncodedJSValue);
static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES setHasGeneric(Instance*, JSCell*, EncodedJSValue);

JSC_DEFINE_JIT_OPERATION(operationAOTMapGet, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue lookupKey))
{
    JSValue value;
    switch (tryFindSubstringQuickly(uncheckedDowncast<JSMap>(map), JSValue::decode(lookupKey), value)) {
    case CompactTable::QuickLookup::Found:
        return { JSValue::encode(value), nullptr };
    case CompactTable::QuickLookup::Absent:
        return { JSValue::encode(jsUndefined()), nullptr };
    case CompactTable::QuickLookup::Unknown:
        break;
    }
    [[clang::musttail]] return mapGetGeneric(instance, map, lookupKey);
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapHas, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue lookupKey))
{
    if (auto lookup = tryFindKeyQuickly(uncheckedDowncast<JSMap>(map), JSValue::decode(lookupKey)); lookup != CompactTable::QuickLookup::Unknown)
        return { JSValue::encode(jsBoolean(lookup == CompactTable::QuickLookup::Found)), nullptr };
    [[clang::musttail]] return mapHasGeneric(instance, map, lookupKey);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetHas, EncodedJSValue, (Instance* instance, JSCell* set, EncodedJSValue lookupKey))
{
    if (auto lookup = tryFindKeyQuickly(uncheckedDowncast<JSSet>(set), JSValue::decode(lookupKey)); lookup != CompactTable::QuickLookup::Unknown)
        return { JSValue::encode(jsBoolean(lookup == CompactTable::QuickLookup::Found)), nullptr };
    [[clang::musttail]] return setHasGeneric(instance, set, lookupKey);
}

static OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES mapGetGeneric(Instance* instance, JSCell* map, EncodedJSValue lookupKey)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMapGet);
    CompactTable* table = compactTableOrNull(uncheckedDowncast<JSMap>(map));
    if (!table)
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    auto entry = table->find(globalObject, JSValue::decode(lookupKey));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(entry ? table->valueAt(*entry) : jsUndefined()));
}

static OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES mapHasGeneric(Instance* instance, JSCell* map, EncodedJSValue lookupKey)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMapHas);
    CompactTable* table = compactTableOrNull(uncheckedDowncast<JSMap>(map));
    if (!table)
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(false)));
    auto entry = table->find(globalObject, JSValue::decode(lookupKey));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(!!entry)));
}

static OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES setHasGeneric(Instance* instance, JSCell* set, EncodedJSValue lookupKey)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetHas);
    CompactTable* table = compactTableOrNull(uncheckedDowncast<JSSet>(set));
    if (!table)
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(false)));
    auto entry = table->find(globalObject, JSValue::decode(lookupKey));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(!!entry)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapSet, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue lookupKey, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMapSet);
    uncheckedDowncast<JSMap>(map)->set(globalObject, JSValue::decode(lookupKey), JSValue::decode(value));
    OPERATION_RETURN(scope, JSValue::encode(map));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetAdd, EncodedJSValue, (Instance* instance, JSCell* set, EncodedJSValue lookupKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetAdd);
    uncheckedDowncast<JSSet>(set)->add(globalObject, JSValue::decode(lookupKey));
    OPERATION_RETURN(scope, JSValue::encode(set));
}

JSC_DEFINE_JIT_OPERATION(operationAOTWeakMapSet, EncodedJSValue, (Instance* instance, JSCell* map, JSCell* lookupKey, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTWeakMapSet);
    uncheckedDowncast<JSWeakMap>(map)->set(vm, lookupKey, JSValue::decode(value));
    OPERATION_RETURN(scope, JSValue::encode(map));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
