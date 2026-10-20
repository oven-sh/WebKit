/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowIDs.h"
#include "AOTIR.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Forward.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class ProgramInstances : uint8_t {
    Unknown,
    AtMostOne,
};

enum class FlowIndirectCallKind : uint8_t {
    Accessor,
    BuiltinCallback,
    Protocol,
    Host,
};

struct FlowIndirectCall {
    FunctionID caller;
    AccessSiteID site;
    FunctionID callee;
    FlowIndirectCallKind kind { FlowIndirectCallKind::Host };
    BuiltinCallbackTimes times { BuiltinCallbackTimes::Later };
    bool isInCycle { false };

    friend bool operator==(const FlowIndirectCall&, const FlowIndirectCall&) = default;
};

struct FlowSiteLoss {
    UnprovenCause cause { UnprovenCause::Unknown };
    LimitID limit { LimitID::None };
    Witness witness { noWitness };

    friend bool operator==(const FlowSiteLoss&, const FlowSiteLoss&) = default;
};

struct SinkCandidates {
    Vector<StringID> strings;
    bool mayBeUndefined { true };
    bool isComplete { false };

    friend bool operator==(const SinkCandidates&, const SinkCandidates&) = default;
};

struct FlowSinkOperand {
    AccessSiteID call;
    uint32_t operandIndex { 0 };
    SinkCandidates candidates;
};

struct FlowTypedResultCall {
    AccessSiteID call;
    TypeID type;
    Vector<LayoutID, 4> layouts;

    friend bool operator==(const FlowTypedResultCall&, const FlowTypedResultCall&) = default;
};

struct FlowDataNodeStamp {
    AllocationSiteID site;
    AllocationClassID allocationClass;
    bool keepsShape { false };
    TypeID elements;

    friend bool operator==(const FlowDataNodeStamp&, const FlowDataNodeStamp&) = default;
};

struct FlowDataNode {
    AllocationSiteID rootSite;
    uint32_t ordinal { 0 };
    FlowDataNodeStamp stamp;

    friend bool operator==(const FlowDataNode&, const FlowDataNode&) = default;
};

enum class LegacyRegExpStatics : uint8_t {
    Unknown,
    NeverRead,
};

enum class BuiltinHolderState : uint8_t {
    Unknown,
    Intact,
};

struct FlowLocaleUse {
    LocaleUseKind kind { LocaleUseKind::Unknown };
    StringID tag;
    BuiltinID builtin;
    Witness witness { noWitness };

    friend bool operator==(const FlowLocaleUse&, const FlowLocaleUse&) = default;
};

// Whether the closures keep the prototype they were created with and what they were created with under the keys.
bool keepsCreatedMembers(const AllocationClass& closures, std::span<const PropertyKeyID> ownKeys);

struct FlowApplication;

struct FlowApplicationDeleter {
    void operator()(FlowApplication*) const;
};

struct FlowFunctionTables {
    uint32_t functionCountAsRead { 0 };
    Vector<FunctionFlow> functions;
    Vector<Vector<OperandSummary>> parameterSummaries;
    Vector<Vector<OperandUses>> framePointerParameterUses;
    Vector<Vector<OperandUses>> storedValueUses;
    Vector<Vector<OperandUses>> storedFramePointerUses;
    Vector<FlowIndirectCall> indirectCalls;
    BitVector functionsSeeingOnlyFinishedObjects;
};

struct FlowSiteTables {
    Vector<AccessSite> accessSites;
    Vector<FlowSiteLoss> accessSiteLosses;
    Vector<AllocationSiteID> resultSites;
    Vector<bool> callsOfBoundFunctions;
    Vector<AllocationSite> allocationSites;
    Vector<AllocationClass> allocationClasses;
    Vector<FlowSinkOperand> sinkOperands;
    Vector<FlowTypedResultCall> typedResultCalls;
    Vector<AllocationSiteID> typedResultSites;
    Vector<FlowDataNode> dataNodes;
};

