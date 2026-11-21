/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2003-2017 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include "ErrorConstructor.h"

#include "ErrorPrototype.h"
#include "JSCInlines.h"

namespace JSC {

STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(ErrorConstructor);

const ClassInfo ErrorConstructor::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ErrorConstructor) };

static JSC_DECLARE_HOST_FUNCTION(callErrorConstructor);
static JSC_DECLARE_HOST_FUNCTION(constructErrorConstructor);
static JSC_DECLARE_HOST_FUNCTION(errorConstructorCaptureStackTrace);
static JSC_DECLARE_HOST_FUNCTION(errorConstructorIsError);

ErrorConstructor::ErrorConstructor(VM& vm, Structure* structure)
    : InternalFunction(vm, structure, callErrorConstructor, constructErrorConstructor)
{
}

void ErrorConstructor::finishCreation(VM& vm, ErrorPrototype* errorPrototype)
{
    JSGlobalObject* globalObject = errorPrototype->realm();

    Base::finishCreation(vm, 1, vm.propertyNames->Error.string(), PropertyAdditionMode::WithoutStructureTransition);
    // ECMA 15.11.3.1 Error.prototype
    putDirectWithoutTransition(vm, vm.propertyNames->prototype, errorPrototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
    putDirectWithoutTransition(vm, vm.propertyNames->stackTraceLimit, jsNumber(Options::defaultErrorStackTraceLimit()), static_cast<unsigned>(PropertyAttribute::None));

    JSC_NATIVE_FUNCTION_WITHOUT_TRANSITION(vm.propertyNames->captureStackTrace, errorConstructorCaptureStackTrace, static_cast<unsigned>(PropertyAttribute::DontEnum), 0, ImplementationVisibility::Public);
    JSC_NATIVE_INTRINSIC_FUNCTION_WITHOUT_TRANSITION("isError"_s, errorConstructorIsError, static_cast<unsigned>(PropertyAttribute::DontEnum), 1, ImplementationVisibility::Public, ErrorIsErrorIntrinsic);
}

// ECMA 15.9.3

JSC_DEFINE_HOST_FUNCTION(constructErrorConstructor, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue message = callFrame->argument(0);
    JSValue options = callFrame->argument(1);

    JSObject* newTarget = asObject(callFrame->newTarget());
    Structure* errorStructure = JSC_GET_DERIVED_STRUCTURE(vm, errorStructure, newTarget, callFrame->jsCallee());
    RETURN_IF_EXCEPTION(scope, { });

    bool isErrorSubclass = newTarget != callFrame->jsCallee();

    RELEASE_AND_RETURN(scope, JSValue::encode(ErrorInstance::create(globalObject, errorStructure, message, options, nullptr, TypeNothing, ErrorType::Error, false, isErrorSubclass ? newTarget : nullptr)));
}

JSC_DEFINE_HOST_FUNCTION(callErrorConstructor, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSValue message = callFrame->argument(0);
    JSValue options = callFrame->argument(1);
    Structure* errorStructure = globalObject->errorStructure();
    return JSValue::encode(ErrorInstance::create(globalObject, errorStructure, message, options, nullptr, TypeNothing, ErrorType::Error, false));
}

JSC_DEFINE_HOST_FUNCTION(errorConstructorCaptureStackTrace, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // A Proxy keeps its properties on its target: the define below would land on the Proxy's own cell, where nothing reads it.
    // V8 takes no Proxy either.
    if (!callFrame->argument(0).isObject() || callFrame->argument(0).asCell()->type() == ProxyObjectType) {
        throwTypeError(globalObject, scope, "captureStackTrace expects the first argument to be an object"_s);
        return { };
    }

    JSObject* object = callFrame->argument(0).getObject();
    // What follows defines a stack property and tells an ErrorInstance that its own is materialized. For such an object the define
    // is refused, and an ErrorInstance would be left without the stack it has yet to materialize.
    if (object->hasImmutableProperties() && !object->hasFrozenElementsInPlace()) [[unlikely]]
        return throwVMTypeError(globalObject, scope, ImmutablePropertyDefineError);
    // putDirect() does not ask whether the object is extensible, or whether its stack property is configurable. V8 asks both before
    // it captures a frame, and it refuses an object that is not extensible even if that object has a stack property.
    Structure* structure = object->structure();
    if (structure->isNonExtensibleOrHasNonConfigurableProperties()) {
        // A class that answers isExtensible() itself is asked, because its Structure does not say: that of globalThis says "not
        // extensible" from the start, and that of an object of compiled code never says it. Of those classes only a Proxy runs a
        // trap.
        bool isExtensible = structure->typeInfo().overridesIsExtensible() ? object->isExtensible(globalObject) : structure->isStructureExtensible();
        RETURN_IF_EXCEPTION(scope, { });
        if (!isExtensible)
            return throwVMTypeError(globalObject, scope, "Cannot define property stack, object is not extensible"_s);
        unsigned attributes;
        if (object->getDirect(vm, vm.propertyNames->stack, attributes) && (attributes & PropertyAttribute::DontDelete))
            return throwVMTypeError(globalObject, scope, "Cannot redefine property: stack"_s);
    }
    JSValue callerArg = callFrame->argument(1);
    JSCell* caller = callerArg.isCallable() ? callerArg.asCell() : nullptr;

    Vector<StackFrame> stackTrace;
    // skip this function.
    const size_t framesToSkip = 1;

    vm.interpreter.getStackTrace(object, stackTrace, framesToSkip, globalObject->stackTraceLimit().value_or(0), caller);

    if (auto* errorInstance = dynamicDowncast<ErrorInstance>(object)) {
        errorInstance->setStackPropertyProvidedByCapturedStackTrace();
        if (errorInstance->trySaveCapturedStackTraceForLazyMaterialization(vm, stackTrace))
            return encodedJSUndefined();
    }

    JSValue stack = jsString(vm, Interpreter::stackTraceAsString(vm, stackTrace));
    if (!object->putDirect(vm, vm.propertyNames->stack, stack, static_cast<unsigned>(PropertyAttribute::DontEnum))) [[unlikely]] {
        scope.release();
        object->methodTable()->defineOwnProperty(object, globalObject, vm.propertyNames->stack, PropertyDescriptor(stack, static_cast<unsigned>(PropertyAttribute::DontEnum)), true);
    }

    return encodedJSUndefined();
}

JSC_DEFINE_HOST_FUNCTION(errorConstructorIsError, (JSGlobalObject*, CallFrame* callFrame))
{
    JSObject* object = dynamicDowncast<JSObject>(callFrame->argument(0));
    return JSValue::encode(jsBoolean(object && object->isErrorInstance()));
}

} // namespace JSC
