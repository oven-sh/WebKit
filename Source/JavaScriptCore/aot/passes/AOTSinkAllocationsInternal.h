/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTForwardDataflow.h"
#include "AOTFreshResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTLiveness.h"
#include "AOTSinkAllocations.h"
#include "AOTSinkAllocationsState.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class AllocationSinking;
class ObjectInterpreter;

enum class SinkingMode : uint8_t { Analyze, Count, Rewrite };
enum class ReturnedObjects : uint8_t { AreBuilt, StayVirtual };

// An analysis goes through the function about this often: the solver until its decisions stand, a count, and both again for each round of candidates given up.
inline constexpr uint64_t expectedNumberOfSweeps = 16;

enum class ScreeningResult : uint8_t { SomeAllocationMayPay, NoAllocationPays, DependsOnFrequencies };

struct SinkingPolicy {
    ReturnedObjects returnedObjects { ReturnedObjects::AreBuilt };
    bool recordsLosses { true };
    bool leavesWhenNoAllocationPays { false };
    // Ascending: what addTracesOfFruitlessAllocations() has listed behind earlier analyses of the function. Allocations with such a number are not followed again.
    const Vector<uint64_t>* tracesOfFruitlessAllocations { nullptr };
};

struct BuildCause {
    std::optional<Opcode> opcode;
    EscapeReasons reasons;
    LimitID limit { LimitID::None };
    // A store by name counts as code that nobody knows. It may stand where such code follows at once: no guard and no read is carried past that anyway.
    bool admitsStoresByName { false };
};

// What a report says of an object that the use makes necessary.
BuildCause describeCause(FunctionAnalyses&, const Use&);

struct Candidate {
    ValueID definition;
    BlockID block;
    VirtualFamilyID family;
    uint32_t descriptionIndex { 0 };
    std::optional<uint8_t> tagBitWhenAbsent;
    bool isJoin { false };
    bool isAbandoned { false };
    bool needsGuard { false };
    bool diesVirtual { false };
    bool diesOutsideJoins { false };
    bool isStoredLate { false };
    bool isBuiltBeforeBranches { false };
    bool wasFruitlessBefore { false };
    // Abandoned before the families were made: no field of it is known, so nothing may take it up again.
    bool isLeftOut { false };
    unsigned numberOfBuilds { 0 };
    unsigned numberOfRarerBuilds { 0 };
    unsigned numberOfBorrowingBuilds { 0 };
    unsigned numberOfFoldedStores { 0 };
    unsigned numberOfRepeatedStores { 0 };
    BlockID mergeBlock;
    std::optional<BuildCause> firstCause;
    LimitID abandonedBy { LimitID::None };
};

struct VirtualReturn {
    BlockID block;
    ValueID value;
    VirtualObjectID object;
    ObjectState state;
};

// The first five are filled for a block when AllocationSinking::prepare() is called for it.
struct SinkingInputs {
    BitVector throwingValues;
    BitVector valuesWritingObjects;
    BitVector valuesCallingUnknownCode;
    BitVector throwingTerminators;
    BitVector conditionsOnlyBranchedOn;
    BitVector blocksInCycles;
    BitVector mergeBlocks;
    BitVector blocksWithCandidates;
    BitVector blocksWithJoins;
    bool hasOperationsOnSlots { false };
    bool isLargeForItsWork { false };
};

// A name is a value that may stand for a candidate, for a guard of one, for its results or for a test of one: the candidates, and what an interpreter may bind to one. More values may be listed.
// Whatever mentions no name leaves every object as it is, but for what it writes.
struct SinkingNames {
    BitVector names;
    BitVector valuesMentioningNames;
    BitVector terminatorsMentioningNames;
    // A parameter of the block or its subject is a name.
    BitVector blocksBindingNames;
};

struct ObjectValues {
    ValueID tag;
    ValueID cell;
    ValueID guard;
    Vector<ValueID, 4> fields;

    ValueID field(uint32_t index) const { return index < fields.size() ? fields[index] : ValueID(); }
    void setField(uint32_t index, ValueID);
};

class ObjectValueMap {
public:
    ObjectValues& ensure(VirtualObjectID);
    const ObjectValues* find(VirtualObjectID) const;
    void remove(VirtualObjectID);
    void move(VirtualObjectID from, VirtualObjectID to);
    void clear() { m_entries.clear(); }

private:
    struct Entry {
        VirtualObjectID object;
        ObjectValues values;
    };

