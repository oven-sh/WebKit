/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#include "JSArray.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedSetExpectedRowType, void, (Instance* instance, EncodedJSValue encodedQuery, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue query = JSValue::decode(encodedQuery);
    if (query.isCell() && query.asCell()->type() == JSPromiseType)
        instance->typed->setExpectedRowType(vm, asObject(query), type);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTryConvertRow, void, (Instance* instance, EncodedJSValue encodedRow, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    instance->typed->tryConvertRow(vm, asObject(JSValue::decode(encodedRow)), instance->typed->type(type));
}

extern "C" JS_EXPORT_PRIVATE uint32_t JSC__AOT__expectedRowType(JSGlobalObject* globalObject, EncodedJSValue encodedQuery)
{
    Instance* instance = globalObject->aotInstance();
    JSValue query = JSValue::decode(encodedQuery);
    if (!instance || !instance->typed || !query.isObject())
        return 0;
    return instance->typed->expectedRowType(globalObject->vm(), asObject(query));
}

extern "C" JS_EXPORT_PRIVATE void* JSC__AOT__RowDecoder__create(JSGlobalObject* globalObject, uint32_t expectedRowType, const Typed::State::RowColumn* columns, uint32_t columnCount)
{
    Instance* instance = globalObject->aotInstance();
    if (!instance || !instance->typed || !expectedRowType)
        return nullptr;
    return instance->typed->createRowDecoder(globalObject->vm(), globalObject, instance->typed->type(expectedRowType - 1), std::span { columns, columnCount }).release();
}

extern "C" JS_EXPORT_PRIVATE void JSC__AOT__RowDecoder__destroy(void* decoder)
{
    delete static_cast<Typed::State::RowDecoder*>(decoder);
}

extern "C" JS_EXPORT_PRIVATE EncodedJSValue JSC__AOT__RowDecoder__appendRow(JSGlobalObject* globalObject, const void* decoder, const uint8_t* bytes, size_t size, EncodedJSValue encodedArray)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue arrayValue = JSValue::decode(encodedArray);
    JSArray* rows = arrayValue ? dynamicDowncast<JSArray>(arrayValue) : nullptr;
    if (arrayValue && !rows)
        return { };
    Typed::Object* row = globalObject->aotInstance()->typed->tryDecodeRow(vm, globalObject, *static_cast<const Typed::State::RowDecoder*>(decoder), std::span { bytes, size });
    if (!row)
        return { };
    if (rows)
        rows->push(globalObject, row);
    else if ((rows = constructEmptyArray(globalObject, nullptr, 1)))
        rows->putDirectIndex(globalObject, 0, row);
    if (scope.exception()) [[unlikely]] {
        scope.clearExceptionExceptTermination();
        return { };
    }
    countOperationNamed(globalObject->aotInstance(), __func__);
    return JSValue::encode(rows);
}

extern "C" JS_EXPORT_PRIVATE void JSC__AOT__convertLastRow(JSGlobalObject* globalObject, uint32_t expectedRowType, EncodedJSValue encodedArray)
{
    Instance* instance = globalObject->aotInstance();
    JSArray* rows = dynamicDowncast<JSArray>(JSValue::decode(encodedArray));
    if (!instance || !instance->typed || !expectedRowType || !rows || !rows->length() || !rows->canGetIndexQuickly(rows->length() - 1))
        return;
    JSValue row = rows->getIndexQuickly(rows->length() - 1);
    if (row.isObject())
        instance->typed->tryConvertRow(globalObject->vm(), asObject(row), instance->typed->type(expectedRowType - 1));
}

namespace Typed {

std::optional<Writes> writesOfInlineRowDecoder(InlineBuiltin which, std::span<const Type>)
{
    RELEASE_ASSERT(which == InlineBuiltin::SetExpectedRowType);
    return Writes::Heap;
}

std::optional<Operand> Lowering::lowerInlineRowDecoder(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    RELEASE_ASSERT(which == InlineBuiltin::SetExpectedRowType);
    if (TypeID expected = value.prediction())
        AOT_TYPED_OPERATION(SetExpectedRowType, in<Boxed>(value.arguments[0]), word32(expected.index()));
    return ensure(undefined(), m_program.at(m_program.types.undefined), typeOf(result));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
