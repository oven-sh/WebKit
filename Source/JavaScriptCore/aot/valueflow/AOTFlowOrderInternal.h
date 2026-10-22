/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowOrder.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGrouping.h"
#include <algorithm>
#include <compare>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

using FlowScopeLocation = uint32_t;

struct FlowScopeLocationAccess {
    FlowScopeLocation location;
    FunctionID function;
    ValueID value;

    friend auto operator<=>(const FlowScopeLocationAccess&, const FlowScopeLocationAccess&) = default;
};

struct FlowEnvironmentAllocation {
    LayoutID layout;
    FunctionID function;
    ValueID value;

    friend auto operator<=>(const FlowEnvironmentAllocation&, const FlowEnvironmentAllocation&) = default;
};

struct FlowModuleValueRead {
    ModuleID module;
    FunctionID function;
    ValueID value;

    friend auto operator<=>(const FlowModuleValueRead&, const FlowModuleValueRead&) = default;
};

struct FlowFunctionMention {
    FunctionID function;
    FunctionReferenceKind kind;
    ValueID value;
};

struct FlowCallSite {
    FunctionID caller;
    ValueID call;

    friend bool operator==(const FlowCallSite&, const FlowCallSite&) = default;
};

struct FlowScopeCall {
    FunctionID caller;
    FunctionID callee;
    ValueID site;
    // The variable that the callee of the operation comes from, through reads of properties. See variableBehindSubjectOf().
    VariableID subject;

    friend auto operator<=>(const FlowScopeCall&, const FlowScopeCall&) = default;
};

// A call or a property access throws on a callee or a receiver that is undefined or absent. Where that value comes, through reads of properties, from a variable
// that starts so, the operation completes only after a store to the variable.
VariableID variableBehindSubjectOf(const Function&, ValueID operation);

struct FlowScopeCalls {
    Vector<uint32_t> numberOfMentions;
    Vector<FlowCallSite> soleCallSites;
    Vector<FlowScopeCall> byCaller;
    Vector<FlowScopeCall> byCallee;
    BitVector functionsEnteredByHost;
};

struct FlowScopeAccesses {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FlowScopeAccesses);

    Vector<FlowScopeLocationAccess> reads;
    Vector<FlowScopeLocationAccess> stores;
    Vector<FlowEnvironmentAllocation> allocations;
    Vector<FlowModuleValueRead> moduleValueReads;
    Vector<FlowFunctionMention> references;
    Vector<FlowScopeLocation> opaqueVariables;
    Vector<LayoutID> opaqueLayouts;
    Vector<EvalScopeID> evalScopes;
};

struct FlowExportedVariable {
    VariableID variable;
    ModuleID exporter;

    friend auto operator<=>(const FlowExportedVariable&, const FlowExportedVariable&) = default;
};

struct FlowScopeOwner {
    FunctionID function;
    ModuleID module;
    Vector<VariableID> variables;
    Vector<LayoutID> environments;
    Vector<FlowScopeLocation> members;
    Vector<FlowExportedVariable> exports;
    Vector<ModuleID> exporters;
};

struct FlowScopeIndex {
    const FlowOrderInputs& inputs;
    const FlowModuleOrder& moduleOrder;
    const FlowScopeAccessesByFunction& bodies;
    FlowScopeLocationNumbering numbering;
    Vector<FlowScopeLocationAccess> reads;
    Vector<FlowScopeLocationAccess> stores;
    Vector<FlowScopeLocationAccess> accessesThroughClassObjects;
    Vector<FlowScopeLocationAccess> storesThroughClassObjects;
    Vector<FlowEnvironmentAllocation> allocations;
    Vector<FlowModuleValueRead> moduleValueReads;
    // The order of the first location holds when the second one is stored before every read.
    Vector<std::pair<FlowScopeLocation, FlowScopeLocation>> assumedStores;
    BitVector opaqueLocations;
    BitVector syntheticVariables;
    BitVector staticFieldsOfEscapedClasses;
    // No mention in the body of an owner says when such a function is first entered: the host, or a call that a pass creates later, enters it or a function that leads to it by mentions.
    BitVector functionsEnteredWithoutMention;
    BitVector modulesOfFunctionsEnteredWithoutMention;
    // The host reads the value of such a module at a time nobody knows.
    BitVector modulesReadByHost;
    Vector<FunctionID> locationOwners;
    Vector<FunctionID> environmentOwners;
    FlowScopeCalls calls;
    Vector<FlowScopeOwner> owners;
    BitVector modulesLoadedByName;

    const Program& program() const { return inputs.program(); }
};

inline std::span<const FlowScopeLocationAccess> accessesAt(std::span<const FlowScopeLocationAccess> sorted, FlowScopeLocation location)
{
    return recordsWith(sorted, location, &FlowScopeLocationAccess::location);
}

