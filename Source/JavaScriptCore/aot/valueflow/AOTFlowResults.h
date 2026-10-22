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
#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Forward.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class RelationalSummaries;
struct HandedToCalledParameter;
struct LinkedFate;
struct ResultOrigin;

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

// What becomes of the results of the calls of bind that bind this many arguments to a closure of the target.
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
    // What is proven of the array that the node becomes. None for an object with a layout, whose allocation says all there is, and where nothing is proven.
    TypeID type;

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
    Vector<Vector<OperandUses>> storedValueUses;
    Vector<Vector<OperandUses>> storedFramePointerUses;
    Vector<FlowIndirectCall> indirectCalls;
    // Ascending by target and number of bound arguments.
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
    // By allocation site of an instance of a built-in class: nothing is stored to, added to, taken from or redefined on an object of the site once it is made, its prototype stays, and no unknown code gets it.
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
};

struct FlowProgramTables {
    Vector<ModuleFlow> modules;
    Vector<UnseenCodeRecord> unseenCode;
    // Machine code is no JavaScript that nobody has seen: no fact falls for it, and the run time admits no text for its sake, so it is none of the records above.
    Vector<UnseenCodeRecord> loadsOfMachineCode;
    ProgramInstances instances { ProgramInstances::Unknown };
    Vector<PropertyKeyID> keysStoredThroughUnknownReceivers;
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

// Ascending by variable.
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
    // A bound function is derived from its target, so function() answers for closures of the target.
    ClosureUse closureUseOfBoundFunction(FunctionID target, uint32_t numberOfBoundArguments) const;
    // No operation and no built-in that flow has followed can tell two closures of the function apart: none stands on both sides of `==` or `===`, none is looked at by a built-in, none is a key of a Map or a Set.
    bool closuresAreNeverCompared(FunctionID) const;
    // The program makes a closure of the function at one place, which runs at most once: two values that are closures of it are one object.
    bool hasOneClosure(FunctionID) const;
    OperandSummary parameterSummary(FunctionID, uint32_t entryParameter) const;
    void removeEntryParameters(FunctionID, const BitVector& removedEntryParameters);
    // Whoever asks includes AOTRelationalSummaries.h. The function may be any version of a function that was read. "Anything" before the table is set.
    LinkedFate fateOfParameter(FunctionID, uint32_t entryParameter) const;
    // The same at one call, of the operand with that index: all that the callees which the call may enter do with it. A bit of storedIntoParameters speaks of an operand of the call.
    LinkedFate fateOfOperandAt(const Function&, const Value& call, uint32_t operandIndex) const;
    LinkedFate fateApartFromCalledParameters(FunctionID, uint32_t entryParameter) const;
    // In terms of the entry parameters of the version. Not complete: the version is no longer passed something that it calls, and fateOfParameter() is all there is.
    Vector<HandedToCalledParameter, 4> handedToCalledParameters(FunctionID, bool& isComplete) const;
    ResultOrigin originOfResult(FunctionID, uint32_t result) const;
    void setRelationalSummaries(std::unique_ptr<RelationalSummaries>&&);
    const RelationalSummaries* relationalSummaries() const LIFETIME_BOUND { return m_relationalSummaries.get(); }
    // By function as read and entry parameter, and by layout and field.
    void setOperandSummaries(Vector<Vector<OperandSummary>>&& parameterSummaries, Vector<Vector<OperandUses>>&& storedValueUses);
    OperandUses storedValueUses(FieldReference) const;
    // The same two answers for a value that may be in no cell of the heap: a store into any cell retains it, also a cell that dies with the activation.
    OperandSummary framePointerSummary(FunctionID, uint32_t entryParameter) const;
    OperandUses storedFramePointerUses(FieldReference) const;
    void setBorrowedParameters(BorrowedParameters&& table) { m_borrowedParameters = WTF::move(table); }
    const BorrowedParameters& borrowedParameters() const { return m_borrowedParameters; }
    void addBorrowedParameters(FunctionID function, uint64_t word) { m_borrowedParameters.add(function, word); }
    // Whether an array that lies in a frame of a caller may be handed to the entry parameter. Says no before the table is set.
    bool mayTakeFrameArray(FunctionID function, uint32_t entryParameter) const { return m_frameArrayParameters.mayTake(function, entryParameter); }
    const FrameArrayParameters& frameArrayParameters() const LIFETIME_BOUND { return m_frameArrayParameters; }
    void setFrameArrayParameters(FrameArrayParameters&& table) { m_frameArrayParameters = WTF::move(table); }
    // AOTWholeLiterals.h says what this is and who asks it. False before the table is set.
    bool isSiteOfWholeLiterals(AllocationSiteID site) const { return site && m_sitesOfWholeLiterals.get(site.index()); }
    void setSitesOfWholeLiterals(BitVector&& sites) { m_sitesOfWholeLiterals = WTF::move(sites); }
    // What the list holds that a variable of a module has from its one store on: primitive constants, and nothing ever changes the list. Null: nobody knows, as before the table is set.
    const Vector<KnownValue>* constantListIn(VariableID variable) const LIFETIME_BOUND { return m_constantLists.find(variable); }
    void setConstantLists(ConstantLists&& table) { m_constantLists = WTF::move(table); }
    std::span<const FlowIndirectCall> indirectCalls() const LIFETIME_BOUND { return m_tables.functions.indirectCalls.span(); }
    std::span<const FlowIndirectCall> indirectCallsFrom(FunctionID callerAsRead) const LIFETIME_BOUND;

