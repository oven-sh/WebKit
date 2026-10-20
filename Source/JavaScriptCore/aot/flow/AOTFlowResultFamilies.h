/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowApplication.h"
#include "AOTFlowCallOperations.h"
#include "AOTFlowObjectOperations.h"
#include "AOTFlowPropertyOperations.h"
#include "AOTFlowTypeConversion.h"
#include "AOTThreads.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowObjectFacts {
    OptionSet<ObjectMutation> mutations;
    OptionSet<FlowObjectEvent> events;
    OptionSet<FlowObjectUse> uses;
    // A string or a symbol that nobody can name. An array index is no name, and the names stored through the unknown object are listed on it.
    bool mayGetUnlistedKey { false };
    bool mayGetIndexKey { false };
    bool mayGetKeysOfUnknownObject { false };
    bool mayGetIndexedAccessor { false };
    // An operation that flow saw stores on the object under an index or under a key that nobody can name. Unknown JavaScript that reaches the object is no such operation.
    bool isStoredUnderUnnamedKeys { false };
};

struct FlowBaseResult {
    BaseState state { BaseState::Unknown };
    LayoutID base;
    BuiltinClassID baseClass;
    Witness witness { noWitness };
};

// The CopyDataProperties right behind a NewObject without fields and without a prototype, with that object as its target: nothing has seen the object before the copy.
ValueID findCopyFillingLiteral(const Function&, ValueID literal);

class FlowHierarchy {
    WTF_MAKE_TZONE_ALLOCATED(FlowHierarchy);
    WTF_MAKE_NONCOPYABLE(FlowHierarchy);
public:
    FlowHierarchy(const FlowSolution&, Vector<FlowBaseResult>&& basesByLayout);

    const FlowBaseResult& baseOf(LayoutID layout) const LIFETIME_BOUND { return m_bases[layout.index()]; }
    bool hasProvenChain(LayoutID layout) const { return m_hasProvenChain.get(layout.index()); }
    const FlowBaseResult* findBuiltinBase(LayoutID) const LIFETIME_BOUND;
    bool hasOnlyOrdinaryObjects(AllocationClassID) const;
    // Empty object literals that the program stores to under an index or under a computed key nobody can name. No layout describes what they hold, so they are created as ordinary objects.
    // A literal that names a property is a record, whatever is spread into it or stored to it afterwards. A copy of the properties of another object is no store under a key
    // that the program computes: it is expanded by the layouts of that object, into a literal with a layout.
    // An untyped literal that begins with a copy of keys nobody can name holds what its source holds, which no layout describes either: findCopyFillingLiteral().
    bool createsDictionaries(AllocationClassID allocationClass) const { return m_dictionaryClasses.get(allocationClass.index()); }
    std::span<const LayoutID> layoutsBelow(LayoutID) const LIFETIME_BOUND;
    std::span<const FlowObjectID> objectsOf(LayoutID) const LIFETIME_BOUND;
    std::span<const LayoutID> layoutsFromBasesDown() const LIFETIME_BOUND { return m_layoutsFromBasesDown.span(); }
    std::optional<FieldReference> findDeclaredField(LayoutID, PropertyKeyID) const;

private:
    void computeChains();
    void computeLayoutsBelow();
    void computeDictionaryClasses();
    void computeObjects();

    const FlowSolution& m_solution;
    Vector<FlowBaseResult> m_bases;
    BitVector m_hasProvenChain;
    BitVector m_dictionaryClasses;
    Vector<LayoutID> m_layoutsFromBasesDown;
    Vector<uint32_t> m_firstLayoutBelow;
    Vector<LayoutID> m_layoutsBelow;
    Vector<uint32_t> m_firstObject;
    Vector<FlowObjectID> m_objects;
};

class FlowCallIndex {
    WTF_MAKE_NONCOPYABLE(FlowCallIndex);
public:
    explicit FlowCallIndex(const FlowSolution&);

