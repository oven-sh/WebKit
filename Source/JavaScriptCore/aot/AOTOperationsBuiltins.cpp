/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperationsBuiltins.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMath, double, (double x, uint32_t which))
{
    switch (static_cast<MathFunction>(which)) {
    case MathFunction::Sin:
        return Math::sinDouble(x);
    case MathFunction::Cos:
        return Math::cosDouble(x);
    case MathFunction::Tan:
        return Math::tanDouble(x);
    case MathFunction::Asin:
        return Math::asinDouble(x);
    case MathFunction::Acos:
        return Math::acosDouble(x);
    case MathFunction::Atan:
        return Math::atanDouble(x);
    case MathFunction::Sinh:
        return Math::sinhDouble(x);
    case MathFunction::Cosh:
        return Math::coshDouble(x);
    case MathFunction::Tanh:
        return Math::tanhDouble(x);
    case MathFunction::Asinh:
        return Math::asinhDouble(x);
    case MathFunction::Acosh:
        return Math::acoshDouble(x);
    case MathFunction::Atanh:
        return Math::atanhDouble(x);
    case MathFunction::Log:
        return Math::logDouble(x);
    case MathFunction::Log2:
        return Math::log2Double(x);
    case MathFunction::Log10:
        return Math::log10Double(x);
    case MathFunction::Log1p:
        return Math::log1pDouble(x);
    case MathFunction::Exp:
        return Math::expDouble(x);
    case MathFunction::Expm1:
        return Math::expm1Double(x);
    case MathFunction::Cbrt:
        return Math::cbrtDouble(x);
    }
    RELEASE_ASSERT_NOT_REACHED();
    return 0;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMathAtan2, double, (double y, double x))
{
    return atan2(y, x);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRandom, double, (Instance* instance))
{
    countOperationNamed(instance, __func__);
    JSGlobalObject* globalObject = instance->globalObject;
    return globalObject->weakRandomNumber();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTWeakMapGet, EncodedJSValue, (JSCell* map, EncodedJSValue encodedKey))
{
    JSValue key = JSValue::decode(encodedKey);
    if (!key.isCell()) [[unlikely]]
        return JSValue::encode(jsUndefined());
    return JSValue::encode(uncheckedDowncast<JSWeakMap>(map)->get(key.asCell()));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTWeakMapHas, size_t, (JSCell* map, EncodedJSValue encodedKey))
{
    JSValue key = JSValue::decode(encodedKey);
    return key.isCell() && uncheckedDowncast<JSWeakMap>(map)->has(key.asCell());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTWeakSetHas, size_t, (JSCell* set, EncodedJSValue encodedKey))
{
    JSValue key = JSValue::decode(encodedKey);
    return key.isCell() && uncheckedDowncast<JSWeakSet>(set)->has(key.asCell());
}

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

JSC_DEFINE_JIT_OPERATION(operationAOTMapDelete, size_t, (Instance* instance, JSCell* map, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, uncheckedDowncast<JSMap>(map)->remove(globalObject, JSValue::decode(key)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetDelete, size_t, (Instance* instance, JSCell* set, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, uncheckedDowncast<JSSet>(set)->remove(globalObject, JSValue::decode(key)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPushMultiple, EncodedJSValue, (Instance* instance, JSArray* array, EncodedJSValue* values, uint32_t count))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    uint64_t length = array->length();
    if (length + count > std::numeric_limits<uint32_t>::max()) [[unlikely]] {
        for (uint32_t i = 0; i < count; ++i) {
            array->putByIndexInline(globalObject, length + i, JSValue::decode(values[i]), true);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        setLength(globalObject, vm, array, length + count);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(jsNumber(length + count)));
    }
    for (uint32_t i = 0; i < count; ++i) {
        array->pushInline(globalObject, JSValue::decode(values[i]));
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(array->length())));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArraySlice, JSArray*, (Instance* instance, JSArray* array, int32_t start, int32_t end))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    int64_t length = array->length();
    auto clamp = [&](int64_t index) { return index < 0 ? std::max<int64_t>(length + index, 0) : std::min<int64_t>(index, length); };
    int64_t from = clamp(start);
    int64_t to = std::max(clamp(end), from);
    OPERATION_RETURN(scope, JSArray::fastSlice(globalObject, array, from, to - from));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDateField, EncodedJSValue, (VM* vm, DateInstance* date, uint32_t which))
{
    countOperationNamed(*vm, __func__);
    auto time = which & dateFieldIsUTC ? date->gregorianDateTimeUTC(vm->dateCache) : date->gregorianDateTime(vm->dateCache);
    if (!time)
        return JSValue::encode(jsNaN());
    switch (static_cast<DateField>(which & ~dateFieldIsUTC)) {
    case DateField::FullYear:
        return JSValue::encode(jsNumber(time.year()));
    case DateField::Month:
        return JSValue::encode(jsNumber(time.month()));
    case DateField::Date:
        return JSValue::encode(jsNumber(time.monthDay()));
    case DateField::Day:
        return JSValue::encode(jsNumber(time.weekDay()));
    case DateField::Hours:
        return JSValue::encode(jsNumber(time.hour()));
    case DateField::Minutes:
        return JSValue::encode(jsNumber(time.minute()));
    case DateField::Seconds:
        return JSValue::encode(jsNumber(time.second()));
    case DateField::TimezoneOffset:
        return JSValue::encode(jsNumber(-time.utcOffsetInMinute()));
    }
    RELEASE_ASSERT_NOT_REACHED();
    return 0;
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayFromValues, JSCell*, (Instance* instance, const EncodedJSValue* values, uint32_t count, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(static_cast<IndexingType>(indexingType));
    OPERATION_RETURN(scope, constructArray(globalObject, structure, std::bit_cast<const JSValue*>(values), count));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
