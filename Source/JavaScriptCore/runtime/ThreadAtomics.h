/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "JSCJSValue.h"
#include "PropertyName.h"

namespace JSC {

class JSGlobalObject;
class JSObject;

// Atomics.* extended to (object, propertyName) pairs (SPEC-api 4.5).
// All operations are SeqCst and trivially atomic under the phase-1 GIL.

enum class AtomicsRMWOp : uint8_t { Add, Sub, And, Or, Xor, Exchange };

JS_EXPORT_PRIVATE JSValue atomicsLoadOnProperty(JSGlobalObject*, JSObject*, PropertyName);
JS_EXPORT_PRIVATE JSValue atomicsStoreOnProperty(JSGlobalObject*, JSObject*, PropertyName, JSValue);
JS_EXPORT_PRIVATE JSValue atomicsRMWOnProperty(JSGlobalObject*, JSObject*, PropertyName, AtomicsRMWOp, JSValue operand);
JS_EXPORT_PRIVATE JSValue atomicsCompareExchangeOnProperty(JSGlobalObject*, JSObject*, PropertyName, JSValue expected, JSValue replacement);

// wait/waitAsync/notify on property waiters, keyed (JSCell*, UniquedStringImpl*)
// (SPEC-api 5.6). Host-call only; never reached from JIT operations.
JS_EXPORT_PRIVATE JSValue atomicsWaitOnProperty(JSGlobalObject*, JSObject*, PropertyName, JSValue expected, JSValue timeout);
JS_EXPORT_PRIVATE JSValue atomicsWaitAsyncOnProperty(JSGlobalObject*, JSObject*, PropertyName, JSValue expected, JSValue timeout);
JS_EXPORT_PRIVATE JSValue atomicsNotifyOnProperty(JSGlobalObject*, JSObject*, PropertyName, JSValue count);

} // namespace JSC