    const ModuleFlow& module(ModuleID) const LIFETIME_BOUND;

    BuiltinLocationState builtinLocation(BuiltinID member) const;
    BuiltinHolderState builtinHolder(BuiltinID holder) const;
    const BuiltinLocationFlow* findBuiltinLocation(BuiltinID holder, PropertyKeyID) const LIFETIME_BOUND;
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
    const FlowSiteLoss& lossOf(AccessSiteID) const LIFETIME_BOUND;
    bool seesOnlyFinishedObjects(AccessSiteID) const;
    // Such a call enters the target, and its callee is no closure of the target.
    bool mayCallBoundFunction(AccessSiteID) const;
    const SinkCandidates& sinkCandidates(AccessSiteID call, uint32_t operandIndex) const LIFETIME_BOUND;
    TypeID typedResultType(AccessSiteID call) const;
    std::span<const LayoutID> typedResultLayouts(AccessSiteID call) const LIFETIME_BOUND;
    AllocationSiteID typedResultSite(LayoutID) const;
    FlowDataNodeStamp dataNodeStamp(AllocationSiteID rootSite, uint32_t ordinalInPreorder) const;
    bool mayReadLegacyRegExpStatics() const { return m_tables.builtins.legacyRegExpStatics != LegacyRegExpStatics::NeverRead; }

    bool mayRunUnseenCode() const { return !hasResults() || !m_tables.wholeProgram.unseenCode.isEmpty(); }
    // Code that the build has not seen can run: text that is evaluated, machine code that is loaded. A record that only names a module at run time lets none in.
    bool unseenCodeCanRun() const { return m_unseenCodeCanRun; }
    // Such code can be a script, which adds let, const and class bindings to the realm.
    bool unseenScriptsCanRun() const { return m_unseenScriptsCanRun; }
    std::span<const UnseenCodeRecord> unseenCode() const LIFETIME_BOUND { return m_tables.wholeProgram.unseenCode.span(); }
    std::span<const UnseenCodeRecord> loadsOfMachineCode() const LIFETIME_BOUND { return m_tables.wholeProgram.loadsOfMachineCode.span(); }
    std::span<const BuiltinUseRecord> builtinUses() const LIFETIME_BOUND { return m_tables.builtins.uses.span(); }
    std::span<const FeatureUseRecord> featureUses() const LIFETIME_BOUND { return m_tables.builtins.featureUses.span(); }
    std::span<const FlowLocaleUse> localeUses() const LIFETIME_BOUND { return m_tables.builtins.localeUses.span(); }
    OptionSet<BuiltinUse> usesOf(BuiltinID) const;
    ProgramInstances programInstances() const { return m_tables.wholeProgram.instances; }
    std::span<const PropertyKeyID> keysStoredThroughUnknownReceivers() const LIFETIME_BOUND { return m_tables.wholeProgram.keysStoredThroughUnknownReceivers.span(); }
    bool standsForSeveralSymbols(PropertyKeyID key) const { return std::ranges::binary_search(m_tables.wholeProgram.keysOfSeveralSymbols, key); }

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
    BorrowedParameters m_borrowedParameters;
    FrameArrayParameters m_frameArrayParameters;
    ConstantLists m_constantLists;
    BitVector m_sitesOfWholeLiterals;
    std::unique_ptr<RelationalSummaries> m_relationalSummaries;
    bool m_unseenCodeCanRun { true };
    bool m_unseenScriptsCanRun { true };
    UnknownRecords m_unknown { };
};

struct FlowApplyAccess {
    static const FlowApplication* applicationOf(const FlowResults& results) { return results.m_tables.application.get(); }
    static std::unique_ptr<FlowApplication, FlowApplicationDeleter> takeApplication(FlowResults& results) { return WTF::move(results.m_tables.application); }
    static const FlowResultTables& tablesOf(const FlowResults& results) { return results.m_tables; }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
