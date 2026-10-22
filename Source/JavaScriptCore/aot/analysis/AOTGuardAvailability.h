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

enum class TestedSlotState : uint8_t { None, Filled, HoldsDeclaredType };

TestedSlotState slotStateTestedBy(const Value& test);
TestedSlotState slotStateProvenBy(const Function&, const GuardFact&);
bool leavesSlotsAsTested(const Program&, const Function&, const Value& writer, const Effects& effectsOfWriter, TestedSlotState);

// The mask of a location takes a walk over a layout set, so it is computed once for all the writers that are compared with it.
struct DependedLocations {
    DependedLocations() = default;
    DependedLocations(const Program&, Vector<MemoryLocation, 2>&&);

    Vector<MemoryLocation, 2> locations;
    Vector<uint64_t, 2> masksOfLocations;
    LocationMasks masks;
    TestedSlotState slotState { TestedSlotState::None };
};

struct GuardDependencies : DependedLocations {
    using DependedLocations::DependedLocations;

    ValueID subject;
};

// Nothing for a test whose proof no write ends.
std::optional<GuardDependencies> dependenciesOfGuardOf(FunctionAnalyses&, const Value& test);

// The value may be an object that was there before the function began, or that other code made: an entry parameter, what a read or a call gave, or what a block is passed one of those for.
bool comesFromOutside(FunctionAnalyses&, ValueID);

// Answers for one writer whether it may end what tests prove. What that takes to know about the writer is computed once.
class GuardWriter {
    WTF_MAKE_NONCOPYABLE(GuardWriter);
public:
    GuardWriter(FunctionAnalyses&, ValueID writer);
    ~GuardWriter();

    bool mayEnd(const GuardDependencies&);

    // mayEnd() for many proofs that depend on the same locations: what does not hang on the object is found once.
    enum class EndedProofs : uint8_t { None, All, AboutSomeObjects };
    struct WrittenDependencies {
        EndedProofs endedProofs { EndedProofs::None };
        uint32_t index { 0 };
    };
    WrittenDependencies writtenOf(const DependedLocations&);
    bool mayEndProofAbout(ValueID subject, bool subjectComesFromOutside, const WrittenDependencies&);

private:
    struct Query;

    std::unique_ptr<Query> m_query;
};

// The facts of all guards are computed at once. Where they hold is solved for the guards that somebody asks about, and an answer does not depend on what was asked before.
class GuardAvailability {
    WTF_MAKE_NONCOPYABLE(GuardAvailability);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(GuardAvailability);
public:
    explicit GuardAvailability(FunctionAnalyses&);

    bool gaveUp() const;

    std::span<const ValueID> guards() const LIFETIME_BOUND { return m_guards.span(); }
    const GuardFacts& factsOf(ValueID guard) const LIFETIME_BOUND;

    GuardState stateAt(ValueID guard, ProgramPoint) const;
    GuardState stateAt(ValueID guard, ProofSet reliedOn, ProgramPoint) const;
    GuardState stateAt(ValueID guard, ProofSet reliedOn, ValueID subject, ProgramPoint) const;
    GuardState stateAt(ValueID guard, const Value& user, ProgramPoint) const;
    // For whoever holds a cursor: what the cursor would say, where a walk back to the definition of the guard shows it.
    std::optional<GuardState> stateSeenWithoutSolving(ValueID guard, ProofSet reliedOn, ValueID subject, ProgramPoint) const;
    GuardState stateOnEdge(ValueID guard, Edge) const;
    bool mayBeEndedAt(ValueID guard, const Value& user, ProgramPoint writer) const;

    // For whoever asks about the guards that the operations of a block name.
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
        friend class GuardAvailability;

        Cursor(const GuardAvailability&, ProgramPoint);

        bool isInSolvedBlock() const;
        void replay() const;
        void solveFor(ValueID guard) const;

