/*
 * Copyright (C) 2026 Oven-sh Inc. All rights reserved.
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

#if USE(BUN_JSC_ADDITIONS)

#include "FFISignature.h"
#include "FFIType.h"
#include "JSCJSValue.h"
#include <optional>
#include <wtf/Forward.h>
#include <wtf/RefPtr.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSFFICallback;
class JSFFIFunction;
class JSGlobalObject;
class JSObject;

// Embedder-facing entry points for the engine's FFI machinery (JSFFIFunction / JSFFICallback).
// Every function that can fail throws (TypeError, or OOM) on the global object's scope and
// returns nullptr / std::nullopt, so callers only need an exception check.
// FFI-SPEC-GAP: the spec (sections 1, 3, 6) names only signatureFromJS and setNapiEnv; the
// remaining entry points (typeFromJS, napiEnv, createFunction, createCallback) are this row's
// naming for the "creation entry points" the row is required to provide.
namespace FFI {

// Parses a single FFI type from JS: either a canonical name / alias string ("i32", "int32_t",
// "ptr", "void*", ...) or a numeric FFI::Type tag (0..20, wire-compatible with Bun's FFIType).
// Throws a TypeError and returns std::nullopt for anything else.
JS_EXPORT_PRIVATE std::optional<Type> typeFromJS(JSGlobalObject*, JSValue);

// Reads a signature descriptor of the form { args: (string|number)[], returns: string|number }
// (numbers are the FFI::Type tag values) and returns the interned Signature. A missing/undefined
// `args` is an empty argument list; a missing/undefined `returns` is Type::Void (Bun parity). On
// ANY validation failure (unknown type, Void argument, napi_env / buffer return, more than
// Signature::maxArguments arguments) throws a TypeError and returns nullptr.
JS_EXPORT_PRIVATE RefPtr<Signature> signatureFromJS(JSGlobalObject*, JSValue descriptor);

// The synthetic napi_env pointer supplied for Type::NapiEnv parameters. Every tier live-loads it
// at call time, so it may be (re)set after functions have been created.
JS_EXPORT_PRIVATE void setNapiEnv(JSGlobalObject*, void* napiEnv);
JS_EXPORT_PRIVATE void* napiEnv(JSGlobalObject*);

// Creates a JSFFIFunction calling `target` with `signature`. `length` is the signature's JS
// argument count and `name` its function name.
JS_EXPORT_PRIVATE JSFFIFunction* createFunction(JSGlobalObject*, Ref<Signature>&&, void* target, const String& name);
JS_EXPORT_PRIVATE JSFFIFunction* createFunction(JSGlobalObject*, JSValue signatureDescriptor, void* target, const String& name);

// Creates a JSFFICallback wrapping `callable` (which must be callable); its native entry point
// is available via JSFFICallback::nativeEntrypoint() / the JS-visible read-only "ptr" property.
JS_EXPORT_PRIVATE JSFFICallback* createCallback(JSGlobalObject*, Ref<Signature>&&, JSObject* callable);
JS_EXPORT_PRIVATE JSFFICallback* createCallback(JSGlobalObject*, JSValue signatureDescriptor, JSObject* callable);

} // namespace FFI

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
