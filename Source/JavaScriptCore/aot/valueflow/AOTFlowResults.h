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
#include "AOTFlowModuleOrder.h"
#include "AOTFrameArrayParameters.h"
#include "AOTIR.h"
#include "AOTKnownValue.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Forward.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace AOT {

struct CompileOptions;

namespace Typed {

enum class ProgramInstances : uint8_t { Unknown, AtMostOne };

enum class FlowIndirectCallKind : uint8_t { Accessor, BuiltinCallback, Protocol, Host };

struct FlowIndirectCall {
    FunctionID caller;
    AccessSiteID site;
    FunctionID callee;
    FlowIndirectCallKind kind { FlowIndirectCallKind::Host };
    // Read off the built-ins that were seen at the site.
    Expected<BuiltinCallbackTimes> times { BuiltinCallbackTimes::Later };
    bool isInCycle { false };
    bool isAlwaysConstruction { false };

    friend bool operator==(const FlowIndirectCall& a, const FlowIndirectCall& b)
    {
        return a.caller == b.caller && a.site == b.site && a.callee == b.callee && a.kind == b.kind && a.times.isSamePredictionAs(b.times) && a.isInCycle == b.isInCycle && a.isAlwaysConstruction == b.isAlwaysConstruction;
    }
};

struct FlowBoundFunctionUse {
    FunctionID target;
    uint32_t numberOfBoundArguments { 0 };
    ClosureUse closureUse { ClosureUse::Unknown };

    friend bool operator==(const FlowBoundFunctionUse&, const FlowBoundFunctionUse&) = default;
};

// Made: the site calls bind on a closure of the target. Called: the site calls what such a bind returned. Other values may reach the site too.
enum class FlowBoundFunctionUseAtSite : uint8_t { Made, Called };

struct FlowBoundFunctionAtSite {
    AccessSiteID site;
    FlowBoundFunctionUseAtSite use { FlowBoundFunctionUseAtSite::Made };
    FunctionID target;
    uint32_t numberOfBoundArguments { 0 };

    friend bool operator==(const FlowBoundFunctionAtSite&, const FlowBoundFunctionAtSite&) = default;
};

// `_extends = Object.assign.bind()`: whatever bound function the site calls binds undefined and no argument to this built-in. Other callables may reach the site too.
struct FlowBuiltinBoundAtSite {
    AccessSiteID site;
    BuiltinID target;

    friend bool operator==(const FlowBuiltinBoundAtSite&, const FlowBuiltinBoundAtSite&) = default;
};

// `on("close", listener)`: a function that stores under a key that is its parameter was followed for this key, and a closure of this function was stored under it.
struct FlowClosureStoredUnderKey {
    PropertyKeyID key;
    FunctionID function;

    friend bool operator==(const FlowClosureStoredUnderKey&, const FlowClosureStoredUnderKey&) = default;
};

struct FlowSiteLoss {
    UnprovenCause cause { UnprovenCause::Unknown };
    LimitID limit { LimitID::None };
    Witness witness { noWitness };
    bool seesConstructedObject { false };
    bool hasOtherRoots { false };
    // One more than an index into FlowSiteTables::otherOrigins, or 0.
    uint16_t otherOrigins { 0 };

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
    FunctionID function;
    bool isUnprofitable { false };

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

// The path of an embedded file that may be passed to `new Worker()`.
struct FlowWorkerEntry {
    StringID path;
    // Flow lost track of the path and some Worker gets an unknown value, or another native function looks at the path too.
    // False: flow followed the path to the Worker and nowhere else.
    bool isGuess { false };
    // The Worker may also get an options object.
    bool hasOptions { false };
    // One `new Worker()` that may get the path.
    Witness witness { noWitness };