    std::span<const FlowCallLinkID> linksFrom(FlowInstanceID) const LIFETIME_BOUND;
    std::span<const FlowCallLinkID> linksAt(FlowInstanceID, ValueID site) const LIFETIME_BOUND;
    std::span<const FlowCallLinkID> linksTo(FunctionID callee) const LIFETIME_BOUND;
    std::span<const FlowBuiltinCallLink> builtinLinksAt(FlowInstanceID, ValueID site) const LIFETIME_BOUND;
    bool hasUnknownCalleeAt(FlowInstanceID, ValueID site) const;

private:
    const FlowGraph& m_graph;
    const FlowRecords& m_records;
    Vector<uint32_t> m_firstLinkOfInstance;
    Vector<FlowCallLinkID> m_linksByCaller;
    Vector<uint32_t> m_firstLinkOfCallee;
    Vector<FlowCallLinkID> m_linksByCallee;
};

struct FlowObservedLifetimes {
    // Somebody can tell when one of these objects is collected.
    FlowObjectSet observed;
    // Those, and every object that refers to one of them in any number of steps. Whoever lets go of one can be found out.
    FlowObjectSet keepingObservedAlive;
    bool includesSymbols { false };
};

FlowObservedLifetimes computeObservedLifetimes(const FlowSolution&);
// The unknown object stands for a prototype that flow does not know. Built-in objects and namespace objects are left out: they live as long as the realm.
void forEachObjectReferredTo(const FlowSolution&, FlowObjectID, NOESCAPE const ScopedLambda<void(FlowObjectID)>&);

struct FlowFamilyInputs {
    const FlowSolution& solution;
    const FlowHierarchy& hierarchy;
    const FlowCallIndex& calls;
    std::span<const FlowObjectFacts> factsByObject;
    std::span<const ExecutionCount> executionsByFunction;
    const FlowObservedLifetimes& observedLifetimes;

    const Program& program() const { return solution.program; }
    const FlowGraph& graph() const { return solution.graph; }
    const FlowRecords& records() const { return solution.records; }
    const FlowObjectTable& objects() const { return solution.objects; }
    BuildContext& build() const { return solution.build; }
    const ParallelLoops& loops() const { return solution.build.parallelLoops(); }
    bool isEnabled(FlowResultKind kind, uint32_t subject) const { return solution.build.isEnabled(kind, subject); }
    const FlowObjectFacts& factsOf(FlowObjectID object) const LIFETIME_BOUND { return factsByObject[object.index()]; }
};

struct FlowFieldSubject {
    LayoutID layout;
    std::optional<uint32_t> declaredIndex;
    PropertyKeyID key;
};

FlowNodeID fieldNodeOn(const FlowGraph&, FlowObjectID, const FlowFieldSubject&);

void forEachFieldNode(const FlowFamilyInputs& inputs, const FlowFieldSubject& subject, NOESCAPE const Invocable<void(FlowObjectID, FlowNodeID)> auto& functor)
{
    for (LayoutID layout : inputs.hierarchy.layoutsBelow(subject.layout)) {
        for (FlowObjectID object : inputs.hierarchy.objectsOf(layout)) {
            if (FlowNodeID node = fieldNodeOn(inputs.graph(), object, subject))
                functor(object, node);
        }
    }
}

using FlowAppendedKeys = Vector<Vector<PropertyKeyID>>;

struct FlowLocationFacts {
    OptionSet<FlowLocationEvent> events;
    Witness firstStore { noWitness };
    Witness firstLoss { noWitness };
    FunctionID deferrableFunction;
    ClosureDeferral deferrableCapture { ClosureDeferral::Unknown };
    bool hasOneDeferrableFunction { true };
    bool hasNode { false };

    void add(const FlowLocationRecord&);
};

FlowLocationFacts collectLocationFacts(const FlowFamilyInputs&, const FlowFieldSubject&);
OptionSet<FlowLocationEvent> aliasedEventsOf(const FlowSolution&, FlowObjectID, PropertyKeyID);
bool isTypedLocation(const FlowGraph&, FlowNodeID);
bool holdsValueOutsideFieldType(const FlowFamilyInputs&, const FlowFieldSubject&, FlowNodeID location);
bool mayBeStoredWithoutLocation(const FlowFamilyInputs&, FlowObjectID, PropertyKeyID);
ReadState readStateOf(OptionSet<FlowLocationEvent>);
// A store that nothing reads still keeps its value alive.
bool mayKeepObservedObjectAlive(const FlowFamilyInputs&, FlowNodeID location);
bool isStoredByOperation(OptionSet<FlowLocationEvent>);

