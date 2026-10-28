/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTEffectsOfOperands.h"
#include "AOTLargeVectors.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class BodyReferences;
class BodySummaries;
class BorrowedParameters;
class FlowResults;
class FunctionAnalyses;

struct ParameterSummary {
    OperandSummary operand;
    OperandMemory memory;

    friend bool operator==(const ParameterSummary&, const ParameterSummary&) = default;
};

struct UnknownCodeWitness {
    FunctionID function;
    ValueID value;

    explicit operator bool() const { return !!function; }
    friend bool operator==(const UnknownCodeWitness&, const UnknownCodeWitness&) = default;
};

struct FunctionSummary {
    EffectsOnEdges elsewhere { EffectsOnEdges::unknownCode() };
    OperandMemory throughParameters;
    Vector<ParameterSummary> parameters;
    UnknownCodeWitness unknownCodeWitness;

    const ParameterSummary& parameter(uint32_t entryParameter) const LIFETIME_BOUND;
    EffectsOnEdges onCall() const;
    FunctionSummary withoutParameters(const BitVector& removedEntryParameters) const;

    friend bool operator==(const FunctionSummary&, const FunctionSummary&) = default;
};

struct AddedEffects {
    FunctionID function;
    Effects effects;
};

class CompactEffects {
public:
    static constexpr unsigned idBits = 26;

    constexpr CompactEffects() = default;
    constexpr CompactEffects(uint32_t reads, uint32_t writes, uint8_t flags = 0)
        : m_bits(static_cast<uint64_t>(reads) | static_cast<uint64_t>(writes) << idBits | static_cast<uint64_t>(flags) << (2 * idBits))
    {
    }

    enum Flag : uint8_t {
        MayThrow = 1 << 0,
        MayThrowByRule = 1 << 1,
        MayCollect = 1 << 2,
        MayCallUnknownCode = 1 << 3,
        MaySuspend = 1 << 4,
        MayBeTerminated = 1 << 5,
    };

    constexpr uint32_t reads() const { return m_bits & idMask; }
    constexpr uint32_t writes() const { return m_bits >> idBits & idMask; }
    constexpr uint8_t flags() const { return static_cast<uint8_t>(m_bits >> (2 * idBits)); }
    constexpr bool mayThrow() const { return flags() & MayThrow; }
    constexpr bool maySuspend() const { return flags() & MaySuspend; }
    constexpr bool mayCallUnknownCode() const { return flags() & MayCallUnknownCode; }
    constexpr CompactEffects withFlags(uint8_t flags) const { return CompactEffects(reads(), writes(), flags); }
    constexpr CompactEffects withoutSuspension() const { return withFlags(flags() & ~MaySuspend); }

    friend constexpr bool operator==(CompactEffects, CompactEffects) = default;

private:
    static constexpr uint64_t idMask = (uint64_t { 1 } << idBits) - 1;

    uint64_t m_bits { 0 };
};

class InternedLocationMasks final : public ThreadSafeRefCounted<InternedLocationMasks> {
    WTF_MAKE_TZONE_ALLOCATED(InternedLocationMasks);
    WTF_MAKE_NONCOPYABLE(InternedLocationMasks);
public:
    using ID = uint32_t;
    static constexpr ID empty = 0;

    static Ref<InternedLocationMasks> create() { return adoptRef(*new InternedLocationMasks); }
    ~InternedLocationMasks();

    ID intern(const LocationMasks&);

    const LocationMasks& at(ID id) const LIFETIME_BOUND
    {
        Place place = placeOf(id);
        return m_chunks[place.chunk].load(std::memory_order_relaxed)[place.offset];
    }

    CompactEffects compress(const Effects&);
    CompactEffects compress(const OperandMemory& memory)
    {
        ID reads = intern(memory.reads);
        return CompactEffects(reads, intern(memory.writes));
    }
    Effects expand(CompactEffects) const;
    OperandMemory expandMemory(CompactEffects effects) const { return { at(effects.reads()), at(effects.writes()) }; }
    void mergeInto(Effects&, CompactEffects) const;
    ID merge(ID, ID);

    unsigned size() const { return m_size.load(std::memory_order_relaxed); }

private:
    InternedLocationMasks();

    static constexpr unsigned firstChunkBits = 10;
    static constexpr unsigned largestChunkBits = 16;
    static constexpr unsigned numberOfGrowingChunks = largestChunkBits - firstChunkBits + 1;
    static constexpr ID firstIDInEqualChunks = ((1u << numberOfGrowingChunks) - 1) << firstChunkBits;
    static constexpr unsigned numberOfChunks = numberOfGrowingChunks + (((1u << CompactEffects::idBits) - firstIDInEqualChunks) >> largestChunkBits) + 1;
    static constexpr unsigned shardBits = 6;

    struct Place {
        unsigned chunk;
        uint32_t offset;
    };

