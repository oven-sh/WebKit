/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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

#include "VM.h"
#include "VMEntryScope.h"
#include <memory>
#include <new>

namespace JSC {

ALWAYS_INLINE VMEntryScope::VMEntryScope(VM& vm, JSGlobalObject* globalObject)
    : m_vm(vm)
    , m_globalObject(globalObject)
{
    if (!vm.entryScope)
        setUpSlow();
    vm.clearLastException();
}

ALWAYS_INLINE VMEntryScope::~VMEntryScope()
{
    if (m_vm.entryScope != this)
        return;
    tearDownSlow();
}

ALWAYS_INLINE VMEntryScope* OptionalVMEntryScope::scope()
{
    return std::launder(reinterpret_cast<VMEntryScope*>(m_storage));
}

ALWAYS_INLINE VMEntryScope* OptionalVMEntryScope::operator->()
{
    ASSERT(m_isEntered);
    return scope();
}

ALWAYS_INLINE void OptionalVMEntryScope::emplace(VM& vm, JSGlobalObject* globalObject)
{
    ASSERT(!m_isEntered);
    new (NotNull, m_storage) VMEntryScope(vm, globalObject);
    m_isEntered = 1;
}

ALWAYS_INLINE void OptionalVMEntryScope::reset()
{
    if (!m_isEntered)
        return;
    std::destroy_at(scope());
    m_isEntered = 0;
}

ALWAYS_INLINE OptionalVMEntryScope::~OptionalVMEntryScope()
{
    reset();
}

} // namespace JSC