UnprovenCause causeOf(ObjectMutation);
UnprovenCause causeOf(FlowObjectEvent);
void keepFirstLoss(FlowSiteLoss& current, const FlowSiteLoss& candidate);

Vector<ExecutionCount> computeExecutionResults(const FlowSolution&, const FlowCallIndex&);
Vector<FlowBaseResult> computeBaseResults(const FlowSolution&);
Vector<FlowObjectFacts> computeObjectFacts(const FlowSolution&);
Vector<PropertyKeyID> computeCreatedSymbolResults(const FlowFamilyInputs&);

Vector<ClassFlow> computeClassResults(const FlowFamilyInputs&);
bool isCreatedOnce(const FlowFamilyInputs&, FlowObjectID);

struct FlowShapeResults {
    Vector<FlowShapeResult> shapes;
    Vector<AllocationClass> allocationClasses;
};

FlowShapeResults computeShapeResults(const FlowFamilyInputs&, std::span<const ClassFlow> classesByLayout, const FlowAppendedKeys&, const FlowBuiltinTables& builtinLocations);

// Groups the call-site instances of a function by what their allocations get, and gives the objects of each group an allocation class of their own.
FlowCallSiteVersions planCallSiteVersions(const FlowFamilyInputs&, std::span<const PropertyKeyID> symbolsCreatedOnce, FlowObjectTable&);
// Adds one layout to the program per set of names that the objects of some allocation classes of a layout get beside its fields, and moves those classes to it.
// The hierarchy of the inputs is out of date afterward.
void deriveLayouts(const FlowFamilyInputs&, const FlowCallSiteVersions&, std::span<const PropertyKeyID> symbolsCreatedOnce, Program&, FlowObjectTable&);
FlowAppendedKeys computeAppendedFieldKeys(const FlowFamilyInputs&, std::span<const PropertyKeyID> symbolsCreatedOnce);

struct FlowFieldResults {
    Vector<Vector<FieldFlow>> declared;
    Vector<Vector<FieldFlow>> appended;
};

FlowFieldResults computeFieldResults(const FlowFamilyInputs&, const FlowAppendedKeys&);
void computeDeferredClosureResults(const FlowFamilyInputs&, const FlowAppendedKeys&, FlowFieldResults&);

struct FlowReplacedDefinition {
    FunctionID initializer;
    ValueID definition;
    Value value;
};

struct FlowKeptDefinition {
    FlowNodeID location;
    FlowNodeID value;

    friend auto operator<=>(const FlowKeptDefinition&, const FlowKeptDefinition&) = default;
};

// What becomes of the field definitions whose values nobody sees. A field that holds an integer, a boolean, null or a string anyway gets such a value in place of the defined one.
// Any other field has room for the defined value, which no read of it gets.
struct FlowFieldDefinitionPlan {
    Vector<FlowReplacedDefinition> replaced;
    Vector<FlowKeptDefinition> kept;

    std::span<const FlowReplacedDefinition> replacedIn(FunctionID initializer) const LIFETIME_BOUND;
    std::span<const FlowKeptDefinition> keptIn(FlowNodeID location) const LIFETIME_BOUND;
};

FlowFieldDefinitionPlan planFieldDefinitions(const FlowFamilyInputs&);

struct FlowEvaluationResults {
    Vector<ModuleFlow> modules;
};

FlowEvaluationResults computeEvaluationResults(const FlowFamilyInputs&);
Vector<LocationFlow> computeVariableResults(const FlowFamilyInputs&, std::span<const ModuleFlow>);

Vector<LayoutFlow> computeLayoutResults(const FlowFamilyInputs&, std::span<const FlowShapeResult> shapesByAllocationClass);

