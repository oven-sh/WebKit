/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBorrowedParameters.h"
#include "AOTCompactList.h"
#include "AOTEffectSummaries.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/MallocPtr.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class PhaseTimes;

namespace Typed {

class BodyReferenceCollector;
class BodyReferences;

enum class SummaryPart : uint32_t { OnReturn, OnThrow, Parameter, UnknownCode };

struct SummaryNode {
    FunctionID function;
    uint32_t parameter : 30 { 0 };
    SummaryPart part : 2 { SummaryPart::UnknownCode };

    static SummaryNode onReturn(FunctionID function) { return { function, 0, SummaryPart::OnReturn }; }
    static SummaryNode onThrow(FunctionID function) { return { function, 0, SummaryPart::OnThrow }; }
    static SummaryNode parameterOf(FunctionID function, uint32_t parameter)
    {
        RELEASE_ASSERT(parameter < 1u << 30);
        return { function, parameter, SummaryPart::Parameter };
    }
    static SummaryNode unknownCode() { return { }; }

    friend bool operator==(const SummaryNode&, const SummaryNode&) = default;
};
static_assert(sizeof(SummaryNode) == 8);

struct SummaryDependency {
    SummaryNode source;
    SummaryNode target;

    friend bool operator==(const SummaryDependency&, const SummaryDependency&) = default;
};

struct BodySummary {
    Effects onReturn { Effects::none() };
    Effects onThrow { Effects::none() };
    Vector<ParameterSummary> parameters;
    Vector<SummaryDependency> dependencies;
    UnknownCodeWitness unknownCodeWitness;
    bool followsFieldsOfOperands { false };

    friend bool operator==(const BodySummary&, const BodySummary&) = default;
};

class StoredBodySummary {
    WTF_MAKE_NONCOPYABLE(StoredBodySummary);
public:
    StoredBodySummary() = default;
    StoredBodySummary(StoredBodySummary&&) = default;
    StoredBodySummary& operator=(StoredBodySummary&&) = default;
    StoredBodySummary(InternedLocationMasks&, FunctionID, const BodySummary&);

    class DependencyGroup {
    public:
        DependencyGroup(std::span<const uint32_t> words, bool isWide)
            : m_words(words)
            , m_isWide(isWide)
        {
        }

        void forEach(NOESCAPE const Invocable<void(uint32_t sourceNode, uint32_t targetNode)> auto& functor) const
        {
            if (m_isWide) {
                for (size_t position = 0; position < m_words.size(); position += 2)
                    functor(m_words[position], m_words[position + 1]);
                return;
            }
            for (uint32_t word : m_words)
                functor(word >> 16, word & 0xffff);
        }

    private:
        std::span<const uint32_t> m_words;
        bool m_isWide;
    };

    CompactEffects onReturn() const { return m_onReturn; }
    CompactEffects onThrow() const { return m_onThrow; }
    void setOnReturn(CompactEffects effects) { m_onReturn = effects; }
    void setOnThrow(CompactEffects effects) { m_onThrow = effects; }
    uint32_t numberOfParameters() const { return m_numberOfParameters; }
    uint32_t numberOfDependencies() const { return m_numberOfDependencies; }
    bool followsFieldsOfOperands() const { return m_followsFieldsOfOperands; }
    UnknownCodeWitness unknownCodeWitness(FunctionID function) const { return m_hasWitness ? UnknownCodeWitness { function, m_witness } : UnknownCodeWitness(); }

    std::span<const StoredParameterSummary> parameters() const LIFETIME_BOUND { return { m_lists.get(), m_numberOfParameters }; }
    std::span<StoredParameterSummary> mutableParameters() LIFETIME_BOUND { return { m_lists.get(), m_numberOfParameters }; }

