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

class JSObject;

namespace Python {

// What `struct _import_state` of CPython has for each interpreter, but for sys.modules, which the realm has.
struct ImportState {
    WriteBarrier<JSObject> importlib; // The module _frozen_importlib, which is importlib._bootstrap.
    WriteBarrier<Unknown> importFunction; // What builtins.__import__ was to begin with.
    unsigned lockDepth { 0 }; // How many times the import lock has been taken. There is one thread.
    int overrideOfFrozenModules { 0 };
    bool isStarted { false }; // Whether all that is done before a program is run has been done.
    bool hasCodecRegistry { false }; // Whether a codec can be looked for by name: `interp->unicode.fs_codec.encoding`, which _PyUnicode_InitEncodings() sets
    bool hasUnhandledKeyboardInterrupt { false }; // _PyRuntime.signals.unhandled_keyboard_interrupt
    bool mainHasRaised { false }; // See mainHasRaised().
    // What Python was doing as it started, for saying what it was that could not be done: the PyStatus that CPython would end with
    ASCIILiteral startingFunction;
    ASCIILiteral startingFailure;
    unsigned typedStatementCount { 0 }; // interp->_interactive_src_count
    // For -X importtime: `find_and_load` of CPython's struct _import_state
    bool hasImportTimeHeaderToPrint { true };
    int importLevel { 0 };
    int64_t accumulatedImportTime { 0 };

    template<typename Visitor>
    void visit(Visitor& visitor)
    {
        visitor.append(importlib);
        visitor.append(importFunction);
    }
};

} } // namespace JSC::Python
