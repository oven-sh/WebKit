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

#include "WriteBarrier.h"

namespace JSC {

class JSArray;
class PyDict;
class PyType;

namespace Python {

// What is known of codecs, for each realm: `codecs` of CPython's PyInterpreterState.
struct CodecRegistryState {
    WriteBarrier<JSArray> searchPath; // The functions that are asked for a codec by name, in the order in which they were registered.
    WriteBarrier<PyDict> searchCache; // What they have answered.
    WriteBarrier<PyDict> errorRegistry; // The error handlers, by name.
    WriteBarrier<PyType> encodingMap; // The class of what codecs.charmap_build() returns.

    template<typename Visitor>
    void visit(Visitor& visitor)
    {
        visitor.append(searchPath);
        visitor.append(searchCache);
        visitor.append(errorRegistry);
        visitor.append(encodingMap);
    }
};

} } // namespace JSC::Python
