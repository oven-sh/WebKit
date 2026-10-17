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
#include <wtf/text/RapidHash.h>

namespace JSC { namespace AOT {

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
    auto find = [&]<typename CharacterType>(std::span<const CharacterType> text) ALWAYS_INLINE_LAMBDA -> std::optional<typename Table::TableIndex> {
        unsigned hash = RapidHash::computeHashAndMaskTop8Bits<CharacterType>(text);
        for (JSValue next = Helper::get(storage, Helper::bucketIndex(Helper::capacity(storage), hash)); !next.isEmpty();) {
            typename Table::TableIndex index = Helper::toNumber(next);
            next = Helper::get(storage, index + Helper::ChainOffset);
            JSValue entryKey = Helper::get(storage, index);
            if (!entryKey.isString())
                continue;
            StringImpl* impl = asString(entryKey)->tryGetValueImpl();
            if (!impl)
                return std::nullopt;
            if (impl->hash() != hash)
                continue;
            if (impl->is8Bit() == (sizeof(CharacterType) == 1) ? !Typed::CompactTable::hasCharacters(*impl, text) : StringView { *impl } != StringView { text })
                continue;
            if (impl->is8Bit() || !string->is8Bit())
                uncheckedDowncast<JSRopeString>(string)->resolveSubstringBySharing(*impl);
            return index;
        }
        return Helper::InvalidTableIndex;
    };
    return characters->is8Bit() ? find(characters->span8()) : find(characters->span16());
}

template<typename Table>
static ALWAYS_INLINE Typed::CompactTable::Quickly tryFindSubstringQuickly(Instance* instance, Table* table, JSValue key, JSValue& value)
{
    using Helper = typename Table::Helper;
    using Quickly = Typed::CompactTable::Quickly;
    if (Options::useAOTOperationCounters() || !key.isString() || !asString(key)->isSubstring())
        return Quickly::Unknown;
    UNUSED_PARAM(instance);
    static_assert(sizeof(void*) == 8 && std::endian::native == std::endian::little);
    auto* words = std::bit_cast<const uint8_t*>(asString(key));
    size_t length = WTF::unalignedLoad<uint32_t>(words + JSRopeString::offsetOfLength());
    size_t offset = WTF::unalignedLoad<uint64_t>(words + JSRopeString::offsetOfFiber2()) >> 16;
    auto* whole = std::bit_cast<JSString*>(static_cast<uintptr_t>(WTF::unalignedLoad<uint32_t>(words + JSRopeString::offsetOfFiber1Lower())) | static_cast<uintptr_t>(WTF::unalignedLoad<uint16_t>(words + JSRopeString::offsetOfFiber2())) << 32);
    const StringImpl* wholeCharacters = whole->tryGetValueImpl();
    auto useCharactersOf = [&](const StringImpl& impl) ALWAYS_INLINE_LAMBDA {
        if (impl.is8Bit() || !asString(key)->is8Bit())
            uncheckedDowncast<JSRopeString>(asString(key))->resolveSubstringBySharing(const_cast<StringImpl&>(impl));
    };
    auto find = [&]<typename CharacterType>(std::span<const CharacterType> text) ALWAYS_INLINE_LAMBDA {
        unsigned hash = RapidHash::computeHashAndMaskTop8Bits<CharacterType>(text);
        if (!table->m_storage) {
            if (!table->m_compactTable)
                return Quickly::Absent;
            auto* compactTable = uncheckedDowncast<Typed::CompactTable>(table->m_compactTable.get());
            uint32_t entry;
            Quickly answer = compactTable->tryFindCharactersQuickly(text, hash, entry);
            if (answer == Quickly::Found) {
                value = compactTable->valueAt(entry);
                useCharactersOf(*compactTable->keyAt(entry)->tryGetValueImpl());
            }
            return answer;
        }
        auto& storage = table->storageRef();
        for (JSValue next = Helper::get(storage, Helper::bucketIndex(Helper::capacity(storage), hash)); !next.isEmpty();) {
            typename Table::TableIndex index = Helper::toNumber(next);
            next = Helper::get(storage, index + Helper::ChainOffset);
            JSValue entryKey = Helper::get(storage, index);
            if (!entryKey.isString())
                continue;
            const StringImpl* impl = asString(entryKey)->tryGetValueImpl();
            if (!impl || !impl->hasHash())
                return Quickly::Unknown;
            if (impl->existingHash() != hash || !Typed::CompactTable::equalsCharacters(*impl, text))
                continue;
            if constexpr (std::is_same_v<Table, JSMap>)
                value = Helper::get(storage, index + 1);
            useCharactersOf(*impl);
            return Quickly::Found;
        }
        return Quickly::Absent;
    };
    return wholeCharacters->is8Bit() ? find(std::span { wholeCharacters->span8().data() + offset, length }) : find(std::span { wholeCharacters->span16().data() + offset, length });
}

static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES mapGetGeneric(Instance*, JSCell*, EncodedJSValue);
static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES mapHasGeneric(Instance*, JSCell*, EncodedJSValue);
static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES setHasGeneric(Instance*, JSCell*, EncodedJSValue);

template<typename Table>
static ALWAYS_INLINE Typed::CompactTable* tableInImageAskedForFlatString(JSCell* collection, EncodedJSValue key)
{
    JSCell* table = uncheckedDowncast<Table>(collection)->m_compactTable.get();
    if (!table || !uncheckedDowncast<Typed::CompactTable>(table)->isInImage() || Options::useAOTOperationCounters())
        return nullptr;
    return JSValue::decode(key).isString() && asString(JSValue::decode(key))->tryGetValueImpl() ? uncheckedDowncast<Typed::CompactTable>(table) : nullptr;
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapGet, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue key))
{
    if (auto* table = tableInImageAskedForFlatString<JSMap>(map, key)) {
        auto entry = table->tableInImage().find(*asString(JSValue::decode(key))->tryGetValueImpl());
        return { JSValue::encode(entry ? table->valueAt(*entry) : jsUndefined()), nullptr };
    }
    JSValue value;
    switch (tryFindSubstringQuickly(instance, uncheckedDowncast<JSMap>(map), JSValue::decode(key), value)) {
    case Typed::CompactTable::Quickly::Found:
        return { JSValue::encode(value), nullptr };
    case Typed::CompactTable::Quickly::Absent:
        return { JSValue::encode(jsUndefined()), nullptr };
    case Typed::CompactTable::Quickly::Unknown:
        break;
    }
    [[clang::musttail]] return mapGetGeneric(instance, map, key);
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapHas, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue key))
{
    if (auto* table = tableInImageAskedForFlatString<JSMap>(map, key))
        return { JSValue::encode(jsBoolean(!!table->tableInImage().find(*asString(JSValue::decode(key))->tryGetValueImpl()))), nullptr };
    JSValue value;
    if (auto answer = tryFindSubstringQuickly(instance, uncheckedDowncast<JSMap>(map), JSValue::decode(key), value); answer != Typed::CompactTable::Quickly::Unknown)
        return { JSValue::encode(jsBoolean(answer == Typed::CompactTable::Quickly::Found)), nullptr };
    [[clang::musttail]] return mapHasGeneric(instance, map, key);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetHas, EncodedJSValue, (Instance* instance, JSCell* set, EncodedJSValue key))
{
    if (auto* table = tableInImageAskedForFlatString<JSSet>(set, key))
        return { JSValue::encode(jsBoolean(!!table->tableInImage().find(*asString(JSValue::decode(key))->tryGetValueImpl()))), nullptr };
    JSValue value;
    if (auto answer = tryFindSubstringQuickly(instance, uncheckedDowncast<JSSet>(set), JSValue::decode(key), value); answer != Typed::CompactTable::Quickly::Unknown)
        return { JSValue::encode(jsBoolean(answer == Typed::CompactTable::Quickly::Found)), nullptr };
    [[clang::musttail]] return setHasGeneric(instance, set, key);
}

