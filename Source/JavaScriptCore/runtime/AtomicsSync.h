/*
 * Copyright (C) 2026 Jordan Hall. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "JSCJSValue.h"

namespace JSC {

class JSArrayBuffer;
class JSGlobalObject;
class JSObject;
class VM;

// Defines Atomics.Mutex, Atomics.Mutex.UnlockToken and Atomics.Condition on `atomics`.
void installAtomicsSync(VM&, JSGlobalObject*, JSObject* atomics);

// A mutex or condition is an object around a one-int32 SharedArrayBuffer that is its whole state. An embedder
// shares one between agents by sharing that buffer and wrapping it again in the receiving global object.
// These return null, with or without an exception pending, if the global object has no Atomics.Mutex/Condition.
JS_EXPORT_PRIVATE JSObject* createAtomicsMutex(JSGlobalObject*, JSArrayBuffer* state);
JS_EXPORT_PRIVATE JSObject* createAtomicsCondition(JSGlobalObject*, JSArrayBuffer* state);

// The state buffer of a mutex or a condition, or null if `value` is not one.
JS_EXPORT_PRIVATE JSArrayBuffer* atomicsMutexState(VM&, JSValue);
JS_EXPORT_PRIVATE JSArrayBuffer* atomicsConditionState(VM&, JSValue);

} // namespace JSC
