/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "BunFFI.h"

#if USE(BUN_JSC_ADDITIONS)

#include "Error.h"
#include "FFIContext.h"
#include "IdentifierInlines.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSFFICallback.h"
#include "JSFFIFunction.h"
#include "JSGlobalObject.h"
#include "JSObjectInlines.h"
#include <cmath>
#include <wtf/text/MakeString.h>

namespace JSC { namespace FFI {

std::optional<Type> typeFromJS(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (value.isNumber()) {
        double tag = value.asNumber();
        if (tag == std::trunc(tag) && tag >= 0 && tag < numberOfTypes)
            return static_cast<Type>(static_cast<uint8_t>(tag));
        throwTypeError(globalObject, scope, makeString("Unknown FFI type tag "_s, tag));
        return std::nullopt;
    }

    if (value.isString()) {
        String string = value.toWTFString(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (std::optional<Type> type = parseType(string))
            return type;
        throwTypeError(globalObject, scope, makeString("Unknown FFI type '"_s, string, '\''));
        return std::nullopt;
    }

    throwTypeError(globalObject, scope, "An FFI type must be a string or a number"_s);
    return std::nullopt;
}

RefPtr<Signature> signatureFromJS(JSGlobalObject* globalObject, JSValue descriptor)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* object = descriptor.getObject();
    if (!object) {
        throwTypeError(globalObject, scope, "Expected an FFI signature descriptor object of the form { args, returns }"_s);
        return nullptr;
    }

    Vector<Type, 16> argumentTypes;
    JSValue argsValue = object->get(globalObject, Identifier::fromString(vm, "args"_s));
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!argsValue.isUndefinedOrNull()) {
        JSObject* argsObject = argsValue.getObject();
        if (!argsObject) {
            throwTypeError(globalObject, scope, "FFI signature 'args' must be an array of types"_s);
            return nullptr;
        }
        uint64_t length = toLength(globalObject, argsObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (length > Signature::maxArguments) {
            throwTypeError(globalObject, scope, makeString("FFI signatures support at most "_s, Signature::maxArguments, " arguments"_s));
            return nullptr;
        }
        argumentTypes.reserveInitialCapacity(static_cast<unsigned>(length));
        for (unsigned i = 0; i < length; ++i) {
            JSValue element = argsObject->getIndex(globalObject, i);
            RETURN_IF_EXCEPTION(scope, nullptr);
            std::optional<Type> type = typeFromJS(globalObject, element);
            RETURN_IF_EXCEPTION(scope, nullptr);
            ASSERT(type);
            if (!isValidArgumentType(*type)) {
                throwTypeError(globalObject, scope, makeString("FFI argument type '"_s, name(*type), "' is not allowed"_s));
                return nullptr;
            }
            argumentTypes.append(*type);
        }
    }

    Type returnType = Type::Void;
    JSValue returnsValue = object->get(globalObject, Identifier::fromString(vm, "returns"_s));
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!returnsValue.isUndefinedOrNull()) {
        std::optional<Type> type = typeFromJS(globalObject, returnsValue);
        RETURN_IF_EXCEPTION(scope, nullptr);
        ASSERT(type);
        if (!isValidReturnType(*type)) {
            throwTypeError(globalObject, scope, makeString("FFI return type '"_s, name(*type), "' is not allowed"_s));
            return nullptr;
        }
        returnType = *type;
    }

    RefPtr<Signature> signature = Signature::tryCreate(argumentTypes.span(), returnType);
    if (!signature) {
        throwTypeError(globalObject, scope, "Invalid FFI signature"_s);
        return nullptr;
    }
    return signature;
}

JSFFICallback* createCallback(JSGlobalObject* globalObject, Ref<Signature>&& signature, JSObject* callable, bool threadsafe, void* embedderContext)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!callable || !JSValue(callable).isCallable()) [[unlikely]] {
        throwTypeError(globalObject, scope, "bun:ffi callback requires a callable JS function"_s);
        return nullptr;
    }

    if (!Options::useJIT()) [[unlikely]] {
        throwTypeError(globalObject, scope, "bun:ffi requires the JIT"_s);
        return nullptr;
    }

    RELEASE_AND_RETURN(scope, JSFFICallback::create(vm, globalObject, globalObject->ffiCallbackStructure(), callable, WTF::move(signature), threadsafe, embedderContext));
}

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
