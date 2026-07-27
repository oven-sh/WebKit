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

#include <wtf/Platform.h>

#if USE(BUN_JSC_ADDITIONS)


#if USE(JSVALUE64)

#include "FFIContext.h"
#include "FFIType.h"
#include "JSCJSValue.h"
#include "JSExportMacros.h"
#include "OperationResult.h"
#include <bit>

namespace JSC {

class JSGlobalObject;

namespace FFI {

// All JS-visible conversion semantics live here so that every tier (C++
// host path, IC-stub slow path, DFG/FTL operations, callback dispatch) is
// behaviorally identical. See SPEC section 5 for the normative tables.

// JS -> slot. Returns false with an exception thrown on the current VM when
// the value cannot be converted. `arena` provides the call-scoped storage for
// the UTF-8 copy of a JS string passed as Type::CString; nullptr merely
// selects the FFIContext's own arena, it does NOT remove the bracketing
// requirement: a JS string may only be converted for Type::CString while the
// destination arena is inside a bracket (StringArena::Scope or
// operationFFIArenaEnter/Exit) -- an un-bracketed CString conversion ASSERTs.
// `arena` may be nullptr freely when `type != Type::CString`.
JS_EXPORT_PRIVATE bool writeSlotFromJSValue(JSGlobalObject*, FFIContext&, Type, JSValue, uint64_t& slotOut, StringArena* arena);

// slot -> JS. Never throws except for out-of-memory when allocating a heap
// BigInt. Used for native return values (JS <- C) and for callback
// arguments (C -> JS).
JS_EXPORT_PRIVATE JSValue jsValueFromSlot(JSGlobalObject*, FFIContext&, Type, uint64_t slot);


// A native pointer exposed to JS: null -> null, an address <= 2^53-1 (Number.MAX_SAFE_INTEGER) -> an exact double, and a
// higher address (5-level page tables / tagged pointers) -> an exact BigInt. This is the single
// boxing rule for every pointer surfaced to JS (return slots AND the `.ptr` own properties
// properties), so `x.ptr` can always be fed back into the FFI without silently losing bits.
JS_EXPORT_PRIVATE JSValue pointerToJSValue(JSGlobalObject*, uint64_t address);

// The ONLY double -> 64-bit-integer conversions used by every tier. Their
// semantics are DEFINED as the target hardware truncation that the JIT tiers
// emit via MacroAssembler::truncateDoubleToInt64 (x86-64 cvttsd2si: NaN or
// |d| >= 2^63 -> 0x8000000000000000; arm64 fcvtzs: saturate to the int64
// range, NaN -> 0). Never a bare static_cast<int64_t>(double), which is UB in
// C++ for NaN / out-of-range inputs.
JS_EXPORT_PRIVATE int64_t doubleToInt64(double);

// Bun's uint64 conversion is a C cast of the int64 conversion (FFI.h), i.e. a
// reinterpretation of doubleToInt64's bits.
inline uint64_t doubleToUInt64(double value)
{
    return std::bit_cast<uint64_t>(doubleToInt64(value));
}

} // namespace FFI

// JIT operations shared by the IC stub (A7), DFG (A9) and FTL (A10).
// typeTag = static_cast<uint32_t>(FFI::Type).
JSC_DECLARE_JIT_OPERATION(operationFFIBoxSlot, EncodedJSValue, (JSGlobalObject*, uint32_t typeTag, uint64_t slot));
JSC_DECLARE_JIT_OPERATION(operationFFIWriteSlot, void, (JSGlobalObject*, FFI::FFIContext*, uint32_t typeTag, EncodedJSValue value, uint64_t* slot));
JSC_DECLARE_JIT_OPERATION(operationFFIArenaEnter, void, (JSGlobalObject*));
JSC_DECLARE_JIT_OPERATION(operationFFIArenaExit, void, (JSGlobalObject*));

} // namespace JSC

#endif // USE(JSVALUE64)

#endif // USE(BUN_JSC_ADDITIONS)