    Vector<Entry, 2> m_entries;
};

struct IncomingBinding {
    enum class Kind : uint8_t { Argument, NarrowedOperand, Guard, Other };

    Kind kind { Kind::Other };
    ValueID value;
};

struct OutgoingEdge {
    Edge edge;
    BlockID target;
    Vector<IncomingBinding, 4> bindings;
};

OutgoingEdge outgoingEdgeOf(const Function&, Edge);

enum class HeadValueKind : uint8_t { Tag, Cell, Guard, Field };

struct HeadValue {
    VirtualObjectID object;
    HeadValueKind kind { HeadValueKind::Field };
    uint32_t field { 0 };
};

struct RewriteTables {
    Vector<Vector<ValueID, 4>> headValues;
    Vector<uint32_t> firstHeadParameter;
    BitVector blocksWithHeadParameters;
    // Those that a rewritten block still leads to once its terminator is folded, and the entry.
    BitVector enteredBlocks;
    Vector<std::pair<ValueID, ValueID>> replacedReads;
    Vector<ValueID> entryConstants;
    Vector<BlockID> order;
    // No object is there, none is made and none is entered into a block behind: to rewrite such a block is to put it together again as it was.
    BitVector blocksLeftAsTheyAre;
    // Of a function that reads its arguments. No analysis can be made of a function that is half rewritten.
    Vector<ValueID> identityOfValue;
};

class RewriteCursor {
    WTF_MAKE_NONCOPYABLE(RewriteCursor);
public:
    RewriteCursor(AllocationSinking&, IRBuilder&);

    IRBuilder& builder() { return m_builder; }
    BlockID block() const { return m_builder.currentBlock(); }
    BlockID originalBlock() const { return m_originalBlock; }

    void beginBlock(BlockID original, std::optional<Target>&& handler, ObjectInterpreter&);
    void finishBlock(Terminator&&);

    ValueID emit(Value&&);
    void keep(ValueID);
    ValueID takeConditionOf(const Terminator&);
    void setOrigin(Origin origin) { m_builder.setOrigin(origin); }

    void stateDidChange() { m_handlerArgumentsAreStale = true; }
    void willKeepThrowingValue(ObjectInterpreter&);
    void cut(ObjectInterpreter&);

    BlockID addUncoveredBlock();
    BlockID addCoveredBlock(ObjectInterpreter&);
    void moveTo(BlockID);

private:
    std::optional<Target> handlerTargetFor(ObjectInterpreter&);

    AllocationSinking& m_sinking;
    IRBuilder& m_builder;
    Function& m_function;
    BlockID m_originalBlock;
    std::optional<Target> m_originalHandler;
    BlockFrequency m_frequency { BlockFrequency::Normal };
    bool m_handlerArgumentsAreStale { false };
};

// What an analysis ended with: which candidates it gave up, and what it decided to build where.
struct SinkingFindings {
    Vector<Candidate> candidates;
    SinkingDecisions decisions;
};

class AllocationSinking {
    WTF_MAKE_NONCOPYABLE(AllocationSinking);
public:
    using Heads = Vector<std::optional<SinkingState>>;

    AllocationSinking(FunctionAnalyses&, const SinkingPolicy&);
    ~AllocationSinking();

    bool collectCandidates();
    void analyze();
    // For the same function with some of its edges split. An empty block on an edge changes nothing that the analysis found, so it need not find it step by step again.
    bool analyzeStartingFrom(SinkingFindings&&);
    SinkingFindings takeFindings() { return { WTF::move(m_candidates), WTF::move(m_decisions) }; }
    bool hasCandidates() const;
    bool splitEdgesNeedingCode(IRBuilder&);
    bool duplicateLiveRange(IRBuilder&);
    void rewrite(IRBuilder&, const FreshResultShape* = nullptr);
    void recordLosses();
    void countStatistics();
    Vector<SunkAllocation> outcomes() const;

    FunctionAnalyses& analyses() { return m_analyses; }
    const FunctionAnalyses& analyses() const { return m_analyses; }
    const AnalysisContext& context() const { return m_analyses.context(); }
    const Program& program() const { return m_program; }
    const Function& function() const { return m_function; }
    const SinkingPolicy& policy() const { return m_policy; }
    const VirtualObjectContext& objectContext() const LIFETIME_BOUND { return m_objectContext; }
    ShapeContext shapeContext(VirtualObjectID) const;