struct ClosedAbsentName {
    BuiltinID holder;
    PropertyKeyID key;
};

struct FlowBuiltinTables {
    Vector<BuiltinLocationFlow> locations;
    BitVector constantMembers;
    BitVector intactHolders;
    // Ascending by holder and key.
    Vector<ClosedAbsentName> closedAbsentNames;
    // Ascending. The holders whose link only start-up fixes.
    Vector<BuiltinID> holdersWithFixedPrototypeLink;
    Vector<BuiltinUseRecord> uses;
    Vector<FeatureUseRecord> featureUses;
    Vector<FlowLocaleUse> localeUses;
    LegacyRegExpStatics legacyRegExpStatics { LegacyRegExpStatics::Unknown };
};

struct FlowProgramTables {
    Vector<ModuleFlow> modules;
    Vector<UnseenCodeRecord> unseenCode;
    ProgramInstances instances { ProgramInstances::Unknown };
};

struct FlowResultTables {
    const Program* program { nullptr };
    FlowFunctionTables functions;
    FlowSiteTables sites;
    FlowBuiltinTables builtins;
    FlowProgramTables wholeProgram;
    std::unique_ptr<FlowApplication, FlowApplicationDeleter> application;
};

class FlowResults {
    WTF_MAKE_TZONE_ALLOCATED(FlowResults);
    WTF_MAKE_NONCOPYABLE(FlowResults);
public:
    FlowResults();
    explicit FlowResults(FlowResultTables&&);
    FlowResults(FlowResults&&);
    FlowResults& operator=(FlowResults&&);
    ~FlowResults();

    bool hasResults() const { return m_tables.program; }
    uint32_t functionCountAsRead() const { return m_tables.functions.functionCountAsRead; }
    uint32_t functionCount() const { return static_cast<uint32_t>(m_tables.functions.functions.size()); }

    const FunctionFlow& function(FunctionID) const LIFETIME_BOUND;
    bool mayBeReached(FunctionID function) const { return this->function(function).reachability != FunctionReachability::Unreached; }
    bool runsAtMostOnce(FunctionID function) const { return this->function(function).executions == ExecutionCount::AtMostOnce; }
    bool runsAtMostOnce(const Program&, FunctionID) const;
    OperandSummary parameterSummary(FunctionID, uint32_t entryParameter) const;
    void removeEntryParameters(FunctionID, const BitVector& removedEntryParameters);
    OperandUses storedValueUses(FieldReference) const;
    // The same two answers for a value that may be in no cell of the heap: a store into any cell retains it, also a cell that dies with the activation.
    OperandSummary framePointerSummary(FunctionID, uint32_t entryParameter) const;
    OperandUses storedFramePointerUses(FieldReference) const;
    std::span<const FlowIndirectCall> indirectCalls() const LIFETIME_BOUND { return m_tables.functions.indirectCalls.span(); }
    std::span<const FlowIndirectCall> indirectCallsFrom(FunctionID callerAsRead) const LIFETIME_BOUND;

    const ModuleFlow& module(ModuleID) const LIFETIME_BOUND;

    BuiltinLocationState builtinLocation(BuiltinID member) const;
    BuiltinHolderState builtinHolder(BuiltinID holder) const;
    const BuiltinLocationFlow* findBuiltinLocation(BuiltinID holder, PropertyKeyID) const LIFETIME_BOUND;
    std::span<const BuiltinLocationFlow> builtinLocations() const LIFETIME_BOUND { return m_tables.builtins.locations.span(); }
    std::span<const ClosedAbsentName> closedAbsentNames() const LIFETIME_BOUND { return m_tables.builtins.closedAbsentNames.span(); }
    std::span<const BuiltinID> holdersWithFixedPrototypeLink() const LIFETIME_BOUND { return m_tables.builtins.holdersWithFixedPrototypeLink.span(); }

