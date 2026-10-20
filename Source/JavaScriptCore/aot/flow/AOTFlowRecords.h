/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowGraph.h"
#include <algorithm>
#include <array>
#include <compare>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename Flag>
constexpr OptionSet<Flag> everyFlagOf()
{
    OptionSet<Flag> flags;
    for (Flag flag : allMembersOf<Flag>)
        flags.add(flag);
    return flags;
}

inline constexpr OptionSet<ObjectMutation> allObjectMutations = everyFlagOf<ObjectMutation>();

inline constexpr OptionSet<ObjectMutation> reachMutations {
    ObjectMutation::ReachNativeCode,
    ObjectMutation::ReachUnknownJavaScript,
};

inline constexpr OptionSet<ObjectMutation> builtinBehaviorMutations {
    ObjectMutation::ReachUnknownJavaScript,
    ObjectMutation::AddProperty,
    ObjectMutation::RedefineProperty,
    ObjectMutation::ChangePrototype,
};

enum class FlowObjectUse : uint16_t {
    ReadByName = 1 << 0,
    ReadByUnknownKey = 1 << 1,
    Enumerated = 1 << 2,
    Called = 1 << 3,
    Constructed = 1 << 4,
    CalledRepeatedly = 1 << 5,
    TestedByInstanceOf = 1 << 6,
    Serialized = 1 << 7,
    ReachesTypedParse = 1 << 8,
    Awaited = 1 << 9,
    Iterated = 1 << 10,
};

inline constexpr OptionSet<FlowObjectUse> unnamedReadUses { FlowObjectUse::ReadByUnknownKey, FlowObjectUse::Enumerated };

// A lookup finds one property or none. Only an operation that lists the keys sees their order.
inline FlowObjectUse lookupUseOf(const FlowKeySet& keys)
{
    return keys.hasUnnamedKeys() ? FlowObjectUse::ReadByUnknownKey : FlowObjectUse::ReadByName;
}

enum class FlowObjectEvent : uint16_t {
    Allocated = 1 << 0,
    AllocatedByConstructor = 1 << 1,
    AllocatedRepeatedly = 1 << 2,
    StoredByUnknownKey = 1 << 3,
    StoredByComputedKey = 1 << 4,
    DeletedByUnknownKey = 1 << 5,
    OtherNewTarget = 1 << 6,
    ElementsMoved = 1 << 7,
    ContentsStored = 1 << 8,
    StoredUnderNumericName = 1 << 9,
    HoleMade = 1 << 10,
    StoredWithinLength = 1 << 11,
    CopiedUnderUnknownKeys = 1 << 12,
};

enum class FlowLocationEvent : uint16_t {
    WrittenByUnknownCode = 1 << 0,
    ReadByUnknownCode = 1 << 1,
    InitializedAtAllocation = 1 << 2,
    MissingAtAllocation = 1 << 3,
    StoredByOperation = 1 << 4,
    StoredByManyOperations = 1 << 5,
    ReadAsValue = 1 << 6,
    ReadAsCallee = 1 << 7,
    PresenceObserved = 1 << 8,
    StoredDeferrableClosure = 1 << 9,
    StoredOtherValue = 1 << 10,
    Deleted = 1 << 11,
    Redefined = 1 << 12,
    StoredOutsideType = 1 << 13,
    ReadBeforeInitialization = 1 << 14,
    StoredWithoutNamingLocation = 1 << 15,
};

// Whether the operation behind a store says which location it stores to: by layout and field, or by a key and the receivers that flow stamps on it.
enum class FlowStoreNaming : uint8_t { Unknown, NamesLocation };

inline constexpr OptionSet<FlowLocationEvent> repeatedUnnamedStoreEvents { FlowLocationEvent::StoredByManyOperations, FlowLocationEvent::StoredWithoutNamingLocation };
inline constexpr OptionSet<FlowLocationEvent> overwrittenLocationEvents = repeatedUnnamedStoreEvents | FlowLocationEvent::StoredOtherValue;
inline constexpr OptionSet<FlowLocationEvent> valueReadEvents { FlowLocationEvent::ReadAsValue, FlowLocationEvent::PresenceObserved };
// After one of these an accessor that an operation has defined is not known to be what every lookup of the name finds.
inline constexpr OptionSet<FlowLocationEvent> eventsChangingDefinedAccessor {
    FlowLocationEvent::StoredByManyOperations,
    FlowLocationEvent::WrittenByUnknownCode,
    FlowLocationEvent::Deleted,
    FlowLocationEvent::Redefined,
    FlowLocationEvent::ReadBeforeInitialization,
};
// After one of these a lookup may pass the location and go on to the prototypes.
inline constexpr OptionSet<FlowLocationEvent> emptyingLocationEvents { FlowLocationEvent::Deleted, FlowLocationEvent::Redefined, FlowLocationEvent::MissingAtAllocation };

