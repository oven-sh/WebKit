/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResults.h"

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTEffectSummaries.h"
#include "AOTFlowUnseenCode.h"
#include "AOTProvisionalFunctions.h"
#include "AOTSort.h"
#include <algorithm>
#include <tuple>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT { namespace Typed {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FlowResults);

static_assert(Witness { } == noWitness, "A default-constructed record names no witness.");
static_assert(SourceLocation { } == noSourceLocation, "A default-constructed record names no location.");

static auto sortKeyOf(const FlowIndirectCall& call) { return std::tuple(call.caller, call.site, call.callee, call.kind); }
static auto sortKeyOf(const FlowBoundFunctionUse& use) { return std::tuple(use.target, use.numberOfBoundArguments); }
static auto sortKeyOf(const FlowBoundFunctionAtSite& bound) { return std::tuple(bound.site, bound.use, bound.target, bound.numberOfBoundArguments); }
static auto sortKeyOf(const FlowClosureStoredUnderKey& stored) { return std::tuple(stored.key, stored.function); }
static auto sortKeyOf(const FlowProxiesSeenAtSite& seen) { return seen.site; }
static auto sortKeyOf(const FlowSinkOperand& operand) { return std::tuple(operand.call, operand.operandIndex); }
static auto sortKeyOf(const FlowTypedResultCall& record) { return record.call; }
static auto sortKeyOf(const FlowDataNode& node) { return std::tuple(node.rootSite, node.ordinal); }
static auto sortKeyOf(const BuiltinLocationFlow& location) { return std::tuple(location.holder, location.key); }
static auto sortKeyOf(const BuiltinUseRecord& record) { return record.builtin; }

template<typename Record>
static bool isStrictlyAscending(const Vector<Record>& table)
{
    auto isOutOfOrder = [&](const Record& first, const Record& second) {
        return !(sortKeyOf(first) < sortKeyOf(second));
    };
    return std::ranges::adjacent_find(table, isOutOfOrder) == table.end();
}

template<typename Record>
static bool isEmptyOrParallelTo(const Vector<Record>& table, const Vector<AccessSite>& accessSites)
{
    return table.isEmpty() || table.size() == accessSites.size();
}

static void checkTableOrder(const FlowResultTables& tables)
{
    RELEASE_ASSERT(tables.program);

    RELEASE_ASSERT(tables.functions.functionCountAsRead <= tables.functions.functions.size());
    RELEASE_ASSERT(tables.functions.parameterSummaries.size() == tables.functions.functions.size());
    RELEASE_ASSERT(isStrictlyAscending(tables.functions.indirectCalls));
    RELEASE_ASSERT(isStrictlyAscending(tables.functions.boundFunctionUses));

    RELEASE_ASSERT(isEmptyOrParallelTo(tables.sites.accessSiteLosses, tables.sites.accessSites));
    RELEASE_ASSERT(isEmptyOrParallelTo(tables.sites.callsOfBoundFunctions, tables.sites.accessSites));
    RELEASE_ASSERT(isEmptyOrParallelTo(tables.sites.receiversThatMayHaveDecidedStorage, tables.sites.accessSites));
    RELEASE_ASSERT(isEmptyOrParallelTo(tables.sites.receiversSeenAtSites, tables.sites.accessSites));
    RELEASE_ASSERT(isStrictlyAscending(tables.sites.sinkOperands));
    RELEASE_ASSERT(isStrictlyAscending(tables.sites.typedResultCalls));
    RELEASE_ASSERT(isStrictlyAscending(tables.sites.boundFunctionsAtSites));
    RELEASE_ASSERT(isStrictlyAscending(tables.sites.closuresStoredUnderKeys));
    RELEASE_ASSERT(isStrictlyAscending(tables.sites.dataNodes));

    RELEASE_ASSERT(isStrictlyAscending(tables.builtins.locations));
    RELEASE_ASSERT(isStrictlyAscending(tables.builtins.uses));
}

template<typename Record, typename IDType>
static const Record& recordAt(const Vector<Record>& table LIFETIME_BOUND, IDType id, const Record& unknown LIFETIME_BOUND)
{
    return id.index() < table.size() ? table[id.index()] : unknown;
}