inline std::span<const FlowEnvironmentAllocation> allocationsOf(const FlowScopeIndex& index, LayoutID layout)
{
    return recordsWith(index.allocations.span(), layout, &FlowEnvironmentAllocation::layout);
}

inline std::span<const FlowModuleValueRead> valueReadsOf(const FlowScopeIndex& index, ModuleID module)
{
    return recordsWith(index.moduleValueReads.span(), module, &FlowModuleValueRead::module);
}

inline constexpr FlowScopeEntry unknownScopeEntry { FlowScopeEntryState::Unknown, { }, { } };
inline constexpr FlowScopeEntry neverScopeEntry { FlowScopeEntryState::Never, { }, { } };

constexpr FlowScopeEntry scopeEntryAfter(FunctionID function, ProgramPoint point) { return { FlowScopeEntryState::AfterPoint, function, point }; }

FlowScopeLocationNumbering createScopeLocationNumbering(const Program&);
void numberMemberLocations(const FlowOrderInputs&, FlowScopeLocationNumbering&);
void collectScopeAccesses(const Program&, const FlowScopeLocationNumbering&, FunctionID, FlowScopeAccesses&);
void ensureScopeAccesses(const FlowOrderInputs&, const FlowScopeLocationNumbering&, FlowScopeAccessesByFunction&);
FlowScopeIndex createScopeIndex(const FlowOrderInputs&, const FlowModuleOrder&, const FlowScopeAccessesByFunction&, FlowScopeLocationNumbering&&);
void computeSyntheticExportOrders(const FlowScopeIndex&, std::span<FlowLocationOrder>);

// The body of the owner, or of a function whose one mention in reached code is a direct call in a frame. Such a function runs only while that call runs.
struct FlowScopeFrame {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FlowScopeFrame);

    FlowScopeFrame(const FlowScopeIndex&, FunctionID, FlowScopeFrame* caller, ValueID call);

    FunctionID function;
    const Function& body;
    FunctionAnalyses analyses;
    FlowScopeFrame* caller;
    ValueID call;
    unsigned depth;
    std::optional<FlowScopeEntry> exit;
};

class FlowScopeOwnerAnalysis {
    WTF_MAKE_NONCOPYABLE(FlowScopeOwnerAnalysis);
public:
    FlowScopeOwnerAnalysis(const FlowScopeIndex&, const FlowScopeOwner&);

    void computeEntries();
    void computeOrders(std::span<FlowLocationOrder>);
    Vector<FlowReadOfLocation> takeReadsAfterInitialization() { return std::exchange(m_readsAfterInitialization, { }); }

private:
    enum class StoredClosureEntry : uint8_t { AtStore, AtRead };

    struct FollowedValue {
        FlowScopeFrame* frame;
        ValueID value;
    };

    struct StoredFunction {
        FlowScopeLocation location;
        FunctionID function;
        FlowScopeEntry store;
    };

    // Whoever reads a location may take out each function that is stored there, and whoever reads the value of a module each function in a variable that the module exports.
    // The functions are a run of m_functionsTakenOut.
    struct StoredFunctionReader {
        FunctionID reader;
        uint32_t first;
        uint32_t count;
    };

    using InitializingStores = Vector<FlowScopeLocationAccess, 4>;

    void createFrames();
    void appendFrame(FunctionID, FlowScopeFrame* caller, ValueID call);
    FlowScopeFrame& ownerFrame() { return *m_frames[0]; }
    FlowScopeFrame* findFrame(FunctionID);
    FlowScopeFrame& frameOf(const FlowScopeEntry&);

    ProgramPoint meet(FlowScopeFrame&, ProgramPoint, ProgramPoint);
    bool isPassedBefore(const FlowScopeEntry& earlier, const FlowScopeEntry& later);
    FlowScopeEntry meet(const FlowScopeEntry&, const FlowScopeEntry&);
    FlowScopeEntry laterOf(const FlowScopeEntry&, const FlowScopeEntry&);
    FlowScopeEntry entryInCaller(const FlowScopeEntry&);
    FlowScopeEntry& ensureEntry(FunctionID);
    void lower(FunctionID, FlowScopeEntry);
    std::optional<ProgramPoint> pointOf(FlowScopeFrame&, ValueID);
    FlowScopeEntry entryAt(FlowScopeFrame&, ValueID);
    FlowScopeEntry entryOf(FunctionID);
    FlowScopeEntry entryByUseOf(FunctionID);
    FlowScopeEntry entryByCallsOf(FunctionID);
    FlowScopeEntry entryByStoresTo(VariableID);
    FlowScopeEntry computeEntryByStoresTo(VariableID);
    FlowScopeEntry entryBySubjectOf(FunctionID, ValueID operation);
    FlowScopeEntry entryOf(const FlowScopeLocationAccess&);

