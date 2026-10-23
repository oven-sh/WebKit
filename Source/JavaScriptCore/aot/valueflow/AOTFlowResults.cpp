/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResults.h"

#if ENABLE(AOT)

#include "AOTEffectSummaries.h"
#include "AOTFlowUnseenCode.h"
#include "AOTProvisionalFunctions.h"
#include "AOTRelationalSummaries.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <tuple>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT { namespace Typed {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FlowResults);

static_assert(Witness { } == noWitness, "A default-constructed record names no witness.");
static_assert(SourceLocation { } == noSourceLocation, "A default-constructed record names no location.");

static auto sortKeyOf(const FlowIndirectCall& call) { return std::tuple(call.caller, call.site, call.callee, call.kind); }
static auto sortKeyOf(const FlowBoundFunctionUse& use) { return std::tuple(use.target, use.numberOfBoundArguments); }
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
    RELEASE_ASSERT(isStrictlyAscending(tables.sites.sinkOperands));
    RELEASE_ASSERT(isStrictlyAscending(tables.sites.typedResultCalls));
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
    AOT::stableSort(closureClasses, { }, [&](AllocationClassID id) {
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

const FunctionFlow& FlowResults::function(FunctionID id) const
{
    return recordAt(m_tables.functions.functions, findAnalyzedFunction(id), m_unknown.function);
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

bool FlowResults::hasOneClosure(FunctionID id) const
{
    return id.index() < m_tables.functions.functionCountAsRead && m_tables.functions.functionsWithOneClosure.get(id.index());
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

void FlowResults::setRelationalSummaries(std::unique_ptr<RelationalSummaries>&& table)
{
    m_relationalSummaries = WTF::move(table);
}

LinkedFate FlowResults::fateOfParameter(FunctionID function, uint32_t entryParameter) const
{
    if (!m_relationalSummaries)
        return LinkedFate::anything();
    FunctionID analyzed = function;
    std::optional<uint32_t> parameter = findAnalyzedParameter(analyzed, entryParameter);
    if (!parameter)
        return LinkedFate::anything();
    LinkedFate fate = m_relationalSummaries->fateOfParameter(analyzed, *parameter);
    if (analyzed == function || fate.isAnything())
        return fate;

    // A version numbers its parameters and its results in its own way.
    uint32_t storedIntoParameters = 0;
    for (uint32_t rest = fate.storedIntoParameters; rest; rest &= rest - 1) {
        std::optional<uint32_t> derived = findDerivedParameter(function, static_cast<uint32_t>(std::countr_zero(rest)));
        if (derived && *derived < 32)
            storedIntoParameters |= 1u << *derived;
        else
            fate.itself.add(ValueFate::IsStoredInUnknownObject);
    }
    fate.storedIntoParameters = storedIntoParameters;
    if (fate.results)
        fate.results = UINT32_MAX;
    if (fate.storedIntoResults)
        fate.storedIntoResults = UINT32_MAX;
    return fate;
}

// A call of such a version answers what a call of the function answers that it was derived from. A worker answers parts, a constructor's body takes the object, a ramp answers a promise.
static bool answersLikeItsSource(FunctionRole role)
{
    return role == FunctionRole::Source || role == FunctionRole::Library || role == FunctionRole::GenericVersion || role == FunctionRole::CallSiteVersion || role == FunctionRole::CalleeVersion
        || role == FunctionRole::EntryChecks || role == FunctionRole::ParameterWrapper || role == FunctionRole::ResultWrapper;
}

ResultOrigin FlowResults::originOfResult(FunctionID function, uint32_t result) const
{
    if (!m_relationalSummaries || !m_tables.program)
        return { };
    FunctionID analyzed = function;
    while (analyzed && !isAnalyzed(m_tables.functions, analyzed)) {
        const Derivation* derivation = findDerivation(*m_tables.program, analyzed);
        if (!derivation || !answersLikeItsSource(m_tables.program->at(analyzed).role))
            return { };
        analyzed = derivation->source;
    }
    if (!analyzed)
        return { };
    // It has the body of a constructor function with an object of its own in the place of `this`, and no record. The constructor answers for it.
    if (const Function& asked = m_tables.program->at(analyzed); asked.has(FunctionFlags::AllocatesOwnThis)) {
        ResultOrigin made;
        made.isFresh = asked.derivation.source && m_relationalSummaries->originOfResult(asked.derivation.source, result).isFreshWhenConstructed;
        return made;
    }
    ResultOrigin origin = m_relationalSummaries->originOfResult(analyzed, result);
    if (analyzed == function || !origin.isFullyListed)
        return origin;

    // A version numbers its parameters in its own way. One that it no longer takes is a value that nobody can list.
    for (LinkedOrigin& listed : origin.origins) {
        if (listed.kind != LinkedOrigin::Kind::Parameter)
            continue;
        if (std::optional<uint32_t> derived = findDerivedParameter(function, listed.parameter))
            listed.parameter = *derived;
        else
            listed.kind = LinkedOrigin::Kind::Anything;
    }
    bool hasUnlistedRoot = std::ranges::any_of(origin.mayBe, [&](uint32_t root) {
        return origin.origins[root].kind == LinkedOrigin::Kind::Anything && !origin.origins[root].type;
    });
    return hasUnlistedRoot ? ResultOrigin { } : origin;
}

void FlowResults::removeEntryParameters(FunctionID function, const BitVector& removedEntryParameters)
{
    if (removedEntryParameters.isEmpty() || !hasResults() || !isAnalyzed(m_tables.functions, function))
        return;
    RELEASE_ASSERT(!m_borrowedParameters.size());
    if (m_relationalSummaries)
        m_relationalSummaries->removeEntryParameters(function, removedEntryParameters);

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

OperandUses FlowResults::storedValueUses(FieldReference reference) const
{
    return usesOfFieldValue(m_tables.functions.storedValueUses, reference);
}

OperandSummary FlowResults::framePointerSummary(FunctionID function, uint32_t entryParameter) const
{
    return m_borrowedParameters.summaryOf(function, entryParameter);
}

OperandUses FlowResults::storedFramePointerUses(FieldReference reference) const
{
    return usesOfFieldValue(m_tables.functions.storedFramePointerUses, reference);
}

std::span<const FlowIndirectCall> FlowResults::indirectCallsFrom(FunctionID callerAsRead) const
{
    const Vector<FlowIndirectCall>& calls = m_tables.functions.indirectCalls;
    auto callsFromCaller = std::ranges::equal_range(calls, callerAsRead, { }, &FlowIndirectCall::caller);
    return calls.subspan(callsFromCaller.begin() - calls.begin(), callsFromCaller.size());
}

const ModuleFlow& FlowResults::module(ModuleID id) const
{
    return recordAt(m_tables.wholeProgram.modules, id, m_unknown.module);
}

BuiltinLocationState FlowResults::builtinLocation(BuiltinID member) const
{
    return member && m_tables.builtins.constantMembers.get(member.index()) ? BuiltinLocationState::Constant : BuiltinLocationState::Unknown;
}

BuiltinHolderState FlowResults::builtinHolder(BuiltinID holder) const
{
    return holder && m_tables.builtins.intactHolders.get(holder.index()) ? BuiltinHolderState::Intact : BuiltinHolderState::Unknown;
}

const BuiltinLocationFlow* FlowResults::findBuiltinLocation(BuiltinID holder, PropertyKeyID key) const
{
    return findRecord(m_tables.builtins.locations, std::tuple(holder, key));
}

const AccessSite& FlowResults::at(AccessSiteID id) const
{
    return recordAt(m_tables.sites.accessSites, id, m_unknown.accessSite);
}

const AllocationSite& FlowResults::at(AllocationSiteID id) const
{
    return recordAt(m_tables.sites.allocationSites, id, m_unknown.allocationSite);
}

const AllocationClass& FlowResults::at(AllocationClassID id) const
{
    return recordAt(m_tables.sites.allocationClasses, id, m_unknown.allocationClass);
}

bool keepsCreatedMembers(const AllocationClass& closures, std::span<const PropertyKeyID> ownKeys)
{
    constexpr OptionSet<ObjectMutation> mutationsChangingCreatedState { ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype };
    if (!closures.impossibleMutations.containsAll(mutationsChangingCreatedState) || !closures.addedKeysAreComplete)
        return false;
    return std::ranges::none_of(ownKeys, [&](PropertyKeyID key) {
        return closures.addedKeys.contains(key);
    });
}

bool FlowResults::areClosuresAsCreated(FunctionID closureFunction, std::span<const PropertyKeyID> ownKeys) const
{
    const Vector<AllocationClass>& classes = m_tables.sites.allocationClasses;
    auto closureClasses = std::ranges::equal_range(m_closureClassesByFunction, closureFunction, { }, [&](AllocationClassID id) {
        return classes[id.index()].closureFunction;
    });
    return !closureClasses.empty() && std::ranges::all_of(closureClasses, [&](AllocationClassID id) {
        return keepsCreatedMembers(classes[id.index()], ownKeys);
    });
}

const FlowSiteLoss& FlowResults::lossOf(AccessSiteID id) const
{
    return recordAt(m_tables.sites.accessSiteLosses, id, m_unknown.loss);
}

bool FlowResults::seesOnlyFinishedObjects(AccessSiteID id) const
{
    FunctionID functionAsRead = at(id).function;
    return functionAsRead && m_tables.functions.functionsSeeingOnlyFinishedObjects.get(functionAsRead.index());
}

bool FlowResults::mayCallBoundFunction(AccessSiteID call) const
{
    return recordAt(m_tables.sites.callsOfBoundFunctions, call, false);
}

const SinkCandidates& FlowResults::sinkCandidates(AccessSiteID call, uint32_t operandIndex) const
{
    const FlowSinkOperand* operand = findRecord(m_tables.sites.sinkOperands, std::tuple(call, operandIndex));
    return operand ? operand->candidates : m_unknown.sinkCandidates;
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

AllocationSiteID FlowResults::typedResultSite(LayoutID layout) const
{
    return recordAt(m_tables.sites.typedResultSites, layout, AllocationSiteID());
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

OperandSummary publishedParameterSummary(const FlowResults& flow, FunctionID function, uint32_t entryParameter)
{
    return flow.parameterSummary(function, entryParameter);
}

CallerState callerStateOf(const FlowResults& flow, FunctionID function)
{
    return flow.function(function).callers;
}

bool runsAtMostOnce(const FlowResults& flow, FunctionID function)
{
    return flow.runsAtMostOnce(function);
}

BitVector functionsCalledIndirectly(const FlowResults& flow, size_t numberOfFunctions)
{
    BitVector result(numberOfFunctions);
    for (const FlowIndirectCall& call : flow.indirectCalls())
        result.set(call.callee.index());
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