template<typename Record, typename SortKey>
static const Record* findRecord(const Vector<Record>& table LIFETIME_BOUND, const SortKey& key)
{
    const Record* record = std::ranges::lower_bound(table, key, { }, [](const Record& candidate) {
        return sortKeyOf(candidate);
    });
    return record != table.end() && sortKeyOf(*record) == key ? record : nullptr;
}

static bool isAnalyzed(const FlowFunctionTables& tables, FunctionID id)
{
    return !ProvisionalFunctions::contains(id) && id.index() < tables.functions.size();
}

static const Derivation* findDerivation(const Program& program, FunctionID id)
{
    if (!ProvisionalFunctions::contains(id) && id.index() >= program.functions.size())
        return nullptr;
    const Derivation& derivation = program.at(id).derivation;
    RELEASE_ASSERT(!derivation.source || ProvisionalFunctions::contains(id) || derivation.source.index() < id.index());
    return &derivation;
}

FlowResults::FlowResults() = default;
FlowResults::FlowResults(FlowResults&&) = default;
FlowResults& FlowResults::operator=(FlowResults&&) = default;
FlowResults::~FlowResults() = default;

static Vector<AllocationClassID> sortClosureClassesByFunction(const Vector<AllocationClass>& classes)
{
    Vector<AllocationClassID> closureClasses;
    for (unsigned index = 0; index < classes.size(); ++index) {
        if (classes[index].closureFunction)
            closureClasses.append(AllocationClassID(index));
    }
    AOT::stableSortInLine(closureClasses, { }, [&](AllocationClassID id) {
        return classes[id.index()].closureFunction;
    });
    return closureClasses;
}

FlowResults::FlowResults(FlowResultTables&& tables)
    : m_tables(WTF::move(tables))
    , m_closureClassesByFunction(sortClosureClassesByFunction(m_tables.sites.allocationClasses))
    , m_unseenCodeCanRun(std::ranges::any_of(m_tables.wholeProgram.unseenCode, admitsUnknownJavaScript, &UnseenCodeRecord::cause))
    , m_unseenScriptsCanRun(std::ranges::any_of(m_tables.wholeProgram.unseenCode, admitsScripts, &UnseenCodeRecord::cause))
{
    checkTableOrder(m_tables);
}

FunctionID FlowResults::findAnalyzedFunction(FunctionID id) const
{
    if (!m_tables.program)
        return { };
    while (id && !isAnalyzed(m_tables.functions, id)) {
        const Derivation* derivation = findDerivation(*m_tables.program, id);
        id = derivation ? derivation->source : FunctionID();
    }
    return id;
}

std::optional<uint32_t> FlowResults::findAnalyzedParameter(FunctionID& function, uint32_t entryParameter) const
{
    if (!m_tables.program)
        return std::nullopt;
    while (function && !isAnalyzed(m_tables.functions, function)) {
        const Derivation* derivation = findDerivation(*m_tables.program, function);
        if (!derivation || entryParameter >= derivation->parameterMap.size())
            return std::nullopt;
        std::optional<uint32_t> original = derivation->parameterMap[entryParameter].original;
        if (!original)
            return std::nullopt;
        entryParameter = *original;
        function = derivation->source;
    }
    if (!function)
        return std::nullopt;
    return entryParameter;
}

std::optional<uint32_t> FlowResults::findDerivedParameter(FunctionID derived, uint32_t analyzedParameter) const
{
    std::optional<uint32_t> found;
    unsigned numberOfMatches = 0;
    const Vector<ParameterOrigin>& parameterMap = m_tables.program->at(derived).derivation.parameterMap;
    for (uint32_t parameter = 0; parameter < parameterMap.size(); ++parameter) {
        FunctionID function = derived;
        if (findAnalyzedParameter(function, parameter) != analyzedParameter)
            continue;
        found = parameter;
        ++numberOfMatches;
    }
    return numberOfMatches == 1 ? found : std::nullopt;
}

const FunctionFlow& FlowResults::function(FunctionID id) const { return recordAt(m_tables.functions.functions, findAnalyzedFunction(id), m_unknown.function); }
const ModuleFlow& FlowResults::module(ModuleID id) const { return recordAt(m_tables.wholeProgram.modules, id, m_unknown.module); }
const AccessSite& FlowResults::at(AccessSiteID id) const { return recordAt(m_tables.sites.accessSites, id, m_unknown.accessSite); }

