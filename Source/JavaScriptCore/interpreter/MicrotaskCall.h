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

#include <JavaScriptCore/CallLinkInfoBase.h>
#include <JavaScriptCore/ExceptionHelpers.h>
#include <JavaScriptCore/FunctionExecutable.h>
#include <JavaScriptCore/JSFunction.h>
#include <memory>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/MathExtras.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class Interpreter;
class VM;

class MicrotaskCall final : public CallLinkInfoBase {
    WTF_MAKE_NONCOPYABLE(MicrotaskCall);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    static constexpr unsigned maxCallArguments = 6;

    MicrotaskCall()
        : CallLinkInfoBase(CallSiteType::MicrotaskCall)
    { }

    ~MicrotaskCall()
    {
        // AB17e F4 (object-lifetime closure): delist FIRST, before the member
        // teardown below — same contract as ~CachedCall. A locked drain can
        // be mid-unlinkOrUpgradeImpl on this node (reading m_addressForCall /
        // m_codeBlock) when this object dies; removeOnDestruction acquires
        // the link lock unconditionally gilOff. ~CallLinkInfoBase's own
        // gilOff delist would run only AFTER this store — too late.
        if (processIsGILOff()) [[unlikely]]
            removeOnDestruction();
        WTF::atomicStore(&m_addressForCall, static_cast<void*>(nullptr), std::memory_order_relaxed); // THREADS: see unlinkOrUpgradeImpl.
    }

    void initialize(VM&, JSFunction*);

    template<typename... Args> requires (std::is_convertible_v<Args, JSValue> && ...)
    JSValue tryCallWithArguments(VM&, JSFunction*, JSValue thisValue, JSCell* context, Args...);

    bool isInitializedFor(ExecutableBase* executable) const
    {
        return m_functionExecutable == executable;
    }

    FunctionExecutable* functionExecutable() { return m_functionExecutable; }

    void unlinkOrUpgradeImpl(VM&, CodeBlock* oldCodeBlock, CodeBlock* newCodeBlock);
    void relink(VM&, JSFunction*);

    void clear();
    void reconcileWeakReferencesAtGCEnd(VM&);

private:
    CodeBlock* m_codeBlock { nullptr };
    FunctionExecutable* m_functionExecutable { nullptr };
    void* m_addressForCall { nullptr };
    unsigned m_numParameters { 0 };
    friend class Interpreter;
};

class MicrotaskCallCache final {
    WTF_MAKE_NONCOPYABLE(MicrotaskCallCache);
    WTF_MAKE_TZONE_ALLOCATED(MicrotaskCallCache);
public:
    static constexpr unsigned cacheSize = 8;
    static_assert(hasOneBitSet(cacheSize));

    // The cache lives on the stack, where the conservative scan reads whole words. A MicrotaskCall has
    // padding (after CallLinkInfoBase's one-byte type) that its constructor does not write, so an entry
    // built over a slot an earlier frame left a cell pointer in would keep all but the low byte of it,
    // and the scan takes that for a pointer into the cell. The entries are built over zeroed storage.
    MicrotaskCallCache()
    {
        zeroBytes(m_storage);
        for (unsigned i = 0; i < cacheSize; ++i)
            std::construct_at(&entries()[i]);
    }

    ~MicrotaskCallCache()
    {
        for (auto& entry : entries())
            std::destroy_at(&entry);
    }

    ALWAYS_INLINE MicrotaskCall* find(JSValue functionObject)
    {
        if (!functionObject.isCell()) [[unlikely]]
            return nullptr;
        auto* cell = functionObject.asCell();
        if (cell->type() != JSFunctionType) [[unlikely]]
            return nullptr;
        auto* executable = uncheckedDowncast<JSFunction>(cell)->executable();
        for (auto& entry : entries()) {
            if (entry.isInitializedFor(executable))
                return &entry;
        }
        return nullptr;
    }

    ALWAYS_INLINE MicrotaskCall* nextEntryToReplace()
    {
        auto* result = &entries()[m_nextEntryIndex];
        m_nextEntryIndex = (m_nextEntryIndex + 1) & (cacheSize - 1);
        return result;
    }

    void clear()
    {
        for (auto& entry : entries())
            entry.clear();
    }

    void reconcileWeakReferencesAtGCEnd(VM& vm)
    {
        for (auto& entry : entries())
            entry.reconcileWeakReferencesAtGCEnd(vm);
    }

private:
    std::span<MicrotaskCall, cacheSize> entries() { return std::span<MicrotaskCall, cacheSize> { std::bit_cast<MicrotaskCall*>(&m_storage[0]), cacheSize }; }

    alignas(MicrotaskCall) std::array<uint8_t, sizeof(MicrotaskCall) * cacheSize> m_storage;
    unsigned m_nextEntryIndex { 0 };
};

} // namespace JSC
