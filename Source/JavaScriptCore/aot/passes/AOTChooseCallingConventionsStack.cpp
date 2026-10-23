/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTChooseCallingConventionsInternal.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTConvention.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <compare>
#include <numeric>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct TailCallEdge {
    friend auto operator<=>(const TailCallEdge&, const TailCallEdge&) = default;

    FunctionID callee;
    FunctionID caller;
};

struct EntryRequest {
    friend auto operator<=>(const EntryRequest&, const EntryRequest&) = default;

    FunctionID callee;
    uint8_t firstOmittedGPR { 0 };
};

class StackWordReservation {
public:
    StackWordReservation(const ConventionInputs&, const CallingConventionTables&);

    bool isEmpty() const { return m_worklist.isEmpty(); }
    void reserveForCallsByValue(std::span<const TailCallSite>);
    void propagateToCallers(std::span<const TailCallSite>);
    void store(CallingConventionTables&) const;

private:
    bool mayReserve(FunctionID) const;
    void raise(FunctionID, uint32_t numberOfWords);

    const ConventionInputs& m_context;
    const Program& m_program;
    Vector<uint32_t> m_reserved;
    Vector<FunctionID> m_worklist;
};

} // anonymous namespace

StackWordReservation::StackWordReservation(const ConventionInputs& context, const CallingConventionTables& tables)
    : m_context(context)
    , m_program(context.program())
    , m_reserved(FillWith { }, m_program.functions.size(), 0u)
{
    m_worklist = collectInParallel<FunctionID>(context.build.parallelLoops(), m_reserved.size(), [&](unsigned index, Vector<FunctionID>& functionsWithWords) {
        FunctionID id(index);
        if (!mayReserve(id))
            return;
        m_reserved[index] = computeArgumentLocations(m_program, m_program.at(id), tables.conventions[index], tables.registers).numberOfStackWords();
        if (m_reserved[index])
            functionsWithWords.append(id);
    });
}

bool StackWordReservation::mayReserve(FunctionID id) const
{
    RecordFacts facts = recordFactsOf(m_program, id);
    return facts.hasBody && hasStackArgumentAreaOf(facts.role);
}

void StackWordReservation::raise(FunctionID id, uint32_t numberOfWords)
{
    uint32_t& reserved = m_reserved[id.index()];
    if (reserved >= numberOfWords)
        return;
    reserved = numberOfWords;
    m_worklist.append(id);
}

void StackWordReservation::reserveForCallsByValue(std::span<const TailCallSite> sites)
{
    for (const TailCallSite& site : sites) {
        if (site.calleeKind != TailCalleeKind::Unknown || !mayReserve(site.caller))
            continue;
        const Function& caller = m_program.at(site.caller);
        const Value& call = caller.at(caller.at(site.block).terminator.callOperand());
        if (call.opcode != Opcode::Call)
            continue;
        size_t numberOfArguments = call.argumentsOperands().size();
        if (numberOfArguments > numberOfArgumentGPRs)
            raise(site.caller, static_cast<uint32_t>(numberOfArguments - numberOfArgumentGPRs));
    }
}

void StackWordReservation::propagateToCallers(std::span<const TailCallSite> sites)
{
    Vector<TailCallEdge> edges;
    for (const TailCallSite& site : sites) {
        if (site.calleeKind != TailCalleeKind::Unknown && mayReserve(site.caller))
            edges.append(TailCallEdge { site.callee, site.caller });
    }
    sortAndRemoveDuplicates(m_context.build.parallelLoops(), edges);

    while (!m_worklist.isEmpty()) {
        FunctionID callee = m_worklist.takeLast();
        uint32_t numberOfWords = m_reserved[callee.index()];
        for (const TailCallEdge& edge : std::ranges::equal_range(edges, callee, { }, &TailCallEdge::callee))
            raise(edge.caller, numberOfWords);
    }
}

void StackWordReservation::store(CallingConventionTables& tables) const
{
    BuildContext& build = m_context.build;
    uint64_t limit = build.limit(LimitID::MaxReservedStackArguments);
    for (unsigned index = 0; index < m_reserved.size(); ++index) {
        uint32_t numberOfWords = m_reserved[index];
        if (numberOfWords > limit) {
            build.reportLimitExceeded(LimitID::MaxReservedStackArguments, numberOfWords, Subject { .function = FunctionID(index) });
            continue;
        }
        tables.conventions[index].reservedStackWords = static_cast<uint16_t>(numberOfWords);
    }
}

void reserveStackWords(const ConventionInputs& context, std::span<const TailCallSite> sites, CallingConventionTables& tables)
{
    StackWordReservation reservation(context, tables);
    reservation.reserveForCallsByValue(sites);
    if (reservation.isEmpty())
        return;
    reservation.propagateToCallers(sites);
    reservation.store(tables);
}