Wire::SourceLocation FlowResults::writtenLocationOf(AccessSiteID id) const
{
    Wire::SourceLocation location = at(id).location;
    const Vector<WrittenPositionOfSite>& written = m_tables.sites.writtenPositionsOfSites;
    auto found = std::ranges::lower_bound(written, id.index(), { }, [](const WrittenPositionOfSite& entry) { return entry.site.index(); });
    if (found != written.end() && found->site == id)
        location.position = found->position;
    return location;
}
const AllocationSite& FlowResults::at(AllocationSiteID id) const { return recordAt(m_tables.sites.allocationSites, id, m_unknown.allocationSite); }
const AllocationClass& FlowResults::at(AllocationClassID id) const { return recordAt(m_tables.sites.allocationClasses, id, m_unknown.allocationClass); }
const FlowSiteLoss& FlowResults::lossOf(AccessSiteID id) const { return recordAt(m_tables.sites.accessSiteLosses, id, m_unknown.loss); }
bool FlowResults::mayCallBoundFunction(AccessSiteID call) const { return recordAt(m_tables.sites.callsOfBoundFunctions, call, false); }

const FlowProxiesSeenAtSite* FlowResults::findProxiesSeenAt(AccessSiteID access) const { return access ? findRecord(m_tables.sites.proxiesSeenAtSites, access) : nullptr; }

bool FlowResults::mayChangeStorageOfReceiver(AccessSiteID access) const
{
    const Vector<bool>& mayHaveDecidedStorage = m_tables.sites.receiversThatMayHaveDecidedStorage;
    return mayHaveDecidedStorage.isEmpty() || (access && !recordAt(mayHaveDecidedStorage, access, true));
}

std::optional<LayoutSetID> FlowResults::receiversSeenAt(AccessSiteID access) const
{
    uint32_t seen = access ? recordAt(m_tables.sites.receiversSeenAtSites, access, 0u) : 0;
    if (!seen)
        return std::nullopt;
    return seen == 1 ? LayoutSetID() : LayoutSetID(seen - 2);
}

AllocationSiteID FlowResults::typedResultSite(LayoutID layout) const { return recordAt(m_tables.sites.typedResultSites, layout, AllocationSiteID()); }
const BuiltinLocationFlow* FlowResults::findBuiltinLocation(BuiltinID holder, PropertyKeyID key) const { return findRecord(m_tables.builtins.locations, std::tuple(holder, key)); }

Expected<std::span<const FlowBoundFunctionAtSite>> FlowResults::boundFunctionsSeenAt(AccessSiteID site, FlowBoundFunctionUseAtSite use) const
{
    auto [first, end] = std::ranges::equal_range(m_tables.sites.boundFunctionsAtSites, std::pair(site, use), { }, [](const FlowBoundFunctionAtSite& bound) {
        return std::pair(bound.site, bound.use);
    });
    return std::span<const FlowBoundFunctionAtSite>(first, end);
}

Expected<std::span<const FlowClosureStoredUnderKey>> FlowResults::closuresSeenStoredUnder(PropertyKeyID key) const
{
    auto [first, end] = std::ranges::equal_range(m_tables.sites.closuresStoredUnderKeys, key, { }, &FlowClosureStoredUnderKey::key);
    return std::span<const FlowClosureStoredUnderKey>(first, end);
}

bool FlowResults::runsAtMostOnce(const Program& program, FunctionID id) const
{
    ASSERT_UNUSED(program, !m_tables.program || m_tables.program == &program);
    return runsAtMostOnce(id);
}

ClosureUse FlowResults::closureUseOfBoundFunction(FunctionID target, uint32_t numberOfBoundArguments) const
{
    const FlowBoundFunctionUse* use = findRecord(m_tables.functions.boundFunctionUses, std::tuple(findAnalyzedFunction(target), numberOfBoundArguments));
    return use ? use->closureUse : ClosureUse::Unknown;
}

