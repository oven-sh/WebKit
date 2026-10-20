/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTChooseCallingConventionsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTDiagnostics.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTThreads.h"
#include <algorithm>
#include <compare>
#include <ranges>
#include <wtf/Atomics.h>
#include <wtf/UnionFind.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ResultNode : WTF::UnionFind<ResultNode> { };

struct TailCallEdge {
    friend auto operator<=>(const TailCallEdge&, const TailCallEdge&) = default;

    FunctionID callee;
    FunctionID caller;
};

class ResultLocations {
public:
    ResultLocations(const ConventionInputs&, const CallingConventionTables&);

    Representation chosenRepresentation(FunctionID id) const { return m_locations[id.index()].representation; }
    bool returnsNothing(FunctionID id) const { return m_locations[id.index()].kind == ArgumentLocation::Kind::None; }
    void recompute(FunctionID);

private:
    const Program& m_program;
    const CallingConventionTables& m_tables;
    Vector<ArgumentLocation> m_locations;
};

class ResultChooser {
public:
    ResultChooser(const ConventionInputs&, std::span<const TailCallSite>, const DirectCallUse&, CallingConventionTables&);

    void boxMixedClasses();
    void boxTailCallees();
    uint64_t numberOfBoxedResults() const { return m_numberOfBoxedResults; }

private:
    void box(FunctionID);
    void boxMemberOfMixedClass(FunctionID);
    bool haveSameResult(FunctionID, FunctionID) const;

    bool hasRequiredTailCall(FunctionID) const;
    bool canBoxTailCallee(FunctionID) const;
    BitVector findBoxableTailCallees() const;
    Vector<TailCallEdge> collectEdgesTo(const BitVector& callees) const;
    bool hasOnlyBoxedCallers(std::span<const TailCallEdge>, FunctionID callee) const;
    std::span<const FunctionID> functionsSharingResult(const FunctionID&) const LIFETIME_BOUND;

    const ConventionInputs& m_context;
    const Program& m_program;
    std::span<const TailCallSite> m_sites;
    const DirectCallUse& m_use;
    CallingConventionTables& m_tables;
    ResultLocations m_results;
    uint64_t m_numberOfBoxedResults { 0 };
};

} // anonymous namespace

static ValueID tailCallOf(const Block& block)
{
    return block.terminator.kind == TerminatorKind::ReturnCall ? block.terminator.callOperand() : ValueID();
}

