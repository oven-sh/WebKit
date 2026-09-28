/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#include "PythonAST.h"

namespace JSC {

class VM;

namespace Python {

class Arena;

// _PyAST_Preprocess(), but for what it warns of, which is collectControlFlowWarnings(). It is what compile(..., PyCF_OPTIMIZED_AST) shows: what generates code here works these things out for itself.
//
//   - With `optimizationLevel` 2, docstrings are taken out.
//
// And unless it is `onlyLookedOver`, which is compile(..., PyCF_ONLY_AST):
//
//   - `__debug__` is True or False.
//   - `'%s and %r' % (a, b)` is `f'{a!s} and {b!r}'`.
//   - In a pattern, `-1` and `1+2j` are constants.
//
// False if it is too deep.
bool optimize(VM&, Arena&, Module&, unsigned optimizationLevel, unsigned futureFeatures, bool onlyLookedOver);

} } // namespace JSC::Python