bool FlowResults::closuresAreNeverCompared(FunctionID id) const
{
    FunctionID analyzed = findAnalyzedFunction(id);
    return analyzed && m_tables.functions.functionsWhoseClosuresAreNeverCompared.get(analyzed.index());
}

OperandSummary FlowResults::parameterSummary(FunctionID function, uint32_t entryParameter) const
{
    FunctionID analyzed = function;
    std::optional<uint32_t> parameter = findAnalyzedParameter(analyzed, entryParameter);
    if (!parameter)
        return { };
    const Vector<OperandSummary>& summaries = m_tables.functions.parameterSummaries[analyzed.index()];
    if (*parameter >= summaries.size())
        return { };

    OperandSummary summary = summaries[*parameter];
    if (analyzed == function || summary.retainedIn != RetainedIn::Operand)
        return summary;
    if (std::optional<uint32_t> retainingOperand = findDerivedParameter(function, summary.retainingOperand)) {
        summary.retainingOperand = *retainingOperand;
        return summary;
    }
    summary.retainedIn = RetainedIn::UnknownMemory;
    summary.retainingOperand = 0;
    return summary;
}

void FlowResults::removeEntryParameters(FunctionID function, const BitVector& removedEntryParameters)
{
    if (removedEntryParameters.isEmpty() || !hasResults() || !isAnalyzed(m_tables.functions, function))
        return;
    RELEASE_ASSERT(!m_borrowedParameters.size());

    Vector<OperandSummary>& summaries = m_tables.functions.parameterSummaries[function.index()];
    Vector<uint32_t> newIndexes(summaries.size());
    uint32_t numberOfRemainingParameters = 0;
    for (uint32_t parameter = 0; parameter < summaries.size(); ++parameter) {
        newIndexes[parameter] = numberOfRemainingParameters;
        numberOfRemainingParameters += !removedEntryParameters.get(parameter);
    }

    for (uint32_t parameter = 0; parameter < summaries.size(); ++parameter) {
        if (removedEntryParameters.get(parameter))
            continue;
        OperandSummary summary = summaries[parameter];
        if (summary.retainedIn == RetainedIn::Operand) {
            bool hasRetainingOperand = summary.retainingOperand < summaries.size() && !removedEntryParameters.get(summary.retainingOperand);
            summary.retainedIn = hasRetainingOperand ? RetainedIn::Operand : RetainedIn::UnknownMemory;
            summary.retainingOperand = hasRetainingOperand ? newIndexes[summary.retainingOperand] : 0;
        }
        summaries[newIndexes[parameter]] = summary;
    }
    summaries.shrink(numberOfRemainingParameters);
}

void FlowResults::replaceEntryParameter(FunctionID function, uint32_t entryParameter, const OperandSummary& summaryOfNewValue)
{
    if (!hasResults() || !isAnalyzed(m_tables.functions, function))
        return;
    RELEASE_ASSERT(!m_borrowedParameters.size());

    Vector<OperandSummary>& summaries = m_tables.functions.parameterSummaries[function.index()];
    if (entryParameter >= summaries.size())
        return;
    summaries[entryParameter] = summaryOfNewValue;
    for (OperandSummary& summary : summaries) {
        if (summary.retainedIn == RetainedIn::Operand && summary.retainingOperand == entryParameter)
            summary = { summary.uses, RetainedIn::UnknownMemory, 0 };
    }
}

void FlowResults::setOperandSummaries(Vector<Vector<OperandSummary>>&& parameterSummaries, Vector<Vector<OperandUses>>&& storedValueUses)
{
    RELEASE_ASSERT(parameterSummaries.size() == m_tables.functions.functions.size());
    m_tables.functions.parameterSummaries = WTF::move(parameterSummaries);
    m_tables.functions.storedValueUses = WTF::move(storedValueUses);
}

static OperandUses usesOfFieldValue(const Vector<Vector<OperandUses>>& usesByLayout, FieldReference reference)
{
    if (reference.layout.index() >= usesByLayout.size())
        return OperandUses::all();
    const Vector<OperandUses>& usesByField = usesByLayout[reference.layout.index()];
    return reference.field < usesByField.size() ? usesByField[reference.field] : OperandUses::all();
}

