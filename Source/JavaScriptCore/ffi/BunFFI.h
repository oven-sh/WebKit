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
class JSGlobalObject;
class JSObject;

namespace FFI {

JS_EXPORT_PRIVATE std::optional<Type> typeFromJS(JSGlobalObject*, JSValue);

JS_EXPORT_PRIVATE RefPtr<Signature> signatureFromJS(JSGlobalObject*, JSValue descriptor);

class ThreadsafeInvocation;
JS_EXPORT_PRIVATE JSFFICallback* createCallback(JSGlobalObject*, Ref<Signature>&&, JSObject* callable, bool threadsafe, void* embedderContext);
JS_EXPORT_PRIVATE void runThreadsafeInvocation(ThreadsafeInvocation&);
// For an invocation the embedder's dispatch accepted but will not run (owning thread; takes the API lock itself).
JS_EXPORT_PRIVATE void retireThreadsafeInvocation(ThreadsafeInvocation&);

} // namespace FFI

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
