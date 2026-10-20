/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTEffects.h"
#include "AOTEscapeAnalysis.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;

using ProofSet = EnumeratorBitSet<Proof, numberOfEnumerators<Proof>>;

inline constexpr ProofSet proofsRenewedByShapeEpoch { Proof::Shape, Proof::OwnInstance, Proof::Pristine };

enum class GuardSource : uint8_t { Test, SwitchCase, Allocation };

struct GuardOrigin {
    GuardSource source { GuardSource::Test };
    ValueID test;
    BlockID switchBlock;
    uint32_t caseIndex { 0 };

    friend bool operator==(const GuardOrigin&, const GuardOrigin&) = default;
};

// On every way to the guard one of the origins proved it.
struct GuardFact {
    ProofSet proofs;
    ValueID subject;
    Vector<MemoryLocation, 2> dependsOn;
    Vector<GuardOrigin, 1> origins;

    bool proves(Proof proof) const { return proofs.contains(proof); }
    bool isRenewedByShapeEpoch() const { return (proofs | proofsRenewedByShapeEpoch) == proofsRenewedByShapeEpoch; }
    const GuardOrigin* singleOrigin() const LIFETIME_BOUND { return origins.size() == 1 ? &origins.first() : nullptr; }

    friend bool operator==(const GuardFact&, const GuardFact&) = default;
};

enum class GuardKnowledge : uint8_t { Unknown, Known };

struct GuardFacts {
    GuardKnowledge knowledge { GuardKnowledge::Unknown };
    Vector<GuardFact, 1> facts;

    bool isKnown() const { return knowledge == GuardKnowledge::Known; }
    const GuardFact* find(Proof, ValueID subject) const LIFETIME_BOUND;
    const GuardFact* find(Proof) const LIFETIME_BOUND;

    friend bool operator==(const GuardFacts&, const GuardFacts&) = default;
};

enum class GuardState : uint8_t { Unknown, Unavailable, Available };

constexpr GuardState meet(GuardState a, GuardState b)
{
    if (a == GuardState::Unavailable || b == GuardState::Unavailable)
        return GuardState::Unavailable;
    if (a == GuardState::Unknown || b == GuardState::Unknown)
        return GuardState::Unknown;
    return GuardState::Available;
}

bool isGuardValue(const Function&, ValueID);
BitVector guardValuesOf(const Function&, const CFG&);
bool allocatesOwnLayout(const Program&, const Value& allocation);
ProofSet proofsOfAllocation(const Program&, const Function&, const Value& allocation);
bool allocationProves(const Program&, const Function&, const Value& allocation, const Value& test);
ValueID guardedOperandOf(const Value& user);
ProofSet proofsNeededFromGuard(const Program&, const Function&, const Value& user);
bool haveSameCondition(FunctionAnalyses&, const Value& test, const Value& otherTest);
Vector<MemoryLocation, 2> locationsReadBy(const Value& test);
Vector<MemoryLocation, 2> locationsReadBy(const Terminator& provingTerminator, uint32_t caseIndex);
Vector<MemoryLocation, 2> locationsBehindAllocationProof(Proof);

struct GuardDependencies {
    Vector<MemoryLocation, 2> locations;
    LocationMasks masks;
    ValueID subject;
};

// Nothing for a test whose proof no write ends.
std::optional<GuardDependencies> dependenciesOfGuardOf(FunctionAnalyses&, const Value& test);

// Answers for one writer whether it may end what tests prove. What that takes to know about the writer is computed once.
class GuardWriter {
    WTF_MAKE_NONCOPYABLE(GuardWriter);
public:
    GuardWriter(FunctionAnalyses&, ValueID writer);
    ~GuardWriter();

    bool mayEnd(const GuardDependencies&);

private:
    struct Query;

    std::unique_ptr<Query> m_query;
};

class GuardAvailability {
    WTF_MAKE_NONCOPYABLE(GuardAvailability);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(GuardAvailability);
public:
    explicit GuardAvailability(FunctionAnalyses&);

    bool gaveUp() const { return m_gaveUp; }

    std::span<const ValueID> guards() const LIFETIME_BOUND { return m_guards.span(); }
    const GuardFacts& factsOf(ValueID guard) const LIFETIME_BOUND;

    GuardState stateAt(ValueID guard, ProgramPoint) const;
    GuardState stateAt(ValueID guard, ProofSet reliedOn, ProgramPoint) const;
    GuardState stateAt(ValueID guard, ProofSet reliedOn, ValueID subject, ProgramPoint) const;
    GuardState stateAt(ValueID guard, const Value& user, ProgramPoint) const;
    GuardState stateOnEdge(ValueID guard, Edge) const;
    bool mayBeEndedAt(ValueID guard, const Value& user, ProgramPoint writer) const;

    class Cursor {
    public:
        Cursor(const GuardAvailability&, BlockID);

        void advanceTo(uint32_t index);
        uint32_t index() const { return m_index; }
        GuardState stateOf(ValueID guard) const;
        GuardState stateOf(ValueID guard, ProofSet reliedOn) const;
        GuardState stateOf(ValueID guard, ProofSet reliedOn, ValueID subject) const;
        GuardState stateOf(ValueID guard, const Value& user) const;

    private:
        const GuardAvailability& m_availability;
        BlockID m_block;
        uint32_t m_index { 0 };
        BitVector m_available;
    };

    void dump(PrintStream&) const;

private:
    friend class Cursor;

    struct Component {
        ValueID guard;
        uint32_t factIndex { 0 };
        uint32_t killClass { 0 };
    };