OperandUses FlowResults::storedValueUses(FieldReference reference) const { return usesOfFieldValue(m_tables.functions.storedValueUses, reference); }
OperandUses FlowResults::storedFramePointerUses(FieldReference reference) const { return usesOfFieldValue(m_tables.functions.storedFramePointerUses, reference); }

std::span<const FlowIndirectCall> FlowResults::indirectCallsFrom(FunctionID callerAsRead) const
{
    const Vector<FlowIndirectCall>& calls = m_tables.functions.indirectCalls;
    auto callsFromCaller = std::ranges::equal_range(calls, callerAsRead, { }, &FlowIndirectCall::caller);
    return calls.subspan(callsFromCaller.begin() - calls.begin(), callsFromCaller.size());
}

bool builtinHolderNeverGains(const FlowBuiltinTables& tables, BuiltinID holder, PropertyKeyID key)
{
    if (!tables.intactHolders.get(holder.index()))
        return false;
    const BuiltinUseRecord* uses = findRecord(tables.uses, holder.index());
    if (uses && OptionSet<BuiltinUse>::fromRaw(static_cast<uint32_t>(uses->uses)).contains(BuiltinUse::Escaped))
        return false;
    return !findRecord(tables.locations, std::tuple(holder, key));
}

static bool keepsCreatedMembers(const AllocationClass& closures, std::span<const PropertyKeyID> ownKeys)
{
    constexpr OptionSet<ObjectMutation> mutationsChangingCreatedState { ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype };
    if (!closures.impossibleMutations.containsAll(mutationsChangingCreatedState) || !closures.addedKeysAreComplete)
        return false;
    return std::ranges::none_of(ownKeys, [&](PropertyKeyID key) {
        return closures.addedKeys.contains(key);
    });
}

static bool holdsOfEveryClosureClass(const Vector<AllocationClass>& classes, const Vector<AllocationClassID>& closureClassesByFunction, FunctionID closureFunction, NOESCAPE const Invocable<bool(const AllocationClass&)> auto& predicate)
{
    auto closureClasses = std::ranges::equal_range(closureClassesByFunction, closureFunction, { }, [&](AllocationClassID id) {
        return classes[id.index()].closureFunction;
    });
    return !closureClasses.empty() && std::ranges::all_of(closureClasses, [&](AllocationClassID id) {
        return predicate(classes[id.index()]);
    });
}

bool FlowResults::areClosuresAsCreated(FunctionID closureFunction, std::span<const PropertyKeyID> ownKeys) const
{
    return holdsOfEveryClosureClass(m_tables.sites.allocationClasses, m_closureClassesByFunction, closureFunction, [&](const AllocationClass& closures) {
        return keepsCreatedMembers(closures, ownKeys);
    });
}

bool FlowResults::closuresMayBeRenamed(FunctionID closureFunction) const
{
    if (!hasResults())
        return true;
    FunctionID analyzed = findAnalyzedFunction(closureFunction);
    return analyzed && m_tables.functions.functionsWhoseClosuresMayBeRenamed.get(analyzed.index());
}

bool FlowResults::closuresMayBeRenamedOnlyWhereNobodySees(FunctionID closureFunction) const
{
    FunctionID analyzed = findAnalyzedFunction(closureFunction);
    return analyzed && m_tables.functions.functionsWhoseClosuresMayBeRenamedOnlyWhereNobodySees.get(analyzed.index());
}

bool FlowResults::closuresGoOutOfSightUnnamed(FunctionID closureFunction) const
{
    FunctionID analyzed = findAnalyzedFunction(closureFunction);
    return analyzed && m_tables.functions.functionsWhoseClosuresGoOutOfSightUnnamed.get(analyzed.index());
}

bool FlowResults::closuresMayBeNamedOutOfSight(FunctionID closureFunction) const
{
    if (!hasResults())
        return true;
    FunctionID analyzed = findAnalyzedFunction(closureFunction);
    return analyzed && m_tables.functions.functionsWhoseClosuresMayBeNamedOutOfSight.get(analyzed.index());
}

bool FlowResults::closuresAreGivenDisplayName(FunctionID closureFunction) const
{
    if (!hasResults())
        return true;
    FunctionID analyzed = findAnalyzedFunction(closureFunction);
    return analyzed && m_tables.functions.functionsWhoseClosuresAreGivenDisplayName.get(analyzed.index());
}