    void forEachGroupOfDependencies(NOESCAPE const Invocable<void(FunctionID source, const DependencyGroup&)> auto& functor) const
    {
        std::span<const uint32_t> words = wordsOfDependencies();
        unsigned wordsInDependency = m_hasWideDependencies ? 2 : 1;
        for (size_t position = 0; position < words.size();) {
            uint32_t source = words[position];
            size_t size = static_cast<size_t>(words[position + 1]) * wordsInDependency;
            functor(source ? FunctionID(source - 1) : FunctionID(), DependencyGroup(words.subspan(position + 2, size), m_hasWideDependencies));
            position += 2 + size;
        }
    }

    void forEachFunctionInDependencies(FunctionID function, NOESCAPE const Invocable<void(FunctionID)> auto& functor) const
    {
        forEachGroupOfDependencies([&](FunctionID source, const DependencyGroup&) {
            if (source && source != function)
                functor(source);
        });
    }

    friend bool operator==(const StoredBodySummary&, const StoredBodySummary&);

private:
    std::span<const uint32_t> wordsOfDependencies() const LIFETIME_BOUND { return { reinterpret_cast<const uint32_t*>(m_lists.get() + m_numberOfParameters), m_numberOfWordsOfDependencies }; }
    size_t sizeOfLists() const { return m_numberOfParameters * sizeof(StoredParameterSummary) + m_numberOfWordsOfDependencies * sizeof(uint32_t); }

    CompactEffects m_onReturn;
    CompactEffects m_onThrow;
    MallocPtr<StoredParameterSummary> m_lists;
    uint32_t m_numberOfParameters : 30 { 0 };
    uint32_t m_hasWitness : 1 { 0 };
    uint32_t m_followsFieldsOfOperands : 1 { 0 };
    uint32_t m_numberOfDependencies { 0 };
    uint32_t m_numberOfWordsOfDependencies : 31 { 0 };
    uint32_t m_hasWideDependencies : 1 { 0 };
    ValueID m_witness;
};
static_assert(sizeof(StoredBodySummary) == 40);

struct StoredAddedEffects {
    FunctionID function;
    CompactEffects effects;
};

class BodySummaries {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BodySummaries);
    WTF_MAKE_NONCOPYABLE(BodySummaries);
public:
    BodySummaries() = default;

    enum class Records : uint8_t { MaySayLess, SayNoLess };
    enum class Edits : uint8_t { Any, OnlySetStorage };
    void didChange(std::span<const FunctionID>, Records = Records::MaySayLess, Edits = Edits::Any);
    void didLower(std::span<const FunctionID>);
    void didChangeDoingNoMore(std::span<const FunctionID>, Edits = Edits::Any);
    void didAddEffects(std::span<const AddedEffects>);
    bool followsFieldsOfOperands(FunctionID id) const { return id.index() < m_bodies.size() && m_bodies[id.index()].followsFieldsOfOperands(); }
    bool keepsSomethingOf(FunctionID id) const { return id.index() < m_isChanged.size() && !(m_isChanged[id.index()] && m_isChangedForTable[id.index()] && m_recordSays[id.index()] == RecordSays::MaybeLess); }

    bool describesFunctionsAsTheyAre(size_t numberOfFunctions) const { return m_bodies.size() == numberOfFunctions && !m_isChangedForTable.contains(1) && !m_isChangedForTableIfLowered.contains(1); }
    const BorrowedParameters& borrowedParameters() const LIFETIME_BOUND { return m_borrowed; }
    BorrowedParameters computeBorrowedParametersOfChangedBodies(const Program&, BuildContext&);
    const EffectSummaries& seed() const LIFETIME_BOUND { return m_seed; }

private:
    friend class EffectSummarizer;

    static constexpr unsigned functionsInChunk = 256;

    void countLoweredBodiesWithNewStorageAsChanged();
    BorrowedParameters computeBorrowedParametersFromLatestTable(const Program&, BuildContext&, std::span<const uint64_t> stampsOfRecords, std::span<const uint8_t> recordLooksDifferent);
    void didTouch(FunctionID id)
    {
        if (id.index() < m_wasTouchedSinceLatestTable.size())
            m_wasTouchedSinceLatestTable[id.index()] = 1;
    }

