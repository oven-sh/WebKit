/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTFlowCallOperations.h"
#include <memory>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Forward.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowOrderFingerprint {
    unsigned numberOfInstances { 0 };
    unsigned numberOfObjects { 0 };
    unsigned numberOfAllocatedObjects { 0 };
    unsigned numberOfEvaluatedModules { 0 };
    unsigned numberOfModuleLoads { 0 };
    unsigned numberOfUnseenCodeRecords { 0 };
    unsigned numberOfFunctionsPastWorkLimit { 0 };
    unsigned numberOfCallLinks { 0 };
    unsigned numberOfBuiltinCallLinks { 0 };
    unsigned numberOfUnknownCallees { 0 };
    unsigned numberOfStaticFieldAccesses { 0 };
    unsigned numberOfMemberAccesses { 0 };
    unsigned numberOfUncertainMemberStores { 0 };
    unsigned numberOfPrototypes { 0 };
    unsigned numberOfObjectsChangingIntegrity { 0 };
    unsigned numberOfObjectsInNativeCode { 0 };
    unsigned numberOfObjectsInUnknownJavaScript { 0 };

    friend bool operator==(const FlowOrderFingerprint&, const FlowOrderFingerprint&) = default;
};

// All that the order of execution is computed from beside the program. The value of a node is not offered: what the order has to know of one, the statement that reads the node records as it arrives.
class FlowOrderInputs {
public:
    virtual ~FlowOrderInputs() = default;

    const Program& program() const { return m_program; }
    BuildContext& build() const { return m_build; }

    virtual unsigned numberOfInstances() const = 0;
    virtual bool isHost(FlowInstanceID) const = 0;
    virtual bool isHost(FunctionID) const = 0;
    virtual FunctionID functionOf(FlowInstanceID) const = 0;
    virtual const FlowCallSiteKey& callSiteOf(FlowInstanceID) const LIFETIME_BOUND = 0;
    virtual bool isReached(FunctionID) const = 0;
    virtual std::span<const FlowModuleOperation> moduleOperationsOf(FunctionID) const LIFETIME_BOUND = 0;
    virtual const FlowObjectTable& objects() const LIFETIME_BOUND = 0;
    virtual bool isAllocated(FlowObjectID) const = 0;

    virtual const BitVector& evaluatedModules() const LIFETIME_BOUND = 0;
    virtual std::span<const FlowModuleLoadRecord> moduleLoads() const LIFETIME_BOUND = 0;
    virtual bool mayLoadAnyModule() const = 0;
    virtual bool isPastWorkLimit(FunctionID) const = 0;
    virtual std::span<const FlowCallLink> callLinks() const LIFETIME_BOUND = 0;
    virtual std::span<const FlowBuiltinCallLink> builtinCallLinks() const LIFETIME_BOUND = 0;
    virtual std::span<const FlowUnknownCallee> unknownCallees() const LIFETIME_BOUND = 0;
    virtual std::span<const FlowStaticFieldAccess> staticFieldAccesses() const LIFETIME_BOUND = 0;
    virtual std::span<const FlowMemberAccess> memberAccesses() const LIFETIME_BOUND = 0;
    virtual std::span<const FlowMemberOperation> uncertainMemberStores() const LIFETIME_BOUND = 0;
    virtual bool isPrototype(FlowObjectID) const = 0;
    virtual bool hasEscaped(FlowObjectID) const = 0;
    virtual bool hasClassObjectEscaped(FunctionID constructor) const = 0;
    // What is done to the unknown object is done to every object that has escaped.
    virtual bool mayChangeIntegrity(FlowObjectID) const = 0;

protected:
    FlowOrderInputs(const Program& program, BuildContext& build)
        : m_program(program)
        , m_build(build)
    {
    }

private:
    const Program& m_program;
    BuildContext& m_build;
};

// Each answer follows from lists and sets of the solver that only grow and that the fingerprint counts, so equal fingerprints mean equal orders.
class FlowSolverOrderInputs final : public FlowOrderInputs {
public:
    FlowSolverOrderInputs(const Program& program, const FlowGraph& graph, const FlowRecords& records, BuildContext& build)
        : FlowOrderInputs(program, build)
        , m_graph(graph)
        , m_records(records)
    {
    }

    FlowOrderFingerprint fingerprint() const;

