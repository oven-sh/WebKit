/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBorrowedParameters.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowIDs.h"
#include "AOTFrameArrayParameters.h"
#include "AOTIR.h"
#include "AOTKnownValue.h"
#include "AOTVector.h"
#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Forward.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace AOT { namespace Typed {

enum class ProgramInstances : uint8_t { Unknown, AtMostOne };

enum class FlowIndirectCallKind : uint8_t { Accessor, BuiltinCallback, Protocol, Host };

struct FlowIndirectCall {
    FunctionID caller;
    AccessSiteID site;
    FunctionID callee;
    FlowIndirectCallKind kind { FlowIndirectCallKind::Host };
    BuiltinCallbackTimes times { BuiltinCallbackTimes::Later };
    bool isInCycle { false };

    friend bool operator==(const FlowIndirectCall&, const FlowIndirectCall&) = default;
};

struct FlowBoundFunctionUse {
    FunctionID target;
    uint32_t numberOfBoundArguments { 0 };
    ClosureUse closureUse { ClosureUse::Unknown };

    friend bool operator==(const FlowBoundFunctionUse&, const FlowBoundFunctionUse&) = default;
};

struct FlowSiteLoss {
    UnprovenCause cause { UnprovenCause::Unknown };
    LimitID limit { LimitID::None };
    Witness witness { noWitness };
    bool seesConstructedObject { false };
    bool hasOtherRoots { false };

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
    TypeID elements;
    ArrayStorage arrayStorage { ArrayStorage::Other };
    TypeID type;

    friend bool operator==(const FlowDataNodeStamp&, const FlowDataNodeStamp&) = default;
};

struct FlowDataNode {
    AllocationSiteID rootSite;
    uint32_t ordinal { 0 };
    FlowDataNodeStamp stamp;

    friend bool operator==(const FlowDataNode&, const FlowDataNode&) = default;
};

enum class LegacyRegExpStatics : uint8_t { Unknown, NeverRead };
enum class BuiltinHolderState : uint8_t { Unknown, Intact };

struct FlowLocaleUse {
    LocaleUseKind kind { LocaleUseKind::Unknown };
    StringID tag;
    BuiltinID builtin;
    Witness witness { noWitness };

    friend bool operator==(const FlowLocaleUse&, const FlowLocaleUse&) = default;
};

struct FlowApplication;

struct FlowApplicationDeleter {
    void operator()(FlowApplication*) const;
};

struct FlowFunctionTables {
    uint32_t functionCountAsRead { 0 };
    Vector<FunctionFlow> functions;
    Vector<Vector<OperandSummary>> parameterSummaries;
    Vector<Vector<OperandUses>> storedValueUses;
    Vector<Vector<OperandUses>> storedFramePointerUses;
    Vector<FlowIndirectCall> indirectCalls;
    Vector<FlowBoundFunctionUse> boundFunctionUses;
    BitVector functionsSeeingOnlyFinishedObjects;
    BitVector functionsWhoseClosuresAreNeverCompared;
    BitVector functionsWithOneClosure;
};

struct FlowSiteTables {
    Vector<AccessSite> accessSites;
    Vector<FlowSiteLoss> accessSiteLosses;
    Vector<bool> callsOfBoundFunctions;
    Vector<AllocationSite> allocationSites;
    Vector<AllocationClass> allocationClasses;
    BitVector sitesOfObjectsThatNeverChange;
    Vector<FlowSinkOperand> sinkOperands;
    Vector<FlowTypedResultCall> typedResultCalls;
    Vector<AllocationSiteID> typedResultSites;
    Vector<FlowDataNode> dataNodes;
};

struct FlowBuiltinTables {
    Vector<BuiltinLocationFlow> locations;
    BitVector constantMembers;
    BitVector intactHolders;
    Vector<BuiltinUseRecord> uses;
    Vector<FeatureUseRecord> featureUses;
    Vector<FlowLocaleUse> localeUses;
    LegacyRegExpStatics legacyRegExpStatics { LegacyRegExpStatics::Unknown };
    bool areFilled { false };
};

bool builtinHolderNeverGains(const FlowBuiltinTables&, BuiltinID holder, PropertyKeyID);