    const Ref<InternedLocationMasks> m_masks { InternedLocationMasks::create() };
    Vector<StoredBodySummary> m_bodies;
    Vector<CompactList<FunctionID>> m_functionsOfClosuresMade;
    Vector<uint8_t> m_isChanged;
    Vector<uint8_t> m_isChangedForTable;
    Vector<uint8_t> m_isChangedIfLowered;
    Vector<uint8_t> m_isChangedForTableIfLowered;
    enum class RecordSays : uint8_t { WhatItSaid, NoLess, MaybeLess };
    Vector<RecordSays> m_recordSays;
    Vector<uint8_t> m_wasLoweredSinceSummarized;
    bool m_hasUpperBounds { false };
    Vector<StoredAddedEffects> m_addedEffects;
    Vector<uint64_t> m_stampsOfRecords;
    Vector<uint64_t> m_stampsOfEntries;
    BorrowingBodies m_borrowingBodies;
    Vector<uint8_t> m_wasTouchedSinceLatestTable;
    Vector<uint64_t> m_stampsOfRecordsAtLatestTable;
    BorrowedParameters m_borrowed;
    Vector<uint64_t> m_parametersWhoseContentsAreCalled;
    EffectSummaries m_seed { Ref { m_masks }, EffectSummaries::Contents::SayNothingOfBodies };

    bool m_isSolved { false };
    Vector<uint8_t> m_isEnteredByUnknownCode;
    Vector<uint32_t> m_firstNode;
    Vector<uint32_t> m_numberOfNodesOf;
    uint32_t m_numberOfNodes { 0 };
    Vector<CompactEffects> m_effects;
    Vector<uint8_t> m_takesInUnknownCode;
    Vector<uint32_t> m_firstUser;
    Vector<FunctionID> m_users;
    UncheckedKeyHashMap<uint32_t, Vector<FunctionID>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_usersAddedSince;
    Vector<Effects> m_effectsEnteredInChunk;
    Vector<uint8_t> m_takesInUnknownCodeInChunk;
    Effects m_unknownCode { Effects::unknownCode() };
};

enum class ReasonToSummarize : uint8_t { None, IsNew, HasChanged, HasNewSeed, NamesFunctionThatLooksDifferent, MakesClosureWithNewSeed };
inline constexpr unsigned numberOfReasonsToSummarize = static_cast<unsigned>(ReasonToSummarize::MakesClosureWithNewSeed) + 1;

struct ExitReachability {
    BitVector reachesReturn;
    BitVector reachesThrow;
};

bool describeByRole(const Program&, const Function&, EffectDescription&);
constexpr bool isSummarizedByRole(FunctionRole role) { return role == FunctionRole::Serializer || role == FunctionRole::Parser || role == FunctionRole::ValidatingParser; }

uint64_t parametersWhoseContentsAreCalled(const Program&, FunctionID, const BorrowedParameters&);

ExitReachability computeExitReachability(const Function&);
BodySummary summarizeBodyLocally(const EffectContext&, FunctionID, FunctionAnalyses&, BodyReferenceCollector* = nullptr);
FunctionSummary createFunctionSummary(Effects onReturn, Effects onThrow, Vector<ParameterSummary>&&, UnknownCodeWitness);

FunctionSummary createSummaryFromFlow(const Program&, const FlowResults&, const BorrowedParameters&, BuildContext&, FunctionID);

class EffectSummarizer {
public:
    enum class Witnesses : uint8_t { Omitted, Found };

    EffectSummarizer(const Program&, const FlowResults&, BuildContext&, BodySummaries&, Witnesses);

    void setIsOnlyCheck() { m_isOnlyCheck = true; }
    void giveReferencesTo(BodyReferences* references) { m_references = references; }
    void update(EffectSummaries&);
    void bringSeedUpToDate(EffectSummaries&);
    void finishUpdate(EffectSummaries&);

private:
    void findRecordsThatLookDifferent();
    void updateSeed(BorrowedParameters&&);
    Vector<ReasonToSummarize> findReasonsToSummarize() const;
    void summarizeBodies();
    using CountsByReason = std::array<uint64_t, numberOfReasonsToSummarize>;
    void logReasons(std::span<const ReasonToSummarize>, const CountsByReason& numberOfBodiesSummarizedAsBefore) const;
    void findFunctionsEnteredByUnknownCode();