        const GuardAvailability& m_availability;
        BlockID m_block;
        uint32_t m_index { 0 };
        mutable unsigned m_numberOfSolvesSeen { 0 };
        mutable BitVector m_available;
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
        uint32_t locations { 0 };
        ValueID subject;
        BitVector components;
        uint32_t classBetweenTestAndBranch { 0 };
    };

    struct KillSet {
        BitVector killClasses;
        BitVector components;
    };

    struct Kill {
        uint32_t index { 0 };
        uint32_t killSet { 0 };
    };

    struct Writer {
        uint32_t blockOrder { 0 };
        uint32_t index { 0 };
    };

    struct Solution {
        bool isPrepared { false };
        bool gaveUp { false };
        bool didReportWorkLimit { false };
        bool solvesUnnamedAllocations { true };
        bool didSolveAllNamedGuards { false };
        bool didSolveAllUnnamedAllocations { false };
        unsigned numberOfSolves { 0 };
        unsigned numberOfSeparateSolves { 0 };
        Vector<Writer> writers;
        BitVector killClassesWithKills;
        Vector<KillSet> killSets;
        HashedIndex killSetIndex;
        Vector<Vector<Kill>> killsOfBlock;
        Vector<BitVector> killedOnThrow;
        BitVector solvedGuards;
        BitVector solvedBlocks;
        Vector<BitVector> availableAtHead;
        Vector<uint64_t> valuesWalkedSinceDefinition;
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

    void describeGuards();
    void numberComponents();
    uint32_t internLocations(const Vector<MemoryLocation, 2>&, TestedSlotState, HashedIndex&);
    uint32_t internKillClass(const GuardFact&, HashedIndex& locationsIndex, HashedIndex& killClassIndex);
    uint32_t internKillClass(const GuardFact&, TestedSlotState, HashedIndex& locationsIndex, HashedIndex& killClassIndex);
    bool exceedsWorkLimit(uint64_t work) const;
    bool exceedsWorkLimit() const;
    uint64_t countWriters() const;
    void leaveOutGuardsInRarerBlocks();
    void reportWorkLimit() const;

    void prepareToSolve() const;
    bool canSolve(ValueID guard) const;
    bool canSolveForAnyWriters(uint32_t guardIndex) const;
    enum class BehindTerminator : bool { No, Yes };
    std::optional<BitVector> componentsHeldSinceDefinition(ValueID guard, ProgramPoint) const;
    std::optional<BitVector> componentsHeldAt(ValueID guard, ProgramPoint, BehindTerminator, unsigned depth) const;
    std::optional<BitVector> componentsProvenAtDefinition(ValueID guard, ValueLocation, unsigned depth) const;
    bool endsProofsOfClass(ProgramPoint writer, const EffectsOnEdges&, uint32_t killClass) const;
    bool hasWriterThatEndsProofsOfClass(BlockID, uint32_t firstIndex, uint32_t killClass) const;
    bool hasWriterBetweenTestAndBranch(ValueID test, BlockID branch, uint32_t killClass) const;
    void solveFor(ValueID guard) const;
    bool isWorthSolvingAlone(uint32_t guardIndex) const;
    void solveNamedGuards() const;
    void solveUnnamedAllocations() const;
    void solveGuardsBetween(uint32_t firstGuardIndex, uint32_t endGuardIndex) const;
    void addGuardsBehind(ValueID guard, BitVector& guardIndexes) const;
    void solve(const BitVector& guardIndexes) const;
    void collectKillsOf(ValueID guard) const;
    void collectKills(const BitVector& killClasses) const;
    void addKill(BlockID, uint32_t index, BitVector&& killClasses) const;
    uint32_t internKillSet(BitVector&& killClasses) const;

    void applyDefinitions(BitVector& available, const BitVector& guardIndexes, BlockID, uint32_t index) const;
    void applyKills(BitVector& available, BlockID, uint32_t index) const;
    void enterEdge(BitVector& available, const BitVector& guardIndexes, Edge) const;
    bool isProvenOnEdge(Edge, const EdgeResult&, const Component&) const;
    bool mayEndBetweenTestAndBranch(ValueID test, BlockID branch, uint32_t killClass) const;
    bool isAvailableBeforeEdge(const BitVector& available, Edge, ValueID guard, const GuardFact& factAfterEdge) const;
    bool isAvailableIn(const BitVector& available, ValueID guard, const GuardFact&) const;
    bool isAvailableIn(const BitVector& available, ValueID guard, const GuardFact&, ValueID subject) const;
    bool ends(const Kill&, uint32_t killClass) const;
    std::span<const Kill> killsIn(BlockID) const LIFETIME_BOUND;
    std::span<const Kill> killsAt(ProgramPoint) const LIFETIME_BOUND;
    std::optional<uint32_t> guardIndexOf(ValueID guard) const;
    std::span<const Component> componentsOfGuardAt(uint32_t guardIndex) const LIFETIME_BOUND;
    std::span<const Component> componentsOf(ValueID guard) const LIFETIME_BOUND;
    uint32_t firstComponentOf(ValueID guard) const;
    GuardState stateIn(const BitVector& available, ValueID guard, const ProofSet* reliedOn) const;
    GuardState stateIn(const BitVector& available, ValueID guard, const ProofSet* reliedOn, ValueID subject) const;
    GuardState stateOfSolvableGuardIn(const BitVector& available, ValueID guard, const ProofSet* reliedOn, ValueID subject) const;
    GuardState stateIn(const BitVector& available, ValueID guard, const Value& user) const;

    FunctionAnalyses& m_analyses;
    const Function& m_function;
    GuardFacts m_unknownFacts;
    Vector<ValueID> m_guards;
    uint32_t m_numberOfNamedGuards { 0 };
    Vector<std::optional<uint32_t>> m_guardIndexOfValue;
    BitVector m_guardsLeftOut;
    Vector<GuardFacts> m_facts;
    Vector<Component> m_components;
    Vector<uint32_t> m_firstComponent;
    Vector<DependedLocations> m_dependedLocations;
    Vector<KillClass> m_killClasses;
    Vector<uint32_t> m_killClassesBySubject;
    uint32_t m_numberOfKillClassesOfNamedGuards { 0 };
    mutable Solution m_solution;
};

enum class EarlierTest : uint8_t { Unknown, Failed };

class FailedTests {
    WTF_MAKE_NONCOPYABLE(FailedTests);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FailedTests);
public:
    explicit FailedTests(FunctionAnalyses&);

    EarlierTest find(const Value& test, BlockID at) const;
    // False: find() answers Unknown for every test of the value, wherever it is asked.
    bool hasTestOf(ValueID) const;

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
