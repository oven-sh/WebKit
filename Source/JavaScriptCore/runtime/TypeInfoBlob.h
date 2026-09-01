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

    // A Structure's blob is read lock-free by other threads (compiler threads,
    // foreign mutators dispatching on possibly stale header bytes; a stale
    // read re-dispatches) while the owning thread writes it. The reader side
    // is a relaxed atomic only under TSAN: these loads sit on every type check
    // and indexing-mode test, and a relaxed atomic load cannot be folded with
    // its neighbours, CSE'd, or hoisted out of a loop, so production builds
    // keep the plain load. Same construction as cellHeaderConcurrentLoad in
    // JSCell.h (not included here to avoid a header cycle).
    template<typename T>
    static ALWAYS_INLINE T tsanRelaxedLoad(const T& field)
    {
#if TSAN_ENABLED
        static_assert(std::is_trivially_copyable_v<T>);
        T result;
        __atomic_load(const_cast<T*>(&field), &result, __ATOMIC_RELAXED);
        return result;
#else
        return field;
#endif
    }

    // Post-construction writer side: a relaxed atomic in every build, so the
    // TSAN reader above pairs with an atomic store. A single-word relaxed
    // store is the same mov as the plain store and these sites are cold.
    template<typename T>
    static ALWAYS_INLINE void relaxedStore(T& field, std::type_identity_t<T> value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        __atomic_store(&field, &value, __ATOMIC_RELAXED);
    }

    // Constructor bulk-init only: plain non-TSAN so the Structure-ctor-tail
    // store sequence stays coalescible.
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
        // Single word store (byte-identical to four field stores on both
        // endiannesses; typeInfoBlob() encodes the layout), relaxed under TSAN
        // so construction over recycled memory pairs with concurrent readers.
        tsanRelaxedStore(u.word, typeInfoBlob(indexingModeIncludingHistory, typeInfo.type(), typeInfo.inlineTypeFlags()));
    }

    // Invoked only from Structure constructor tails; the source is a freshly
    // built temporary with no concurrent writer.
    void operator=(const TypeInfoBlob& other) { tsanRelaxedStore(u.word, other.u.word); }

    IndexingType indexingModeIncludingHistory() const { return tsanRelaxedLoad(u.fields.indexingModeIncludingHistory); }
    Dependency fencedIndexingModeIncludingHistory(IndexingType& indexingType)
    {
        return Dependency::loadAndFence(&u.fields.indexingModeIncludingHistory, indexingType);
    }
    void setIndexingModeIncludingHistory(IndexingType indexingModeIncludingHistory) { relaxedStore(u.fields.indexingModeIncludingHistory, indexingModeIncludingHistory); }
    JSType type() const { return tsanRelaxedLoad(u.fields.type); }
    TypeInfo::InlineTypeFlags inlineTypeFlags() const { return tsanRelaxedLoad(u.fields.inlineTypeFlags); }

    TypeInfo typeInfo(TypeInfo::OutOfLineTypeFlags outOfLineTypeFlags) const { return TypeInfo(type(), inlineTypeFlags(), outOfLineTypeFlags); }
    CellState defaultCellState() const { return tsanRelaxedLoad(u.fields.defaultCellState); }

    static constexpr uint32_t typeInfoBlob(IndexingType indexingModeIncludingHistory, JSType type, TypeInfo::InlineTypeFlags inlineTypeFlags)
    {
#if CPU(LITTLE_ENDIAN)
        return (static_cast<uint32_t>(indexingModeIncludingHistory) << 0) | (static_cast<uint32_t>(type) << 8) | (static_cast<uint32_t>(inlineTypeFlags) << 16) | (static_cast<uint32_t>(CellState::DefinitelyWhite) << 24);
#else
        return (static_cast<uint32_t>(indexingModeIncludingHistory) << 24) | (static_cast<uint32_t>(type) << 16) | (static_cast<uint32_t>(inlineTypeFlags) << 8) | (static_cast<uint32_t>(CellState::DefinitelyWhite) << 0);
#endif
    }

    uint32_t blob() const { return tsanRelaxedLoad(u.word); }

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

        // Runs inside the Structure constructor over recycled Structure memory
        // that concurrent readers may still probe through stale StructureIDs;
        // such readers tolerate the poison value and re-dispatch.
        Data() { tsanRelaxedStore(word, 0xbbadbeefu); }
    };

    Data u;
};

} // namespace JSC
