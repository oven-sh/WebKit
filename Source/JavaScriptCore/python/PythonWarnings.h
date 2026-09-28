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
class JSObject;
class JSString;
class PyDict;

namespace Python {

// What _warnings has for each realm: WarningsState of CPython's pycore_warnings.h.
struct WarningsState {
    WriteBarrier<JSArray> filters;
    WriteBarrier<PyDict> onceRegistry;
    WriteBarrier<JSString> defaultAction;
    WriteBarrier<JSObject> context; // A ContextVar.
    // What has been warned of is forgotten when the filters change.
    long filtersVersion { 0 };
    // There is one thread, so all that there is to the lock is how many times it has been taken.
    unsigned lockDepth { 0 };

    template<typename Visitor>
    void visit(Visitor& visitor)
    {
        visitor.append(filters);
        visitor.append(onceRegistry);
        visitor.append(defaultAction);
        visitor.append(context);
    }
};

} } // namespace JSC::Python