    struct KillClass {
        Vector<MemoryLocation, 2> locations;
        LocationMasks masks;
        ValueID subject;
        BitVector components;
    };

    struct Kill {
        uint32_t index { 0 };
        uint32_t killClass { 0 };
    };

    struct EpochSnapshot {
        ValueID epochRead;
        ValueID renewedGuard;
        uint32_t firstBit { 0 };
    };

    void collectGuards();
    void collectUnnamedAllocations();
    void addGuard(ValueID);
    void removeGuardsFrom(size_t firstRemovedGuard);
    void computeFacts();
    GuardFacts factsOfEdgeResult(BlockID, uint32_t parameterIndex, const EdgeResult&) const;
    GuardFacts factsOfAllocation(ValueID) const;
    GuardFacts factsAfterEdge(const GuardFacts& factsBeforeEdge, Edge) const;
    std::optional<ValueID> subjectBeforeEdge(ValueID subjectAfterEdge, Edge) const;
    bool leavesUnspecified(Edge, ValueID subjectAfterEdge) const;
    GuardFacts meetFactsAfterEdges(std::span<const Edge>, std::span<const GuardFacts> factsAfterEdges) const;
    GuardFacts meetFacts(const GuardFacts&, const GuardFacts&) const;
    GuardFacts uniteFacts(std::span<const ValueID> guards) const;
    bool haveSameOrigin(const GuardOrigin&, const GuardOrigin&) const;
    bool haveSameOrigins(const GuardFact&, const GuardFact&) const;
    bool areSameFact(const GuardFact&, const GuardFact&) const;
    bool originImplies(const GuardOrigin&, const GuardOrigin& implied, ProofSet) const;
    bool isAsStrongAs(const GuardFact&, const GuardFact& other) const;
    ProofSet proofsToUnite(const GuardFact&, const GuardFact&) const;
    bool tryWiden(GuardFact&, const GuardFacts& factsOnOtherWay) const;
    bool implies(const GuardFact&, const GuardFact& implied) const;
    bool anyImplies(const GuardFacts&, const GuardFact& implied) const;
    bool hasEquivalentFact(const GuardFacts&, const GuardFact&) const;
    std::optional<uint32_t> findSameFact(const GuardFacts&, const GuardFact&) const;

    bool trySolve();
    void numberComponents();
    void numberEpochSnapshots();
    uint32_t internKillClass(const GuardFact&, HashedIndex&);
    bool tryCollectKills();
    void solve();

    void applyDefinitions(BitVector& available, BlockID, uint32_t index) const;
    void applyKills(BitVector& available, BlockID, uint32_t index) const;
    void enterEdge(BitVector& available, Edge) const;
    bool isProvenOnEdge(const BitVector& available, Edge, const EdgeResult&, const Component&) const;
    bool mayEndBetweenTestAndBranch(ValueID test, BlockID branch, uint32_t killClass) const;
    bool isAvailableBeforeEdge(const BitVector& available, Edge, ValueID epoch, ValueID guard, const GuardFact& factAfterEdge) const;
    bool isAvailableIn(const BitVector& available, ValueID epoch, ValueID guard, const GuardFact&) const;
    bool isAvailableIn(const BitVector& available, ValueID epoch, ValueID guard, const GuardFact&, ValueID subject) const;
    std::optional<uint32_t> firstBitOf(ValueID epoch, ValueID guard) const;
    std::span<const Kill> killsIn(BlockID) const;
    std::span<const Component> componentsOf(ValueID guard) const;
    uint32_t firstComponentOf(ValueID guard) const;
    GuardState stateIn(const BitVector& available, ValueID guard, const ProofSet* reliedOn) const;
    GuardState stateIn(const BitVector& available, ValueID guard, const ProofSet* reliedOn, ValueID subject) const;
    GuardState stateIn(const BitVector& available, ValueID guard, const Value& user) const;

    FunctionAnalyses& m_analyses;
    const Function& m_function;
    bool m_gaveUp { false };
    GuardFacts m_unknownFacts;
    Vector<ValueID> m_guards;
    Vector<std::optional<uint32_t>> m_guardIndexOfValue;
    Vector<GuardFacts> m_facts;
    Vector<Component> m_components;
    Vector<uint32_t> m_firstComponent;
    Vector<EpochSnapshot> m_epochSnapshots;
    Vector<EpochSnapshot> m_epochSnapshotsByGuard;
    uint32_t m_numberOfBits { 0 };
    Vector<KillClass> m_killClasses;
    Vector<Kill> m_kills;
    Vector<uint32_t> m_firstKill;
    Vector<BitVector> m_killedOnThrow;
    Vector<BitVector> m_availableAtHead;
    BitVector m_isSolved;
};

enum class EarlierTest : uint8_t { Unknown, Failed };

class FailedTests {
    WTF_MAKE_NONCOPYABLE(FailedTests);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FailedTests);
public:
    explicit FailedTests(FunctionAnalyses&);

    EarlierTest find(const Value& test, BlockID at) const;

private:
    struct FailedEdge {
        Opcode opcode { Opcode::Undefined };
        ValueID subject;
        ValueID test;
        BlockID target;
    };

    struct FailedSwitch {
        ValueID subject;
        BlockID from;
        BlockID target;
    };

    bool isCoveredBySwitch(const Value& shapeTest, const FailedSwitch&) const;

    FunctionAnalyses& m_analyses;
    const Function& m_function;
    Vector<FailedEdge> m_failedEdges;
    Vector<FailedSwitch> m_failedSwitches;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