    friend bool operator==(const FlowWorkerEntry&, const FlowWorkerEntry&) = default;
};

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
    // Only for --explain, and empty otherwise: whether the closures escape by a path that does not start at FunctionFlow::witness, and where, as in FlowSiteLoss.
    BitVector functionsWithOtherEscapeOrigins;
    Vector<uint16_t> otherEscapeOriginsOfFunctions;
    Vector<Vector<OperandSummary>> parameterSummaries;
    Vector<Vector<OperandUses>> storedValueUses;
    Vector<Vector<OperandUses>> storedFramePointerUses;
    Vector<FlowIndirectCall> indirectCalls;
    Vector<FlowBoundFunctionUse> boundFunctionUses;
    BitVector functionsSeeingOnlyFinishedObjects;
    BitVector functionsWhoseClosuresAreNeverCompared;
    BitVector functionsWithOneClosure;
    BitVector functionsWhoseClosuresMayBeRenamed;
    BitVector functionsWhoseClosuresMayBeRenamedOnlyWhereNobodySees;
    BitVector functionsWhoseClosuresGoOutOfSightUnnamed;
    BitVector functionsWhoseClosuresMayBeNamedOutOfSight;
    BitVector functionsWhoseClosuresAreGivenDisplayName;
    BitVector functionsWhoseClosuresChangeIntegrity;
    BitVector functionsWhoseClosuresAreMadeOnce;
    // By the function as it is written: all its forms together.
    BitVector writtenFunctionsWithOneFunctionObject;
};

struct WrittenPositionOfSite {
    AccessSiteID site;
    uint32_t position { 0 };
};

// Proxies that the program makes are seen to arrive as the receiver of the access, and little else. The layouts are those seen to arrive as the handler and as the target where those proxies are made.
// A list is empty where nothing with a layout is seen and where more kinds are seen than a place can tell apart. A prediction: other proxies may arrive, and other objects.
struct FlowProxiesSeenAtSite {
    AccessSiteID site;
    Vector<LayoutID, 4> handlers;
    Vector<LayoutID, 4> targets;
    // Lists are seen among the targets, beside what has one of the layouts.
    bool seesListsAsTargets { false };
};

struct FlowSiteTables {
    Vector<AccessSite> accessSites;
    // Sorted by site. The location of a site is where an error is reported; only a site that the source text has elsewhere is listed.
    Vector<WrittenPositionOfSite> writtenPositionsOfSites;
    Vector<FlowSiteLoss> accessSiteLosses;
    // Only for --explain: the independent origins of a loss beside its witness.
    Vector<Wire::OtherOriginsRecord> otherOrigins;
    Vector<bool> callsOfBoundFunctions;
    // Empty where no array has such a kind.
    Vector<bool> receiversThatMayHaveDecidedStorage;
    bool keepsStorageOfListThatMayReadUndefined { false };
    // 0: flow says nothing. 1: no object that was seen to arrive has the name of the access. Else the number of the stamped set plus 2. Empty where nothing is stamped.
    Vector<uint32_t> receiversSeenAtSites;
    // By site: all that the read or the call hands over, if it is an object, is plain data from outside, as out of JSON.parse. Empty where no flow results were asked for.
    Vector<bool> sitesThatHandOverOnlyDataFromOutside;
    // By site: a call, and of the objects among its arguments all are such data, and there is one. Empty where no flow results were asked for.
    Vector<bool> callsHandedOnlyDataFromOutside;
    // Sorted by site.
    Vector<FlowProxiesSeenAtSite> proxiesSeenAtSites;
    // Sorted by site, use, target and number of bound arguments.
    Vector<FlowBoundFunctionAtSite> boundFunctionsAtSites;
    // Sorted by site.
    Vector<FlowBuiltinBoundAtSite> builtinsBoundAtSites;
    // Sorted by key and function. A key under which more was stored than a call tests for has no entry.
    Vector<FlowClosureStoredUnderKey> closuresStoredUnderKeys;
    Vector<AllocationSite> allocationSites;
    Vector<AllocationClass> allocationClasses;
    BitVector sitesOfObjectsThatNeverChange;
    BitVector sitesOfListsNeverToldApart;
    BitVector layoutsNeverHeldWeakly;
    Vector<FlowSinkOperand> sinkOperands;
    Vector<FlowTypedResultCall> typedResultCalls;
    Vector<AllocationSiteID> typedResultSites;
    Vector<FlowDataNode> dataNodes;
};

// A name that a test may add to a built-in object which takes no new names.
struct FlowAddedName {
    BuiltinID holder;
    Vector<uint8_t> name;
};

struct FlowBuiltinTables {
    Vector<BuiltinLocationFlow> locations;
    // One record for each place in the source that writes a location, by holder, key and place.
    Vector<BuiltinLocationFlow> writesOfLocations;
    BitVector constantMembers;
    BitVector intactHolders;
    BitVector holdersReachedThroughUnknownObject;
    BitVector keysWrittenThroughUnknownObject;
    bool isEveryKeyWrittenThroughUnknownObject { false };
    // The image locks these when the program starts. What is said of them rests on that.
    BitVector membersLockedAtStartUp;
    // Of an image that shares its realm: a write of this program may reach them, so it locks none of them. An image built over this one sees no such write and reads them here.
    BitVector membersWrittenInSharedRealm;
    // An operation that lists own keys is handed one of these, by canonical ID.
    BitVector holdersWithListedKeys;
    Vector<BuiltinUseRecord> uses;
    // Ascending. Flow wanted to enter them, and the program does not hold them: what it says of the program leaves out what they do.
    Vector<RuntimeFunctionID> libraryFunctionsLacked;
    Vector<FeatureUseRecord> featureUses;
    BitVector builtinsThatMayBeTrimmed;
    Vector<FlowLocaleUse> localeUses;
    // Sorted by path, without duplicates.
    Vector<FlowWorkerEntry> workerEntries;
    // A place that may start a Worker and does not name the constructor: `new globalThis[name](..)`.
    std::optional<Witness> workerStartWithoutName;
    // The exports that a reached call of a spy may replace and that the front end has not made replaceable. Sorted.
    Vector<VariableID> exportsThatSpiesReplace;
    Vector<BuiltinID> builtinsThatTestsReplace;
    Vector<FlowAddedName> namesThatTestsAdd;
    LegacyRegExpStatics legacyRegExpStatics { LegacyRegExpStatics::Unknown };
    bool areFilled { false };
};

bool builtinHolderNeverGains(const FlowBuiltinTables&, BuiltinID holder, PropertyKeyID);

using FeatureBits = std::array<uint64_t, 8>;

std::optional<RemovableFeature> guardOf(const BuiltinTable&, BuiltinID);
bool isGuardedByRemovedFeature(const BuiltinTable&, BuiltinID, const FeatureBits& removedFeatures);
// The host keeps a feature by the same records and options.
BitVector findBuiltinsThatMayBeTrimmed(const BuiltinTable&, std::span<const FeatureUseRecord>, const AOT::CompileOptions&);

struct FlowProgramTables {
    Vector<ModuleFlow> modules;
    BitVector modulesWithNamespaceNamedByOperations;
    FlowModuleOrder moduleOrder;
    Vector<UnseenCodeRecord> unseenCode;
    Vector<UnseenCodeRecord> loadsOfMachineCode;
    bool findsFilesOfProgramByName { true };
    bool storesPathOfMainModule { true };
    // CompileOptions::admitsFilesRunByHost()
    bool hostRunsFilesOutsideImage { false };
    // The functions that are reached count what code that nobody has read may call. A copy from disk of a file of the program has a record above and is no such code: it calls what the file calls.
    bool reachedFunctionsCountUnreadCode { true };
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

struct ConstantMember {
    PropertyKeyID key;
    KnownValue value;
};

struct ConstantLists {
    struct Entry {
        VariableID variable;
        Vector<KnownValue> elements;
    };
    // `const options = { style: "currency", currency: "USD" };`
    struct ObjectEntry {
        VariableID variable;
        Vector<ConstantMember> members;
    };

