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

struct SinkingPolicy {
    ReturnedObjects returnedObjects { ReturnedObjects::AreBuilt };
    bool recordsLosses { true };
};

struct BuildCause {
    std::optional<Opcode> opcode;
    Origin origin;
    EscapeReasons reasons;
    LimitID limit { LimitID::None };
};

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
    bool isStoredLate { false };
    bool isBuiltBeforeBranches { false };
    unsigned numberOfBuilds { 0 };
    unsigned numberOfRarerBuilds { 0 };
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

struct SinkingInputs {
    BitVector throwingValues;
    BitVector valuesWritingObjects;
    BitVector throwingTerminators;
    BitVector conditionsOnlyBranchedOn;
    BitVector blocksInCycles;
    BitVector mergeBlocks;
    BitVector blocksWithCandidates;
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
    Vector<std::pair<ValueID, ValueID>> replacedReads;
    Vector<ValueID> entryConstants;
    Vector<BlockID> order;
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

class AllocationSinking {
    WTF_MAKE_NONCOPYABLE(AllocationSinking);
public:
    using Heads = Vector<std::optional<SinkingState>>;

    AllocationSinking(FunctionAnalyses&, const SinkingPolicy&);
    ~AllocationSinking();

    bool collectCandidates();
    void analyze();
    bool hasCandidates() const;
    bool splitEdgesNeedingCode(IRBuilder&);
    bool duplicateLiveRange(IRBuilder&);
    void rewrite(IRBuilder&, const FreshResultShape* = nullptr);
    void recordLosses();
    void countStatistics();
    Vector<SunkAllocation> outcomes() const;

    FunctionAnalyses& analyses() { return m_analyses; }
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
    KeyOrderTable& orders() { return m_orders; }
    const KeyOrderTable& orders() const { return m_orders; }
    SinkingDecisions& decisions() { return m_decisions; }
    const SinkingInputs& inputs() const { return m_inputs; }
    const Heads& heads() const { return m_heads; }
    const SinkingState* headOf(BlockID) const;
    std::span<const VirtualReturn> virtualReturns() const LIFETIME_BOUND { return m_returns.span(); }
    const FreshResultShape* resultShape() const { return m_resultShape; }

    uint64_t limit(LimitID) const;
    bool exceeds(LimitID, uint64_t amount, ValueID);
    void abandon(VirtualObjectID, LimitID = LimitID::None);
    void tagVariants(BlockID, VirtualObjectID, ObjectState&);
    void buildAtHeadForLackOfTagBits(BlockID, VirtualObjectID);
    bool isIgnored(Edge) const;
    void addVirtualReturn(VirtualReturn&&);
    void addEdgeNeedingCode(Edge);
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
    void collectJoins();
    void uniteFamilies(Vector<uint32_t>& parentOfCandidate);
    void createFamilies(std::span<const DescribedObject>, std::span<const uint32_t> rootOfCandidate);
    bool isJoinParameter(BlockID, uint32_t parameterIndex) const;
    void computeInputs();

    void solve();
    void giveUpOnNewDecisions();
    void invalidateResults();
    void count();
    void countBlock(BlockID);
    bool abandonUnprofitableCandidates();
    bool buildBeforeBranchesInsteadOfAbandoning();
    bool paysToSink(const Candidate&) const;
    void resetCounters();

    Vector<Edge, 2> collectCellEdges(VirtualObjectID, BlockID mergeBlock);
    Vector<BlockID> collectLiveRange(VirtualObjectID, BlockID mergeBlock);
    bool paysWithoutEdges(VirtualObjectID, std::span<const Edge>);
    void duplicate(IRBuilder&, std::span<const BlockID> region, std::span<const Edge> cellEdges);

    void addHeadParameters(IRBuilder&);
    bool needsHeadParameters(BlockID) const;
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
    Vector<VirtualFamily> m_families;
    KeyOrderTable m_orders;
    SinkingDecisions m_decisions;
    SinkingInputs m_inputs;
    Heads m_heads;
    Vector<VirtualReturn, 2> m_returns;
    Vector<Edge, 2> m_ignoredEdges;
    Vector<Edge, 2> m_edgesNeedingCode;
    const FreshResultShape* m_resultShape { nullptr };
    bool m_mustSolveAgain { false };
    std::optional<RewriteTables> m_tables;
    std::optional<RewriteCursor> m_cursor;
};

class SinkingProblem {
public:
    using State = SinkingState;

    explicit SinkingProblem(AllocationSinking& sinking)
        : m_sinking(sinking)
    {
    }

    State stateAtEntry();
    void execute(State&, const DataflowStep&);
    void executeTerminator(State&, const DataflowTerminatorStep&);
    void enterEdge(State&, Edge);
    bool merge(State& into, const State& from);

private:
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