    unsigned numberOfInstances() const final { return m_graph.numberOfInstances(); }
    bool isHost(FlowInstanceID instance) const final { return m_graph.isHost(instance); }
    bool isHost(FunctionID function) const final { return m_graph.isHost(function); }
    FunctionID functionOf(FlowInstanceID instance) const final { return m_graph.at(instance).function; }
    const FlowCallSiteKey& callSiteOf(FlowInstanceID instance) const LIFETIME_BOUND final { return *m_graph.at(instance).callSite; }
    bool isReached(FunctionID function) const final { return m_graph.isReached(function); }
    std::span<const FlowModuleOperation> moduleOperationsOf(FunctionID function) const LIFETIME_BOUND final { return m_graph.summaryOf(function).statements.moduleOperations.span(); }
    const FlowObjectTable& objects() const LIFETIME_BOUND final { return m_graph.objects(); }
    bool isAllocated(FlowObjectID object) const final { return m_records.eventsOf(object).contains(FlowObjectEvent::Allocated); }

    const BitVector& evaluatedModules() const LIFETIME_BOUND final { return m_records.evaluatedModules(); }
    std::span<const FlowModuleLoadRecord> moduleLoads() const LIFETIME_BOUND final { return m_records.moduleLoads(); }
    bool mayLoadAnyModule() const final { return m_records.mayRunUnknownJavaScript() || m_records.hasUnseenCode(UnseenCodeCause::LoadsModuleByName); }
    bool isPastWorkLimit(FunctionID function) const final { return m_records.functionsPastWorkLimit().get(function.index()); }
    std::span<const FlowCallLink> callLinks() const LIFETIME_BOUND final { return m_records.callLinks(); }
    std::span<const FlowBuiltinCallLink> builtinCallLinks() const LIFETIME_BOUND final { return m_records.builtinCallLinks(); }
    std::span<const FlowUnknownCallee> unknownCallees() const LIFETIME_BOUND final { return m_records.unknownCallees(); }
    std::span<const FlowStaticFieldAccess> staticFieldAccesses() const LIFETIME_BOUND final { return m_records.staticFieldAccesses(); }
    std::span<const FlowMemberAccess> memberAccesses() const LIFETIME_BOUND final { return m_records.memberAccesses(); }
    std::span<const FlowMemberOperation> uncertainMemberStores() const LIFETIME_BOUND final { return m_records.uncertainMemberStores(); }
    bool isPrototype(FlowObjectID object) const final { return m_records.possibleMutations(object).contains(ObjectMutation::BecomePrototype); }
    bool hasEscaped(FlowObjectID object) const final { return m_records.possibleMutations(object).containsAny(reachMutations); }
    bool hasClassObjectEscaped(FunctionID constructor) const final;
    bool mayChangeIntegrity(FlowObjectID) const final;

private:
    const FlowGraph& m_graph;
    const FlowRecords& m_records;
};

struct FlowModuleOrder {
    Vector<uint32_t> componentOf;
    BitVector modulesInCycles;
    Vector<BitVector> componentsCompletedBefore;

    FlowModuleOrdering orderingOf(ModuleID first, ModuleID second) const;
    bool runsAfterRequests(ModuleID) const;
    void dump(PrintStream&) const;
};

FlowModuleOrder computeModuleOrder(const FlowOrderInputs&);

enum class FlowScopeEntryState : uint8_t {
    Unknown,
    AfterPoint,
    Never,
};

// The point lies in the body of the owner, or of a function that runs only as a part of the owner's activation: its one mention in reached code is a direct call there.
struct FlowScopeEntry {
    FlowScopeEntryState state { FlowScopeEntryState::Unknown };
    FunctionID function;
    ProgramPoint point;

    friend bool operator==(const FlowScopeEntry&, const FlowScopeEntry&) = default;
};

struct FlowScopeTimeline {
    FunctionID owner;
    Vector<std::pair<FunctionID, FlowScopeEntry>> entries;
    Vector<std::pair<FunctionID, FlowScopeEntry>> entriesByUse;

    FlowScopeEntry entryOf(FunctionID) const;
};

struct FlowLocationOrder {
    InitializationState initialization { InitializationState::Unknown };
    StoreCount stores { StoreCount::Unknown };

