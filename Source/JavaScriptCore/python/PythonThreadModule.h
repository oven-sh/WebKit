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

class PyType;

namespace Python {

// What _thread has for each realm: thread_module_state of CPython's Modules/_threadmodule.c. The classes are made when the module is first imported.
struct ThreadModuleState {
    WriteBarrier<PyType> lockType;
    WriteBarrier<PyType> recursiveLockType;
    WriteBarrier<PyType> localType;
    WriteBarrier<PyType> handleType;
    WriteBarrier<PyType> exceptHookArgsType;
    uint64_t mainThread { 0 };
    size_t stackSize { 0 }; // _thread.stack_size()

    template<typename Visitor>
    void visit(Visitor& visitor)
    {
        visitor.append(lockType);
        visitor.append(recursiveLockType);
        visitor.append(localType);
        visitor.append(handleType);
        visitor.append(exceptHookArgsType);
    }
};

} } // namespace JSC::Python