    std::span<const AccessSite> accessSites() const LIFETIME_BOUND { return m_tables.sites.accessSites.span(); }
    std::span<const AllocationSite> allocationSites() const LIFETIME_BOUND { return m_tables.sites.allocationSites.span(); }
    std::span<const AllocationClass> allocationClasses() const LIFETIME_BOUND { return m_tables.sites.allocationClasses.span(); }
    unsigned numberOfAccessSites() const { return m_tables.sites.accessSites.size(); }
    unsigned numberOfAllocationSites() const { return m_tables.sites.allocationSites.size(); }
    unsigned numberOfAllocationClasses() const { return m_tables.sites.allocationClasses.size(); }
    const AccessSite& at(AccessSiteID) const LIFETIME_BOUND;
    const AllocationSite& at(AllocationSiteID) const LIFETIME_BOUND;
    const AllocationClass& at(AllocationClassID) const LIFETIME_BOUND;
    const AllocationClass& classOf(AllocationSiteID site) const LIFETIME_BOUND { return at(at(site).allocationClass); }
    bool areClosuresAsCreated(FunctionID closureFunction, std::span<const PropertyKeyID> ownKeys) const;
    const FlowSiteLoss& lossOf(AccessSiteID) const LIFETIME_BOUND;
    bool seesOnlyFinishedObjects(AccessSiteID) const;
    // Such a call enters the target, and its callee is no closure of the target.
    bool mayCallBoundFunction(AccessSiteID) const;
    AllocationSiteID resultSiteOf(AccessSiteID call) const;
    const SinkCandidates& sinkCandidates(AccessSiteID call, uint32_t operandIndex) const LIFETIME_BOUND;
    TypeID typedResultType(AccessSiteID call) const;
    std::span<const LayoutID> typedResultLayouts(AccessSiteID call) const LIFETIME_BOUND;
    AllocationSiteID typedResultSite(LayoutID) const;
    FlowDataNodeStamp dataNodeStamp(AllocationSiteID rootSite, uint32_t ordinalInPreorder) const;
    bool mayReadLegacyRegExpStatics() const { return m_tables.builtins.legacyRegExpStatics != LegacyRegExpStatics::NeverRead; }

    bool mayRunUnseenCode() const { return !hasResults() || !m_tables.wholeProgram.unseenCode.isEmpty(); }
    std::span<const UnseenCodeRecord> unseenCode() const LIFETIME_BOUND { return m_tables.wholeProgram.unseenCode.span(); }
    std::span<const BuiltinUseRecord> builtinUses() const LIFETIME_BOUND { return m_tables.builtins.uses.span(); }
    std::span<const FeatureUseRecord> featureUses() const LIFETIME_BOUND { return m_tables.builtins.featureUses.span(); }
    std::span<const FlowLocaleUse> localeUses() const LIFETIME_BOUND { return m_tables.builtins.localeUses.span(); }
    OptionSet<BuiltinUse> usesOf(BuiltinID) const;
    ProgramInstances programInstances() const { return m_tables.wholeProgram.instances; }

    void dump(PrintStream&, const Program&) const;

private:
    friend struct FlowApplyAccess;

    struct UnknownRecords {
        FunctionFlow function;
        ModuleFlow module;
        AccessSite accessSite;
        AllocationSite allocationSite;
        AllocationClass allocationClass;
        FlowSiteLoss loss;
        SinkCandidates sinkCandidates;
    };

    FunctionID findAnalyzedFunction(FunctionID) const;
    std::optional<uint32_t> findAnalyzedParameter(FunctionID&, uint32_t entryParameter) const;
    std::optional<uint32_t> findDerivedParameter(FunctionID derived, uint32_t analyzedParameter) const;

    FlowResultTables m_tables;
    Vector<AllocationClassID> m_closureClassesByFunction;
    UnknownRecords m_unknown { };
};

struct FlowApplyAccess {
    static const FlowApplication* applicationOf(const FlowResults& results) { return results.m_tables.application.get(); }
    static const FlowResultTables& tablesOf(const FlowResults& results) { return results.m_tables; }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
