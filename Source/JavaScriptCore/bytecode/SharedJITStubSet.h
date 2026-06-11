/*
 * Copyright (C) 2008-2025 Apple Inc. All rights reserved.
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

#include "DOMJITGetterSetter.h"
#include "PropertyInlineCache.h"
#include <wtf/Lock.h>

namespace JSC {

#if ENABLE(JIT)

class SharedJITStubSet {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(SharedJITStubSet);
public:
    SharedJITStubSet() = default;

    using PropertyInlineCacheKey = std::tuple<AccessType, bool, bool, bool, bool>;
    using StatelessCacheKey = std::tuple<PropertyInlineCacheKey, AccessCase::AccessType>;
    using DOMJITCacheKey = std::tuple<PropertyInlineCacheKey, const DOMJIT::GetterSetter*>;

    static PropertyInlineCacheKey propertyCacheKey(const PropertyInlineCache& propertyCache)
    {
        return std::tuple { propertyCache.accessType, static_cast<bool>(propertyCache.propertyIsInt32), static_cast<bool>(propertyCache.propertyIsString), static_cast<bool>(propertyCache.propertyIsSymbol), static_cast<bool>(propertyCache.prototypeIsKnownObject) };
    }

    struct Hash {
        struct Key {
            Key() = default;

            Key(PropertyInlineCacheKey propertyCacheKey, PolymorphicAccessJITStubRoutine* wrapped)
                : m_wrapped(wrapped)
                , m_propertyCacheKey(propertyCacheKey)
            { }

            Key(WTF::HashTableDeletedValueType)
                : m_wrapped(std::bit_cast<PolymorphicAccessJITStubRoutine*>(static_cast<uintptr_t>(1)))
            { }

            bool isHashTableDeletedValue() const { return m_wrapped == std::bit_cast<PolymorphicAccessJITStubRoutine*>(static_cast<uintptr_t>(1)); }

            friend bool operator==(const Key&, const Key&) = default;

            PolymorphicAccessJITStubRoutine* m_wrapped { nullptr };
            PropertyInlineCacheKey m_propertyCacheKey { };
        };

        using KeyTraits = SimpleClassHashTraits<Key>;

        static unsigned hash(const Key& p)
        {
            if (!p.m_wrapped)
                return 1;
            return p.m_wrapped->hash();
        }

        static bool equal(const Key& a, const Key& b)
        {
            return a == b;
        }

        static constexpr bool safeToCompareToEmptyOrDeleted = false;
    };

    struct Searcher {
        struct Translator {
            static unsigned hash(const Searcher& searcher)
            {
                return searcher.m_hash;
            }

            static bool equal(const Hash::Key a, const Searcher& b)
            {
                if (a.m_propertyCacheKey == b.m_propertyCacheKey && Hash::hash(a) == b.m_hash) {
                    if (a.m_wrapped->cases().size() != 1)
                        return false;
                    const auto& aCase = a.m_wrapped->cases()[0];
                    const auto& bCase = b.m_accessCase;
                    if (!AccessCase::canBeShared(aCase.get(), bCase.get()))
                        return false;
                    return true;
                }
                return false;
            }
        };

        Searcher(PropertyInlineCacheKey&& propertyCacheKey, Ref<AccessCase>&& accessCase)
            : m_propertyCacheKey(WTF::move(propertyCacheKey))
            , m_accessCase(WTF::move(accessCase))
            , m_hash(m_accessCase->hash())
        {
        }

        PropertyInlineCacheKey m_propertyCacheKey;
        const Ref<AccessCase> m_accessCase;
        unsigned m_hash { 0 };
    };

    struct PointerTranslator {
        static unsigned hash(const PolymorphicAccessJITStubRoutine* stub)
        {
            return stub->hash();
        }

        static bool equal(const Hash::Key& key, const PolymorphicAccessJITStubRoutine* stub)
        {
            return key.m_wrapped == stub;
        }
    };

    // Review round 2 (R2-2): every accessor takes the set's own m_lock. The
    // set is per-VM, but its routines are shared across CodeBlocks and its
    // mutators are NOT all serialized by one CodeBlock lock: IC-miss slow
    // paths of different CodeBlocks (phase-B: different mutator threads in
    // one VM), GC-End removeDeadOwners, and observeZeroRefCountImpl (runs on
    // whatever thread drops the last ref — handler-chain retirement included)
    // all reach here. An unsynchronized HashSet/HashMap rehash race is heap
    // corruption. All paths are slow paths; an uncontended WTF::Lock is one
    // CAS.
    void add(Hash::Key&& key)
    {
        Locker locker { m_lock };
        m_stubs.add(WTF::move(key));
    }

    void remove(PolymorphicAccessJITStubRoutine* stub)
    {
        Locker locker { m_lock };
        auto iter = m_stubs.find<PointerTranslator>(stub);
        if (iter != m_stubs.end())
            m_stubs.remove(iter);
    }

    RefPtr<PolymorphicAccessJITStubRoutine> find(const Searcher& searcher)
    {
        Locker locker { m_lock };
        auto entry = m_stubs.find<SharedJITStubSet::Searcher::Translator>(searcher);
        if (entry != m_stubs.end())
            return entry->m_wrapped;
        return nullptr;
    }

    RefPtr<PolymorphicAccessJITStubRoutine> getStatelessStub(StatelessCacheKey) const;
    void setStatelessStub(StatelessCacheKey, Ref<PolymorphicAccessJITStubRoutine>);

    MacroAssemblerCodeRef<JITStubRoutinePtrTag> getDOMJITCode(DOMJITCacheKey) const;
    void setDOMJITCode(DOMJITCacheKey, MacroAssemblerCodeRef<JITStubRoutinePtrTag>);

    RefPtr<InlineCacheHandler> NODELETE getSlowPathHandler(AccessType) const;
    void setSlowPathHandler(AccessType, Ref<InlineCacheHandler>);

private:
    mutable Lock m_lock; // R2-2: guards every container below.
    UncheckedKeyHashSet<Hash::Key, Hash, Hash::KeyTraits> m_stubs;
    UncheckedKeyHashMap<StatelessCacheKey, Ref<PolymorphicAccessJITStubRoutine>> m_statelessStubs;
    UncheckedKeyHashMap<DOMJITCacheKey, MacroAssemblerCodeRef<JITStubRoutinePtrTag>> m_domJITCodes;
    std::array<RefPtr<InlineCacheHandler>, numberOfAccessTypes> m_fallbackHandlers { };
    std::array<RefPtr<InlineCacheHandler>, numberOfAccessTypes> m_slowPathHandlers { };
};

#else

class PropertyInlineCache;

#endif // ENABLE(JIT)

} // namespace JSC