struct FlowProgramTables {
    Vector<ModuleFlow> modules;
    Vector<UnseenCodeRecord> unseenCode;
    Vector<UnseenCodeRecord> loadsOfMachineCode;
    ProgramInstances instances { ProgramInstances::Unknown };
    Vector<PropertyKeyID> keysStoredThroughUnknownReceivers;
    Vector<PropertyKeyID> keysDefinedOnObjectsWithoutKnownLayout;
    Vector<PropertyKeyID> keysOfSeveralSymbols;
};

struct FlowResultTables {
    const Program* program { nullptr };
    FlowFunctionTables functions;
    FlowSiteTables sites;
    FlowBuiltinTables builtins;
    FlowProgramTables wholeProgram;
    std::unique_ptr<FlowApplication, FlowApplicationDeleter> application;
};

struct ConstantLists {
    struct Entry {
        VariableID variable;
        Vector<KnownValue> elements;
    };

    const Vector<KnownValue>* find(VariableID variable) const LIFETIME_BOUND
    {
        auto entry = std::ranges::lower_bound(entries, variable, { }, &Entry::variable);
        return entry != entries.end() && entry->variable == variable ? &entry->elements : nullptr;
    }

    Vector<Entry> entries;
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
    ClosureUse closureUseOfBoundFunction(FunctionID target, uint32_t numberOfBoundArguments) const;
    bool closuresAreNeverCompared(FunctionID) const;
    bool hasOneClosure(FunctionID id) const { return id.index() < m_tables.functions.functionCountAsRead && m_tables.functions.functionsWithOneClosure.get(id.index()); }
    OperandSummary parameterSummary(FunctionID, uint32_t entryParameter) const;
    void removeEntryParameters(FunctionID, const BitVector& removedEntryParameters);
    void setOperandSummaries(Vector<Vector<OperandSummary>>&& parameterSummaries, Vector<Vector<OperandUses>>&& storedValueUses);
    OperandUses storedValueUses(FieldReference) const;
    OperandSummary framePointerSummary(FunctionID function, uint32_t entryParameter) const { return m_borrowedParameters.summaryOf(function, entryParameter); }
    OperandUses storedFramePointerUses(FieldReference) const;
    void setBorrowedParameters(BorrowedParameters&& table) { m_borrowedParameters = WTF::move(table); }
    const BorrowedParameters& borrowedParameters() const { return m_borrowedParameters; }
    void addBorrowedParameters(FunctionID function, uint64_t word) { m_borrowedParameters.add(function, word); }
    bool mayTakeFrameArray(FunctionID function, uint32_t entryParameter) const { return m_frameArrayParameters.mayTake(function, entryParameter); }
    const FrameArrayParameters& frameArrayParameters() const LIFETIME_BOUND { return m_frameArrayParameters; }
    void setFrameArrayParameters(FrameArrayParameters&& table) { m_frameArrayParameters = WTF::move(table); }
    bool isSiteOfWholeLiterals(AllocationSiteID site) const { return site && m_sitesOfWholeLiterals.get(site.index()); }
    void setSitesOfWholeLiterals(BitVector&& sites) { m_sitesOfWholeLiterals = WTF::move(sites); }
    const Vector<KnownValue>* constantListIn(VariableID variable) const LIFETIME_BOUND { return m_constantLists.find(variable); }
    void setConstantLists(ConstantLists&& table) { m_constantLists = WTF::move(table); }
    std::span<const FlowIndirectCall> indirectCalls() const LIFETIME_BOUND { return m_tables.functions.indirectCalls.span(); }
    std::span<const FlowIndirectCall> indirectCallsFrom(FunctionID callerAsRead) const LIFETIME_BOUND;

    const ModuleFlow& module(ModuleID) const LIFETIME_BOUND;