static ValueID tailCallOf(const Block& block)
{
    return block.terminator.kind == TerminatorKind::ReturnCall ? block.terminator.callOperand() : ValueID();
}

static bool isUndefined(const Function& function, ValueID id)
{
    return function.at(definitionThroughUpcasts(function, id)).opcode == Opcode::Undefined;
}

static bool passesUndefined(const Function& caller, const Value& call)
{
    return std::ranges::any_of(call.argumentsOperands(), [&](ValueID argument) {
        return isUndefined(caller, argument);
    });
}

static bool canHaveSecondaryEntries(const ConventionInputs& context, FunctionID id)
{
    const Function& function = context.program().at(id);
    return function.hasBody()
        && entryConventionOf(function) == EntryConvention::Direct
        && function.role != FunctionRole::TableEntrypoint
        && function.role != FunctionRole::TableLookup
        && function.role != FunctionRole::StubBody
        && context.isEnabledFor(id);
}

static bool passesIntegersOnStack(const ArgumentLocations& locations)
{
    return std::ranges::any_of(locations.parameters, [](const ArgumentLocation& location) {
        return location.kind == ArgumentLocation::Kind::Stack && !isPassedInFPR(location.representation);
    });
}

static std::optional<uint8_t> findFirstOmittedGPR(const Function& caller, const Value& call, const ArgumentLocations& locations)
{
    std::span<const ValueID> arguments = call.argumentsOperands();
    RELEASE_ASSERT(arguments.size() == locations.parameters.size());
    unsigned numberOfGPRs = 0;
    unsigned firstOmittedGPR = 0;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const ArgumentLocation& location = locations.parameters[index];
        switch (location.kind) {
        case ArgumentLocation::Kind::GPR:
            ++numberOfGPRs;
            if (location.representation != Representation::JSValue || !isUndefined(caller, arguments[index]))
                firstOmittedGPR = numberOfGPRs;
            break;
        case ArgumentLocation::Kind::None:
        case ArgumentLocation::Kind::FPR:
        case ArgumentLocation::Kind::Stack:
            break;
        }
    }
    if (firstOmittedGPR == numberOfGPRs)
        return std::nullopt;
    return static_cast<uint8_t>(firstOmittedGPR);
}

static void collectEntryRequests(const ConventionInputs& context, const CallingConventionTables& tables, FunctionID callerID, Vector<EntryRequest>& requests)
{
    const Program& program = context.program();
    // An argument that is undefined is an operation of the caller.
    if (!mayHoldValueWith(program, callerID, { Opcode::Undefined }))
        return;
    const Function& caller = program.functions[callerID.index()];
    forEachListedValue(program, callerID, { Opcode::CallDirect }, [&](BlockID block, uint32_t, ValueID id, const Value& call) {
        if (!passesUndefined(caller, call) || id == tailCallOf(caller.at(block)))
            return;
        FunctionID callee = call.function();
        if (!canHaveSecondaryEntries(context, callee))
            return;
        ArgumentLocations locations = computeArgumentLocations(program, program.at(callee), tables.conventions[callee.index()], tables.registers);
        if (passesIntegersOnStack(locations))
            return;
        if (std::optional<uint8_t> firstOmittedGPR = findFirstOmittedGPR(caller, call, locations))
            requests.append(EntryRequest { callee, *firstOmittedGPR });
    });
}

void chooseSecondaryEntries(const ConventionInputs& context, CallingConventionTables& tables)
{
    const Program& program = context.program();
    const ParallelLoops& loops = context.build.parallelLoops();
    unsigned count = program.functions.size();
    Vector<EntryRequest> requests = collectInParallel<EntryRequest>(loops, count, [&](unsigned index, Vector<EntryRequest>& collected) {
        collectEntryRequests(context, tables, FunctionID(index), collected);
    });
    sortAndRemoveDuplicates(loops, requests);

    tables.firstSecondaryEntry.fill(0, count + 1);
    tables.secondaryEntries.reserveInitialCapacity(requests.size());
    for (const EntryRequest& request : requests) {
        uint32_t& numberOfEntries = tables.firstSecondaryEntry[request.callee.index() + 1];
        tables.secondaryEntries.append(SecondaryEntry { static_cast<uint16_t>(++numberOfEntries), request.firstOmittedGPR });
    }
    std::partial_sum(tables.firstSecondaryEntry.begin(), tables.firstSecondaryEntry.end(), tables.firstSecondaryEntry.begin());
    context.build.statistics().count(StatisticID::SecondaryEntries, requests.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
