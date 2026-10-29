/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTEffects.h"
#include "AOTEscapeAnalysis.h"
#include "AOTUseLists.h"
#include "AOTVector.h"
#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/IterationStatus.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

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

struct GuardFact {
    ProofSet proofs;
    ValueID subject;
    Vector<MemoryLocation, 2> dependsOn;
    Vector<GuardOrigin, 1> origins;
    ValueID position;

    bool proves(Proof proof) const { return proofs.contains(proof); }
    const GuardOrigin* singleOrigin() const LIFETIME_BOUND { return origins.size() == 1 ? &origins.first() : nullptr; }

    friend bool operator==(const GuardFact&, const GuardFact&) = default;
};

enum class GuardKnowledge : uint8_t { Unknown, Known };

struct GuardFacts {
    GuardKnowledge knowledge { GuardKnowledge::Unknown };
    Vector<GuardFact, 1> facts;

    bool isKnown() const { return knowledge == GuardKnowledge::Known; }
    const GuardFact* find(Proof, std::optional<ValueID> subject = std::nullopt) const LIFETIME_BOUND;

    friend bool operator==(const GuardFacts&, const GuardFacts&) = default;
};

enum class GuardState : uint8_t { Unknown, Unavailable, Available };

bool isGuardValue(const Function&, ValueID);
BitVector guardValuesOf(const Function&, const CFG&);
bool allocatesOwnLayout(const Program&, const Value& allocation);
ProofSet proofsOfAllocation(const Program&, const Function&, const Value& allocation);
bool allocationProves(const Program&, const Function&, const Value& allocation, const Value& test);
ValueID guardedOperandOf(const Value& user);
ProofSet proofsNeededFromGuard(const Program&, const Function&, const Value& user);
using ParameterLocator = ScopedLambda<ValueLocation(ValueID parameter)>;
using GuardOriginVisitor = ScopedLambda<IterationStatus(Proof, const GuardOrigin&)>;
void forEachGuardOrigin(const Program&, const Function&, const CFG&, NOESCAPE const ParameterLocator&, ValueID guard, NOESCAPE const GuardOriginVisitor&);
bool hasGuardForElementProofs(const Program&, const Function&, const CFG&, NOESCAPE const ParameterLocator&, const Value& access);
bool haveSameCondition(FunctionAnalyses&, const Value& test, const Value& otherTest, uint32_t firstComparedOperand = 0);
Vector<MemoryLocation, 2> locationsReadBy(const Value& test);
Vector<MemoryLocation, 2> locationsBehindAllocationProof(Proof);

enum class TestedSlotState : uint8_t { None, Filled, HoldsDeclaredType };

TestedSlotState slotStateTestedBy(const Value& test);
TestedSlotState slotStateProvenBy(const Function&, const GuardFact&);
bool leavesSlotsAsTested(const Program&, const Function&, const Value& writer, const Effects& effectsOfWriter, TestedSlotState);

// What these say of an object is about the fields of its layout and about the object as a whole, not about the properties it has beside the fields.
inline constexpr ProofSet proofsLeftByPropertiesBesideFields { Proof::NoFieldOutsideSlot, Proof::NotFrozen };
constexpr bool areLeftByPropertiesBesideFields(ProofSet proofs) { return !proofs.isEmpty() && (proofs | proofsLeftByPropertiesBesideFields) == proofsLeftByPropertiesBesideFields; }
bool definesPropertyBesideFields(const Program&, const Value& writer, const Effects& effectsOfWriter, const Value& object);

struct DependedLocations {
    DependedLocations() = default;
    DependedLocations(const Program&, Vector<MemoryLocation, 2>&&);

    Vector<MemoryLocation, 2> locations;
    Vector<uint64_t, 2> masksOfLocations;
    LocationMasks masks;
    TestedSlotState slotState { TestedSlotState::None };
    bool isLeftByPropertiesBesideFields { false };
};

struct GuardDependencies : DependedLocations {
    using DependedLocations::DependedLocations;

    ValueID subject;
};

std::optional<GuardDependencies> dependenciesOfGuardOf(FunctionAnalyses&, const Value& test);

bool comesFromOutside(FunctionAnalyses&, ValueID);

class GuardWriter {
    WTF_MAKE_NONCOPYABLE(GuardWriter);
public:
    GuardWriter(FunctionAnalyses&, ValueID writer);
    ~GuardWriter();

