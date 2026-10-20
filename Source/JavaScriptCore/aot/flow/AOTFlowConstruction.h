/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowSolution.h"
#include "AOTFunctionAnalyses.h"
#include "AOTLayout.h"
#include "AOTSortedSet.h"
#include <compare>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class FlowConstructionRootKind : uint8_t {
    EntryParameter,
    Allocation,
    SuperConstruct,
};

struct FlowConstructionRoot {
    FlowConstructionRootKind kind { FlowConstructionRootKind::EntryParameter };
    ValueID definition;

    friend bool operator==(const FlowConstructionRoot&, const FlowConstructionRoot&) = default;
};

struct FlowConstructionName {
    PropertyKeyID key;
    FieldReference field;

    friend bool operator==(const FlowConstructionName&, const FlowConstructionName&) = default;
    friend auto operator<=>(const FlowConstructionName&, const FlowConstructionName&) = default;
};

using FlowConstructionNames = SortedSet<FlowConstructionName>;

enum class FlowConstructionEventKind : uint8_t {
    Leak,
    Store,
    Read,
    Call,
    Exit,
};

struct FlowConstructionEvent {
    FlowConstructionEventKind kind { FlowConstructionEventKind::Leak };
    bool isExact { false };
    bool storesSelfReference { false };
    ProgramPoint point;
    ValueID origin;
    ValueID operand;
    uint32_t operandIndex { 0 };
    std::optional<uint32_t> name;
    // The event is about the result of this call, which took the object. Whether the result holds the object is decided from the links of the call.
    ValueID holder;

    friend bool operator==(const FlowConstructionEvent&, const FlowConstructionEvent&) = default;
};

enum class FlowConstructionTracking : uint8_t {
    Unknown,
    Tracked,
};

struct FlowConstructionSummary {
    FlowConstructionRoot root;
    FlowConstructionTracking tracking { FlowConstructionTracking::Unknown };
    Vector<FlowConstructionName> names;
    Vector<FlowConstructionEvent> events;

    std::span<const FlowConstructionEvent> eventsAt(ProgramPoint) const LIFETIME_BOUND;
    uint32_t indexOfFirstEventAt(ProgramPoint) const;
    std::optional<uint32_t> indexOf(const FlowConstructionName&) const;
};

// A read of the object under construction finds no own property while no store under the name can have reached the object.
// The read waits for the first refinement, so that a read behind a store never looks at the prototypes. A later refinement may still move the start from the prototype to the object.
// The answer is one for every instance of the function: they share the code that the read becomes.
enum class FlowLookupStart : uint8_t {
    Undecided,
    Prototype,
    Object,
};

struct FlowReadOfCreatedObject {
    ValueID read;
    FlowLookupStart start { FlowLookupStart::Undecided };
    Vector<FlowStatementSite, 1> sites;
};

// What the definition stores stays out of the field until a refinement finds that somebody may see it: every constructor that runs the initializer may store under the name again first.
struct FlowFieldDefinition {
    ValueID definition;
    bool isObserved { false };
    Vector<FlowStatementSite, 1> sites;
};

struct FlowFunctionConstruction {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FlowFunctionConstruction);
    WTF_MAKE_NONCOPYABLE(FlowFunctionConstruction);

    FlowFunctionConstruction(const AnalysisContext&, FunctionID, const Function&);

    const FlowConstructionSummary* find(ValueID rootDefinition) const LIFETIME_BOUND;
    FlowReadOfCreatedObject* findReadOfCreatedObject(ValueID) LIFETIME_BOUND;
    FlowFieldDefinition* findFieldDefinition(ValueID) LIFETIME_BOUND;

    FunctionID function;
    FunctionAnalyses analyses;
    Vector<FlowConstructionSummary> summaries;
    // The reads by name whose receiver is the object that this activation creates, and no other. Ascending.
    Vector<FlowReadOfCreatedObject> readsOfCreatedObject;
    // The definitions of an initializer that store a primitive, on the object that it initializes and no other. Ascending.
    Vector<FlowFieldDefinition> fieldDefinitions;
    bool returnsOnlyConstructedObject { false };
};

bool allocatesConstructedObject(const Program&, const Function&, const Value&);
Vector<FlowConstructionRoot, 2> constructorRootsOf(const Program&, const Function&);
FlowConstructionName constructionNameOf(const Program&, const Value& access);
bool isReadOfCreatedObject(const Function&, const FlowConstructionRoot&, const FlowConstructionEvent&);
std::unique_ptr<FlowFunctionConstruction> buildFunctionConstruction(const Program&, BuildContext&, FunctionID);

enum class FlowConstructionStores : uint8_t {
    Unknown,
    OnlyInitialOrder,
};

struct FlowSolvedConstruction {
    ThisLeak leak { ThisLeak::Unknown };
    FlowConstructionStores stores { FlowConstructionStores::Unknown };
    Witness leakWitness { noWitness };
    FlowConstructionNames storedBeforeLeak;
    FlowConstructionNames storedAtExit;
    FlowConstructionNames readBeforeStore;
    FlowConstructionNames readNames;
    FlowConstructionNames possiblyStoredNames;
    FlowConstructionNames definedNames;
    // Somebody may see what a field definition has stored under these: a read, whoever gets the object at a leak or at the exit of the constructor.
    FlowConstructionNames observedDefinitions;
    bool listsEveryStoredName { false };
    // Under these the object may hold itself, or a closure or an environment that holds it.
    FlowConstructionNames selfReferenceNames;
    Vector<FlowConstructionName> initialOrder;
};

