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

#include <wtf/Platform.h>

#if USE(BUN_JSC_ADDITIONS)


#include "FFIContext.h"
#include "FFIType.h"
#include "JSCJSValue.h"
#include "JSExportMacros.h"
#include "OperationResult.h"

namespace JSC {

class JSGlobalObject;

namespace FFI {

JS_EXPORT_PRIVATE bool writeSlotFromJSValue(JSGlobalObject*, FFIContext&, Type, JSValue, uint64_t& slotOut, StringArena* arena);

JS_EXPORT_PRIVATE JSValue jsValueFromSlot(JSGlobalObject*, FFIContext&, Type, uint64_t slot);

JS_EXPORT_PRIVATE JSValue pointerToJSValue(JSGlobalObject*, uint64_t address);

// The pointer-typed argument conversion for a Number: the CPU's truncating double -> int64 instruction,
// which is also what the IC stub and the DFG emit inline (truncateDoubleToInt64) for those types.
// i64 / u64 arguments use the modular JSC::toInt64 instead; see writeInt64Slot.
JS_EXPORT_PRIVATE int64_t doubleToInt64(double);

} // namespace FFI

JSC_DECLARE_JIT_OPERATION(operationFFIBoxSlot, EncodedJSValue, (JSGlobalObject*, uint32_t typeTag, uint64_t slot, int32_t exitArena));
JSC_DECLARE_JIT_OPERATION(operationFFIWriteSlot, void, (JSGlobalObject*, FFI::FFIContext*, uint32_t typeTag, EncodedJSValue value, uint64_t* slot));
JSC_DECLARE_JIT_OPERATION(operationFFIArenaEnter, void, (JSGlobalObject*));
JSC_DECLARE_JIT_OPERATION(operationFFIArenaExit, void, (JSGlobalObject*));

} // namespace JSC


#endif // USE(BUN_JSC_ADDITIONS)
