/*
 * Copyright (C) 2014-2022 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "ExceptionHelpers.h"
#include "JSCast.h"
#include "Operations.h"
#include "PropertyNameArray.h"
#include "ResourceExhaustion.h"
#include "StructureChain.h"
#include <wtf/Atomics.h>
#include <wtf/ThreadSanitizerSupport.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class JSPropertyNameEnumerator final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;

    enum Flag : uint8_t {
        InitMode = 0,
        IndexedMode = 1 << 0,
        OwnStructureMode = 1 << 1,
        GenericMode = 1 << 2,
        // Profiling Only
        HasSeenOwnStructureModeStructureMismatch = 1 << 3,
    };

    static constexpr uint8_t enumerationModeMask = (GenericMode << 1) - 1;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return &vm.propertyNameEnumeratorSpace();
    }

    static JSPropertyNameEnumerator* tryCreate(VM&, Structure*, uint32_t, uint32_t, PropertyNameArrayBuilder&&);
    static JSPropertyNameEnumerator* create(VM& vm, Structure* structure, uint32_t indexedLength, uint32_t numberStructureProperties, PropertyNameArrayBuilder&& propertyNames)
    {
        auto* result = tryCreate(vm, structure, indexedLength, numberStructureProperties, WTF::move(propertyNames));
        RELEASE_ASSERT_RESOURCE_AVAILABLE(result, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
        return result;
    }

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    DECLARE_EXPORT_INFO;

    JSString* propertyNameAtIndex(uint32_t index)
    {
        if (index >= sizeOfPropertyNames())
            return nullptr;
        // Relaxed atomic aux-pointer load (AuxiliaryBarrier::get() is a plain
        // read; its writers are already relaxed-atomic). The lane read below
        // goes through WriteBarrier::get(), which is already a relaxed atomic
        // load (WriteBarrier.h cell()), pairing with finishCreation's set().
        // TSAN r12 (reports 12/13): see concurrentRelaxedLoad below — pairs
        // with finishCreation's HAPPENS_BEFORE.
        TSAN_ANNOTATE_HAPPENS_AFTER(this);
        WriteBarrier<JSString>* names = WTF::atomicLoad(m_propertyNames.slot(), std::memory_order_relaxed);
        return names[index].get();
    }

    Structure* cachedStructure(VM& vm) const
    {
        UNUSED_PARAM(vm);
        StructureID id = cachedStructureID();
        if (!id)
            return nullptr;
        Structure* structure = id.decode();
        validateCell(reinterpret_cast<JSCell*>(structure));
        return structure;
    }
    StructureID cachedStructureID() const
    {
        // Same relaxed pattern as StructureRareData::previousID():
        // WriteBarrierStructureID::value() is a plain 32-bit read, but the
        // slot's writers (setWithoutWriteBarrier / GC clear) are relaxed
        // atomic and the cell can be recycled under a stale foreign reader.
        static_assert(sizeof(WriteBarrierStructureID) == sizeof(uint32_t));
        return std::bit_cast<StructureID>(WTF::atomicLoad(reinterpret_cast<uint32_t*>(const_cast<WriteBarrierStructureID*>(&m_cachedStructureID)), std::memory_order_relaxed));
    }
    uint32_t indexedLength() const { return concurrentRelaxedLoad(m_indexedLength); }
    uint32_t endStructurePropertyIndex() const { return concurrentRelaxedLoad(m_endStructurePropertyIndex); }
    uint32_t endGenericPropertyIndex() const { return concurrentRelaxedLoad(m_endGenericPropertyIndex); }
    uint32_t cachedInlineCapacity() const { return concurrentRelaxedLoad(m_cachedInlineCapacity); }
    uint32_t sizeOfPropertyNames() const { return endGenericPropertyIndex(); }
    uint32_t flags() const { return concurrentRelaxedLoad(m_flags); }
    static constexpr ptrdiff_t cachedStructureIDOffset() { return OBJECT_OFFSETOF(JSPropertyNameEnumerator, m_cachedStructureID); }
    static constexpr ptrdiff_t indexedLengthOffset() { return OBJECT_OFFSETOF(JSPropertyNameEnumerator, m_indexedLength); }
    static constexpr ptrdiff_t endStructurePropertyIndexOffset() { return OBJECT_OFFSETOF(JSPropertyNameEnumerator, m_endStructurePropertyIndex); }
    static constexpr ptrdiff_t endGenericPropertyIndexOffset() { return OBJECT_OFFSETOF(JSPropertyNameEnumerator, m_endGenericPropertyIndex); }
    static constexpr ptrdiff_t cachedInlineCapacityOffset() { return OBJECT_OFFSETOF(JSPropertyNameEnumerator, m_cachedInlineCapacity); }
    static constexpr ptrdiff_t flagsOffset() { return OBJECT_OFFSETOF(JSPropertyNameEnumerator, m_flags); }
    static constexpr ptrdiff_t cachedPropertyNamesVectorOffset()
    {
        return OBJECT_OFFSETOF(JSPropertyNameEnumerator, m_propertyNames);
    }

    JSString* computeNext(JSGlobalObject*, JSObject* base, uint32_t& currentIndex, Flag&, bool shouldAllocateIndexedNameString = true);

    DECLARE_VISIT_CHILDREN;

private:
    friend class LLIntOffsetsExtractor;

    // TSAN family structure-fields (r4 residual "slow_path_enumerator_next" /
    // "JSPropertyNameEnumerator finishCreation/reads"): the enumerator is
    // immutable once published, but its IsoSubspace cell can be RECYCLED into
    // a new enumerator while a foreign mutator still holds a stale pointer
    // (cleared rare-data cache); the constructor's plain init stores then
    // pair with these lock-free accessor reads. Relaxed atomic single-word
    // loads (identical ldr/mov codegen) make the reader side C++-defined; a
    // stale snapshot is re-validated by the cachedStructureID check on every
    // fast path, per OM ground truth. NOTE: the matching writer-side relaxed
    // stores belong in JSPropertyNameEnumerator.cpp (constructor init list +
    // m_flags merges), owned separately.
    // TSAN r12 (reports 11/12/14): the HAPPENS_AFTER pairs with the
    // HAPPENS_BEFORE at the end of finishCreation — the enumerator (and its
    // fastMalloc'd names buffer) is consume-published, and a stale probe of
    // a RECYCLED cell otherwise pairs against the new owner's allocation /
    // init writes (blessed staleness; re-validated by cachedStructureID).
    uint32_t concurrentRelaxedLoad(const uint32_t& word) const
    {
        TSAN_ANNOTATE_HAPPENS_AFTER(this);
        return WTF::atomicLoad(const_cast<uint32_t*>(&word), std::memory_order_relaxed);
    }

    JSPropertyNameEnumerator(VM&, Structure*, uint32_t, uint32_t, WriteBarrier<JSString>*, unsigned);
    void finishCreation(VM&, RefPtr<PropertyNameArray>&&);

    // JSPropertyNameEnumerator is immutable data structure, which allows VM to cache the empty one.
    // After instantiating JSPropertyNameEnumerator, we must not change any fields.
    AuxiliaryBarrier<WriteBarrier<JSString>*> m_propertyNames;
    WriteBarrierStructureID m_cachedStructureID;
    uint32_t m_indexedLength;
    uint32_t m_endStructurePropertyIndex;
    uint32_t m_endGenericPropertyIndex;
    uint32_t m_cachedInlineCapacity;
    uint32_t m_flags; // Initialized with a relaxed atomic store in the constructor (THREADS/TSAN).
};

void getEnumerablePropertyNames(JSGlobalObject*, JSObject*, PropertyNameArrayBuilder&, uint32_t& indexedLength, uint32_t& structurePropertyCount);

inline JSPropertyNameEnumerator* propertyNameEnumerator(JSGlobalObject*, JSObject* base);

using EnumeratorMetadata = std::underlying_type_t<JSPropertyNameEnumerator::Flag>;

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