class FlowConstructionLinks {
    WTF_MAKE_NONCOPYABLE(FlowConstructionLinks);
public:
    FlowConstructionLinks(const FlowGraph&, const FlowRecords&);

    void forEachLinkAt(FlowInstanceID caller, ValueID site, NOESCAPE const Invocable<void(FlowCallLinkID, const FlowCallLink&)> auto& functor) const
    {
        for (uint32_t index : m_linksByCaller.of(caller.index())) {
            FlowCallLinkID id(index);
            const FlowCallLink& link = m_records.at(id);
            if (link.site == site)
                functor(id, link);
        }
    }

    void forEachOperandFlowOf(FlowCallLinkID link, NOESCAPE const Invocable<void(const FlowOperandFlow&)> auto& functor) const
    {
        for (uint32_t index : m_operandFlowsByLink.of(link.index()))
            functor(m_records.operandFlows()[index]);
    }

    void forEachBuiltinLinkAt(FlowInstanceID caller, ValueID site, NOESCAPE const Invocable<void(const FlowBuiltinCallLink&)> auto& functor) const
    {
        for (uint32_t index : m_builtinLinksByCaller.of(caller.index())) {
            const FlowBuiltinCallLink& link = m_records.builtinCallLinks()[index];
            if (link.site == site)
                functor(link);
        }
    }

    bool hasUnknownCallee(FlowInstanceID caller, ValueID site) const;

private:
    struct Groups {
        Vector<uint32_t> firstEntry;
        Vector<uint32_t> entries;

        std::span<const uint32_t> of(uint32_t group) const LIFETIME_BOUND
        {
            if (group + 1 >= firstEntry.size())
                return { };
            return entries.subspan(firstEntry[group], firstEntry[group + 1] - firstEntry[group]);
        }
    };

    static Groups groupEntries(uint32_t numberOfGroups, uint32_t numberOfEntries, NOESCAPE const ScopedLambda<uint32_t(uint32_t entry)>& groupOf);

    const FlowRecords& m_records;
    Groups m_linksByCaller;
    Groups m_operandFlowsByLink;
    Groups m_builtinLinksByCaller;
    Groups m_unknownCalleesByCaller;
};

struct FlowResolvedConstructionEvent {
    bool doesNotLeak { false };
    // No function that does not take the object, and no built-in, may run in place of the callees or of the store, and nothing that the object inherits stops the store.
    bool runsOneCallee { false };
    bool resultHoldsObject { false };
    bool holderIsEmpty { false };
    // Whatever the operation stores into the object, it stores under its own name or one of the callees stores it.
    bool storesOnlyThroughCallees { false };
    // The solver runs the read as a lookup that starts at the prototype of the object.
    bool startsAtPrototype { false };
    Witness witness { noWitness };
    Vector<uint32_t, 1> callees;
};

struct FlowConstructionNode {
    FlowInstanceID instance;
    FlowConstructionRoot root;
    FlowFunctionConstruction* function { nullptr };
    const FlowConstructionSummary* summary { nullptr };
    Vector<FlowResolvedConstructionEvent> resolvedEvents;
    Vector<uint32_t> callees;
    FlowSolvedConstruction solved;
};

struct FlowConstructionGraph {
    Vector<FlowConstructionNode> nodes;
    HashMap<std::tuple<FlowInstanceID, ValueID>, uint32_t> nodeIndexes;
    Vector<uint32_t> constructorNodes;
};

FlowConstructionGraph buildConstructionGraph(FlowSolver&, const FlowConstructionLinks&);
void solveStoredNames(FlowConstructionGraph&, uint32_t node);
void computeInitialOrder(FlowConstructionGraph&, uint32_t node);

struct FlowObjectConstruction {
    ThisLeak leak { ThisLeak::Unknown };
    FieldCreation fieldCreation { FieldCreation::Unknown };
    Witness leakWitness { noWitness };
    BitVector fieldsSetAtCreation;
    BitVector fieldsReadBeforeStore;
    Vector<FieldOrder> orders;
};

struct FlowSolvedConstructionEntry {
    FlowInstanceID instance;
    FlowConstructionRoot root;
    FlowSolvedConstruction solved;
    // The names at the front of the initial order that the parent constructor creates.
    size_t parentOrderLength { 0 };
};

struct FlowConstructionState {
    Vector<std::unique_ptr<FlowFunctionConstruction>> functions;
    Vector<FlowSolvedConstructionEntry> solvedConstructors;
    HashMap<FlowObjectID, uint32_t> objectIndexes;
    Vector<FlowObjectConstruction> objects;
    BitVector withdrawnFields;
    // Neither the constructor of a layout nor a function that gets the object of one before the object has leaked.
    BitVector functionsSeeingOnlyFinishedObjects;
};

FlowConstructionState createFlowConstructionState(FlowSolver&);
const FlowObjectConstruction* findObjectConstruction(const FlowConstructionState&, FlowObjectID);
FlowLookupStart lookupStartOf(const FlowConstructionState&, FunctionID, ValueID read);
// A statement that gets another answer than the object runs again when the answer changes.
FlowLookupStart lookupStartOf(FlowSolver&, FlowStatementSite, ValueID read);
// A statement that gets this answer runs again when the answer changes.
bool definesUnobservedValue(FlowSolver&, FlowStatementSite, ValueID definition);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