    uint32_t numberOfCandidates() const { return m_candidates.size(); }
    Candidate& at(VirtualObjectID object) LIFETIME_BOUND { return m_candidates[object.index()]; }
    const Candidate& at(VirtualObjectID object) const LIFETIME_BOUND { return m_candidates[object.index()]; }
    VirtualFamily& familyOf(VirtualObjectID object) LIFETIME_BOUND { return m_families[at(object).family.index()]; }
    const VirtualFamily& familyOf(VirtualObjectID object) const LIFETIME_BOUND { return m_families[at(object).family.index()]; }
    const VirtualObjectDescription& descriptionOf(VirtualObjectID) const LIFETIME_BOUND;
    VirtualObjectID candidateDefinedBy(ValueID) const;
    ValueID sameValueThroughTests(ValueID);
    KeyOrderTable& orders() { return m_orders; }
    const KeyOrderTable& orders() const { return m_orders; }
    SinkingDecisions& decisions() { return m_decisions; }
    const SinkingInputs& inputs() const { return m_inputs; }
    // Before anything is asked about the values or the terminator of the block.
    void prepare(BlockID block)
    {
        if (!m_preparedBlocks.quickSet(block.index()))
            computeInputsOf(block);
    }
    // The indexes of the values that may change a state or that a handler has to see: those that mention a name, the candidates, what writes objects and, under a handler, what may throw.
    // The highest bit marks a value that mentions no name and is no candidate: all it does to a state is to write.
    static constexpr uint32_t onlyWritesOrThrows = 1u << 31;
    std::span<const uint32_t> relevantValuesOf(BlockID block) const LIFETIME_BOUND
    {
        ASSERT(m_preparedBlocks.quickGet(block.index()));
        RelevantValues range = m_relevantValuesOfBlocks[block.index()];
        return m_relevantValues.span().subspan(range.first, range.end - range.first);
    }
    bool mentionsName(const Terminator&, BlockID) const;
    // Nothing is bound, tested or decided by a name on such an edge: objects go along as they are, or die.
    bool isPlain(Edge edge, BlockID target) const
    {
        return !edge.isHandlerEdge() && m_ignoredEdges.isEmpty() && !m_names.terminatorsMentioningNames.quickGet(edge.from.index()) && !m_names.blocksBindingNames.quickGet(target.index());
    }
    bool checksShortcuts() const { return m_checksShortcuts; }
    bool buildsSomeObjectBeforeBranches() const { return m_buildsSomeObjectBeforeBranches; }
    const BitVector& loadableFieldsOf(VirtualObjectID);
    bool storesByNameWaitIn(BlockID) const;
    // The object has to be built where no store by name may stand. It is built where the store stands instead, in the rare arm, as an object that takes no such value always was.
    void refuseUnprovenValues(VirtualObjectID);
    const SinkingState* headOf(BlockID) const;
    // What can be said of the state at a head without making one for a block that is entered with nothing.
    bool isReached(BlockID block) const { return isEnteredWithNothing(block) || headOf(block); }
    bool hasObjectsAtHead(BlockID block) const
    {
        if (isEnteredWithNothing(block))
            return false;
        const SinkingState* head = headOf(block);
        return head && !head->objects.isEmpty();
    }
    bool holdsNothingAtHead(BlockID block) const
    {
        if (isEnteredWithNothing(block))
            return true;
        const SinkingState* head = headOf(block);
        return head && head->objects.isEmpty() && head->aliases.isEmpty() && head->tests.isEmpty();
    }
    std::span<const VirtualReturn> virtualReturns() const LIFETIME_BOUND { return m_returns.span(); }
    const FreshResultShape* resultShape() const { return m_resultShape; }
    bool foundThatNoAllocationPays() const { return m_foundThatNoAllocationPays; }
    void addTracesOfFruitlessAllocations(Vector<uint64_t>&) const;

    // While the function is analyzed. Few of its values name an object or a test of one.
    bool isLiveAtHead(ValueID, BlockID) const;
    // Whether the definition of the candidate is a parameter of the block or live at its head.
    bool isDefinitionLiveIn(VirtualObjectID object, BlockID block)
    {
        const std::optional<BitVector>& blocks = m_blocksWithLiveDefinition[object.index()];
        if (!blocks) [[unlikely]]
            return findBlocksWithLiveDefinition(object).quickGet(block.index());
        return blocks->quickGet(block.index());
    }

