/*
 * Copyright (C) 2013, 2014 Apple Inc. All rights reserved.
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

#include "JSExportMacros.h"
#include <cstddef>
#include <functional>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC {

class JSGlobalObject;
class VM;

class VMEntryScope {
public:
    VMEntryScope(VM&, JSGlobalObject*);
    ~VMEntryScope();

    VM& vm() const { return m_vm; }
    JSGlobalObject* globalObject() const { return m_globalObject; }
    void setGlobalObject(JSGlobalObject* globalObject) { m_globalObject = globalObject; }

private:
    JS_EXPORT_PRIVATE void setUpSlow();
    JS_EXPORT_PRIVATE void tearDownSlow();

    VM& m_vm;
    JSGlobalObject* m_globalObject;
};

// A VMEntryScope that its owner enters once it knows that it has to: what VM::drainMicrotasks() and
// MicrotaskQueue::performMicrotaskCheckpoint() keep in their frames, which stay on the stack for as long as the
// jobs they drain run. It is not a std::optional<VMEntryScope>, because that keeps its flag in one byte and
// nothing ever writes the seven bytes after it: the flag's word keeps the rest of whatever an earlier frame left
// in that stack slot. Over a pointer to a cell that is a pointer into whichever cell lies at (the old address &
// ~0xff) + the flag, and the conservative scan, which reads whole words and accepts a pointer into the middle of a
// cell, marked that cell at every collection made under the drain. Here the flag is a whole word.
// (MicrotaskCallCache has the same note about the type byte of a MicrotaskCall.)
class OptionalVMEntryScope {
    WTF_MAKE_NONCOPYABLE(OptionalVMEntryScope);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    OptionalVMEntryScope() = default;
    ~OptionalVMEntryScope();

    explicit operator bool() const { return m_isEntered; }
    VMEntryScope* operator->();

    void emplace(VM&, JSGlobalObject*);
    void reset();

private:
    VMEntryScope* scope();

    uintptr_t m_isEntered { 0 };
    alignas(VMEntryScope) std::byte m_storage[sizeof(VMEntryScope)];
};
static_assert(sizeof(OptionalVMEntryScope) == sizeof(uintptr_t) + sizeof(VMEntryScope), "no padding: every word of it is written as a whole");

} // namespace JSC