    const Vector<KnownValue>* find(VariableID variable) const LIFETIME_BOUND
    {
        auto entry = std::ranges::lower_bound(entries, variable, { }, &Entry::variable);
        return entry != entries.end() && entry->variable == variable ? &entry->elements : nullptr;
    }

    const Vector<ConstantMember>* findObject(VariableID variable) const LIFETIME_BOUND
    {
        auto entry = std::ranges::lower_bound(objects, variable, { }, &ObjectEntry::variable);
        return entry != objects.end() && entry->variable == variable ? &entry->members : nullptr;
    }

    Vector<Entry> entries;
    Vector<ObjectEntry> objects;
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
    FlowSiteLoss lossOfParameters(FunctionID id) const
    {
        const FunctionFlow& flow = function(id);
        const FlowFunctionTables& tables = m_tables.functions;
        return { .cause = flow.cause, .witness = flow.witness, .hasOtherRoots = tables.functionsWithOtherEscapeOrigins.get(id.index()), .otherOrigins = id.index() < tables.otherEscapeOriginsOfFunctions.size() ? tables.otherEscapeOriginsOfFunctions[id.index()] : uint16_t { 0 } };
    }
    bool mayBeReached(FunctionID function) const { return this->function(function).reachability != FunctionReachability::Unreached; }
    bool runsAtMostOnce(FunctionID function) const { return this->function(function).executions == ExecutionCount::AtMostOnce; }
    bool runsAtMostOnce(const Program&, FunctionID) const;
    ClosureUse closureUseOfBoundFunction(FunctionID target, uint32_t numberOfBoundArguments) const;
    bool closuresAreNeverCompared(FunctionID) const;
    bool closuresGoUnnoticed(FunctionID) const;
    // No WeakRef, FinalizationRegistry, WeakMap or WeakSet takes an object of the layout.
    bool objectsAreNeverHeldWeakly(LayoutID layout) const { return m_tables.sites.layoutsNeverHeldWeakly.get(layout.index()); }
    bool hasOneClosure(FunctionID id) const { return id.index() < m_tables.functions.functionCountAsRead && m_tables.functions.functionsWithOneClosure.get(id.index()); }
    OperandSummary parameterSummary(FunctionID, uint32_t entryParameter) const;
    void removeEntryParameters(FunctionID, const BitVector& removedEntryParameters);
    // The function takes another value there from now on.
    void replaceEntryParameter(FunctionID, uint32_t entryParameter, const OperandSummary&);
    void setOperandSummaries(Vector<Vector<OperandSummary>>&& parameterSummaries, Vector<Vector<OperandUses>>&& storedValueUses);
    OperandUses storedValueUses(FieldReference) const;
    OperandSummary framePointerSummary(FunctionID function, uint32_t entryParameter) const { return m_borrowedParameters.summaryOf(function, entryParameter); }
    OperandUses storedFramePointerUses(FieldReference) const;
    void setBorrowedParameters(BorrowedParameters&& table) { m_borrowedParameters = WTF::move(table); }
    const BorrowedParameters& borrowedParameters() const { return m_borrowedParameters; }
    void addBorrowedParameters(FunctionID function, uint64_t word) { m_borrowedParameters.add(function, word); }
    bool mayTakeFrameArray(FunctionID function, uint32_t entryParameter) const { return m_frameArrayParameters.mayTake(function, entryParameter); }
    bool mayTakeFrameArrayWhenCalledByValue(FunctionID function, uint32_t entryParameter) const { return m_frameArrayParameters.mayTakeWhenCalledByValue(function, entryParameter); }
    const FrameArrayParameters& frameArrayParameters() const LIFETIME_BOUND { return m_frameArrayParameters; }
    void setFrameArrayParameters(FrameArrayParameters&& table) { m_frameArrayParameters = WTF::move(table); }
    bool isSiteOfWholeLiterals(AllocationSiteID site) const { return site && m_sitesOfWholeLiterals.get(site.index()); }
    void setSitesOfWholeLiterals(BitVector&& sites) { m_sitesOfWholeLiterals = WTF::move(sites); }
    const FlowProxiesSeenAtSite* findProxiesSeenAt(AccessSiteID) const LIFETIME_BOUND;
    const Vector<KnownValue>* constantListIn(VariableID variable) const LIFETIME_BOUND { return m_constantLists.find(variable); }
    const Vector<ConstantMember>* constantObjectIn(VariableID variable) const LIFETIME_BOUND { return m_constantLists.findObject(variable); }
    void setConstantLists(ConstantLists&& table) { m_constantLists = WTF::move(table); }
    std::span<const FlowIndirectCall> indirectCalls() const LIFETIME_BOUND { return m_tables.functions.indirectCalls.span(); }
    std::span<const FlowIndirectCall> indirectCallsFrom(FunctionID callerAsRead) const LIFETIME_BOUND;