    BuiltinLocationState builtinLocation(BuiltinID member) const { return member && m_tables.builtins.constantMembers.get(member.index()) ? BuiltinLocationState::Constant : BuiltinLocationState::Unknown; }
    BuiltinHolderState builtinHolder(BuiltinID holder) const { return holder && m_tables.builtins.intactHolders.get(holder.index()) ? BuiltinHolderState::Intact : BuiltinHolderState::Unknown; }
    const BuiltinLocationFlow* findBuiltinLocation(BuiltinID holder, PropertyKeyID) const LIFETIME_BOUND;
    bool builtinHolderNeverGains(BuiltinID holder, PropertyKeyID key) const { return hasResults() && Typed::builtinHolderNeverGains(m_tables.builtins, holder, key); }
    std::span<const BuiltinLocationFlow> builtinLocations() const LIFETIME_BOUND { return m_tables.builtins.locations.span(); }

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
    bool objectsNeverChange(AllocationSiteID site) const { return m_tables.sites.sitesOfObjectsThatNeverChange.get(site.index()); }
    bool areClosuresAsCreated(FunctionID closureFunction, std::span<const PropertyKeyID> ownKeys) const;
    bool closuresInheritFromFunctionPrototype(FunctionID closureFunction, PropertyKeyID) const;
    bool closuresAreChangedByProgram(FunctionID closureFunction) const { return closuresMeetOneOf(closureFunction, { ObjectMutation::AddProperty, ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype, ObjectMutation::StoreAfterCreation }); }
    bool closuresAreRelinkedByProgram(FunctionID closureFunction) const { return closuresMeetOneOf(closureFunction, { ObjectMutation::ChangePrototype }); }
    const FlowSiteLoss& lossOf(AccessSiteID) const LIFETIME_BOUND;
    bool seesOnlyFinishedObjects(AccessSiteID) const;
    bool mayCallBoundFunction(AccessSiteID) const;
    const SinkCandidates& sinkCandidates(AccessSiteID call, uint32_t operandIndex) const LIFETIME_BOUND;
    TypeID typedResultType(AccessSiteID call) const;
    std::span<const LayoutID> typedResultLayouts(AccessSiteID call) const LIFETIME_BOUND;
    AllocationSiteID typedResultSite(LayoutID) const;
    FlowDataNodeStamp dataNodeStamp(AllocationSiteID rootSite, uint32_t ordinalInPreorder) const;
    bool mayReadLegacyRegExpStatics() const { return m_tables.builtins.legacyRegExpStatics != LegacyRegExpStatics::NeverRead; }

    bool mayRunUnseenCode() const { return !hasResults() || !m_tables.wholeProgram.unseenCode.isEmpty(); }
    bool unseenCodeCanRun() const { return m_unseenCodeCanRun; }
    bool unseenScriptsCanRun() const { return m_unseenScriptsCanRun; }
    std::span<const UnseenCodeRecord> unseenCode() const LIFETIME_BOUND { return m_tables.wholeProgram.unseenCode.span(); }
    std::span<const UnseenCodeRecord> loadsOfMachineCode() const LIFETIME_BOUND { return m_tables.wholeProgram.loadsOfMachineCode.span(); }
    std::span<const BuiltinUseRecord> builtinUses() const LIFETIME_BOUND { return m_tables.builtins.uses.span(); }
    std::span<const FeatureUseRecord> featureUses() const LIFETIME_BOUND { return m_tables.builtins.featureUses.span(); }
    std::span<const FlowLocaleUse> localeUses() const LIFETIME_BOUND { return m_tables.builtins.localeUses.span(); }
    OptionSet<BuiltinUse> usesOf(BuiltinID) const;
    ProgramInstances programInstances() const { return m_tables.wholeProgram.instances; }
    std::span<const PropertyKeyID> keysStoredThroughUnknownReceivers() const LIFETIME_BOUND { return m_tables.wholeProgram.keysStoredThroughUnknownReceivers.span(); }
    std::span<const PropertyKeyID> keysDefinedOnObjectsWithoutKnownLayout() const LIFETIME_BOUND { return m_tables.wholeProgram.keysDefinedOnObjectsWithoutKnownLayout.span(); }
    bool standsForSeveralSymbols(PropertyKeyID key) const { return std::ranges::binary_search(m_tables.wholeProgram.keysOfSeveralSymbols, key); }

    void dump(PrintStream&, const Program&) const;

private:
    bool closuresMeetOneOf(FunctionID closureFunction, OptionSet<ObjectMutation>) const;
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
    BorrowedParameters m_borrowedParameters;
    FrameArrayParameters m_frameArrayParameters;
    ConstantLists m_constantLists;
    BitVector m_sitesOfWholeLiterals;
    bool m_unseenCodeCanRun { true };
    bool m_unseenScriptsCanRun { true };
    UnknownRecords m_unknown { };
};

struct FlowApplyAccess {
    static const FlowApplication* applicationOf(const FlowResults& results) { return results.m_tables.application.get(); }
    static std::unique_ptr<FlowApplication, FlowApplicationDeleter> takeApplication(FlowResults& results) { return WTF::move(results.m_tables.application); }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