    FlowScopeEntry computeExit(FlowScopeFrame&);
    FlowScopeEntry exitOf(FlowScopeFrame&);
    std::span<const FlowExportedVariable> exportsOf(FlowScopeLocation) const;
    bool isExported(FlowScopeLocation) const;
    void forEachReader(FlowScopeLocation, NOESCAPE const Invocable<void(const FlowScopeLocationAccess&)> auto&) const;

    void collectPerIterationAllocations();
    StoredClosureEntry storedClosureEntryOf(FlowScopeFrame&, ValueID closure);
    void followClosure(FlowScopeFrame&, ValueID, FunctionID);
    std::optional<ValueID> forwardedValue(const FlowScopeFrame&, const Use&) const;
    std::optional<FollowedValue> parameterReceiving(FlowScopeFrame&, const Use&);
    bool isReceiverOfDefinition(const FlowScopeFrame&, const Use&) const;
    void recordClassEvaluation(FlowScopeFrame&, ValueID closure, FunctionID constructor);
    void recordDefinition(const FlowScopeFrame&, ValueID definition, FunctionID receiver);
    std::optional<FlowScopeLocation> storedLocation(const FlowScopeFrame&, const Use&) const;
    void recordClosureUse(FlowScopeFrame&, const Use&, FunctionID, StoredClosureEntry);
    void recordStoredFunction(FlowScopeLocation, FunctionID, const FlowScopeEntry& store);
    void recordOwnReferences();
    void recordInitialFunctions();
    void recordOutsideReader(FunctionID);
    void recordOutsideReaders();
    void recordReadsOfStoredFunctions();
    void runFixpoint();
    void refineEntriesByCalls();

    bool completesBeforeReturn(FlowScopeFrame&, ValueID);
    bool precedes(const FlowScopeLocationAccess& store, const FlowScopeEntry&);
    bool anyPrecedes(const InitializingStores&, const FlowScopeEntry&);
    std::span<const FlowScopeLocationAccess> definitionsOf(FlowScopeLocation) const;
    InitializingStores initializingStoresOf(FlowScopeLocation);
    bool isDefinedAtEveryAccessThroughClassObject(FlowScopeLocation, const InitializingStores&);
    InitializationState initializationOf(FlowScopeLocation, bool isSetAtCreation, bool startsAbsent);
    StoreCount storeCountOf(FlowScopeLocation, bool isSetAtCreation, NOESCAPE const Invocable<bool(const FlowScopeLocationAccess&)> auto& canRunTwice);
    bool canRunTwice(const FlowScopeLocationAccess&);
    StoreCount storeCountOfMember(FlowScopeLocation);
    bool canRunTwicePerInstance(const FlowScopeLocationAccess& store, BlockID allocation);
    std::optional<BlockID> trackedAllocationBlock(LayoutID, std::span<const FlowEnvironmentAllocation>);
    bool isListedWithUndefinedAlone(std::span<const FlowEnvironmentAllocation>, uint32_t field);
    void computeEnvironmentOrders(LayoutID, std::span<FlowLocationOrder>);

    const FlowScopeIndex& m_index;
    const FlowScopeOwner& m_owner;
    Vector<std::unique_ptr<FlowScopeFrame>, 1> m_frames;
    HashMap<FunctionID, FlowScopeFrame*> m_framesByFunction;
    HashMap<FunctionID, FlowScopeEntry> m_entries;
    HashMap<FunctionID, FlowScopeEntry> m_entriesByUse;
    HashMap<VariableID, FlowScopeEntry> m_entriesByStores;
    Vector<FunctionID> m_queue;
    Vector<StoredFunction> m_storedFunctions;
    Vector<StoredFunctionReader> m_storedFunctionReaders;
    Vector<StoredFunction> m_functionsTakenOut;
    HashMap<uint32_t, Vector<FlowScopeEntry, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_entriesOfReadersOfRuns;
    Vector<FlowScopeLocationAccess> m_definitions;
    Vector<FlowReadOfLocation> m_readsAfterInitialization;
    BitVector m_classesEvaluatedOnce;
    Vector<ValueID> m_perIterationAllocations;
    FlowScopeEntry m_exit { neverScopeEntry };
    FlowScopeEntry m_hostEntry { unknownScopeEntry };
};

void FlowScopeOwnerAnalysis::forEachReader(FlowScopeLocation location, NOESCAPE const Invocable<void(const FlowScopeLocationAccess&)> auto& functor) const
{
    for (const FlowScopeLocationAccess& read : accessesAt(m_index.reads.span(), location))
        functor(read);
    for (const FlowExportedVariable& exported : exportsOf(location)) {
        for (const FlowModuleValueRead& read : valueReadsOf(m_index, exported.exporter))
            functor(FlowScopeLocationAccess { location, read.function, read.value });
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