#define AOT_OPERATION_BEGIN_AS(instance, name) \
    JSGlobalObject* globalObject = (instance)->globalObject; \
    VM& vm = *(instance)->vm; \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    countOperationNamed(instance, name); \
    auto scope = DECLARE_THROW_SCOPE(vm)

static OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES mapGetGeneric(Instance* instance, JSCell* map, EncodedJSValue key)
{
    AOT_OPERATION_BEGIN_AS(instance, "operationAOTMapGet");
    JSMap* table = uncheckedDowncast<JSMap>(map);
    if (table->hasCompactTable()) {
        auto* compactTable = uncheckedDowncast<Typed::CompactTable>(table->m_compactTable.get());
        auto entry = compactTable->find(globalObject, JSValue::decode(key));
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(entry ? compactTable->valueAt(*entry) : jsUndefined()));
    }
    if (auto found = findSubstring(globalObject, table, JSValue::decode(key)))
        OPERATION_RETURN(scope, JSValue::encode(JSMap::Helper::isValidTableIndex(*found) ? table->get(*found) : jsUndefined()));
    OPERATION_RETURN(scope, JSValue::encode(table->get(globalObject, JSValue::decode(key))));
}

static OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES mapHasGeneric(Instance* instance, JSCell* map, EncodedJSValue key)
{
    AOT_OPERATION_BEGIN_AS(instance, "operationAOTMapHas");
    JSMap* table = uncheckedDowncast<JSMap>(map);
    if (table->hasCompactTable()) {
        auto entry = uncheckedDowncast<Typed::CompactTable>(table->m_compactTable.get())->find(globalObject, JSValue::decode(key));
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(!!entry)));
    }
    if (auto found = findSubstring(globalObject, table, JSValue::decode(key)))
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(JSMap::Helper::isValidTableIndex(*found))));
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(table->has(globalObject, JSValue::decode(key)))));
}

static OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES setHasGeneric(Instance* instance, JSCell* set, EncodedJSValue key)
{
    AOT_OPERATION_BEGIN_AS(instance, "operationAOTSetHas");
    JSSet* table = uncheckedDowncast<JSSet>(set);
    if (table->hasCompactTable()) {
        auto entry = uncheckedDowncast<Typed::CompactTable>(table->m_compactTable.get())->find(globalObject, JSValue::decode(key));
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(!!entry)));
    }
    if (auto found = findSubstring(globalObject, table, JSValue::decode(key)))
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(JSSet::Helper::isValidTableIndex(*found))));
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(table->has(globalObject, JSValue::decode(key)))));
}

static ALWAYS_INLINE void mapSetGeneric(JSGlobalObject* globalObject, VM& vm, JSMap* table, JSValue key, JSValue value)
{
    if (auto found = findSubstring(globalObject, table, key); found && JSMap::Helper::isValidTableIndex(*found)) {
        JSMap::Helper::setKeyOrValueData(vm, table->storageRef(), *found + 1, value);
        return;
    }
    table->set(globalObject, key, value);
}

static ALWAYS_INLINE void setAddGeneric(JSGlobalObject* globalObject, JSSet* table, JSValue key)
{
    if (auto found = findSubstring(globalObject, table, key); found && JSSet::Helper::isValidTableIndex(*found))
        return;
    table->add(globalObject, key);
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapSetGeneric, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue key, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    mapSetGeneric(globalObject, vm, uncheckedDowncast<JSMap>(map), JSValue::decode(key), JSValue::decode(value));
    OPERATION_RETURN(scope, JSValue::encode(map));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetAddGeneric, EncodedJSValue, (Instance* instance, JSCell* set, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    setAddGeneric(globalObject, uncheckedDowncast<JSSet>(set), JSValue::decode(key));
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
    mapSetGeneric(globalObject, vm, table, JSValue::decode(key), JSValue::decode(value));
    OPERATION_RETURN(scope, JSValue::encode(map));
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
    setAddGeneric(globalObject, table, JSValue::decode(key));
    OPERATION_RETURN(scope, JSValue::encode(set));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