    bool mayEnd(const GuardDependencies&);

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

class GuardAvailability {
    WTF_MAKE_NONCOPYABLE(GuardAvailability);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(GuardAvailability);
public:
    explicit GuardAvailability(FunctionAnalyses&);

    bool gaveUp() const;

    std::span<const ValueID> guards() const LIFETIME_BOUND { return m_guards.span(); }
    const GuardFacts& factsOf(ValueID guard) const LIFETIME_BOUND;

    GuardState stateAt(ValueID guard, ProgramPoint) const;
    GuardState stateAt(ValueID guard, ProofSet reliedOn, ProgramPoint point) const { return stateAt(guard, reliedOn, ValueID(), point); }
    GuardState stateAt(ValueID guard, ProofSet reliedOn, ValueID subject, ProgramPoint) const;
    GuardState stateAt(ValueID guard, const Value& user, ProgramPoint) const;
    std::optional<GuardState> stateSeenWithoutSolving(ValueID guard, ProofSet reliedOn, ValueID subject, ProgramPoint) const;
    bool mayBeEndedAt(ValueID guard, const Value& user, ProgramPoint writer) const;

    class Cursor {
    public:
        Cursor(const GuardAvailability&, BlockID);

        void advanceTo(uint32_t index);
        GuardState stateOf(ValueID guard) const { return stateOf(guard, nullptr, ValueID()); }
        GuardState stateOf(ValueID guard, ProofSet reliedOn, ValueID subject) const { return stateOf(guard, &reliedOn, subject); }
        GuardState stateOf(ValueID guard, const Value& user) const;

    private:
        friend class GuardAvailability;

        Cursor(const GuardAvailability&, ProgramPoint);

        GuardState stateOf(ValueID guard, const ProofSet* reliedOn, ValueID subject) const;
        bool isInSolvedBlock() const { return m_availability.m_solution.solvedBlocks.get(m_block.index()); }
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
    void computeFacts();
    GuardFacts factsAfterEdge(const GuardFacts& factsBeforeEdge, Edge) const;
    std::optional<ValueID> valueBeforeEdge(ValueID valueAfterEdge, Edge) const;
    bool leavesUnspecified(Edge, ValueID subjectAfterEdge) const;
    GuardFacts meetFactsAfterEdges(std::span<const Edge>, std::span<const GuardFacts> factsAfterEdges) const;
    GuardFacts meetFacts(const GuardFacts&, const GuardFacts&) const;
    GuardFacts uniteFacts(std::span<const ValueID> guards) const;
    bool haveSameOrigin(const GuardOrigin&, const GuardOrigin&) const;
    bool areSameFact(const GuardFact&, const GuardFact&) const;
    bool originImplies(const GuardOrigin&, const GuardOrigin& implied, ProofSet) const;
    bool isAsStrongAs(const GuardFact&, const GuardFact& other) const;
    bool tryWiden(GuardFact&, const GuardFacts& factsOnOtherWay) const;
    // One number written twice is one position: each block has constants of its own.
    bool isSamePosition(ValueID, ValueID) const;
    bool implies(const GuardFact& fact, const GuardFact& implied) const { return fact.subject == implied.subject && isSamePosition(fact.position, implied.position) && isAsStrongAs(fact, implied); }
    bool anyImplies(const GuardFacts&, const GuardFact& implied) const;
    bool hasEquivalentFact(const GuardFacts&, const GuardFact&) const;

    void describeGuards();
    void numberComponents();
    bool exceedsWorkLimit(uint64_t work) const;
    bool exceedsWorkLimit() const;
    uint64_t countWriters() const;
    void leaveOutGuardsInRarerBlocks();
    void reportWorkLimit() const;

    void prepareToSolve() const;
    bool canSolve(ValueID guard) const;
    bool canSolveForAnyWriters(uint32_t guardIndex) const;
    enum class BehindTerminator : bool { No, Yes };
    std::optional<BitVector> componentsHeldAt(ValueID guard, ProgramPoint, BehindTerminator, unsigned depth) const;
    std::optional<BitVector> componentsProvenAtDefinition(ValueID guard, ValueLocation, unsigned depth) const;
    bool hasWriterThatEndsProofsOfClass(BlockID, uint32_t firstIndex, uint32_t killClass) const;
    void solveFor(ValueID guard) const;
    void solveNamedGuards() const;
    void solveUnnamedAllocations() const;
    void solveGuardsBetween(uint32_t firstGuardIndex, uint32_t endGuardIndex) const;
    void addGuardsBehind(ValueID guard, BitVector& guardIndexes) const;
    void solve(const BitVector& guardIndexes) const;
    void collectKills(const BitVector& killClasses) const;
    void addKill(BlockID, uint32_t index, BitVector&& killClasses) const;