    friend bool operator==(const FlowLocationOrder&, const FlowLocationOrder&) = default;
};

struct FlowMemberLocation {
    FlowObjectID holder;
    PropertyKeyID key;

    friend auto operator<=>(const FlowMemberLocation&, const FlowMemberLocation&) = default;
};

// Variables, then the fields of environments, then the members in ascending order.
struct FlowScopeLocationNumbering {
    uint32_t numberOfVariables { 0 };
    Vector<uint32_t> firstField;
    Vector<FlowMemberLocation> members;

    uint32_t firstMember() const { return firstField.isEmpty() ? 0 : firstField.last(); }
    uint32_t numberOfLocations() const { return firstMember() + members.size(); }
    // An environment has a location per activation of its owner.
    bool isOneLocation(uint32_t location) const { return location < numberOfVariables || location >= firstMember(); }
    std::optional<uint32_t> locationOf(VariableID) const;
    std::optional<uint32_t> locationOf(LayoutID environment, uint32_t fieldIndex) const;
    std::optional<uint32_t> locationOf(FlowObjectID holder, PropertyKeyID) const;
};

struct FlowScopeOrders {
    FlowScopeLocationNumbering numbering;
    Vector<FlowLocationOrder> orders;
    Vector<FunctionID> locationOwners;
    Vector<FunctionID> environmentOwners;
    Vector<FlowScopeTimeline> timelines;

    FlowLocationOrder orderOf(VariableID) const;
    FlowLocationOrder orderOf(LayoutID environment, uint32_t fieldIndex) const;
    FlowLocationOrder orderOf(FlowObjectID holder, PropertyKeyID) const;
    const FlowScopeTimeline* timelineOf(FunctionID owner) const LIFETIME_BOUND;
    void dump(PrintStream&) const;
};

struct FlowScopeAccesses;

struct FlowScopeAccessesDeleter {
    void operator()(FlowScopeAccesses*) const;
};

using FlowScopeAccessesByFunction = Vector<std::unique_ptr<FlowScopeAccesses, FlowScopeAccessesDeleter>>;

FlowScopeOrders computeScopeOrders(const FlowOrderInputs&, const FlowModuleOrder&, FlowScopeAccessesByFunction&);

struct FlowOrderState {
    FlowModuleOrder modules;
    FlowScopeOrders scopes;
    FlowScopeAccessesByFunction accessesByFunction;
    std::optional<FlowOrderFingerprint> computedFor;
    BitVector locationsReadBeforeInitialization;
};

void noteReadBeforeInitialization(FlowSolver&, VariableID);
void noteReadBeforeInitialization(FlowSolver&, LayoutID environment, uint32_t fieldIndex);

enum class FlowStaticFieldAccessKind : uint8_t {
    Read,
    Store,
};

void noteStaticFieldAccess(FlowSolver&, FlowStatementSite, VariableID, FlowStaticFieldAccessKind);
// Whether the order of execution is followed for the named properties of the object. An object of the program counts once it is the prototype of another,
// and of its properties only the lookups that start below it are listed.
enum class FlowMemberHolder : uint8_t { No, WhenPrototype, Always };
FlowMemberHolder memberHolderKindOf(const FlowObjectTable&, FlowObjectID);
bool followsMembersOf(const FlowOrderInputs&, FlowObjectID);
// A lookup that starts at the unknown object names that object: it is a read of the member of each holder that has escaped.
void noteMemberRead(FlowSolver&, FlowStatementSite, FlowObjectID holder, PropertyKeyID);
void noteMemberStore(FlowSolver&, FlowStatementSite, FlowObjectID holder, PropertyKeyID);
// The statement may complete without having written a member that it is listed as a store of. It says so whenever it finds that out, in whichever run.
void noteUncertainMemberStore(FlowSolver&, FlowStatementSite);
uint16_t valueBeforeInitialization(const Program&, const Variable&);
uint16_t valueBeforeInitialization(const Program&, const Field&);

FlowLocationOrder locationOrderOf(const FlowSolution&, VariableID);
FlowLocationOrder locationOrderOf(const FlowSolution&, LayoutID environment, uint32_t fieldIndex);
// The answer holds for a holder that is created at most once.
FlowLocationOrder locationOrderOf(const FlowSolution&, FlowObjectID holder, PropertyKeyID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