// By layout: the orders in which the object literals that are completed by pieces create their keys, the most common first. Each is an expectation, never a proof.
// Where nothing creates a field behind an allocation, the fields that such a literal lists join the constructor orders of the layout instead.
using FlowLiteralKeyOrders = Vector<Vector<Vector<PropertyKeyID>>>;
FlowLiteralKeyOrders computeLiteralKeyOrders(const FlowFamilyInputs&, std::span<LayoutFlow>);

FlowContentsResults computeContentsResults(const FlowFamilyInputs&, std::span<const FlowShapeResult> shapesByAllocationClass, TypeTable&);

FlowVersionPlan computeVersionResults(const FlowFamilyInputs&, const FlowTypeConverter&, TypeTable&);

struct FlowBoundFunctionResults {
    FlowBoundFunctionPlan plan;
    Vector<FunctionID> functionByObject;
};

FlowBoundFunctionResults computeBoundFunctionResults(const FlowFamilyInputs&, std::span<const AllocationClass>, const FlowTypeConverter&, const FlowVersionPlan&, TypeTable&);

struct FlowReachabilityResults {
    Vector<FunctionFlow> functions;
    Vector<OptionSet<FunctionCapabilities>> provenCapabilities;
    ProgramInstances instances { ProgramInstances::Unknown };
};

FlowReachabilityResults computeReachabilityResults(const FlowFamilyInputs&);
FlowSiteLoss findCallerLoss(const FlowFamilyInputs&, const BitVector& functionsCalledByHost, FunctionID);

struct FlowTailCallResults {
    Vector<Vector<FlowTailCallUpdate>> sitesByVersion;
    Vector<ResultCheck> resultChecksByVersion;
};

FlowTailCallResults computeTailCallResults(const FlowFamilyInputs&, const FlowTypeConverter&, const FlowVersionPlan&, TypeTable&);

struct FlowTypeInputs {
    const FlowFamilyInputs& families;
    const FlowTypeConverter& converter;
    const FlowVersionPlan& versions;
    const FlowBoundFunctionPlan& boundFunctions;
    const FlowFieldDefinitionPlan& fieldDefinitions;
    const FlowAppendedKeys& appendedKeys;
    const FlowAccessSiteNumbering& accessSites;
    const FlowTailCallResults& tailCalls;
};

struct FlowTypeOutputs {
    FlowApplication& application;
    FlowFieldResults& fields;
    Vector<LocationFlow>& variables;
    Vector<FunctionFlow>& functions;
    FlowSiteTables& sites;
};

void computeTypeResults(const FlowTypeInputs&, const FlowTypeOutputs&, TypeTable&);

struct FlowPositionCounts {
    uint64_t proven { 0 };
    uint64_t unproven { 0 };
    uint64_t narrowed { 0 };
    uint64_t removedClaims { 0 };
    uint64_t remainingClaims { 0 };

    void add(const FlowPositionCounts&);
};

struct FlowBodyTask {
    const FlowTypeInputs& inputs;
    const FlowFunctionVersion& version;
    TypeArena& types;
    FlowBodyUpdate& update;
    FlowPositionCounts& counts;
};

Vector<FlowNodeID, 4> nodesOf(const FlowGraph&, const FlowFunctionVersion&, FlowLocalID);
Vector<FlowNodeID, 4> nodesOf(const FlowGraph&, const FlowFunctionVersion&, ValueID);

void computeValueProofs(const FlowBodyTask&);

struct FlowEscapeResults {
    Vector<Vector<OperandSummary>> parameterSummaries;
    Vector<Vector<OperandUses>> framePointerParameterUses;
    Vector<Vector<OperandUses>> storedValueUses;
    Vector<Vector<OperandUses>> storedFramePointerUses;
    Vector<FrameLocality> localityByAllocationClass;
};

FlowEscapeResults computeEscapeResults(const FlowFamilyInputs&, const FlowVersionPlan&);

void computeBuiltinLocationResults(const FlowFamilyInputs&, FlowBuiltinTables&);
void computeFeatureResults(const FlowFamilyInputs&, FlowBuiltinTables&, Vector<UnseenCodeRecord>&);
Vector<FlowSinkOperand> computeSinkCandidateResults(const FlowFamilyInputs&, const FlowAccessSiteNumbering&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