    uint64_t limit(LimitID) const;
    void didWork(uint64_t amount) { m_work += amount; }
    void startWorkOver() { m_work = 0; }
    void didCarry(VirtualObjectID object) { ++m_blocksCarrying[object.index()]; }
    bool hasSpentItsWork() const { return m_work > m_maxWork; }
    bool hasStartedOverWithFewerCandidates() const { return m_hasStartedOverWithFewerCandidates; }
    bool exceeds(LimitID, uint64_t amount, ValueID);
    void abandon(VirtualObjectID, LimitID = LimitID::None);
    void tagVariants(BlockID, VirtualObjectID, ObjectState&);
    void buildAtHeadForLackOfTagBits(BlockID, VirtualObjectID);
    bool isIgnored(Edge) const;
    void addVirtualReturn(VirtualReturn&&);
    void addEdgeNeedingCode(Edge);
    void didEnterJoin(VirtualObjectID join, VirtualObjectID source) { m_entriesIntoJoins.append(EntryIntoJoin { join, source }); }
    void didBuildAfterMerge(VirtualObjectID, BlockID mergeBlock);

    RewriteTables& tables() { return *m_tables; }
    RewriteCursor& cursor() { return *m_cursor; }
    ValueID entryConstant(Value&&);
    void forEachHeadValue(const SinkingState& head, NOESCAPE const ScopedLambda<void(const HeadValue&)>&) const;
    Vector<ValueID, 8> collectHeadValues(ObjectInterpreter&, BlockID target, bool isHandlerEdge);
    TypeID typeOfHeadValue(const SinkingState& head, const HeadValue&, TypeArena&) const;

private:
    friend class RewriteCursor;
    friend class SinkingProblem;


    Vector<DescribedObject> collectAllocations();
    bool findsThatNoAllocationPays(std::span<const DescribedObject>);
    template<typename ObjectMask> ScreeningResult screen(std::span<const DescribedObject>, BitVector& allocationsThatMayPay);
    void collectJoins();
    void uniteFamilies(Vector<uint32_t>& parentOfCandidate);
    void createFamilies(std::span<const DescribedObject>, std::span<const uint32_t> rootOfCandidate);
    bool isJoinParameter(BlockID, uint32_t parameterIndex) const;
    void findNames();
    const BitVector& findBlocksWithLiveDefinition(VirtualObjectID);
    void findWhetherObjectsAreBuiltBeforeBranches();
    void computeInputs();
    void computeInputsOf(BlockID);

    void solve();
    void giveUpOnNewDecisions();
    void giveUpOnEveryCandidate();
    bool startOverWithFewerCandidates();
    void giveUpForLackOfWork(std::span<const uint32_t> candidates);
    void analyzeUntilNothingChanges();
    void setAsideJoinsThatCarryOnlyCells();
    void setAsideJoinsOfAllocationsBuiltInTheirBlocks();
    bool isBuiltInItsBlock(const Candidate&) const;
    bool isBuiltAtOnce(const Candidate&) const;
    void abandonAllocationsBuiltAtOnce();
    void setAside(uint32_t join);
    void takeBackJoinsSetAside();
    void invalidateResults();
    void count();
    void countBlock(BlockID);
    bool abandonUnprofitableCandidates();
    bool buildBeforeBranchesInsteadOfAbandoning();
    void abandonJoinsThatOnlyFeedAbandonedJoins();
    bool paysToSink(const Candidate&) const;
    void resetCounters();

    Vector<Edge, 2> collectCellEdges(VirtualObjectID, BlockID mergeBlock);
    Vector<BlockID> collectLiveRange(VirtualObjectID, BlockID mergeBlock);
    bool paysWithoutEdges(VirtualObjectID, std::span<const Edge>);
    void duplicate(IRBuilder&, std::span<const BlockID> region, std::span<const Edge> cellEdges);

    void addHeadParameters(IRBuilder&);
    bool needsHeadParameters(BlockID) const;
    bool isLeftAsItIs(BlockID) const;
    void rewriteBlock(BlockID);
    void rewriteEdges(ObjectInterpreter&, Terminator&, std::span<const OutgoingEdge>);
    void rewriteEdge(ObjectInterpreter&, Terminator&, const OutgoingEdge&);
    void foldTerminator(ObjectInterpreter&, Terminator&);
    void convertProofsToArguments(ObjectInterpreter&, const OutgoingEdge&, Target&);
    void loadHeadValues(BlockID, const SinkingState& head, ObjectValueMap&);
    void fillTarget(ObjectInterpreter&, const OutgoingEdge&, Target&);
    bool isBoundParameter(BlockID, ValueID parameter) const;
    void insertEntryConstants();
    void removeBoundParameters(IRBuilder&);
    void applyReplacements();

