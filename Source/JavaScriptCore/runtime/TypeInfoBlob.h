/*
 * Copyright (C) 2014-2017 Apple Inc. All rights reserved.
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

#include "CellState.h"
#include "IndexingType.h"
#include "JSTypeInfo.h"
#include "StructureID.h"
#include <type_traits>
#include <wtf/Compiler.h>

namespace JSC {

class TypeInfoBlob {
    friend class LLIntOffsetsExtractor;

    // V7 (TSAN, cell-header family; wave-3 review amendment §9.1): a
    // Structure's blob is read by concurrent readers (compiler threads,
    // foreign mutators via the OM §3.0/GT#2 header-byte protocol — stale
    // bytes re-dispatch) while the owning thread constructs or reassigns it.
    // Post-construction accessor sites use the UNCONDITIONAL relaxed pair
    // below (campaign convention: single-word relaxed atomic compiles to the
    // identical mov, zero codegen delta flag-off). Only the constructor
    // bulk-init sequences (this blob is initialized inside the Structure
    // ctor tail, where store coalescing is bench-sensitive — ITEM-2/V5b)
    // keep the TSAN-BUILD-ONLY pair; that retained-UB class is recorded in
    // docs/threads/TSAN-TRIAGE.md §9.1. Same construction as
    // cellHeaderConcurrentLoad/Store in JSCell.h (not included here to avoid
    // a header cycle).
    template<typename T>
    static ALWAYS_INLINE T relaxedLoad(const T& field)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        T result;
        __atomic_load(const_cast<T*>(&field), &result, __ATOMIC_RELAXED);
        return result;
    }

    template<typename T>
    static ALWAYS_INLINE void relaxedStore(T& field, std::type_identity_t<T> value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        __atomic_store(&field, &value, __ATOMIC_RELAXED);
    }

    // Constructor bulk-init only (TSAN-TRIAGE §9.1): plain non-TSAN so the
    // Structure-ctor-tail store sequence stays coalescible (ITEM-2).
    template<typename T>
    static ALWAYS_INLINE void tsanRelaxedStore(T& field, std::type_identity_t<T> value)
    {
#if TSAN_ENABLED
        relaxedStore(field, value);
#else
        field = value;
#endif
    }

public:
    TypeInfoBlob() = default;

    TypeInfoBlob(IndexingType indexingModeIncludingHistory, const TypeInfo& typeInfo)
    {
        // Single word store (byte-identical to the previous four field
        // stores on both endiannesses — typeInfoBlob() encodes the layout),
        // relaxed under TSAN so construction over recycled memory pairs with
        // concurrent readers. Ctor bulk-init class (§9.1): plain non-TSAN.
        tsanRelaxedStore(u.word, typeInfoBlob(indexingModeIncludingHistory, typeInfo.type(), typeInfo.inlineTypeFlags()));
    }

    // operator= is invoked only from Structure ctor tails (Structure.cpp) —
    // ctor bulk-init class (§9.1), so the store side stays TSAN-only; the
    // load side reads a freshly built temporary (no concurrent writer).
    void operator=(const TypeInfoBlob& other) { tsanRelaxedStore(u.word, other.u.word); }

    IndexingType indexingModeIncludingHistory() const { return relaxedLoad(u.fields.indexingModeIncludingHistory); }
    Dependency fencedIndexingModeIncludingHistory(IndexingType& indexingType)
    {
        // Dependency::loadAndFence performs a plain load; do the load
        // relaxed-atomically here so it pairs with the relaxed writers
        // above, then fence on the loaded value exactly as loadAndFence does.
        // The atomic load is already opaque to the compiler, so the
        // anti-value-speculation property loadAndFence exists for is kept
        // (Dependency::fence consumes the loaded value). Same single-load
        // codegen on every target.
        IndexingType value = relaxedLoad(u.fields.indexingModeIncludingHistory);
        Dependency dependency = Dependency::fence(value);
        indexingType = value;
        return dependency;
    }
    void setIndexingModeIncludingHistory(IndexingType indexingModeIncludingHistory) { relaxedStore(u.fields.indexingModeIncludingHistory, indexingModeIncludingHistory); }
    JSType type() const { return relaxedLoad(u.fields.type); }
    TypeInfo::InlineTypeFlags inlineTypeFlags() const { return relaxedLoad(u.fields.inlineTypeFlags); }

    TypeInfo typeInfo(TypeInfo::OutOfLineTypeFlags outOfLineTypeFlags) const { return TypeInfo(type(), inlineTypeFlags(), outOfLineTypeFlags); }
    CellState defaultCellState() const { return relaxedLoad(u.fields.defaultCellState); }

    static constexpr uint32_t typeInfoBlob(IndexingType indexingModeIncludingHistory, JSType type, TypeInfo::InlineTypeFlags inlineTypeFlags)
    {
#if CPU(LITTLE_ENDIAN)
        return (static_cast<uint32_t>(indexingModeIncludingHistory) << 0) | (static_cast<uint32_t>(type) << 8) | (static_cast<uint32_t>(inlineTypeFlags) << 16) | (static_cast<uint32_t>(CellState::DefinitelyWhite) << 24);
#else
        return (static_cast<uint32_t>(indexingModeIncludingHistory) << 24) | (static_cast<uint32_t>(type) << 16) | (static_cast<uint32_t>(inlineTypeFlags) << 8) | (static_cast<uint32_t>(CellState::DefinitelyWhite) << 0);
#endif
    }

    uint32_t blob() const { return relaxedLoad(u.word); }

    static constexpr ptrdiff_t indexingModeIncludingHistoryOffset()
    {
        return OBJECT_OFFSETOF(TypeInfoBlob, u.fields.indexingModeIncludingHistory);
    }

private:
    union Data {
        struct {
            IndexingType indexingModeIncludingHistory;
            JSType type;
            TypeInfo::InlineTypeFlags inlineTypeFlags;
            CellState defaultCellState;
        } fields;
        uint32_t word;

        // V7 (TSAN, cell-header family): this default ctor runs inside the
        // Structure ctor, over recycled Structure memory that concurrent
        // readers may still probe through the relaxed accessors above (the
        // §3.0/GT#2 stale-header re-dispatch tolerates the poison value).
        // Ctor bulk-init class (§9.1): relaxed under TSAN, plain non-TSAN.
        // The durable close for this interleaving is the family-9 release
        // publication of new Structures.
        Data() { tsanRelaxedStore(word, 0xbbadbeefu); }
    };

    Data u;
};

} // namespace JSC
