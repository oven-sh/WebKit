/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "DecimalObject.h"

#include "JSBigInt.h"
#include "JSCInlines.h"

namespace JSC {

const ClassInfo DecimalObject::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(DecimalObject) };

Structure* DecimalObject::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

// https://tc39.es/proposal-decimal/#sec-number-constructor-number-value
double DecimalObject::toNumberValue() const
{
    return m_value.toDouble();
}

// https://tc39.es/proposal-decimal/#sec-numbertobigint
JSValue DecimalObject::toBigIntValue(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!m_value.isInteger()) [[unlikely]] {
        throwRangeError(globalObject, scope, "Not an integer"_s);
        return { };
    }
    RELEASE_AND_RETURN(scope, JSBigInt::stringToBigInt(globalObject, m_value.toIntegerString()));
}

String DecimalObject::textForClone() const
{
    return m_value.toExponentialString();
}

DecimalObject* DecimalObject::tryCreateFromCloneText(JSGlobalObject* globalObject, StringView text)
{
    // Bytes can come from a process with other options.
    if (!canBeCloned())
        return nullptr;
    auto value = Decimal128::parse(text);
    if (!value)
        return nullptr;
    return create(globalObject->vm(), globalObject->decimalStructure(), *value);
}

} // namespace JSC