constexpr FlowLocationEvent readEventOf(FlowReadUse use)
{
    switch (use) {
    case FlowReadUse::Value:
        return FlowLocationEvent::ReadAsValue;
    case FlowReadUse::CalleeOnly:
        return FlowLocationEvent::ReadAsCallee;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct FlowLocationRecord {
    OptionSet<FlowLocationEvent> events;
    Witness firstStore { noWitness };
    Witness firstLoss { noWitness };
    FunctionID deferrableFunction;
    ClosureDeferral deferrableCapture { ClosureDeferral::Unknown };
    // StoredOtherValue also stands for what a store under an unknown key may have put here. This is set by an operation that names the location.
    bool isGivenOtherValueByName { false };
};

enum class FlowCallLinkKind : uint8_t {
    Call,
    Construct,
    Accessor,
    BuiltinCallback,
    Protocol,
    Host,
};

struct FlowCallLink {
    FlowInstanceID callerInstance;
    ValueID site;
    FlowInstanceID calleeInstance;
    FunctionID caller;
    FunctionID callee;
    FlowCallLinkKind kind { FlowCallLinkKind::Call };
    BuiltinCallbackTimes times { BuiltinCallbackTimes::Once };
    bool isInCycle { false };
    FlowNodeID result;
    FlowNodeID thrown;
};

struct FlowOperandFlow {
    FlowNodeID from;
    FlowNodeID parameter;
    FlowCallLinkID link;
};

// A built-in reads parts of the objects in the container into the reader, so what becomes of the reader is a use of the contents of the container.
struct FlowContentsRead {
    FlowNodeID container;
    FlowNodeID reader;

    friend bool operator==(const FlowContentsRead&, const FlowContentsRead&) = default;
};

struct FlowBuiltinCallLink {
    FlowInstanceID callerInstance;
    ValueID site;
    BuiltinID builtin;
    BuiltinOverloadID overload;
    BuiltinCallMode mode { BuiltinCallMode::Call };
    FlowCallLinkKind kind { FlowCallLinkKind::Call };

    friend auto operator<=>(const FlowBuiltinCallLink&, const FlowBuiltinCallLink&) = default;
};

struct FlowUnknownCallee {
    FlowInstanceID callerInstance;
    ValueID site;
    FlowCallLinkKind kind { FlowCallLinkKind::Call };

    friend auto operator<=>(const FlowUnknownCallee&, const FlowUnknownCallee&) = default;
};

struct FlowClassEvaluation {
    LayoutID layout;
    FlowInstanceID instance;
    ValueID origin;
    FlowObjectID classObject;
    FlowObjectID prototypeObject;
    bool isInCycle { false };
};

struct FlowMemberDefinition {
    LayoutID layout;
    ClassSide side { ClassSide::Prototype };
    MethodKind kind { MethodKind::Method };
    uint32_t index { 0 };
    PropertyKeyID constantKey;
    PropertyKeyID key;
    FunctionID function;
    FlowObjectID holder;
    Witness witness { noWitness };
};

struct FlowModuleLoadRecord {
    FlowInstanceID caller;
    ValueID origin;
    ModuleID module;

    friend bool operator==(const FlowModuleLoadRecord&, const FlowModuleLoadRecord&) = default;
};

// An operation reaches the variable of a static field through the class object, so it does not name the variable. The function is null for an access of the host.
struct FlowStaticFieldAccess {
    VariableID variable;
    FunctionID function;
    ValueID origin;
    bool isStore { false };

    friend bool operator==(const FlowStaticFieldAccess&, const FlowStaticFieldAccess&) = default;
};

// An operation as the order of execution names it. The function is null for the host. The instance is named where it stands for the calls of one site.
struct FlowMemberOperation {
    FunctionID function;
    ValueID origin;
    FlowInstanceID callSiteInstance;

    friend bool operator==(const FlowMemberOperation&, const FlowMemberOperation&) = default;
};

// A named property of an object whose members the order of execution may follow. A lookup that starts at the unknown object names that object, which stands for the holders that have escaped.
struct FlowMemberAccess {
    FlowObjectID holder;
    PropertyKeyID key;
    FlowMemberOperation operation;
    bool isStore { false };

    friend bool operator==(const FlowMemberAccess&, const FlowMemberAccess&) = default;
};

struct FlowBuiltinUse {
    OptionSet<BuiltinUse> uses;
    Witness firstUse { noWitness };
    Witness firstWrite { noWitness };
};

struct FlowBuiltinOperandValue {
    FlowInstanceID callerInstance;
    ValueID site;
    BuiltinOverloadID overload;
    BuiltinOperand operand { BuiltinOperand::receiver() };
    OptionSet<BuiltinOperandRole> roles;
    FlowNodeID node;
};

// The new array has a hole wherever an array in the node has one.
struct FlowHoleCopy {
    FlowNodeID source;
    FlowObjectID result;
};

using UnknownObjectOriginID = ID<struct FlowUnknownObjectOriginIDTag>;

struct FlowUnknownObjectOrigin {
    FlowNodeID firstNode;
    Witness witness;
};

struct FlowUnknownObjectEntry {
    FlowNodeID node;
    UnknownObjectOriginID origin;
};

struct FlowUnknownObjectDerivation {
    FlowNodeID subject;
    FlowNodeID result;
};

class FlowRecords {
    WTF_MAKE_NONCOPYABLE(FlowRecords);
public:
    FlowRecords() = default;
    FlowRecords(FlowRecords&&) = default;
    FlowRecords& operator=(FlowRecords&&) = default;

    OptionSet<ObjectMutation> possibleMutations(FlowObjectID) const;
    unsigned numberOfObjectsWith(ObjectMutation) const;
    unsigned numberOfAllocatedObjects() const { return m_objects.numberOfAllocatedObjects; }
    OptionSet<FlowObjectUse> usesOf(FlowObjectID) const;
    OptionSet<FlowObjectEvent> eventsOf(FlowObjectID) const;
    Witness witnessOf(FlowObjectID, ObjectMutation) const;
    // Why unseen code holds the object.
    Witness witnessOfReach(FlowObjectID) const;
    Witness witnessOf(FlowObjectID, FlowObjectEvent) const;
    std::span<const Vector<uint32_t>> initialFieldLists(FlowObjectID) const LIFETIME_BOUND;

    OperandUses directUsesOf(FlowNodeID) const;
    const FlowLocationRecord& recordOf(FlowNodeID location) const LIFETIME_BOUND;
    Witness unknownObjectWitness(FlowNodeID) const;
    // After finish(). An origin is an operation that lets the unknown object in and finds it through none of its own operands. An entry is a node that an origin puts the object into,
    // and a derivation leads from a node that holds the object to a node that gets it because an operation looks through the first. With the copies of the graph they are every way the object takes.
    std::span<const FlowUnknownObjectOrigin> unknownObjectOrigins() const LIFETIME_BOUND { return m_nodes.unknownObjectOrigins.span(); }
    std::span<const FlowUnknownObjectEntry> unknownObjectEntries() const LIFETIME_BOUND { return m_nodes.unknownObjectEntries.span(); }
    std::span<const FlowUnknownObjectDerivation> unknownObjectDerivations() const LIFETIME_BOUND { return m_nodes.unknownObjectDerivations.span(); }

    std::span<const FlowCallLink> callLinks() const LIFETIME_BOUND { return m_calls.links.span(); }
    const FlowCallLink& at(FlowCallLinkID link) const LIFETIME_BOUND { return m_calls.links[link.index()]; }
    std::span<const FlowOperandFlow> operandFlows() const LIFETIME_BOUND { return m_calls.operandFlows.span(); }
    std::span<const FlowContentsRead> contentsReads() const LIFETIME_BOUND { return m_calls.contentsReads.span(); }
    // Holds what the calls that enter the function for unknown operands pass beside the unknown object.
    FlowNodeID predictedEntryParameter(FunctionID function, uint32_t entryParameter) const { return m_calls.predictedEntryParameters.get(numberOfEntryParameter(function, entryParameter)); }
    std::span<const FlowBuiltinCallLink> builtinCallLinks() const LIFETIME_BOUND { return m_calls.builtinLinks.span(); }
    std::span<const FlowUnknownCallee> unknownCallees() const LIFETIME_BOUND { return m_calls.unknownCallees.span(); }
    std::span<const FlowBuiltinOperandValue> builtinOperandValues() const LIFETIME_BOUND { return m_calls.builtinOperandValues.span(); }
    std::span<const FlowHoleCopy> holeCopies() const LIFETIME_BOUND { return m_calls.holeCopies.span(); }

    std::span<const FlowClassEvaluation> classEvaluations() const LIFETIME_BOUND { return m_program.classEvaluations.span(); }
    std::span<const FlowMemberDefinition> memberDefinitions() const LIFETIME_BOUND { return m_program.memberDefinitions.span(); }
    const BitVector& evaluatedModules() const LIFETIME_BOUND { return m_program.evaluatedModules; }
    // The modules that a built-in requires, before it returns, under a name the solver knows. No operation of the caller names them.
    std::span<const FlowModuleLoadRecord> moduleLoads() const LIFETIME_BOUND { return m_program.moduleLoads.span(); }
    // After finish(): ascending by function and value, each site once.
    std::span<const Witness> loadsByUnknownName() const LIFETIME_BOUND { return m_program.loadsByUnknownName.span(); }
    std::span<const FlowStaticFieldAccess> staticFieldAccesses() const LIFETIME_BOUND { return m_program.staticFieldAccesses.span(); }
    std::span<const FlowMemberAccess> memberAccesses() const LIFETIME_BOUND { return m_program.memberAccesses.span(); }
    // The operations that may complete without having written a member they are listed as a store of.
    std::span<const FlowMemberOperation> uncertainMemberStores() const LIFETIME_BOUND { return m_program.uncertainMemberStores.span(); }
    const BitVector& createdNamespaces() const LIFETIME_BOUND { return m_program.createdNamespaces; }
    const BitVector& functionsPastWorkLimit() const LIFETIME_BOUND { return m_program.functionsPastWorkLimit; }
    const FlowBuiltinUse& builtinUse(BuiltinID) const LIFETIME_BOUND;
    // Stores and definitions under a name that is no member of its locked holder. After finish(): ascending by holder and key.
    std::span<const BuiltinLocationFlow> lockedBuiltinWrites() const LIFETIME_BOUND { return m_program.lockedBuiltinWrites.span(); }
    std::span<const UnseenCodeRecord> unseenCode() const LIFETIME_BOUND { return m_program.unseenCode.span(); }
    bool hasUnseenCode(UnseenCodeCause) const;
    // Code that the build has not seen can run: text that is evaluated, machine code that is loaded. It reaches the global object, what seen code hands to it and what it makes itself.
    bool unseenCodeCanRun() const { return m_program.unseenCodeCanRun; }
    // Such code also reaches whatever has reached native code, and every module by its name.
    bool mayRunUnknownJavaScript() const { return m_program.mayRunUnknownJavaScript; }

    OptionSet<ObjectMutation> addMutations(FlowObjectID, OptionSet<ObjectMutation>, const Witness&);
    bool addUse(FlowObjectID, FlowObjectUse);
    bool addEvent(FlowObjectID, FlowObjectEvent, const Witness&);
    void addInitialFieldList(FlowObjectID, std::span<const uint32_t>);
    void addDirectUses(FlowNodeID, OperandUses);
    FlowLocationRecord& ensureLocationRecord(FlowNodeID) LIFETIME_BOUND;
    void noteUnknownObjectOrigin(FlowNodeID, const Witness&);

    FlowCallLinkID addCallLink(const FlowCallLink&);
    void addOperandFlow(const FlowOperandFlow& flow) { m_calls.operandFlows.append(flow); }
    void setPredictedEntryParameter(FunctionID function, uint32_t entryParameter, FlowNodeID node) { m_calls.predictedEntryParameters.set(numberOfEntryParameter(function, entryParameter), node); }
    void addContentsRead(FlowNodeID container, FlowNodeID reader);
    void addBuiltinCallLink(const FlowBuiltinCallLink& link) { m_calls.builtinLinks.append(link); }
    void addUnknownCallee(const FlowUnknownCallee& callee) { m_calls.unknownCallees.append(callee); }
    void addBuiltinOperandValue(const FlowBuiltinOperandValue& value) { m_calls.builtinOperandValues.append(value); }
    void addHoleCopy(FlowNodeID source, FlowObjectID result);

    void addClassEvaluation(const FlowClassEvaluation& evaluation) { m_program.classEvaluations.append(evaluation); }
    void addMemberDefinition(const FlowMemberDefinition& definition) { m_program.memberDefinitions.append(definition); }
    bool addEvaluatedModule(ModuleID);
    void addModuleLoad(const FlowModuleLoadRecord&);
    void addLoadByUnknownName(const Witness&);
    void addStaticFieldAccess(const FlowStaticFieldAccess&);
    void addMemberAccess(const FlowMemberAccess&);
    void addUncertainMemberStore(const FlowMemberOperation&);
    bool addCreatedNamespace(ModuleID);
    bool addFunctionPastWorkLimit(FunctionID);
    OptionSet<BuiltinUse> addBuiltinUse(BuiltinID, OptionSet<BuiltinUse>, const Witness&);
    void addLockedBuiltinWrite(BuiltinID holder, PropertyKeyID, const Witness&);
    bool addUnseenCode(const UnseenCodeRecord&);
    void setUnseenCodeCanRun() { m_program.unseenCodeCanRun = true; }
    void setMayRunUnknownJavaScript() { m_program.mayRunUnknownJavaScript = true; }

    void finish(const FlowGraph&);

    FlowRecords copy() const;
    // Names each thing that these records hold and the earlier ones lack. A witness is no part of what a record says.
    void forEachAdditionSince(const FlowRecords& earlier, NOESCAPE const ScopedLambda<void(const String&)>&) const;
    // The fixpoint check takes one fact back and states it again, which repeats what the arrival of the fact set off.
    void retractEvaluatedModule(ModuleID);
    FlowBuiltinUse retractBuiltinUse(BuiltinID);
    void retractEvent(FlowObjectID, FlowObjectEvent);
    void retractUnknownJavaScript()
    {
        m_program.unseenCodeCanRun = false;
        m_program.mayRunUnknownJavaScript = false;
    }

private:
    using LocationRecordID = ID<struct FlowLocationRecordIDTag>;

    struct ObjectRecords {
        Vector<OptionSet<ObjectMutation>> mutations;
        std::array<unsigned, numberOfEnumerators<ObjectMutation>> numberOfObjectsWithMutation { };
        Vector<OptionSet<FlowObjectUse>> uses;
        Vector<OptionSet<FlowObjectEvent>> events;
        unsigned numberOfAllocatedObjects { 0 };
        HashMap<std::pair<FlowObjectID, uint16_t>, Witness> mutationWitnesses;
        HashMap<std::pair<FlowObjectID, uint16_t>, Witness> eventWitnesses;
        HashMap<FlowObjectID, Vector<Vector<uint32_t>>> initialFieldLists;
    };

    struct NodeRecords {
        Vector<OperandUses> directUses;
        Vector<LocationRecordID> locationRecordIDs;
        Vector<FlowLocationRecord> locationRecords;
        FlowLocationRecord emptyLocationRecord;
        Vector<FlowUnknownObjectOrigin> unknownObjectOrigins;
        HashedIndex unknownObjectOriginIndex;
        Vector<UnknownObjectOriginID> unknownObjectLabels;
        Vector<FlowUnknownObjectEntry> unknownObjectEntries;
        Vector<FlowUnknownObjectDerivation> unknownObjectDerivations;
    };

    struct CallRecords {
        Vector<FlowCallLink> links;
        Vector<FlowOperandFlow> operandFlows;
        Vector<FlowContentsRead> contentsReads;
        HashedIndex contentsReadIndex;
        Vector<FlowBuiltinCallLink> builtinLinks;
        Vector<FlowUnknownCallee> unknownCallees;
        Vector<FlowBuiltinOperandValue> builtinOperandValues;
        Vector<FlowHoleCopy> holeCopies;
        HashSet<std::pair<FlowNodeID, FlowObjectID>> recordedHoleCopies;
        HashMap<uint64_t, FlowNodeID> predictedEntryParameters;
    };

    static uint64_t numberOfEntryParameter(FunctionID function, uint32_t entryParameter) { return (static_cast<uint64_t>(function.index()) + 1) << 32 | entryParameter; }

    struct ProgramRecords {
        Vector<FlowClassEvaluation> classEvaluations;
        Vector<FlowMemberDefinition> memberDefinitions;
        BitVector evaluatedModules;
        Vector<FlowModuleLoadRecord> moduleLoads;
        Vector<Witness> loadsByUnknownName;
        Vector<FlowStaticFieldAccess> staticFieldAccesses;
        HashedIndex staticFieldAccessIndex;
        Vector<FlowMemberAccess> memberAccesses;
        HashedIndex memberAccessIndex;
        Vector<FlowMemberOperation> uncertainMemberStores;
        HashedIndex uncertainMemberStoreIndex;
        BitVector createdNamespaces;
        BitVector functionsPastWorkLimit;
        Vector<FlowBuiltinUse> builtinUses;
        FlowBuiltinUse unusedBuiltin;
        Vector<BuiltinLocationFlow> lockedBuiltinWrites;
        Vector<UnseenCodeRecord> unseenCode;
        bool unseenCodeCanRun { false };
        bool mayRunUnknownJavaScript { false };
    };

    void computeUnknownObjectLabels(const FlowGraph&);

    ObjectRecords m_objects;
    NodeRecords m_nodes;
    CallRecords m_calls;
    ProgramRecords m_program;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
