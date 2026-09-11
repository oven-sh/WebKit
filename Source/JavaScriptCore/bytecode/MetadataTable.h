/*
 * Copyright (C) 2018-2024 Apple Inc. All rights reserved.
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

#include "Instruction.h"
#include "Opcode.h"
#include "UnlinkedMetadataTable.h"
#include "ValueProfile.h"
#include <wtf/RefCounted.h>
#include <wtf/TZoneMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class CodeBlock;
class MetadataTable;

// One of the value profiles of a MetadataTable. LLInt and Baseline code only ever store to its bucket, which sits right in front
// of the table. Its prediction is in an array that the table allocates the first time one of its profiles has something to predict,
// which for most code is never; until then every prediction is SpecNone.
class ValueProfileRef {
public:
    static constexpr unsigned numberOfBuckets = 1;
    static constexpr unsigned totalNumberOfBuckets = 1;

    ValueProfileRef() = default;
    ValueProfileRef(MetadataTable&, unsigned profileOffset);

    explicit operator bool() const { return !!m_table; }

    inline SpeculatedType prediction() const;
    inline void mergePrediction(SpeculatedType) const;
    inline SpeculatedType computeUpdatedPrediction() const;
    inline void computeUpdatedPredictionForExtraValue(JSValue&) const;

    unsigned numberOfSamples() const { return !!JSValue::decodeConcurrent(m_buckets); }
    bool isSampledBefore() const { return prediction() != SpecNone; }
    unsigned totalNumberOfSamples() const { return numberOfSamples() + isSampledBefore(); }

    CString briefDescription() const
    {
        StringPrintStream out;
        out.print("predicting ", SpeculationDump(computeUpdatedPrediction()));
        return out.toCString();
    }

    void dump(PrintStream& out) const
    {
        out.print("sampled before = ", isSampledBefore(), " live samples = ", numberOfSamples(), " prediction = ", SpeculationDump(prediction()));
        if (JSValue value = JSValue::decodeConcurrent(m_buckets))
            out.print(": ", value);
    }

    EncodedJSValue* m_buckets { nullptr };

private:
    MetadataTable* m_table { nullptr };
    unsigned m_profileOffset { 0 };
};

// MetadataTable has a bit strange memory layout for LLInt optimization.
// [ValueProfile buckets][UnlinkedMetadataTable::LinkingData][MetadataTableOffsets][MetadataContent]
//                                                           ^
//                         The pointer of MetadataTable points at this address.
class MetadataTable {
    WTF_MAKE_TZONE_ALLOCATED(MetadataTable);
    WTF_MAKE_NONCOPYABLE(MetadataTable);
    friend class LLIntOffsetsExtractor;
    friend class UnlinkedMetadataTable;
public:
    ~MetadataTable();

    template<typename Metadata>
    ALWAYS_INLINE Metadata* get()
    {
        auto opcodeID = Metadata::opcodeID;
        ASSERT(opcodeID < NUMBER_OF_BYTECODE_WITH_METADATA);
        uintptr_t ptr = std::bit_cast<uintptr_t>(getWithoutAligning(opcodeID));
        ptr = roundUpToMultipleOf(alignof(Metadata), ptr);
        return std::bit_cast<Metadata*>(ptr);
    }

    template<typename Op, typename Functor>
    ALWAYS_INLINE void forEach(const Functor& func)
    {
        auto* metadata = get<typename Op::Metadata>();
        auto* end = std::bit_cast<typename Op::Metadata*>(getWithoutAligning(Op::opcodeID + 1));
        for (; metadata < end; ++metadata)
            func(*metadata);
    }

    template<typename Functor>
    ALWAYS_INLINE void forEachValueProfile(const Functor& func)
    {
        unsigned numValueProfiles = unlinkedMetadata()->m_numValueProfiles;
        for (unsigned profileOffset = 1; profileOffset <= numValueProfiles; ++profileOffset)
            func(ValueProfileRef(*this, profileOffset));
    }

    EncodedJSValue* valueProfileBucketsEnd()
    {
        return reinterpret_cast_ptr<EncodedJSValue*>(&linkingData());
    }

    ValueProfileRef valueProfileForOffset(unsigned profileOffset)
    {
        ASSERT(profileOffset && profileOffset <= unlinkedMetadata()->m_numValueProfiles);
        return ValueProfileRef(*this, profileOffset);
    }

    // Indexed by profile offset - 1. Both are safe to call from a compiler thread.
    SpeculatedType* valueProfilePredictions() const { return linkingData().valueProfilePredictions.load(std::memory_order_acquire); }
    SpeculatedType* ensureValueProfilePredictions();

    size_t sizeInBytesForGC();

    void ref()
    {
        ++linkingData().refCount;
    }

    void deref()
    {
        if (!--linkingData().refCount) {
            // Setting refCount to 1 here prevents double delete within the destructor but not from another thread
            // since such a thread could have ref'ed this object long after it had been deleted. This is consistent
            // with ThreadSafeRefCounted.h, see webkit.org/b/201576 for the reasoning.
            linkingData().refCount = 1;

            MetadataTable::destroy(this);
            return;
        }
    }

    unsigned refCount() const
    {
        return linkingData().refCount;
    }

    unsigned hasOneRef() const
    {
        return refCount() == 1;
    }

    template <typename Opcode>
    uintptr_t offsetInMetadataTable(const Opcode& opcode)
    {
        uintptr_t baseTypeOffset = is32Bit() ? offsetTable32()[Opcode::opcodeID] : offsetTable16()[Opcode::opcodeID];
        baseTypeOffset = roundUpToMultipleOf(alignof(typename Opcode::Metadata), baseTypeOffset);
        return baseTypeOffset + sizeof(typename Opcode::Metadata) * opcode.m_metadataID;
    }

    void validate() const;

    RefPtr<UnlinkedMetadataTable> unlinkedMetadata() const { return linkingData().unlinkedMetadata.copyRef(); }

    SUPPRESS_ASAN bool isDestroyed() const
    {
        uintptr_t unlinkedMetadataPtr = *std::bit_cast<uintptr_t*>(&linkingData().unlinkedMetadata);
        return !unlinkedMetadataPtr;
    }

private:
    MetadataTable(UnlinkedMetadataTable&);

    UnlinkedMetadataTable::Offset16* offsetTable16() const { return std::bit_cast<UnlinkedMetadataTable::Offset16*>(this); }
    UnlinkedMetadataTable::Offset32* offsetTable32() const { return std::bit_cast<UnlinkedMetadataTable::Offset32*>(std::bit_cast<uint8_t*>(this) + UnlinkedMetadataTable::s_offset16TableSize); }

    size_t totalSize() const
    {
        return unlinkedMetadata()->m_numValueProfiles * sizeof(EncodedJSValue) + sizeof(UnlinkedMetadataTable::LinkingData) + getOffset(UnlinkedMetadataTable::s_offsetTableEntries - 1);
    }

    UnlinkedMetadataTable::LinkingData& linkingData() const
    {
        return *std::bit_cast<UnlinkedMetadataTable::LinkingData*>((std::bit_cast<uint8_t*>(this) - sizeof(UnlinkedMetadataTable::LinkingData)));
    }

    void* buffer() { return this; }

    // Offset of zero means that the 16 bit table is not in use.
    bool is32Bit() const { return !offsetTable16()[0]; }

    ALWAYS_INLINE unsigned getOffset(unsigned i) const
    {
        unsigned offset = offsetTable16()[i];
        if (offset)
            return offset;
        return offsetTable32()[i];
    }

    ALWAYS_INLINE uint8_t* getWithoutAligning(unsigned i)
    {
        return std::bit_cast<uint8_t*>(this) + getOffset(i);
    }

    static void destroy(MetadataTable*);
};

inline ValueProfileRef::ValueProfileRef(MetadataTable& table, unsigned profileOffset)
    : m_buckets(table.valueProfileBucketsEnd() - profileOffset)
    , m_table(&table)
    , m_profileOffset(profileOffset)
{
}

inline SpeculatedType ValueProfileRef::prediction() const
{
    if (SpeculatedType* predictions = m_table->valueProfilePredictions())
        return predictions[m_profileOffset - 1];
    return SpecNone;
}

inline void ValueProfileRef::mergePrediction(SpeculatedType prediction) const
{
    if (prediction == SpecNone)
        return;
    mergeSpeculation(m_table->ensureValueProfilePredictions()[m_profileOffset - 1], prediction);
}

inline SpeculatedType ValueProfileRef::computeUpdatedPrediction() const
{
    if (JSValue value = JSValue::decodeConcurrent(m_buckets)) {
        mergePrediction(speculationFromValueForProfiling(value));
        updateEncodedJSValueConcurrent(*m_buckets, JSValue::encode(JSValue()));
    }
    return prediction();
}

inline void ValueProfileRef::computeUpdatedPredictionForExtraValue(JSValue& value) const
{
    if (value)
        mergePrediction(speculationFromValueForProfiling(value));
    value = JSValue();
}

inline void UnlinkedValueProfile::update(ValueProfileRef& profile)
{
    SpeculatedType newType = profile.prediction() | m_prediction;
    profile.mergePrediction(newType);
    m_prediction = newType;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