    static constexpr Place placeOf(ID id)
    {
        if (id >= firstIDInEqualChunks) {
            ID rest = id - firstIDInEqualChunks;
            return { numberOfGrowingChunks + (rest >> largestChunkBits), rest & ((1u << largestChunkBits) - 1) };
        }
        unsigned chunk = 31 - std::countl_zero((id >> firstChunkBits) + 1);
        return { chunk, id - (((1u << chunk) - 1) << firstChunkBits) };
    }

    static constexpr size_t sizeOfChunk(unsigned chunk) { return size_t { 1 } << std::min(firstChunkBits + chunk, largestChunkBits); }

    struct Slots {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Slots);

        explicit Slots(size_t size);

        Vector<std::atomic<uint32_t>> idsPlusOne;
    };

    struct Shard {
        Lock lock;
        std::atomic<Slots*> slots { nullptr };
        Vector<std::unique_ptr<Slots>> allSlots WTF_GUARDED_BY_LOCK(lock);
        size_t count WTF_GUARDED_BY_LOCK(lock) { 0 };
    };

    std::optional<ID> find(const Slots&, uint32_t hash, const LocationMasks&) const;
    static void insert(Slots&, uint32_t hash, ID);
    Slots& grow(Shard&);
    ID append(const LocationMasks&);

    std::array<std::atomic<LocationMasks*>, numberOfChunks> m_chunks { };
    std::atomic<uint32_t> m_size { 0 };
    Lock m_chunkLock;
    std::array<Shard, 1u << shardBits> m_shards;
};

struct StoredParameterSummary {
    CompactEffects memory;
    uint32_t uses { 0 };
    uint32_t retainedIn { 0 };

    static StoredParameterSummary create(CompactEffects memory, const OperandSummary&);
    OperandSummary operand() const;

    friend bool operator==(const StoredParameterSummary&, const StoredParameterSummary&) = default;
};
static_assert(sizeof(StoredParameterSummary) == 16);

struct StoredFunctionEffects {
    CompactEffects normal;
    CompactEffects handler;
    CompactEffects throughParameters;
    CompactEffects normalOnCall;
    CompactEffects handlerOnCall;

    void computeEffectsOnCall(InternedLocationMasks&);

    friend bool operator==(const StoredFunctionEffects&, const StoredFunctionEffects&) = default;
};

struct StoredParameterRange {
    uint32_t first { 0 };
    uint32_t size { 0 };
};

class FunctionSummaryView {
public:
    explicit FunctionSummaryView(const FunctionSummary& summary LIFETIME_BOUND)
        : m_summary(&summary)
    {
    }

    FunctionSummaryView(const InternedLocationMasks& masks LIFETIME_BOUND, const StoredFunctionEffects& effects LIFETIME_BOUND, std::span<const StoredParameterSummary> parameters LIFETIME_BOUND, UnknownCodeWitness witness)
        : m_masks(&masks)
        , m_effects(&effects)
        , m_parameters(parameters)
        , m_witness(witness)
    {
    }

    uint32_t numberOfParameters() const { return m_summary ? m_summary->parameters.size() : m_parameters.size(); }
    EffectsOnEdges elsewhere() const;
    OperandMemory throughParameters() const;
    OperandSummary operand(uint32_t entryParameter) const;
    OperandMemory memory(uint32_t entryParameter) const;
    EffectsOnEdges onCall() const;
    UnknownCodeWitness unknownCodeWitness() const { return m_summary ? m_summary->unknownCodeWitness : m_witness; }
    FunctionSummary expanded() const;
    FunctionSummary withoutParameters(const BitVector& removedEntryParameters) const { return expanded().withoutParameters(removedEntryParameters); }

    friend bool operator==(const FunctionSummaryView& a, const FunctionSummaryView& b) { return a.expanded() == b.expanded(); }
    friend bool operator==(const FunctionSummaryView& a, const FunctionSummary& b) { return a.expanded() == b; }

private:
    const FunctionSummary* m_summary { nullptr };
    const InternedLocationMasks* m_masks { nullptr };
    const StoredFunctionEffects* m_effects { nullptr };
    std::span<const StoredParameterSummary> m_parameters;
    UnknownCodeWitness m_witness;
};

class EffectSummaries {
    WTF_MAKE_TZONE_ALLOCATED(EffectSummaries);
    WTF_MAKE_NONCOPYABLE(EffectSummaries);
public:
    EffectSummaries() = default;
    EffectSummaries(EffectSummaries&&) = default;
    EffectSummaries& operator=(EffectSummaries&&) = default;

    EffectSummaries(std::span<const FunctionSummary>, const Effects& unknownCode);
    EffectSummaries(const EffectSummaries& known LIFETIME_BOUND, FunctionID firstAdded, std::span<const FunctionSummary> added);
    EffectSummaries(const EffectSummaries& known LIFETIME_BOUND, FunctionID firstAdded, std::initializer_list<FunctionSummary> added)
        : EffectSummaries(known, firstAdded, std::span<const FunctionSummary> { added.begin(), added.size() })
    {
    }

    FunctionSummaryView of(FunctionID) const LIFETIME_BOUND;
    const Effects& unknownCode() const LIFETIME_BOUND { return m_unknownCode; }
    unsigned size() const { return m_known ? 0 : m_parameterRanges.size(); }
    bool isEmpty() const { return m_known ? m_known->isEmpty() : m_parameterRanges.isEmpty(); }
    bool contains(FunctionID) const;

