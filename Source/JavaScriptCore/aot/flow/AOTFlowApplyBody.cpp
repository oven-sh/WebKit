/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTTypePropagation.h"
#include <algorithm>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class TailCallOutcome : uint8_t { Preserved, Demoted };

} // anonymous namespace

void applyStamps(const FlowBodyApplication& body, const FlowBodyUpdate& update)
{
    Function& function = body.function;
    const FlowAccessSiteNumbering& accessSites = body.application.accessSites;
    FunctionID functionAsRead = body.application.versions.bodyOf(body.id);

    forEachAccessSiteValue(function, [&](ValueID value, uint32_t ordinal) {
        RELEASE_ASSERT(ordinal < accessSites.numberOfSitesOf(functionAsRead));
        function.at(value).setAccessSite(accessSites.siteOf(functionAsRead, ordinal));
    });

    for (const FlowAccessStamp& stamp : update.accesses)
        function.at(stamp.value).setReceivers(stamp.receivers);

    for (const FlowCallStamp& stamp : update.calls) {
        Value& call = function.at(stamp.value);
        call.setCallees(stamp.callees);
        if (stamp.keepsShape)
            call.setKeepsShape(true);
    }

    for (const FlowAllocationStamp& stamp : update.allocations) {
        Value& allocation = function.at(stamp.value);
        allocation.setAllocationSite(stamp.site);
        if (PayloadWord::allocationClass(allocation.opcode))
            allocation.setAllocationClass(stamp.allocationClass);
        if (stamp.derivedLayout)
            allocation.setLayout(stamp.derivedLayout);
        if (stamp.keepsShape)
            allocation.setKeepsShape(true);
        if (stamp.elements)
            allocation.setContents(stamp.elements);
    }

    for (const FlowConstructionStamp& stamp : update.constructions)
        function.at(stamp.value).setAllocatedLayout(stamp.derivedLayout);

    for (const FlowDirectCallUpdate& entry : update.directCalls) {
        Value& call = function.at(entry.call);
        call.setFunction(entry.function);
        call.setResultCheck(entry.resultCheck);
    }
}

static ResultCheck ownResultCheckOf(const FlowBodyApplication& body)
{
    return body.results.function(body.id).resultCheck;
}

// A test after the call ends the tail call. Only a function whose callers test its result can do without the test.
static bool testsResultOfTailCall(const FlowBodyApplication& body, const FlowTailCallUpdate& entry)
{
    return entry.requirement == TailCallRequirement::Allowed || ownResultCheckOf(body) == ResultCheck::ByCallee;
}

static void demoteTailCall(const FlowBodyApplication& body, IRBuilder& builder, BlockID block)
{
    builder.demoteReturnCall(block);
    body.build.statistics().count(StatisticID::ReturnCallsDemoted);
}

static bool isClaim(const TailCast& cast)
{
    return cast.resolution == CastResolution::Claim;
}

static TailCallOutcome applyTailClaims(const FlowBodyApplication& body, IRBuilder& builder, BlockID block, const FlowTailCallUpdate& entry, FlowBodyCounts& counts)
{
    Terminator& terminator = body.function.at(block).terminator;
    Vector<TailCast, 2> casts;
    for (TailCast cast : terminator.casts())
        casts.append(cast);
    uint64_t numberOfClaims = static_cast<uint64_t>(std::ranges::count_if(casts, isClaim));
    if (!numberOfClaims)
        return TailCallOutcome::Preserved;

    if (entry.claimsAreRedundant) {
        for (TailCast& cast : casts) {
            if (isClaim(cast))
                cast.resolution = CastResolution::Assume;
        }
        terminator.setCasts(casts.span());
        counts.removedClaims += numberOfClaims;
        return TailCallOutcome::Preserved;
    }

    if (testsResultOfTailCall(body, entry)) {
        demoteTailCall(body, builder, block);
        counts.remainingClaims += numberOfClaims;
        return TailCallOutcome::Demoted;
    }

    // The casts after a Claim were resolved against the tested value, and nothing is tested here any more.
    casts.shrink(casts.findIf(isClaim));
    terminator.setCasts(casts.span());
    return TailCallOutcome::Preserved;
}