bool FlowResults::closureIsMadeOnce(FunctionID closureFunction) const
{
    FunctionID analyzed = findAnalyzedFunction(closureFunction);
    return analyzed && m_tables.functions.functionsWhoseClosuresAreMadeOnce.get(analyzed.index());
}

bool FlowResults::closuresGoUnnoticed(FunctionID closureFunction) const
{
    constexpr OptionSet<ObjectMutation> mutationsThatShowOrFill { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::RedefineProperty, ObjectMutation::AddProperty, ObjectMutation::ChangePrototype };
    FunctionID analyzed = findAnalyzedFunction(closureFunction);
    if (!analyzed || !closuresAreNeverCompared(analyzed))
        return false;
    if (function(analyzed).closureUse == ClosureUse::CalledOnly)
        return true;
    return holdsOfEveryClosureClass(m_tables.sites.allocationClasses, m_closureClassesByFunction, analyzed, [&](const AllocationClass& closures) {
        return closures.impossibleMutations.containsAll(mutationsThatShowOrFill) && closures.addedKeysAreComplete && closures.addedKeys.isEmpty();
    });
}

bool FlowResults::closuresInheritFromFunctionPrototype(FunctionID closureFunction, PropertyKeyID key) const
{
    return holdsOfEveryClosureClass(m_tables.sites.allocationClasses, m_closureClassesByFunction, closureFunction, [&](const AllocationClass& closures) {
        return closures.inheritsFromListedFunctions && !closures.addedKeys.contains(key);
    });
}

bool FlowResults::closuresAreStoredUnder(FunctionID closureFunction, PropertyKeyID key) const
{
    FunctionID analyzed = findAnalyzedFunction(closureFunction);
    return analyzed && holdsOfEveryClosureClass(m_tables.sites.allocationClasses, m_closureClassesByFunction, analyzed, [&](const AllocationClass& closures) {
        return closures.addedKeys.contains(key);
    });
}

bool FlowResults::closuresKeepTheirIntegrity(FunctionID closureFunction) const
{
    FunctionID analyzed = findAnalyzedFunction(closureFunction);
    return analyzed && !m_tables.functions.functionsWhoseClosuresChangeIntegrity.get(analyzed.index());
}

bool FlowResults::closuresMeetOneOf(FunctionID closureFunction, OptionSet<ObjectMutation> mutations) const
{
    return holdsOfEveryClosureClass(m_tables.sites.allocationClasses, m_closureClassesByFunction, closureFunction, [&](const AllocationClass& closures) {
        return closures.impossibleMutations.contains(ObjectMutation::ReachUnknownJavaScript) && !closures.impossibleMutations.containsAll(mutations);
    });
}

bool FlowResults::seesOnlyFinishedObjects(AccessSiteID id) const
{
    FunctionID functionAsRead = at(id).function;
    return functionAsRead && m_tables.functions.functionsSeeingOnlyFinishedObjects.get(functionAsRead.index());
}

const SinkCandidates& FlowResults::sinkCandidates(AccessSiteID call, uint32_t operandIndex) const
{
    const FlowSinkOperand* operand = findRecord(m_tables.sites.sinkOperands, std::tuple(call, operandIndex));
    return operand ? operand->candidates : m_unknown.sinkCandidates;
}

bool FlowResults::isTypedResultUnprofitable(AccessSiteID call) const
{
    const FlowTypedResultCall* record = findRecord(m_tables.sites.typedResultCalls, call);
    return record && record->isUnprofitable;
}

void FlowResults::markTypedResultsUnprofitable(std::span<const AccessSiteID> sortedCalls)
{
    for (FlowTypedResultCall& record : m_tables.sites.typedResultCalls)
        record.isUnprofitable |= std::ranges::binary_search(sortedCalls, record.call);
}

TypeID FlowResults::typedResultType(AccessSiteID call) const
{
    const FlowTypedResultCall* record = findRecord(m_tables.sites.typedResultCalls, call);
    return record ? record->type : TypeID();
}