    void dump(PrintStream&, const Program&) const;

private:
    friend class BodySummaries;
    friend class EffectSummarizer;
    friend EffectSummaries createSummariesFromFlow(const Program&, const FlowResults&, const BorrowedParameters&, BuildContext&);

    enum class Contents : uint8_t { SayNothingOfBodies, Any };
    EffectSummaries(Ref<InternedLocationMasks>&&, Contents);

    bool stores(FunctionID id) const { return id && id.index() >= m_firstStored && id.index() - m_firstStored < m_parameterRanges.size(); }
    void store(std::span<const FunctionSummary>);
    void clear();
    void grow(unsigned size, const ParallelLoops* = nullptr);
    void setNumberOfParameters(unsigned index, uint32_t numberOfParameters);
    void reserveParameters(const ParallelLoops& loops, size_t numberOfMoreParameters) { reserveCapacityInParallel(loops, m_parameters, m_parameters.size() + numberOfMoreParameters); }
    void setParameterCounts(NOESCAPE const Invocable<uint32_t(unsigned index)> auto& parameterCount, const ParallelLoops* loops = nullptr)
    {
        size_t totalParameters = m_parameters.size();
        for (unsigned index = 0; index < m_parameterRanges.size(); ++index) {
            StoredParameterRange& range = m_parameterRanges[index];
            uint32_t numberOfParameters = parameterCount(index);
            if (range.size < numberOfParameters) {
                range.first = totalParameters;
                totalParameters += numberOfParameters;
            }
            range.size = numberOfParameters;
        }
        if (loops)
            reserveCapacityInParallel(*loops, m_parameters, totalParameters);
        m_parameters.grow(totalParameters);
    }
    void set(unsigned index, const FunctionSummary&);
    void set(unsigned index, const StoredFunctionEffects&, UnknownCodeWitness);
    std::span<StoredParameterSummary> parametersOf(unsigned index) LIFETIME_BOUND { return m_parameters.mutableSpan().subspan(m_parameterRanges[index].first, m_parameterRanges[index].size); }

    RefPtr<InternedLocationMasks> m_masks;
    Contents m_contents { Contents::Any };
    StoredFunctionEffects m_effectsOfUnknownFunction;
    Vector<StoredFunctionEffects> m_effects;
    Vector<StoredParameterRange> m_parameterRanges;
    Vector<StoredParameterSummary> m_parameters;
    Vector<UnknownCodeWitness> m_witnesses;
    FunctionSummary m_summaryOfUnknownFunction;
    Effects m_unknownCode { Effects::unknownCode() };
    const EffectSummaries* m_known { nullptr };
    unsigned m_firstStored { 0 };
};

class CalleeSummary {
public:
    CalleeSummary(const EffectContext&, FunctionID);

    FunctionSummaryView operator*() const LIFETIME_BOUND { return m_summaryByRole ? FunctionSummaryView(*m_summaryByRole) : m_computed; }

    struct Arrow {
        FunctionSummaryView view;

        const FunctionSummaryView* operator->() const LIFETIME_BOUND { return &view; }
    };
    Arrow operator->() const LIFETIME_BOUND { return { **this }; }

private:
    FunctionSummaryView m_computed;
    std::optional<FunctionSummary> m_summaryByRole;
};

EffectSummaries createSummariesFromFlow(const Program&, const FlowResults&, BuildContext&);
EffectSummaries createSummariesFromFlow(const Program&, const FlowResults&, const BorrowedParameters&, BuildContext&);
EffectSummaries computeEffectSummaries(const Program&, const FlowResults&, BuildContext&);
void updateEffectSummaries(const Program&, const FlowResults&, BuildContext&, BodySummaries& kept, EffectSummaries&, BodyReferences* = nullptr);
FunctionSummary summarizeBody(const EffectContext&, FunctionID, FunctionAnalyses&);

uint64_t stampOfRecord(const Function&);
uint64_t stampOfRecordForCallers(const Function&);
uint64_t stampOfEntry(const Function&);

struct BaseObject {
    enum class Kind : uint8_t { Untracked, EntryParameter, OwnAllocation };

    Kind kind { Kind::Untracked };
    uint32_t entryParameter { 0 };
};

std::unique_ptr<FunctionAnalyses> createAnalysesForSummary(BuildContext&, const EffectContext&, const FlowResults&, FunctionID, const Function&);
void forEachBaseObject(FunctionAnalyses&, ValueID, NOESCAPE const ScopedLambda<void(const BaseObject&)>&);
OperandSummary summarizeEntryParameter(FunctionAnalyses&, uint32_t entryParameter);
OperandSummary publishedParameterSummary(const FlowResults&, FunctionID, uint32_t entryParameter);
CallerState callerStateOf(const FlowResults&, FunctionID);
bool runsAtMostOnce(const FlowResults&, FunctionID);
BitVector functionsCalledIndirectly(const FlowResults&, size_t numberOfFunctions);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
