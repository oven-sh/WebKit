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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include "DFGDataViewData.h"
#include "NativeFunction.h"
#include <optional>
#include <wtf/Forward.h>

namespace JSC {

class JSGlobalObject;
class JSObject;

namespace FFI {

// Every reader is an ordinary host JSFunction; all share ONE intrinsic (FFIRawReadIntrinsic).
// The DFG ByteCodeParser identifies WHICH reader a call site targets by the callee's (distinct)
// native function pointer, via this lookup: returns the reader's DFG::DataViewData (byteSize /
// isSigned / isFloatingPoint), or nullopt if `function` is not a JIT-lowered reader (i64/u64 are
// BigInt-returning and stay on the host path). No new cell type / IsoSubspace is needed.
JS_EXPORT_PRIVATE std::optional<DFG::DataViewData> rawReaderDataViewData(TaggedNativeFunction function);

// Creates bun:ffi's `read` singleton: an object whose u8/i8/u16/i16/u32/i32/i64/u64/f32/f64/ptr/
// intptr functions read raw memory at `address + byteOffset` with no bounds checking (the caller
// owns the memory, exactly like the historical TinyCC-era readers). `address` accepts everything a
// `ptr` FFI argument accepts (number, BigInt, TypedArray/DataView/ArrayBuffer, null/undefined -> 0).
// Each reader except i64/u64 (BigInt results) carries an intrinsic, so DFG/FTL compile the call site
// down to a bare load (FFIRawRead), with the address as an unboxed Int52 / Int32 / truncated double.
JS_EXPORT_PRIVATE JSObject* createReadObject(JSGlobalObject*);

} // namespace FFI
} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
