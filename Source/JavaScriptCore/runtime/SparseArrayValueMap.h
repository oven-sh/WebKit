/*
 * Copyright (C) 2011-2023 Apple Inc. All rights reserved.
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

#include "JSCast.h"
#include "JSTypeInfo.h"
#include "PropertyDescriptor.h"
#include "PutDirectIndexMode.h"
#include "VM.h"
#include "WriteBarrier.h"
#include <wtf/Atomics.h>
#include <wtf/ThreadSanitizerSupport.h>
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>
#include <optional>

namespace JSC {

class SparseArrayValueMap;

class SparseArrayEntry : private WriteBarrier<Unknown> {
    WTF_MAKE_TZONE_ALLOCATED(SparseArrayEntry);
public:
    using Base = WriteBarrier<Unknown>;

    SparseArrayEntry()
    {
        Base::setWithoutWriteBarrier(jsUndefined());
    }

    void NODELETE get(JSObject*, PropertySlot&) const;
    void get(PropertyDescriptor&) const;
    bool put(JSGlobalObject*, JSValue thisValue, SparseArrayValueMap*, JSValue, bool shouldThrow);
    JSValue NODELETE getNonSparseMode() const;
    JSValue getConcurrently() const;
    JSValue get() const;

    // TSAN wave 4 (triage §3.10 / §8.10 jsvalue-slots residual): the attribute
    // word is read by unlocked concurrent probes (compiler-thread
    // getConcurrently, slot fills); both sides of the pair must be atomic, so
    // the accessor loads relaxed to match forceSet's relaxed store —
    // codegen-identical to the previous plain load, flag-off unchanged.
    unsigned attributes() const { return WTF::atomicLoad(const_cast<unsigned*>(&m_attributes), std::memory_order_relaxed); }

    // Defined below SparseArrayValueMap: the bodies need its complete type.
    inline void forceSet(SparseArrayValueMap*, unsigned attributes);
    inline void forceSet(VM&, SparseArrayValueMap*, JSValue, unsigned attributes);

    // TSAN wave 5 (triage family 10 jsvalue-slots, r4 key 'data race x
    // SparseArrayValueMap::getEntry'): field-wise snapshot copy for GIL-off
    // readers. The default (memberwise/memcpy) copy of a map entry performs
    // PLAIN reads of the value and attribute words, which race the unlocked
    // relaxed-atomic writers (defineOwnIndexedProperty's putIndexedDescriptor
    // -> forceSet runs without the map's cell lock). Copy through the
    // relaxed accessors instead — same staleness tolerance as the other
    // concurrent readers (staleness is blessed; tearing/UB is not). The
    // result is a thread-local snapshot; plain stores into the local copy
    // are fine (it is unshared until returned by value).
    SparseArrayEntry copySnapshotConcurrent() const
    {
        SparseArrayEntry copy;
        copy.Base::setWithoutWriteBarrier(Base::get()); // relaxed load of the shared value word
        copy.m_attributes = attributes(); // relaxed load of the shared attribute word
        return copy;
    }

    WriteBarrier<Unknown>& asValue() { return *this; }

private:
    unsigned m_attributes { 0 };
};

class SparseArrayValueMap final : public JSCell {
public:
    typedef JSCell Base;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;
    
private:
    typedef UncheckedKeyHashMap<uint64_t, SparseArrayEntry, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> Map;

    enum Flags {
        Normal                             = 0,
        SparseMode                         = 1 << 0,
        LengthIsReadOnly                   = 1 << 1,
        HasAnyKindOfGetterSetterProperties = 1 << 2,
    };

    SparseArrayValueMap(VM&);
    
    DECLARE_DEFAULT_FINISH_CREATION;

public:
    DECLARE_EXPORT_INFO;
    
    typedef Map::iterator iterator;
    typedef Map::const_iterator const_iterator;
    typedef Map::AddResult AddResult;

    static SparseArrayValueMap* create(VM&);
    
    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return &vm.sparseArrayValueMapSpace();
    }
    
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    DECLARE_VISIT_CHILDREN;

    // TSAN wave 4 (triage §3.10 / §8.10 jsvalue-slots residual): m_flags is
    // probed unlocked by concurrent readers (e.g. sparseMode() /
    // hasAnyKindOfGetterSetterProperties() on slow paths) while a locked
    // mutator sets bits; plain accesses on that word are UB. Reads are
    // relaxed atomic loads (codegen-identical). Writers: flag-on uses a
    // relaxed RMW or so two racing setters cannot lose a bit (the word is
    // monotonic — bits are only ever added, never cleared); flag-off keeps
    // the plain read-modify-write, so flag-off behavior and codegen are
    // unchanged.
    bool sparseMode()
    {
        return flagsRelaxed() & SparseMode;
    }

    void setSparseMode()
    {
        orFlags(SparseMode);
    }

    bool lengthIsReadOnly()
    {
        return flagsRelaxed() & LengthIsReadOnly;
    }

    void setLengthIsReadOnly()
    {
        orFlags(LengthIsReadOnly);
    }

    bool hasAnyKindOfGetterSetterProperties()
    {
        return flagsRelaxed() & HasAnyKindOfGetterSetterProperties;
    }

    void setHasAnyKindOfGetterSetterProperties()
    {
        orFlags(HasAnyKindOfGetterSetterProperties);
    }

    // These methods may mutate the contents of the map
    bool putEntry(JSGlobalObject*, JSObject*, unsigned, JSValue, bool shouldThrow);
    bool putDirect(JSGlobalObject*, JSObject*, unsigned, JSValue, unsigned attributes, PutDirectIndexMode);
    AddResult add(JSObject*, unsigned);
    // AB18-G: GIL-only. An unlocked probe races a locked mutator's rehash
    // (HashTable.h checkValidity assert / freed-table SEGV), and even a
    // locked probe cannot protect the caller's notFound() comparison or
    // it->value dereference after the lock drops. GIL-off callers must use
    // getEntry()/forEachEntry() instead.
    iterator find(unsigned i) { return m_map.find(i); }
    // This should ASSERT the remove is valid (check the result of the find).
    void remove(iterator it);
    void remove(unsigned i);

    JSValue getConcurrently(unsigned index);

    // AB18-G: GIL-off-safe read API. SparseArrayEntry is plain data
    // (WriteBarrier word + attribute word); a by-value snapshot taken under
    // the cell lock (field-wise via the relaxed accessors — see
    // SparseArrayEntry::copySnapshotConcurrent) is a usable snapshot; the
    // JSValue/GetterSetter cell it names is GC-stable.
    std::optional<SparseArrayEntry> getEntry(unsigned i);

    // Out-of-line cell-lock acquisition: JSCellLock::lock()/unlock() are
    // defined in JSCellInlines.h, which (by inlines layering) this header
    // must not include — constructing Locker<JSCellLock> here references
    // those inlines and trips -Wundefined-inline (an error on the Windows
    // builds) in any TU that includes this header without the inlines.
    void acquireCellLock() const;
    void releaseCellLock() const;
    class HeldCellLock {
        WTF_MAKE_NONCOPYABLE(HeldCellLock);
    public:
        explicit HeldCellLock(const SparseArrayValueMap& map)
            : m_map(map)
        {
            m_map.acquireCellLock();
        }
        ~HeldCellLock() { m_map.releaseCellLock(); }
    private:
        const SparseArrayValueMap& m_map;
    };

    template<typename Functor> void forEachEntry(const Functor& functor)
    {
        // Functor runs under the cell lock: it must not run JS, allocate GC
        // memory, or re-enter this map (regime-3 lock rules).
        HeldCellLock locker { *this };
        tsanAcquireCtorPublication();
        for (auto& entry : m_map)
            functor(entry.key, entry.value);
    }

    // r19 (post-closeout review): pairs with the TSAN_ANNOTATE_HAPPENS_BEFORE
    // at the end of the constructor. The cell lock serializes
    // post-publication accessors against EACH OTHER, but gives TSAN no edge
    // to the CONSTRUCTING thread's m_map header NSDMI stores (publication is
    // the sparse-map install in the array storage, fence + plain store, the
    // §18 ctor class — r19 flicker pair size()/add() vs ctor). Call right
    // after taking cellLock() in this class; narrow by construction (sparse
    // maps are cold, type-specific paths — does not touch hot engine-wide
    // accessors, per the vm() narrowing lesson). No-op outside TSAN.
    void tsanAcquireCtorPublication() const { TSAN_ANNOTATE_HAPPENS_AFTER(this); }

    // These methods do not mutate the contents of the map.
    // AB18-G: notFound() is GIL-only, like find() — the sentinel comparison
    // races a rehash.
    iterator notFound() { return m_map.end(); }
    bool isEmpty() const
    {
        if (Options::useJSThreads()) [[unlikely]] {
            // AB18-G: serialize the probe against a racing add()/remove() rehash.
            HeldCellLock locker { *this };
            tsanAcquireCtorPublication();
            return m_map.isEmpty();
        }
        return m_map.isEmpty();
    }
    bool contains(unsigned i) const
    {
        if (Options::useJSThreads()) [[unlikely]] {
            HeldCellLock locker { *this };
            tsanAcquireCtorPublication();
            return m_map.contains(i);
        }
        return m_map.contains(i);
    }
    size_t size() const
    {
        if (Options::useJSThreads()) [[unlikely]] {
            HeldCellLock locker { *this };
            tsanAcquireCtorPublication();
            return m_map.size();
        }
        return m_map.size();
    }
    // Only allow const begin/end iteration.
    // AB18-G: GIL-only. Unlocked iteration races a mutator rehash; GIL-off
    // callers must use forEachEntry() instead.
    const_iterator begin() const { return m_map.begin(); }
    const_iterator end() const { return m_map.end(); }

private:
    ALWAYS_INLINE unsigned flagsRelaxed() const
    {
        return WTF::atomicLoad(const_cast<unsigned*>(&m_flags), std::memory_order_relaxed);
    }

    ALWAYS_INLINE void orFlags(unsigned bits)
    {
        if (Options::useJSThreads()) [[unlikely]] {
            WTF::atomicExchangeOr(&m_flags, bits, std::memory_order_relaxed);
            return;
        }
        m_flags |= bits;
    }

    Map m_map;
    // TSAN wave 5 (triage family 10 jsvalue-slots, r4 key 'data race x
    // SparseArrayValueMap::SparseArrayValueMap'): m_flags and
    // m_reportedCapacity are initialized in the constructor body via relaxed
    // atomic stores instead of NSDMIs. A GIL-off reader holding a stale ref
    // to a recycled cell can probe flagsRelaxed() concurrently with the
    // constructor's initialization; a plain init store on the shared word is
    // UB against those relaxed loads. Same values, codegen-identical
    // flag-off.
    unsigned m_flags; // Bits from Flags; see the relaxed-atomic comment above sparseMode().
    size_t m_reportedCapacity;
};

inline void SparseArrayEntry::forceSet(SparseArrayValueMap* map, unsigned attributes)
{
    // FIXME: We can expand this for non x86 environments. Currently, loading ReadOnly | DontDelete property
    // from compiler thread is only supported in X86 architecture because of its TSO nature.
    // https://bugs.webkit.org/show_bug.cgi?id=134641
    if (isX86())
        WTF::storeStoreFence();

    if (attributes & PropertyAttribute::Accessor)
        map->setHasAnyKindOfGetterSetterProperties();
    // TSAN wave 4 (triage §3.10 / §8.10): this locked store pairs with the
    // unlocked relaxed reads in attributes()/get()/getConcurrently; a plain
    // store on the shared word is UB against them. Relaxed atomic store —
    // codegen-identical, flag-off unchanged. The storeStoreFence above still
    // orders the value store before this attribute publication for the
    // getConcurrently consume chain.
    WTF::atomicStore(&m_attributes, attributes, std::memory_order_relaxed);
}

inline void SparseArrayEntry::forceSet(VM& vm, SparseArrayValueMap* map, JSValue value, unsigned attributes)
{
    Base::set(vm, map, value);
    forceSet(map, attributes);
}

} // namespace JSC
