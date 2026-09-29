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
class PyType;

namespace Python {

#define FOR_EACH_PYTHON_POSIX_TYPE(v) \
    v(statResult) v(statVFSResult) v(terminalSize) v(timesResult) v(unameResult) v(waitidResult) v(resourceUsage) v(dirEntry) v(scandirIterator)

// What posix has for each realm: _posixstate of CPython's Modules/posixmodule.c.
struct PosixModuleState {
#define DECLARE(name) WriteBarrier<PyType> name;
    FOR_EACH_PYTHON_POSIX_TYPE(DECLARE)
#undef DECLARE
    WriteBarrier<JSObject> module; // Its tables of names are looked up in it, so that a program that changes them is heeded.
    WriteBarrier<Unknown> newOfStatResult; // What any struct sequence has for __new__(). stat_result has one of its own, which calls it.

    template<typename Visitor>
    void visit(Visitor& visitor)
    {
#define VISIT(name) visitor.append(name);
        FOR_EACH_PYTHON_POSIX_TYPE(VISIT)
#undef VISIT
        visitor.append(module);
        visitor.append(newOfStatResult);
    }
};

} } // namespace JSC::Python
