/*
 * Copyright (C) 2013-2022 Apple Inc. All rights reserved.
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

#pragma once

#include "InternalFunction.h"
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(callInt8Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(callInt16Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(callInt32Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(callUint8Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(callUint8ClampedArray);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(callUint16Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(callUint32Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(callFloat16Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(callFloat32Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(callFloat64Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(callBigInt64Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(callBigUint64Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(callDataView);

JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(constructInt8Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(constructInt16Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(constructInt32Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(constructUint8Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(constructUint8ClampedArray);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(constructUint16Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(constructUint32Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(constructFloat16Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(constructFloat32Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(constructFloat64Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(constructBigInt64Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(constructBigUint64Array);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(constructDataView);

template<typename ViewClass>
class JSGenericTypedArrayViewConstructor final : public InternalFunction {
public:
    using Base = InternalFunction;

    static JSGenericTypedArrayViewConstructor* create(
        VM&, JSGlobalObject*, Structure*, JSObject* prototype, const String& name);

    DECLARE_INFO;

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    static constexpr NativeFunction::Ptr callConstructor()
    {
        switch (ViewClass::TypedArrayStorageType) {
        case TypeInt8:
            return JSC_REGISTERED_HOST_FUNCTION(callInt8Array);
        case TypeInt16:
            return JSC_REGISTERED_HOST_FUNCTION(callInt16Array);
        case TypeInt32:
            return JSC_REGISTERED_HOST_FUNCTION(callInt32Array);
        case TypeUint8:
            return JSC_REGISTERED_HOST_FUNCTION(callUint8Array);
        case TypeUint8Clamped:
            return JSC_REGISTERED_HOST_FUNCTION(callUint8ClampedArray);
        case TypeUint16:
            return JSC_REGISTERED_HOST_FUNCTION(callUint16Array);
        case TypeUint32:
            return JSC_REGISTERED_HOST_FUNCTION(callUint32Array);
        case TypeFloat16:
            return JSC_REGISTERED_HOST_FUNCTION(callFloat16Array);
        case TypeFloat32:
            return JSC_REGISTERED_HOST_FUNCTION(callFloat32Array);
        case TypeFloat64:
            return JSC_REGISTERED_HOST_FUNCTION(callFloat64Array);
        case TypeBigInt64:
            return JSC_REGISTERED_HOST_FUNCTION(callBigInt64Array);
        case TypeBigUint64:
            return JSC_REGISTERED_HOST_FUNCTION(callBigUint64Array);
        case TypeDataView:
            return JSC_REGISTERED_HOST_FUNCTION(callDataView);
        default:
            CRASH_UNDER_CONSTEXPR_CONTEXT();
            return nullptr;
        }
    }

    static constexpr NativeFunction::Ptr constructConstructor()
    {
        switch (ViewClass::TypedArrayStorageType) {
        case TypeInt8:
            return JSC_REGISTERED_HOST_FUNCTION(constructInt8Array);
        case TypeInt16:
            return JSC_REGISTERED_HOST_FUNCTION(constructInt16Array);
        case TypeInt32:
            return JSC_REGISTERED_HOST_FUNCTION(constructInt32Array);
        case TypeUint8:
            return JSC_REGISTERED_HOST_FUNCTION(constructUint8Array);
        case TypeUint8Clamped:
            return JSC_REGISTERED_HOST_FUNCTION(constructUint8ClampedArray);
        case TypeUint16:
            return JSC_REGISTERED_HOST_FUNCTION(constructUint16Array);
        case TypeUint32:
            return JSC_REGISTERED_HOST_FUNCTION(constructUint32Array);
        case TypeFloat16:
            return JSC_REGISTERED_HOST_FUNCTION(constructFloat16Array);
        case TypeFloat32:
            return JSC_REGISTERED_HOST_FUNCTION(constructFloat32Array);
        case TypeFloat64:
            return JSC_REGISTERED_HOST_FUNCTION(constructFloat64Array);
        case TypeBigInt64:
            return JSC_REGISTERED_HOST_FUNCTION(constructBigInt64Array);
        case TypeBigUint64:
            return JSC_REGISTERED_HOST_FUNCTION(constructBigUint64Array);
        case TypeDataView:
            return JSC_REGISTERED_HOST_FUNCTION(constructDataView);
        default:
            CRASH_UNDER_CONSTEXPR_CONTEXT();
            return nullptr;
        }
    }

private:
    JSGenericTypedArrayViewConstructor(VM&, Structure*);
    void finishCreation(VM&, JSGlobalObject*, JSObject* prototype, const String& name);
};

JSC_DECLARE_HOST_FUNCTION(uint8ArrayConstructorFromBase64);
JSC_DECLARE_HOST_FUNCTION(uint8ArrayConstructorFromHex);

[[nodiscard]] size_t decodeHex(std::span<const Latin1Character>, std::span<uint8_t> result);
[[nodiscard]] size_t decodeHex(std::span<const char16_t>, std::span<uint8_t> result);

constinit const ASCIILiteral arrayBufferViewErrorMessageOutOfRangeOfBuffer = "Length out of range of buffer"_s;

} // namespace JSC