    const ModuleFlow& module(ModuleID) const LIFETIME_BOUND;
    // An operation in a reachable body named the namespace object when flow analysis ran. What flow analysis says about the object holds for operations that run, and such an operation may be in code that never does.
    bool wasNamespaceNamedByOperation(ModuleID module) const { return m_tables.wholeProgram.modulesWithNamespaceNamedByOperations.get(module.index()); }

    BuiltinLocationState builtinLocation(BuiltinID member) const { return member && m_tables.builtins.constantMembers.get(member.index()) ? BuiltinLocationState::Constant : BuiltinLocationState::Unknown; }
    BuiltinHolderState builtinHolder(BuiltinID holder) const { return holder && m_tables.builtins.intactHolders.get(holder.index()) ? BuiltinHolderState::Intact : BuiltinHolderState::Unknown; }
    bool listsKeysOf(BuiltinID canonicalHolder) const { return m_mayListKeysOfEveryHolder || m_tables.builtins.holdersWithListedKeys.get(canonicalHolder.index()); }
    void setMayListKeysOfEveryHolder() { m_mayListKeysOfEveryHolder = true; }
    // The fields whose types were taken back as the results were applied. Their reads have no checks yet.
    std::span<const FieldReference> fieldsTakenBack() const LIFETIME_BOUND { return m_fieldsTakenBack.span(); }
    void setFieldsTakenBack(Vector<FieldReference>&& fields) { m_fieldsTakenBack = WTF::move(fields); }
    const BuiltinLocationFlow* findBuiltinLocation(BuiltinID holder, PropertyKeyID) const LIFETIME_BOUND;
    bool builtinHolderNeverGains(BuiltinID holder, PropertyKeyID key) const { return hasResults() && Typed::builtinHolderNeverGains(m_tables.builtins, holder, key); }
    std::span<const BuiltinLocationFlow> builtinLocations() const LIFETIME_BOUND { return m_tables.builtins.locations.span(); }
    const BitVector& builtinMembersLockedAtStartUp() const LIFETIME_BOUND { return m_tables.builtins.membersLockedAtStartUp; }
    const BitVector& builtinMembersWrittenInSharedRealm() const LIFETIME_BOUND { return m_tables.builtins.membersWrittenInSharedRealm; }
    std::span<const BuiltinLocationFlow> writesOfBuiltinLocations() const LIFETIME_BOUND { return m_tables.builtins.writesOfLocations.span(); }