static void applyTailCalls(const FlowBodyApplication& body, IRBuilder& builder, const FlowBodyUpdate& update, FlowBodyCounts& counts)
{
    Function& function = body.function;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::ReturnCall)
            continue;

        // Code that flow never reached has no entry and gets the unknown answers: past form AsRead no Claim may stay on a terminator.
        ValueID call = terminator.callOperand();
        const FlowTailCallUpdate* known = findByValue(update.tailCalls.span(), call, &FlowTailCallUpdate::call);
        FlowTailCallUpdate entry = known ? *known : FlowTailCallUpdate { };
        terminator.setRequirement(entry.requirement);
        if (applyTailClaims(body, builder, block, entry, counts) == TailCallOutcome::Demoted)
            continue;

        if (findByValue(update.symbolConstants.span(), call, &FlowSymbolConstant::call)) {
            demoteTailCall(body, builder, block);
            continue;
        }

        const FlowDirectCallUpdate* directCall = findByValue(update.directCalls.span(), call, &FlowDirectCallUpdate::call);
        if (!directCall || directCall->resultCheck != ResultCheck::ByCallers)
            continue;
        // The result of a required tail call is tested by the caller that leaves the cycle.
        if (entry.requirement == TailCallRequirement::Allowed && !directCall->resultIsInsideWrittenType) {
            demoteTailCall(body, builder, block);
            continue;
        }

        // The casts were resolved against the written result type of the callee, which is no longer the type of the call.
        terminator.setCasts({ });
    }
}

static bool isTailCallWithClaim(const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::ReturnCall)
        return false;
    for (TailCast cast : terminator.casts()) {
        if (isClaim(cast))
            return true;
    }
    return false;
}

void demoteTailCallsWithClaims(const Program& program, FunctionID id, Function& function, TypeArena& types, BuildContext& build)
{
    bool hasTailCallWithClaim = std::ranges::any_of(function.blocks, isTailCallWithClaim, &Block::terminator);
    if (!hasTailCallWithClaim)
        return;

    AnalysisContext context { build, program, nullptr, nullptr, id };
    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        if (!isTailCallWithClaim(function.at(block).terminator))
            continue;
        builder.demoteReturnCall(block);
        build.statistics().count(StatisticID::ReturnCallsDemoted);
    }
}

FlowBodyCounts applyBodyUpdate(const FlowBodyApplication& body)
{
    // The update names values by their ids as read. Typing may renumber them, so nothing after it reads the update.
    const FlowBodyUpdate& update = body.application.bodies[body.id.index()];
    applyStamps(body, update);
    FlowBodyCounts counts = applyClaimUpdates(body, update);

    AnalysisContext context { body.build, body.program, nullptr, nullptr, body.id };
    FunctionAnalyses analyses(context, body.function);
    IRBuilder builder(context, body.function, body.types, &analyses);
    applyTailCalls(body, builder, update, counts);

    FlowCastInsertion insertion { body, analyses, builder };
    insertResultClaims(insertion, update, counts);
    insertProofCasts(insertion, update);
    insertSymbolConstants(insertion, update);
    startReadsAtPrototype(insertion, update);
    createBoundClosures(insertion, update);
    replaceDefinedValues(insertion, update);
    createDictionaries(insertion, update);
    builder.applyInsertions();

    TypePropagationInputs inputs { body.program, body.id, body.function, analyses, body.types, body.build };
    typeValuesFromNever(inputs);

    Vector<ValueID> casts = insertCastsForRequiredTypes(insertion);
    if (!casts.isEmpty()) {
        builder.applyInsertions();
        narrowValueTypes(inputs, casts.span());
    }
    if (body.build.validates())
        insertRuntimeValidation(insertion);
    return counts;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
