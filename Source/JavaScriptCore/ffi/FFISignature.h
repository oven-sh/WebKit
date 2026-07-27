/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
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

#include <wtf/Platform.h>

#if USE(BUN_JSC_ADDITIONS)

#include "FFIType.h"
#include "JSCPtrTag.h"
#include "JSExportMacros.h"
#include "MacroAssemblerCodeRef.h"
#include <atomic>
#include <span>
#include <wtf/FixedVector.h>
#include <wtf/Forward.h>
#include <wtf/HashFunctions.h>
#include <wtf/HashSet.h>
#include <wtf/HashTraits.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/PlatformCallingConventions.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace FFI {

using InvokeThunkFunction = void (JIT_OPERATION_ATTRIBUTES *)(void* target, uint64_t* slots);

constexpr size_t slotSize = 8;

constexpr size_t argumentSlotOffset(unsigned index) { return static_cast<size_t>(index) * slotSize; }

class Signature final : public ThreadSafeRefCounted<Signature> {
    WTF_MAKE_TZONE_ALLOCATED(Signature);
    WTF_MAKE_NONCOPYABLE(Signature);
public:
    static constexpr unsigned maxArguments = 32;

    JS_EXPORT_PRIVATE static RefPtr<Signature> tryCreate(std::span<const Type> arguments, Type returnType);

    unsigned argumentCount() const { return static_cast<unsigned>(m_arguments.size()); }
    Type argumentType(unsigned index) const { return m_arguments[index]; }
    std::span<const Type> arguments() const LIFETIME_BOUND { return m_arguments.span(); }
    Type returnType() const { return m_returnType; }

    unsigned slotCount() const { return argumentCount() + 1; }
    size_t slotBufferBytes() const { return static_cast<size_t>(slotCount()) * slotSize; }

    unsigned hash() const { return m_hash; }
    JS_EXPORT_PRIVATE String toString() const;

    JS_EXPORT_PRIVATE bool operator==(const Signature&) const;

    JS_EXPORT_PRIVATE static bool structurallyEquals(std::span<const Type> arguments, Type returnType, const Signature&);
    JS_EXPORT_PRIVATE static unsigned computeHash(std::span<const Type> arguments, Type returnType);

    JS_EXPORT_PRIVATE CodePtr<JITThunkPtrTag> invokeThunk();

private:
    friend class SignatureRegistry;
    Signature(std::span<const Type> arguments, Type returnType);

    FixedVector<Type> m_arguments;
    Type m_returnType;
    unsigned m_hash { 0 };

    Lock m_codeLock;
    MacroAssemblerCodeRef<JITThunkPtrTag> m_invokeThunkCode WTF_GUARDED_BY_LOCK(m_codeLock);
    std::atomic<void*> m_publishedInvokeThunk { nullptr };
};

struct SignatureHash {
    static unsigned hash(const Ref<Signature>& signature) { return signature->hash(); }
    static bool equal(const Ref<Signature>& a, const Ref<Signature>& b) { return a.ptr() == b.ptr() || Signature::structurallyEquals(a->arguments(), a->returnType(), b.get()); }
    static constexpr bool safeToCompareToEmptyOrDeleted = false;
};

class SignatureRegistry {
    WTF_MAKE_TZONE_ALLOCATED(SignatureRegistry);
    WTF_MAKE_NONCOPYABLE(SignatureRegistry);
public:
    SignatureRegistry() = default;

    JS_EXPORT_PRIVATE static SignatureRegistry& singleton();

    JS_EXPORT_PRIVATE Ref<Signature> intern(std::span<const Type> arguments, Type returnType);

private:
    Lock m_lock;
    UncheckedKeyHashSet<Ref<Signature>, SignatureHash> m_signatures WTF_GUARDED_BY_LOCK(m_lock);
};

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