    void recordRemovedAllocation(const Candidate&);
    void recordDecline(const Candidate&);

    FunctionAnalyses& m_analyses;
    const Program& m_program;
    const Function& m_function;
    const SinkingPolicy m_policy;
    const VirtualObjectContext m_objectContext;
    Vector<Candidate> m_candidates;
    Vector<VirtualObjectID> m_candidateOfValue;
    bool m_foundThatNoAllocationPays { false };
    mutable std::optional<LivenessOfAskedValues> m_liveness;
    Vector<std::optional<BitVector>> m_blocksWithLiveDefinition;
    Vector<uint64_t> m_tracesOfAllocations;
    Vector<VirtualFamily> m_families;
    KeyOrderTable m_orders;
    SinkingDecisions m_decisions;
    SinkingInputs m_inputs;
    SinkingNames m_names;
    struct RelevantValues {
        uint32_t first { 0 };
        uint32_t end { 0 };
    };
    BitVector m_preparedBlocks;
    Vector<RelevantValues> m_relevantValuesOfBlocks;
    Vector<uint32_t> m_relevantValues;
    // A family gets fields while it is analyzed.
    struct LoadableFields {
        uint32_t numberOfFieldsLookedAt { 0 };
        BitVector fields;
    };
    Vector<LoadableFields> m_loadableFields;
    const bool m_checksShortcuts;
    bool m_buildsSomeObjectBeforeBranches { false };
    bool isEnteredWithNothing(BlockID block) const { return m_blocksEnteredWithNothing.get(block.index()); }

    // A block of the set has no state in the list until somebody asks for it.
    mutable Heads m_heads;
    mutable BitVector m_blocksEnteredWithNothing;
    Vector<uint32_t> m_joinsSetAside;
    struct EntryIntoJoin {
        VirtualObjectID join;
        VirtualObjectID source;

        friend bool operator==(const EntryIntoJoin&, const EntryIntoJoin&) = default;
    };
    Vector<EntryIntoJoin> m_entriesIntoJoins;
    bool m_setsAsideJoins { false };
    Vector<VirtualReturn, 2> m_returns;
    Vector<Edge, 2> m_ignoredEdges;
    Vector<Edge, 2> m_edgesNeedingCode;
    const FreshResultShape* m_resultShape { nullptr };
    bool m_mustSolveAgain { false };
    uint64_t m_work { 0 };
    // Since the analysis last started over: how often it went through the function, and through how many blocks it carried each candidate.
    uint64_t m_numberOfSweeps { 0 };
    Vector<uint32_t> m_blocksCarrying;
    uint64_t m_maxWork;
    bool m_hasRecordedLackOfWork { false };
    bool m_hasStartedOverWithFewerCandidates { false };
    std::optional<RewriteTables> m_tables;
    std::optional<RewriteCursor> m_cursor;
};

bool isFrameAllocatable(const AllocationSinking&, const Candidate&);
bool buildsItsOperands(const Program&, const Value& user);

class SinkingProblem {
public:
    using State = SinkingState;

    explicit SinkingProblem(AllocationSinking& sinking)
        : m_sinking(sinking)
    {
    }

    State stateAtEntry();
    void enterEdge(State&, Edge);
    bool merge(State& into, const State& from);
    bool merge(State& into, State&& from);

private:
    bool replaces(const State& into, const State& from) const;
    bool mergeParts(State& into, const State& from);
    bool mergeObjects(State& into, const State& from);
    bool mergeObject(State& into, VirtualObjectID, ObjectState& target, const ObjectState& source);
    bool mergeVariants(State& into, VirtualObjectID, ObjectState& target, const ObjectState& source);
    bool mergeHeldObjects(VirtualObjectID, ObjectState& target, const ObjectState& source);
    bool mergeAliases(State& into, const State& from);
    bool mergeTests(State& into, const State& from);
    bool mustBeBuiltBefore(BlockID, VirtualObjectID, const ObjectState&) const;

    AllocationSinking& m_sinking;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