    std::span<const AccessSite> accessSites() const LIFETIME_BOUND { return m_tables.sites.accessSites.span(); }
    std::span<const AllocationSite> allocationSites() const LIFETIME_BOUND { return m_tables.sites.allocationSites.span(); }
    std::span<const AllocationClass> allocationClasses() const LIFETIME_BOUND { return m_tables.sites.allocationClasses.span(); }
    unsigned numberOfAccessSites() const { return m_tables.sites.accessSites.size(); }
    unsigned numberOfAllocationSites() const { return m_tables.sites.allocationSites.size(); }
    unsigned numberOfAllocationClasses() const { return m_tables.sites.allocationClasses.size(); }
    const AccessSite& at(AccessSiteID) const LIFETIME_BOUND;
    Wire::SourceLocation writtenLocationOf(AccessSiteID) const;
    const AllocationSite& at(AllocationSiteID) const LIFETIME_BOUND;
    const AllocationClass& at(AllocationClassID) const LIFETIME_BOUND;
    const AllocationClass& classOf(AllocationSiteID site) const LIFETIME_BOUND { return at(at(site).allocationClass); }
    bool objectsNeverChange(AllocationSiteID site) const { return m_tables.sites.sitesOfObjectsThatNeverChange.get(site.index()); }
    // Nothing changes a list made at the site, and nothing can tell two of them apart: one cell may stand for all.
    bool identityIsNeverObserved(AllocationSiteID site) const { return m_tables.sites.sitesOfListsNeverToldApart.get(site.index()); }
    bool areClosuresAsCreated(FunctionID closureFunction, std::span<const PropertyKeyID> ownKeys) const;
    // By code that flow has seen.
    bool closuresMayBeRenamed(FunctionID closureFunction) const;
    // They may be, and no statement is seen to do it: what renames one goes through the run time, which knows the function object.
    bool closuresMayBeRenamedOnlyWhereNobodySees(FunctionID closureFunction) const;
    // No statement is seen to name one, and they go where nobody follows them.
    bool closuresGoOutOfSightUnnamed(FunctionID closureFunction) const;
    // Of those: a statement of the program defines `name` on an object nobody follows.
    bool closuresMayBeNamedOutOfSight(FunctionID closureFunction) const;
    // By a statement that is seen to reach the closure and writes that key or a key nobody knows.
    bool closuresAreGivenDisplayName(FunctionID closureFunction) const;
    // By one operation that runs once, or by the first read of the variable that starts with the function.
    bool closureIsMadeOnce(FunctionID closureFunction) const;
    bool closuresInheritFromFunctionPrototype(FunctionID closureFunction, PropertyKeyID) const;
    // By a statement that is seen to reach the closures and names the key.
    bool closuresAreStoredUnder(FunctionID closureFunction, PropertyKeyID) const;
    // No freeze, seal or preventExtensions reaches them: a store under a name that is no built-in's takes.
    bool closuresKeepTheirIntegrity(FunctionID closureFunction) const;
    bool closuresAreChangedByProgram(FunctionID closureFunction) const { return closuresMeetOneOf(closureFunction, { ObjectMutation::AddProperty, ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype, ObjectMutation::StoreAfterCreation }); }
    bool closuresAreRelinkedByProgram(FunctionID closureFunction) const { return closuresMeetOneOf(closureFunction, { ObjectMutation::ChangePrototype }); }
    const FlowSiteLoss& lossOf(AccessSiteID) const LIFETIME_BOUND;
    std::span<const Wire::OtherOriginsRecord> otherOrigins() const LIFETIME_BOUND { return m_tables.sites.otherOrigins.span(); }
    bool seesOnlyFinishedObjects(AccessSiteID) const;
    // A prediction: nothing that the program makes with a layout is seen to come out of the site.
    bool handsOverOnlyDataFromOutside(AccessSiteID) const;
    bool isHandedOnlyDataFromOutside(AccessSiteID) const;
    bool mayCallBoundFunction(AccessSiteID) const;
    // Flow decides the kind of storage of some arrays for the whole program, and whoever reads one of them by that decision reads it without a test.
    // So nothing that the compiler adds may change the kind of an array that may be one of them. Whoever adds such an operation asks here for each access that the array is the receiver of; no site stands for an access that flow has not seen.
    bool mayChangeStorageOfReceiver(AccessSiteID) const;
    bool keepsStorageOfListThatMayReadUndefined() const { return m_tables.sites.keepsStorageOfListThatMayReadUndefined; }
    // What was stamped on the access, for a pass that meets the access after the stamp is gone. No answer: flow says nothing. No set: no object that was seen to arrive has the name of the access.
    std::optional<LayoutSetID> receiversSeenAt(AccessSiteID) const;
    Expected<std::span<const FlowBoundFunctionAtSite>> boundFunctionsSeenAt(AccessSiteID, FlowBoundFunctionUseAtSite) const LIFETIME_BOUND;
    // None where a bound function that binds something, or another target, may reach the call.
    BuiltinID builtinBehindBoundFunctionsCalledAt(AccessSiteID) const;
    // For `table[key](..)` in a copy of a function whose caller spells the key: flow has looked at the function with every key at once.
    Expected<std::span<const FlowClosureStoredUnderKey>> closuresSeenStoredUnder(PropertyKeyID) const LIFETIME_BOUND;
    const SinkCandidates& sinkCandidates(AccessSiteID call, uint32_t operandIndex) const LIFETIME_BOUND;
    std::span<const FlowTypedResultCall> typedResultCalls() const LIFETIME_BOUND { return m_tables.sites.typedResultCalls.span(); }
    bool isTypedResultUnprofitable(AccessSiteID call) const;
    void markTypedResultsUnprofitable(std::span<const AccessSiteID> sortedCalls);
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
    bool findsFilesOfProgramByName() const { return !hasResults() || m_tables.wholeProgram.findsFilesOfProgramByName; }
    bool hostRunsFilesOutsideImage() const { return hasResults() && m_tables.wholeProgram.hostRunsFilesOutsideImage; }
    bool reachedFunctionsCountUnreadCode() const { return !hasResults() || m_tables.wholeProgram.reachedFunctionsCountUnreadCode; }
    bool storesPathOfMainModule() const { return !hasResults() || m_tables.wholeProgram.storesPathOfMainModule; }
    // The body of the first has run to its end when the body of the second begins.
    bool completesBefore(ModuleID first, ModuleID second) const { return m_tables.wholeProgram.moduleOrder.completesBefore(first, second); }
    std::span<const BuiltinUseRecord> builtinUses() const LIFETIME_BOUND { return m_tables.builtins.uses.span(); }
    std::span<const RuntimeFunctionID> libraryFunctionsLacked() const LIFETIME_BOUND { return m_tables.builtins.libraryFunctionsLacked.span(); }
    std::span<const FeatureUseRecord> featureUses() const LIFETIME_BOUND { return m_tables.builtins.featureUses.span(); }
    std::span<const FlowLocaleUse> localeUses() const LIFETIME_BOUND { return m_tables.builtins.localeUses.span(); }
    std::span<const FlowWorkerEntry> workerEntries() const LIFETIME_BOUND { return m_tables.builtins.workerEntries.span(); }
    std::optional<Witness> workerStartWithoutName() const { return m_tables.builtins.workerStartWithoutName; }
    std::span<const VariableID> exportsThatSpiesReplace() const LIFETIME_BOUND { return m_tables.builtins.exportsThatSpiesReplace.span(); }
    std::span<const BuiltinID> builtinsThatTestsReplace() const LIFETIME_BOUND { return m_tables.builtins.builtinsThatTestsReplace.span(); }
    std::span<const FlowAddedName> namesThatTestsAdd() const LIFETIME_BOUND { return m_tables.builtins.namesThatTestsAdd.span(); }
    OptionSet<BuiltinUse> usesOf(BuiltinID) const;
    const BitVector& builtinsThatMayBeTrimmed() const LIFETIME_BOUND { return m_tables.builtins.builtinsThatMayBeTrimmed; }
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
    Vector<FieldReference> m_fieldsTakenBack;
    bool m_mayListKeysOfEveryHolder { false };
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
