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

namespace JSC {

// A "buffer accessor" is an ordinary host JSFunction, placed on some prototype by the embedder,
// whose receiver is a JSArrayBufferView (Node.js's Buffer.prototype.readInt32LE / writeDoubleBE
// ... on a Uint8Array) and which reads or writes a fixed-width scalar at a byte-offset argument
// with array-index semantics: `read*(offset = 0)` and `write*(value, offset = 0)`, throwing on any
// non-int32-in-bounds offset (the host function owns the exact error). All accessors share ONE
// intrinsic (BufferAccessorIntrinsic); the width / signedness / float-ness / endianness /
// read-vs-write is data, registered here against the (process-wide unique) native function pointer
// and copied into the DFG's BufferReadInt / BufferReadFloat / BufferWrite node at parse time. The
// DFG/FTL then compile the call site down to a bounds-checked load/store on the receiver's storage,
// OSR-exiting to the host function for everything they do not speculate (bad receiver, non-int32
// or out-of-bounds offset, out-of-range value), so the host function is the single source of truth
// for error behavior.
struct BufferAccessorDescriptor {
    DFG::DataViewData data;
    bool isWrite;
};

// Register `function` as a buffer accessor. Must be called (on any thread, at any time before the
// function can be called from JIT-compiled code -- typically when the embedder creates the prototype)
// exactly once per distinct native function; registering the same function twice is harmless if the
// descriptor matches. Lookups happen concurrently on compiler threads.
JS_EXPORT_PRIVATE void registerBufferAccessor(TaggedNativeFunction function, BufferAccessorDescriptor);

// nullopt if `function` was never registered (the DFG then leaves the plain Call).
JS_EXPORT_PRIVATE std::optional<BufferAccessorDescriptor> bufferAccessorDescriptor(TaggedNativeFunction function);

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