static bool hasCheckedCast(const Terminator& terminator)
{
    return std::ranges::any_of(terminator.casts(), [](const TailCast& cast) {
        switch (cast.resolution) {
        case CastResolution::Unresolved:
        case CastResolution::Claim:
            return true;
        case CastResolution::Nothing:
        case CastResolution::Upcast:
        case CastResolution::Prediction:
        case CastResolution::Assume:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static TailCallSite createTailCallSite(FunctionID caller, const Function& function, BlockID block)
{
    const Terminator& terminator = function.at(block).terminator;
    const Value& call = function.at(terminator.callOperand());
    TailCallSite site { .caller = caller, .block = block, .requirement = terminator.requirement(), .hasCheckedCast = hasCheckedCast(terminator) };
    if (call.opcode == Opcode::CallDirect) {
        site.calleeKind = TailCalleeKind::Direct;
        site.callee = call.function();
    } else if (call.opcode == Opcode::CallGroup) {
        site.calleeKind = TailCalleeKind::Group;
        site.callee = call.function();
    }
    return site;
}

static void collectSitesOf(const Program& program, FunctionID caller, Vector<TailCallSite>& sites)
{
    const Function& function = program.at(caller);
    for (unsigned index = 0; index < function.blocks.size(); ++index) {
        if (function.blocks[index].terminator.kind == TerminatorKind::ReturnCall)
            sites.append(createTailCallSite(caller, function, BlockID(index)));
    }
}

static std::span<const TailCallSite> sitesOf(std::span<const TailCallSite> sites, FunctionID caller)
{
    auto found = std::ranges::equal_range(sites, caller, { }, &TailCallSite::caller);
    return sites.subspan(found.begin() - sites.begin(), found.size());
}

static BitVector createBitVector(const Vector<uint8_t>& flags)
{
    BitVector bits(flags.size());
    for (size_t index = 0; index < flags.size(); ++index) {
        if (flags[index])
            bits.quickSet(index);
    }
    return bits;
}

static bool canBoxResult(const Function& function)
{
    return function.results.size() == 1 && function.hasOrdinaryFrame() && entryConventionOf(function) == EntryConvention::Direct;
}

Vector<TailCallSite> collectTailCallSites(const ConventionInputs& context)
{
    const Program& program = context.program();
    return collectInParallel<TailCallSite>(context.build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<TailCallSite>& sites) {
        collectSitesOf(program, FunctionID(index), sites);
    });
}

static void markEntrypointsOfCalledGroups(const ConventionInputs& context, Vector<uint8_t>& isCalled)
{
    for (const GroupEntrypoints& group : context.groupEntrypoints) {
        bool isGroupCalled = std::ranges::any_of(group.all, [&](FunctionID entrypoint) {
            return !!isCalled[entrypoint.index()];
        });
        if (!isGroupCalled)
            continue;
        for (FunctionID entrypoint : group.all)
            isCalled[entrypoint.index()] = 1;
    }
}

DirectCallUse collectDirectCallUse(const ConventionInputs& context)
{
    const Program& program = context.program();
    unsigned count = program.functions.size();
    Vector<uint8_t> isCalledAndReturnedTo(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> isTailCalled(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(context.build.parallelLoops(), count, [&](unsigned index) {
        const Function& caller = program.functions[index];
        for (const Block& block : caller.blocks) {
            ValueID tailCall = tailCallOf(block);
            for (ValueID id : block.values) {
                const Value& call = caller.at(id);
                if (call.opcode != Opcode::CallDirect && call.opcode != Opcode::CallGroup)
                    continue;
                Vector<uint8_t>& isCalled = id == tailCall ? isTailCalled : isCalledAndReturnedTo;
                WTF::atomicStore(&isCalled[call.function().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
            }
        }
    });
    markEntrypointsOfCalledGroups(context, isCalledAndReturnedTo);
    markEntrypointsOfCalledGroups(context, isTailCalled);
    return { createBitVector(isCalledAndReturnedTo), createBitVector(isTailCalled) };
}

ResultLocations::ResultLocations(const ConventionInputs& context, const CallingConventionTables& tables)
    : m_program(context.program())
    , m_tables(tables)
    , m_locations(m_program.functions.size())
{
    forEachIndexInParallel(context.build.parallelLoops(), m_locations.size(), [&](unsigned index) {
        if (m_program.functions[index].results.size() == 1)
            recompute(FunctionID(index));
    });
}

void ResultLocations::recompute(FunctionID id)
{
    m_locations[id.index()] = computeArgumentLocations(m_program, m_program.at(id), m_tables.conventions[id.index()], m_tables.registers).results[0];
}

ResultChooser::ResultChooser(const ConventionInputs& context, std::span<const TailCallSite> sites, const DirectCallUse& use, CallingConventionTables& tables)
    : m_context(context)
    , m_program(context.program())
    , m_sites(sites)
    , m_use(use)
    , m_tables(tables)
    , m_results(context, tables)
{
}

void ResultChooser::box(FunctionID id)
{
    FunctionConvention& convention = m_tables.conventions[id.index()];
    if (convention.resultIsBoxed)
        return;
    convention.resultIsBoxed = true;
    m_results.recompute(id);
    ++m_numberOfBoxedResults;
}

void ResultChooser::boxMemberOfMixedClass(FunctionID id)
{
    if (canBoxResult(m_program.at(id))) {
        box(id);
        return;
    }
    if (m_results.chosenRepresentation(id) != Representation::JSValue)
        m_context.build.diagnostics().add(InternalError { .kind = InternalErrorKind::RequiredTailCallNotConverted, .function = id, .pass = PassID::ChooseCallingConventions });
}

bool ResultChooser::haveSameResult(FunctionID a, FunctionID b) const
{
    return m_results.chosenRepresentation(a) == m_results.chosenRepresentation(b) && m_results.returnsNothing(a) == m_results.returnsNothing(b);
}

void ResultChooser::boxMixedClasses()
{
    unsigned count = m_program.functions.size();
    unsigned callByValue = count;
    Vector<ResultNode> nodes(count + 1);
    for (const GroupEntrypoints& group : m_context.groupEntrypoints) {
        if (!group.numberOfSites)
            continue;
        for (FunctionID entrypoint : group.all)
            nodes[entrypoint.index()].unify(&nodes[group.declaring().index()]);
    }
    for (const TailCallSite& site : m_sites) {
        if (site.requirement != TailCallRequirement::Required)
            continue;
        unsigned callee = site.calleeKind == TailCalleeKind::Unknown ? callByValue : site.callee.index();
        nodes[site.caller.index()].unify(&nodes[callee]);
    }

    Vector<uint32_t> roots(count + 1);
    for (unsigned index = 0; index <= count; ++index)
        roots[index] = static_cast<uint32_t>(nodes[index].find() - nodes.span().data());

    BitVector isMixed(count + 1);
    isMixed.quickSet(roots[callByValue]);
    for (unsigned index = 0; index < count; ++index) {
        uint32_t root = roots[index];
        if (root != index && root != callByValue && !haveSameResult(FunctionID(index), FunctionID(root)))
            isMixed.quickSet(root);
    }
    for (unsigned index = 0; index < count; ++index) {
        if (isMixed.quickGet(roots[index]))
            boxMemberOfMixedClass(FunctionID(index));
    }
}

bool ResultChooser::hasRequiredTailCall(FunctionID id) const
{
    return std::ranges::any_of(sitesOf(m_sites, id), [](const TailCallSite& site) {
        return site.requirement == TailCallRequirement::Required;
    });
}

bool ResultChooser::canBoxTailCallee(FunctionID id) const
{
    // boxMixedClasses() left the caller and the callee of a required tail call with equal results, so the caller is never boxed alone.
    const Function& function = m_program.at(id);
    return m_use.isTailCalled.get(id.index())
        && !m_tables.conventions[id.index()].resultIsBoxed
        && canBoxResult(function)
        && (!m_use.isCalledAndReturnedTo.get(id.index()) || m_results.returnsNothing(id))
        && !function.mayBeCalledByHost()
        && !hasRequiredTailCall(id)
        && m_context.isEnabledFor(id);
}

BitVector ResultChooser::findBoxableTailCallees() const
{
    unsigned count = m_program.functions.size();
    Vector<uint32_t> indexes = findIndicesInParallel(m_context.build.parallelLoops(), count, [&](unsigned index) {
        return canBoxTailCallee(FunctionID(index));
    });
    BitVector callees(count);
    for (uint32_t index : indexes)
        callees.quickSet(index);

    for (const GroupEntrypoints& group : m_context.groupEntrypoints) {
        if (group.all.isEmpty())
            continue;
        bool canBoxGroup = std::ranges::all_of(group.all, [&](FunctionID entrypoint) {
            return callees.quickGet(entrypoint.index());
        });
        for (FunctionID entrypoint : group.all)
            callees.quickClear(entrypoint.index());
        if (canBoxGroup)
            callees.quickSet(group.declaring().index());
    }
    return callees;
}

Vector<TailCallEdge> ResultChooser::collectEdgesTo(const BitVector& callees) const
{
    Vector<TailCallEdge> edges;
    for (const TailCallSite& site : m_sites) {
        if (site.callee && callees.get(site.callee.index()))
            edges.append(TailCallEdge { site.callee, site.caller });
    }
    sortAndRemoveDuplicates(m_context.build.parallelLoops(), edges);
    return edges;
}

bool ResultChooser::hasOnlyBoxedCallers(std::span<const TailCallEdge> edges, FunctionID callee) const
{
    return std::ranges::all_of(std::ranges::equal_range(edges, callee, { }, &TailCallEdge::callee), [&](const TailCallEdge& edge) {
        return m_results.chosenRepresentation(edge.caller) == Representation::JSValue;
    });
}

std::span<const FunctionID> ResultChooser::functionsSharingResult(const FunctionID& id) const
{
    if (m_program.at(id).role == FunctionRole::GroupEntrypoint)
        return m_context.entrypointsOfSameGroup(id);
    return singleElementSpan(id);
}

void ResultChooser::boxTailCallees()
{
    BitVector callees = findBoxableTailCallees();
    Vector<TailCallEdge> edges = collectEdgesTo(callees);
    Vector<FunctionID> worklist;
    for (size_t index : callees)
        worklist.append(FunctionID(index));

    while (!worklist.isEmpty()) {
        FunctionID callee = worklist.takeLast();
        if (m_tables.conventions[callee.index()].resultIsBoxed || !hasOnlyBoxedCallers(edges.span(), callee))
            continue;
        for (FunctionID function : functionsSharingResult(callee)) {
            box(function);
            for (const TailCallSite& site : sitesOf(m_sites, function)) {
                if (site.callee && callees.get(site.callee.index()))
                    worklist.append(site.callee);
            }
        }
    }
}

static void boxDisabledFunctions(const ConventionInputs& context, CallingConventionTables& tables)
{
    const Program& program = context.program();
    forEachIndexInParallel(context.build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        if (canBoxResult(program.functions[index]) && !context.isEnabledFor(FunctionID(index)))
            tables.conventions[index].resultIsBoxed = true;
    });
}

void chooseResultRepresentations(const ConventionInputs& context, std::span<const TailCallSite> sites, const DirectCallUse& use, CallingConventionTables& tables)
{
    boxDisabledFunctions(context, tables);
    ResultChooser chooser(context, sites, use, tables);
    chooser.boxMixedClasses();
    chooser.boxTailCallees();
    context.build.statistics().count(StatisticID::ResultsBoxedForTailCalls, chooser.numberOfBoxedResults());
}

static bool canRemainTailCall(const ResultLocations& results, const TailCallSite& site)
{
    if (site.hasCheckedCast)
        return false;
    if (results.returnsNothing(site.caller))
        return true;

    Representation expected = results.chosenRepresentation(site.caller);
    switch (site.calleeKind) {
    case TailCalleeKind::Unknown:
        return haveSameRegisterRepresentation(Representation::JSValue, expected);
    case TailCalleeKind::Direct:
    case TailCalleeKind::Group:
        return !results.returnsNothing(site.callee) && haveSameRegisterRepresentation(results.chosenRepresentation(site.callee), expected);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void reportUnconvertedTailCall(const ConventionInputs& context, const TailCallSite& site)
{
    ValueID call = context.program().at(site.caller).at(site.block).terminator.callOperand();
    context.build.diagnostics().add(InternalError { .kind = InternalErrorKind::RequiredTailCallNotConverted, .function = site.caller, .block = site.block, .value = call, .pass = PassID::ChooseCallingConventions });
}

static void demoteTailCallsIn(const ConventionInputs& context, FunctionID caller, Function& function, TaskArenas& arenas, std::span<const TailCallSite> sites)
{
    const Program& program = context.program();
    {
        IRBuilder builder(AnalysisContext { context.build, program, &context.inputs.flow, &context.inputs.effects, caller }, function, arenas.types);
        for (const TailCallSite& site : sites | std::views::reverse)
            builder.demoteReturnCall(site.block);
    }
    simplifyEditedFunction(context.runner, program, context.inputs, caller, function, arenas);
}

void demoteMismatchedTailCalls(const ConventionInputs& context, Vector<TailCallSite>& sites, const CallingConventionTables& tables, BitVector& changedFunctions)
{
    const Program& program = context.program();
    ResultLocations results(context, tables);
    Vector<TailCallSite> demotedSites;
    for (const TailCallSite& site : sites) {
        if (canRemainTailCall(results, site))
            continue;
        switch (site.requirement) {
        case TailCallRequirement::Required:
            reportUnconvertedTailCall(context, site);
            break;
        case TailCallRequirement::Allowed:
            demotedSites.append(site);
            break;
        }
    }
    if (demotedSites.isEmpty())
        return;

    Vector<FunctionID> callers;
    BitVector isEdited(program.functions.size());
    for (const TailCallSite& site : demotedSites) {
        if (!isEdited.quickSet(site.caller.index()))
            callers.append(site.caller);
    }
    editFunctionsInParallel(context.editor, context.build, callers.span(), [&](unsigned, FunctionID caller, Function& function, TaskArenas& arenas) {
        demoteTailCallsIn(context, caller, function, arenas, sitesOf(demotedSites.span(), caller));
    });
    changedFunctions.merge(isEdited);
    context.build.statistics().count(StatisticID::ReturnCallsDemoted, demotedSites.size());

    // Simplification renumbers the blocks of an edited function, so its remaining sites are collected again.
    sites.removeAllMatching([&](const TailCallSite& site) {
        return isEdited.quickGet(site.caller.index());
    });
    size_t numberOfUneditedSites = sites.size();
    for (FunctionID caller : callers)
        collectSitesOf(program, caller, sites);
    std::ranges::inplace_merge(sites, sites.begin() + numberOfUneditedSites, { }, &TailCallSite::caller);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