std::span<const LayoutID> FlowResults::typedResultLayouts(AccessSiteID call) const
{
    const FlowTypedResultCall* record = findRecord(m_tables.sites.typedResultCalls, call);
    return record ? record->layouts.span() : std::span<const LayoutID>();
}

FlowDataNodeStamp FlowResults::dataNodeStamp(AllocationSiteID rootSite, uint32_t ordinalInPreorder) const
{
    const FlowDataNode* node = findRecord(m_tables.sites.dataNodes, std::tuple(rootSite, ordinalInPreorder));
    return node ? node->stamp : FlowDataNodeStamp();
}

OptionSet<BuiltinUse> FlowResults::usesOf(BuiltinID builtin) const
{
    if (!hasResults())
        return OptionSet<BuiltinUse>::all();
    const BuiltinUseRecord* record = builtin ? findRecord(m_tables.builtins.uses, builtin.index()) : nullptr;
    return record ? OptionSet<BuiltinUse>::fromRaw(static_cast<uint32_t>(record->uses)) : OptionSet<BuiltinUse>();
}

// A member of the host without a guard of its own has that of the nearest holder.
static std::span<const BuiltinFeatureUse> linesWithGuardOf(const BuiltinTable& builtins, BuiltinID builtin)
{
    for (BuiltinID guarded = builtin; guarded; guarded = builtins.at(guarded).holder) {
        std::span<const BuiltinFeatureUse> lines = builtins.featureUsesOf(builtins.canonicalID(guarded));
        if (std::ranges::any_of(lines, &BuiltinFeatureUse::isGuard))
            return lines;
        if (builtins.at(guarded).origin != BuiltinOrigin::Host)
            break;
    }
    return { };
}

// Removing this feature puts a function that throws in place of the built-in's native code.
std::optional<RemovableFeature> guardOf(const BuiltinTable& builtins, BuiltinID builtin)
{
    for (const BuiltinFeatureUse& line : linesWithGuardOf(builtins, builtin)) {
        if (line.isGuard)
            return line.feature;
    }
    return std::nullopt;
}

static bool contains(const FeatureBits& features, uint32_t feature)
{
    return features[feature / 64] >> feature % 64 & 1;
}

bool isGuardedByRemovedFeature(const BuiltinTable& builtins, BuiltinID builtin, const FeatureBits& removedFeatures)
{
    std::optional<RemovableFeature> guard = guardOf(builtins, builtin);
    return guard && contains(removedFeatures, static_cast<uint32_t>(*guard));
}

BitVector findBuiltinsThatMayBeTrimmed(const BuiltinTable& builtins, std::span<const FeatureUseRecord> featureUses, const AOT::CompileOptions& options)
{
    FeatureBits usedFeatures { };
    for (const FeatureUseRecord& use : featureUses) {
        if (use.kind == FeatureUseKind::Named || !contains(options.featuresKeptOnlyWhenNamed, use.feature))
            usedFeatures[use.feature / 64] |= 1ull << use.feature % 64;
    }

    // The function that throws also stands there where nothing uses a feature that the native code cannot run without. Which of the features those are is known where the host is linked: they are among the ones that the built-in uses whatever it is called with.
    BitVector mayBeTrimmed(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        mayBeTrimmed.quickSet(index, std::ranges::any_of(linesWithGuardOf(builtins, BuiltinID(index)), [&](const BuiltinFeatureUse& line) {
            uint32_t feature = static_cast<uint32_t>(line.feature);
            if (line.isGuard && contains(options.removedFeatures, feature))
                return true;
            return !line.condition && !line.overload && !contains(usedFeatures, feature);
        }));
    }
    return mayBeTrimmed;
}

OperandSummary publishedParameterSummary(const FlowResults& flow, FunctionID function, uint32_t entryParameter) { return flow.parameterSummary(function, entryParameter); }
CallerState callerStateOf(const FlowResults& flow, FunctionID function) { return flow.function(function).callers; }
bool runsAtMostOnce(const FlowResults& flow, FunctionID function) { return flow.runsAtMostOnce(function); }

BitVector functionsCalledIndirectly(const FlowResults& flow, size_t numberOfFunctions)
{
    BitVector result(numberOfFunctions);
    for (const FlowIndirectCall& call : flow.indirectCalls())
        result.set(call.callee.index());
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