    void applyDefinitions(BitVector& available, const BitVector& guardIndexes, BlockID, uint32_t index) const;
    void walk(BitVector& available, BlockID, uint32_t firstIndex, uint32_t endIndex) const;
    const Cursor& cursorAt(ProgramPoint) const LIFETIME_BOUND;
    void applyKills(BitVector& available, BlockID, uint32_t index) const;
    void enterEdge(BitVector& available, const BitVector& guardIndexes, Edge) const;
    enum class KillsOfSolve : bool { No, Yes };
    bool isProvenOnEdge(Edge, const EdgeResult&, const Component&, KillsOfSolve) const;
    bool isAvailableBeforeEdge(const BitVector& available, Edge, ValueID guard, const GuardFact& factAfterEdge) const;
    bool isAvailableIn(const BitVector& available, ValueID guard, const GuardFact&, ValueID subject, ValueID position) const;
    bool ends(const Kill& kill, uint32_t killClass) const { return m_solution.killSets[kill.killSet].killClasses.get(killClass); }
    std::span<const Kill> killsIn(BlockID block) const LIFETIME_BOUND { return m_solution.killsOfBlock[block.index()].span(); }
    std::span<const Kill> killsAt(ProgramPoint point) const LIFETIME_BOUND { return std::span<const Kill>(std::ranges::equal_range(killsIn(point.block), point.index, { }, &Kill::index)); }
    std::optional<uint32_t> guardIndexOf(ValueID guard) const { return guard.index() < m_guardIndexOfValue.size() ? m_guardIndexOfValue[guard.index()] : std::nullopt; }
    std::span<const Component> componentsOfGuardAt(uint32_t guardIndex) const LIFETIME_BOUND { return m_components.subspan(m_firstComponent[guardIndex], m_firstComponent[guardIndex + 1] - m_firstComponent[guardIndex]); }
    std::span<const Component> componentsOf(ValueID guard) const LIFETIME_BOUND { return guardIndexOf(guard) ? componentsOfGuardAt(*guardIndexOf(guard)) : std::span<const Component>(); }
    uint32_t firstComponentOf(ValueID guard) const;
    GuardState stateIn(const BitVector& available, ValueID guard, const ProofSet* reliedOn, ValueID subject = { }) const;
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
    // Where the last question about one point was asked. Passes ask in the order of the block, so the next question is mostly a few values further on.
    mutable std::optional<Cursor> m_cursorOfLastQuestion;
};

enum class EarlierTest : uint8_t { Unknown, Failed };

class FailedTests {
    WTF_MAKE_NONCOPYABLE(FailedTests);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FailedTests);
public:
    explicit FailedTests(FunctionAnalyses&);

    EarlierTest find(const Value& test, BlockID at) const;
    bool hasTestOf(ValueID) const;

private:
    static constexpr uint32_t noEdge = UINT32_MAX;

    struct FailedEdge {
        Opcode opcode { Opcode::Undefined };
        ValueID subject;
        ValueID test;
        BlockID target;
        // The nearest edge with the same opcode and subject whose target dominates this one's.
        uint32_t enclosing { noEdge };
    };

    // From this number in the dominator tree up to the next boundary, `edge` is the innermost edge whose target dominates the block.
    struct Boundary {
        uint32_t number;
        uint32_t edge;
    };

    struct FailedSwitch {
        ValueID subject;
        BlockID from;
        BlockID target;
    };

    bool isCoveredBySwitch(const Value& shapeTest, const FailedSwitch&) const;
    void nestFailedEdges();

    FunctionAnalyses& m_analyses;
    const Function& m_function;
    Vector<FailedEdge> m_failedEdges;
    // Two for each edge, at twice its index, so a run of edges owns a run of boundaries.
    Vector<Boundary> m_boundaries;
    Vector<uint32_t> m_numberInDominatorTree;
    Vector<FailedSwitch> m_failedSwitches;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
