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

#if ENABLE(DFG_JIT)

#include "SpeculatedType.h"

namespace JSC {

class JSFunction;

namespace DFG {
class Graph;
class InsertionSet;
struct Node;
} // namespace DFG

namespace FFI {

// Call -> CallFFI conversion (SPEC section 10.2), invoked from
// DFGStrengthReductionPhase's Call case with the constant callee it already
// resolved. Handles the JSFFIFunction downcast, the useFFICallInDFG() /
// op == Call / exact-arity gates, the parameter-slot reservation for the
// canonical slot buffer, and the per-argument checks and edge use kinds
// (PredictionPropagation and Fixup have already run, so they are fixed
// here and nowhere else). Returns true if the node was converted.
bool tryConvertCallToCallFFI(DFG::Graph&, DFG::InsertionSet&, unsigned nodeIndex, DFG::Node*, JSFunction* callee);

// The AbstractInterpreter's result filter for a CallFFI node: the
// return-type refinement of SPEC section 10.3, derived from the callee's
// interned signature.
SpeculatedType speculatedResultTypeForCallFFI(DFG::Node*);

} // namespace FFI

} // namespace JSC

#endif // ENABLE(DFG_JIT)

#endif // USE(BUN_JSC_ADDITIONS)
