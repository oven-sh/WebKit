/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "DecimalConstructor.h"

#include "DecimalObject.h"
#include "DecimalPrototype.h"
#include "JSBigInt.h"
#include "JSCInlines.h"

namespace JSC {

STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(DecimalConstructor);

static JSC_DECLARE_HOST_FUNCTION(callDecimal);
static JSC_DECLARE_HOST_FUNCTION(constructDecimal);

const ClassInfo DecimalConstructor::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(DecimalConstructor) };

DecimalConstructor::DecimalConstructor(VM& vm, Structure* structure)
    : Base(vm, structure, callDecimal, constructDecimal)
{
}

DecimalConstructor* DecimalConstructor::create(VM& vm, Structure* structure, DecimalPrototype* prototype)
{
    auto* constructor = new (NotNull, allocateCell<DecimalConstructor>(vm)) DecimalConstructor(vm, structure);
    constructor->finishCreation(vm, prototype);
    return constructor;
}

Structure* DecimalConstructor::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(InternalFunctionType, StructureFlags), info());
}

void DecimalConstructor::finishCreation(VM& vm, DecimalPrototype* prototype)
{
    Base::finishCreation(vm, 1, "Decimal"_s, PropertyAdditionMode::WithoutStructureTransition);
    putDirectWithoutTransition(vm, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
}

JSC_DEFINE_HOST_FUNCTION(callDecimal, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return JSValue::encode(throwConstructorCannotBeCalledAsFunctionTypeError(globalObject, scope, "Decimal"_s));
}

// https://tc39.es/proposal-decimal/#sec-the-decimal-constructor-value
JSC_DEFINE_HOST_FUNCTION(constructDecimal, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue argument = callFrame->argument(0);
    String text;
    if (argument.isBigInt()) {
        text = argument.toWTFString(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    } else if (argument.isNumber()) {
        // Number.prototype.toExponential gives the same digits as this, and like this it drops the sign of -0.
        text = String::number(argument.asNumber() + 0.0);
    } else if (argument.isString()) {
        text = asString(argument)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    } else
        return throwVMTypeError(globalObject, scope, "Decimal requires a string, a number or a BigInt"_s);

    // The prototype is read from newTarget before the text is parsed.
    JSObject* newTarget = asObject(callFrame->newTarget());
    Structure* structure = JSC_GET_DERIVED_STRUCTURE(vm, decimalStructure, newTarget, callFrame->jsCallee());
    RETURN_IF_EXCEPTION(scope, { });

    auto value = Decimal128::parse(text);
    if (!value) [[unlikely]] {
        throwSyntaxError(globalObject, scope, "Failed to parse String to Decimal"_s);
        return { };
    }
    return JSValue::encode(DecimalObject::create(vm, structure, *value));
}

} // namespace JSC
