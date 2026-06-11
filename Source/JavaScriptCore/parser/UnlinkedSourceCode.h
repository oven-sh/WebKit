/*
 * Copyright (C) 2008-2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "SourceProvider.h"
#include <type_traits>
#include <wtf/RefPtr.h>

namespace JSC {

    // TSAN family 5 code-lifecycle (docs/threads/TSAN-TRIAGE.md §3.5; r4 §12
    // "SourceCode/UnlinkedSourceCode ctor-publication singles"): a SourceCode
    // embedded in a (Function/Script)Executable or UnlinkedCodeBlock is
    // constructed/copied on one thread and its words are then read by other
    // mutators / concurrent compiler threads. The REAL ordering is the owning
    // cell's publication (address dependency through the published executable
    // pointer — the §9.1 ctor-publication acceptance); these wrappers are the
    // campaign's ctor-class annotation so TSAN pairs atomic/atomic instead of
    // reporting the plain stores. The previous wave's ctor-BODY
    // tsanRelaxedStore annotation was insufficient on two counts, both
    // observed in the r4 stacks:
    //  - the classes' IMPLICIT copy constructors did plain memberwise stores
    //    (the `SourceCode::SourceCode(const SourceCode&)` /
    //    `UnlinkedSourceCode::UnlinkedSourceCode(const UnlinkedSourceCode&)`
    //    writer frames in ScriptExecutable's m_source init), bypassing the
    //    annotated value-constructor bodies entirely;
    //  - the m_provider RefPtr word was never annotated (the
    //    `SourceCode::provider x UnlinkedSourceCode::UnlinkedSourceCode`
    //    reader pair).
    // The members themselves are wrappers whose storage is ONLY ever touched
    // through relaxed __atomic loads/stores — member storage is a union (or a
    // manually managed pointer word), so no plain default/copy-construction
    // store ever lands in the racing-access set — and the classes' implicit
    // copy/move special members route through the wrappers' atomic ops
    // memberwise.
    //
    // UNCONDITIONAL (wave-5 review amendment): an earlier draft gated these
    // under TSAN_ENABLED only, which was a structural suppression without a
    // §9.1-style recorded justification — production GIL-off builds kept the
    // plain racing accesses (the UB this campaign removes) while TSAN saw
    // atomics, and every future access through these members would have been
    // permanently invisible to TSAN. No bench argument applies: a relaxed
    // atomic load/store of an int or a pointer is codegen-identical to the
    // plain access on all supported targets, so flag-off semantics and
    // codegen are unchanged with the wrappers unconditional (same campaign
    // convention as WriteBarrier.h / ValueProfile.h, wave 3).

    // RefPtr<SourceProvider> stand-in: same single-pointer representation and
    // the exact RefPtr ref/deref discipline (leakRef on adopt-in, refIfNotNull
    // on copy, derefIfNotNull on replace/destroy, hash-table deleted-value
    // marker), but every read/write of the pointer word is a relaxed atomic.
    //
    // NOTE (single-writer contract, made explicit per the wave-5 review): the
    // relaxed atomics make the pointer-word accesses defined; they do NOT
    // make assignment safe against a concurrent reader. operator= derefs the
    // displaced provider, so a reader's load-then-ref can still race a
    // refcount-to-zero if an assignment were ever genuinely concurrent with a
    // reader. The blessed shape is ctor-publication only: the SourceCode is
    // constructed/copied on one thread and published via the owning cell
    // (address dependency through the executable pointer, §9.1 acceptance);
    // post-publication the provider word is immutable. Any future mutation of
    // a published SourceCode's provider needs real synchronization, and TSAN
    // can now see it.
    class RelaxedSourceProviderPtr {
    public:
        ALWAYS_INLINE RelaxedSourceProviderPtr(std::nullptr_t) { storeRelaxed(nullptr); }
        ALWAYS_INLINE RelaxedSourceProviderPtr(WTF::HashTableDeletedValueType) { storeRelaxed(deletedValue()); }
        ALWAYS_INLINE RelaxedSourceProviderPtr(Ref<SourceProvider>&& provider) { storeRelaxed(&provider.leakRef()); }
        ALWAYS_INLINE RelaxedSourceProviderPtr(RefPtr<SourceProvider>&& provider) { storeRelaxed(provider.leakRef()); }
        ALWAYS_INLINE RelaxedSourceProviderPtr(const RelaxedSourceProviderPtr& other)
        {
            SourceProvider* pointer = other.loadRelaxed();
            WTF::DefaultRefDerefTraits<SourceProvider>::refIfNotNull(pointer);
            storeRelaxed(pointer);
        }
        ALWAYS_INLINE RelaxedSourceProviderPtr(RelaxedSourceProviderPtr&& other)
        {
            storeRelaxed(other.loadRelaxed());
            other.storeRelaxed(nullptr);
        }
        ALWAYS_INLINE ~RelaxedSourceProviderPtr() { WTF::DefaultRefDerefTraits<SourceProvider>::derefIfNotNull(loadRelaxed()); }

        ALWAYS_INLINE RelaxedSourceProviderPtr& operator=(const RelaxedSourceProviderPtr& other)
        {
            SourceProvider* pointer = other.loadRelaxed();
            WTF::DefaultRefDerefTraits<SourceProvider>::refIfNotNull(pointer);
            SourceProvider* old = loadRelaxed();
            storeRelaxed(pointer);
            WTF::DefaultRefDerefTraits<SourceProvider>::derefIfNotNull(old);
            return *this;
        }
        ALWAYS_INLINE RelaxedSourceProviderPtr& operator=(RelaxedSourceProviderPtr&& other)
        {
            SourceProvider* pointer = other.loadRelaxed();
            other.storeRelaxed(nullptr);
            SourceProvider* old = loadRelaxed();
            storeRelaxed(pointer);
            WTF::DefaultRefDerefTraits<SourceProvider>::derefIfNotNull(old);
            return *this;
        }
        ALWAYS_INLINE RelaxedSourceProviderPtr& operator=(RefPtr<SourceProvider>&& provider)
        {
            SourceProvider* old = loadRelaxed();
            storeRelaxed(provider.leakRef());
            WTF::DefaultRefDerefTraits<SourceProvider>::derefIfNotNull(old);
            return *this;
        }

        ALWAYS_INLINE SourceProvider* get() const { return loadRelaxed(); }
        ALWAYS_INLINE SourceProvider& operator*() const { return *loadRelaxed(); }
        ALWAYS_INLINE SourceProvider* operator->() const { return loadRelaxed(); }
        ALWAYS_INLINE explicit operator bool() const { return !!loadRelaxed(); }
        ALWAYS_INLINE bool operator!() const { return !loadRelaxed(); }
        ALWAYS_INLINE bool isHashTableDeletedValue() const { return loadRelaxed() == deletedValue(); }
        // CachedTypes' CachedRefPtr::encode takes its RefPtr by value; this
        // takes a ref, exactly like passing a real RefPtr member would.
        ALWAYS_INLINE operator RefPtr<SourceProvider>() const { return RefPtr<SourceProvider> { get() }; }

        ALWAYS_INLINE friend bool operator==(const RelaxedSourceProviderPtr& a, const RelaxedSourceProviderPtr& b) { return a.loadRelaxed() == b.loadRelaxed(); }

    private:
        static SourceProvider* deletedValue() { return WTF::RawPtrTraits<SourceProvider>::hashTableDeletedValue(); }
        ALWAYS_INLINE SourceProvider* loadRelaxed() const { return __atomic_load_n(&m_pointer, __ATOMIC_RELAXED); }
        ALWAYS_INLINE void storeRelaxed(SourceProvider* value) { __atomic_store_n(&m_pointer, value, __ATOMIC_RELAXED); }
        SourceProvider* m_pointer;
    };
    static_assert(sizeof(RelaxedSourceProviderPtr) == sizeof(RefPtr<SourceProvider>));

    // Plain-scalar stand-in: union storage (no constructor ever runs a plain
    // store on the member bytes); all reads/writes are relaxed __atomic ops.
    template<typename T>
    class RelaxedPodMember {
        static_assert(std::is_trivially_copyable_v<T>);
        static_assert(std::is_trivially_destructible_v<T>);
    public:
        // Storage deliberately left uninitialized, like the plain member.
        ALWAYS_INLINE RelaxedPodMember() { }
        ALWAYS_INLINE RelaxedPodMember(T value) { storeRelaxed(value); }
        ALWAYS_INLINE RelaxedPodMember(const RelaxedPodMember& other) { storeRelaxed(other.loadRelaxed()); }
        ALWAYS_INLINE RelaxedPodMember(RelaxedPodMember&& other) { storeRelaxed(other.loadRelaxed()); }
        ALWAYS_INLINE RelaxedPodMember& operator=(T value)
        {
            storeRelaxed(value);
            return *this;
        }
        ALWAYS_INLINE RelaxedPodMember& operator=(const RelaxedPodMember& other)
        {
            storeRelaxed(other.loadRelaxed());
            return *this;
        }
        ALWAYS_INLINE RelaxedPodMember& operator=(RelaxedPodMember&& other)
        {
            storeRelaxed(other.loadRelaxed());
            return *this;
        }
        ALWAYS_INLINE operator T() const { return loadRelaxed(); }

        ALWAYS_INLINE friend bool operator==(const RelaxedPodMember& a, const RelaxedPodMember& b) { return a.loadRelaxed() == b.loadRelaxed(); }

    private:
        ALWAYS_INLINE T loadRelaxed() const
        {
            T result;
            __atomic_load(const_cast<T*>(&m_storage.value), &result, __ATOMIC_RELAXED);
            return result;
        }
        ALWAYS_INLINE void storeRelaxed(T value) { __atomic_store(&m_storage.value, &value, __ATOMIC_RELAXED); }
        union Storage {
            Storage() { }
            T value;
        } m_storage;
        static_assert(sizeof(Storage) == sizeof(T));
    };

    using SourceCodeProviderPtr = RelaxedSourceProviderPtr;
    template<typename T> using SourceCodePodMember = RelaxedPodMember<T>;

    class UnlinkedSourceCode {
        template<typename SourceType>
        friend class CachedUnlinkedSourceCodeShape;
        friend class CachedSourceCodeWithoutProvider;

    public:
        UnlinkedSourceCode()
            : m_provider(nullptr)
            , m_startOffset(0)
            , m_endOffset(0)
        {
        }

        UnlinkedSourceCode(WTF::HashTableDeletedValueType)
            : m_provider(WTF::HashTableDeletedValue)
        {
        }

        UnlinkedSourceCode(Ref<SourceProvider>&& provider)
            : m_provider(WTF::move(provider))
            , m_startOffset(0)
        {
            m_endOffset = static_cast<int>(m_provider->source().length());
        }

        UnlinkedSourceCode(Ref<SourceProvider>&& provider, int startOffset, int endOffset)
            : m_provider(WTF::move(provider))
            , m_startOffset(startOffset)
            , m_endOffset(endOffset)
        {
        }

        UnlinkedSourceCode(RefPtr<SourceProvider>&& provider, int startOffset, int endOffset)
            : m_provider(WTF::move(provider))
            , m_startOffset(startOffset)
            , m_endOffset(endOffset)
        {
        }

        bool isHashTableDeletedValue() const { return m_provider.isHashTableDeletedValue(); }

        SourceProvider& provider() const
        {
            return *m_provider;
        }

        unsigned hash() const
        {
            ASSERT(m_provider);
            return m_provider->hash();
        }

        StringView view() const
        {
            if (!m_provider)
                return StringView();
            return m_provider->getRange(startOffset(), endOffset());
        }

        CString toUTF8() const;

        bool isNull() const { return !m_provider; }
        int startOffset() const { return m_startOffset; }
        int endOffset() const { return m_endOffset; }
        int length() const { return endOffset() - startOffset(); }

        friend bool operator==(const UnlinkedSourceCode&, const UnlinkedSourceCode&) = default;

    protected:
        // FIXME: Make it Ref<SourceProvidier>.
        // https://bugs.webkit.org/show_bug.cgi?id=168325
        SourceCodeProviderPtr m_provider;
        SourceCodePodMember<int> m_startOffset;
        SourceCodePodMember<int> m_endOffset;
    };

} // namespace JSC