    bool solveAgain(EffectSummaries&);
    size_t maxIncrementalSolveSize() const;
    void forEachUser(uint32_t function, NOESCAPE const Invocable<void(FunctionID)> auto&) const;
    void forEachSourceFunction(FunctionID user, NOESCAPE const Invocable<void(FunctionID)> auto&) const;
    bool hasEdgeFrom(FunctionID user, FunctionID function) const;
    Vector<uint8_t> markTransitiveUsers(std::span<const uint32_t> functions) const;
    std::optional<Vector<uint32_t>> addAffectedSources(std::span<const uint32_t> functions, Vector<uint8_t>& states, size_t limit) const;
    void numberNodesOfChangedFunctions();
    Vector<uint32_t> solveFunctions(std::span<const uint32_t> functions, Vector<uint32_t>& firstPositionOf);

    void solveAll(EffectSummaries&, PhaseTimes&);
    void numberAllNodes();
    void createNodes();
    void propagateEffects(PhaseTimes&);
    void takeInSources(uint32_t node);
    Vector<uint32_t> resolveNodesBelowCycles();
    void resolveNodesInAndAboveCycles(std::span<const uint32_t> nodes);
    void checkEffectsAgainstPlainSolve(std::span<const Effects> effectsOfNodesAlone) const;
    void propagateWitnesses();
    void listUsers();

    void uniteEffectsEnteredInChunk(unsigned chunk);
    bool updateEffectsOfUnknownCode();
    void prepareToAssemble();
    CompactEffects compactSolvedEffectsOf(uint32_t node);
    void assemble(EffectSummaries&, FunctionID);
    void checkAgainstSummariesFromNothing(const EffectSummaries&) const;
    void checkCoversSummariesFromNothing(const EffectSummaries&) const;

    Effects effectsOfUnknownCodeAlone() const;
    Effects solvedEffectsOf(uint32_t node) const;
    uint32_t indexOf(const SummaryNode&) const;
    uint32_t indexOf(FunctionID, uint32_t nodeOfFunction) const;
    uint32_t unknownCodeNode() const { return m_kept.m_numberOfNodes; }
    std::span<const uint32_t> sourcesOf(unsigned node) const LIFETIME_BOUND;

    const Program& m_program;
    const FlowResults& m_flow;
    BuildContext& m_build;
    BodySummaries& m_kept;
    const Witnesses m_witnessesWanted;
    bool m_isOnlyCheck { false };
    BodyReferences* m_references { nullptr };
    unsigned m_numberOfFunctionsBefore { 0 };
    Vector<uint8_t> m_recordLooksDifferent;
    Vector<uint8_t> m_recordLooksDifferentToCallers;
    Vector<uint8_t> m_hasNewSeed;
    Vector<uint32_t> m_changedFunctions;
    Vector<uint32_t> m_entryFlippedFunctions;
    Vector<uint32_t> m_renumberedFunctions;

    class MemoByMasks {
    public:
        void reset(size_t size);
        uint32_t get(uint32_t id, NOESCAPE const Invocable<uint32_t()> auto& compute);

    private:
        Vector<std::atomic<uint32_t>> m_resultsPlusOne;
    };
    MemoByMasks m_readsWithUnknownCode;
    MemoByMasks m_writesWithUnknownCode;
    MemoByMasks m_masksOfPerObjectHeaps;
    CompactEffects m_compactUnknownCode;

    Vector<UnknownCodeWitness> m_witnesses;
    Vector<uint32_t> m_firstSource;
    Vector<uint32_t> m_endOfSources;
    Vector<uint32_t> m_sources;
    Vector<uint8_t> m_isResolved;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
