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

#if ENABLE(DFG_JIT)

#include "DFGUseKind.h"
#include "FFIType.h"
#include "SpeculatedType.h"

namespace JSC {

class JSFunction;

namespace DFG {
class Graph;
class InsertionSet;
struct Node;
} // namespace DFG

namespace FFI {

bool tryConvertCallToCallFFI(DFG::Graph&, DFG::InsertionSet&, unsigned nodeIndex, DFG::Node*, JSFunction* callee);

SpeculatedType speculatedResultTypeForCallFFI(DFG::Node*);

// What a JIT tier has to know about an argument before it emits the conversion for it.
//
// conversionCanRunJS: the conversion can call back into JS, and that JS can detach, transfer, or
// resize any buffer. Only an argument the DFG could not type can. An integer or a float argument
// coerces through valueOf or Symbol.toPrimitive, and an object passed for a pointer has its 'ptr'
// property read. 'buffer' and 'buffer_length' take a view or throw, and 'napi_value' passes the
// JSValue through, so those three never run JS.
//
// conversionCanTakeBufferSnapshot: the conversion can read an address or a byte length off a live
// TypedArray, DataView, or ArrayBuffer. Such a read holds only until the next JS runs.
bool conversionCanRunJS(Type, DFG::UseKind);
bool conversionCanTakeBufferSnapshot(Type, DFG::UseKind);

} // namespace FFI

} // namespace JSC

#endif // ENABLE(DFG_JIT)

#endif // USE(BUN_JSC_ADDITIONS)
