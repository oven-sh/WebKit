/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEntrypointsNothingEnters.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCreateEntrypoints.h"
#include "AOTFlowResults.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTResolveCallsCallees.h"
#include "AOTThreads.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// Function: a call by value may find it. Set: it may find the members of the callee set. Anything: nobody can say what it finds.
enum class FoundKind : uint8_t { Function, Set, Anything };

struct Found {
    FoundKind kind;
    uint32_t index;
};

} // anonymous namespace

static ValueID calledOperandOf(const Value& call)
{
    ValueID called;
    call.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (call.operandUses(operandIndex).contains(OperandUse::Calls))
            called = operand;
    });
    return called;
}

static bool mayBeUnlistedFunction(const Program& program, const Type& type)
{
    return admitsUnlistedFunctions(program, type) || !type.classes.isEmpty();
}

static void forEachCallByValue(const Program& program, FunctionID function, NOESCAPE const Invocable<void(const Value&)> auto& functor)
{
    forEachListedValue(program, function, Value::opcodesWithCallees, [&](BlockID, uint32_t, ValueID, const Value& value) {
        functor(value);
    });
}

std::span<const CalleeSetOfSite> CalleesOfSites::calleeSetsOf(AccessSiteID site) const
{
    auto range = std::ranges::equal_range(m_calleeSets, site, { }, &CalleeSetOfSite::site);
    return { range.begin(), range.end() };
}

std::span<const CalleeOfSite> CalleesOfSites::typedCalleesOf(AccessSiteID site) const
{
    auto range = std::ranges::equal_range(m_typedCallees, site, { }, &CalleeOfSite::site);
    return { range.begin(), range.end() };
}

CalleesOfSites collectCalleesOfSites(const Program& program, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    CalleesOfSites result;

    struct Chunk {
        Vector<AccessSiteID> sites;
        Vector<CalleeSetOfSite> calleeSets;
        Vector<CalleeOfSite> typedCallees;
    };
    Vector<Vector<Chunk>> chunks = collectChunksInParallel<Chunk>(loops, count, { }, [&](unsigned index, Vector<Chunk>& found) {
        const Function& function = program.functions[index];
        forEachCallByValue(program, FunctionID(index), [&](const Value& call) {
            AccessSiteID site = call.accessSite();
            if (!site)
                return;
            if (found.isEmpty())
                found.append(Chunk { });
            found[0].sites.append(site);
            if (call.callees())
                found[0].calleeSets.append({ site, call.callees() });
            ValueID callee = calledOperandOf(call);
            if (!callee)
                return;
            CalleeCandidates candidates = calleeCandidates(program, function, callee);
            for (FunctionID listed : program.typeOf(function, candidates.value).functions)
                found[0].typedCallees.append({ site, listed });
            for (FunctionID listed : candidates.expected)
                found[0].typedCallees.append({ site, listed });
        });
    });

    auto join = [&]<typename Entry>(Vector<Entry> Chunk::* list, Vector<Entry>& joined) {
        forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk) {
            if (chunks[chunk].isEmpty())
                return;
            std::ranges::sort(chunks[chunk][0].*list);
            removeRepeatedElements(chunks[chunk][0].*list);
        });
        size_t size = 0;
        bool isAscending = true;
        const Entry* last = nullptr;
        for (const Vector<Chunk>& chunk : chunks) {
            if (chunk.isEmpty() || (chunk[0].*list).isEmpty())
                continue;
            isAscending &= !last || *last < (chunk[0].*list).first();
            last = &(chunk[0].*list).last();
            size += (chunk[0].*list).size();
        }
        joined = createVectorWithCapacityInParallel<Entry>(loops, size);
        for (const Vector<Chunk>& chunk : chunks) {
            if (!chunk.isEmpty())
                joined.appendVector(chunk[0].*list);
        }
        if (!isAscending)
            sortAndRemoveDuplicates(loops, joined);
    };
    runTasksInParallel(loops, {
        [&] {
            uint32_t numberOfSites = 0;
            for (const Vector<Chunk>& chunk : chunks) {
                if (chunk.isEmpty())
                    continue;
                for (AccessSiteID site : chunk[0].sites)
                    numberOfSites = std::max(numberOfSites, site.index() + 1);
            }
            result.m_hadCallByValue.ensureSize(numberOfSites);
            for (const Vector<Chunk>& chunk : chunks) {
                if (chunk.isEmpty())
                    continue;
                for (AccessSiteID site : chunk[0].sites)
                    result.m_hadCallByValue.quickSet(site.index());
            }
        },
        [&] {
            join(&Chunk::calleeSets, result.m_calleeSets);
        },
        [&] {
            join(&Chunk::typedCallees, result.m_typedCallees);
        },
    });
    build.destroyLater(WTF::move(chunks));
    result.m_wasTaken = true;
    return result;
}

static bool isConstructor(FunctionKind kind)
{
    return kind == FunctionKind::BaseConstructor || kind == FunctionKind::DerivedConstructor;
}

// THE CONTRACT OF FLOW THAT THIS FILE RESTS ON, as computeFunctionResults (AOTValueFlowResultsFunctions.cpp) and stampCall (AOTValueFlowResultsCalls.cpp) state it:
// every entry into a function whose callers are all known is a call that names the function, a call whose stamp lists it, or an indirect call out of an operation;
// a call without a stamp, or with one that is not complete, finds beside what is listed only what has escaped or is lost, and no such function has known callers.
// HasOnlyDirectCallers and the known callers of the call graph rest on the same.
static BitVector findFunctionsEnteredBesideCallsInCode(const Program& program, const FlowResults& flow, const ParallelLoops& loops)
{
    BitVector isEntered = functionsCalledByHost(program, loops);
    for (uint32_t index : findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        bool isFollowedByFlow = index < program.numberOfFunctionsAsRead() && flow.function(FunctionID(index)).callers == CallerState::AllKnown;
        // The second entrypoint of a constructor function that reads no new.target is entered by the constructions that would enter the first.
        bool readsNewTarget = entrypointVariantsOf(program, function) != EntrypointVariants::WithoutNewTarget && !hasAllocatingConstructEntrypoint(program, function);
        return !isFollowedByFlow || isConstructor(function.kind) || readsNewTarget;
    }))
        isEntered.quickSet(index);
    for (const FlowIndirectCall& call : flow.indirectCalls())
        isEntered.set(call.callee.index());
    return isEntered;
}

static bool namesItsCallee(const Value& call)
{
    return (PayloadWord::calleeKey(call.opcode) && call.calleeKey()) || (PayloadWord::calleeName(call.opcode) && call.calleeName());
}

// A pass makes a call of its own for a protocol, which flow lists among the indirect calls, and in the lookup of a method table, whose callers stand at the sites of the calls that they replace.
// A call from the source names its callee. One that a pass has made again without its site has lost what its site listed, so nobody can say what it finds.
static bool mayBeCallFromSource(const Function& function, const Value& call)
{
    return function.role != FunctionRole::TableLookup && namesItsCallee(call);
}

// The type of the callee lists functions that the call may find. Only where a value of that type may be another function do the stamps count:
// the one on the call, and what its site listed when flow had just stamped it. Nobody can say what a call finds that stands at a site where no call by value stood then.
static void collectFunctionsThatCallsByValueFind(const Program& program, const CalleesOfSites& sites, FunctionID id, Vector<Found>& found)
{
    const Function& function = program.functions[id.index()];
    forEachCallByValue(program, id, [&](const Value& call) {
        if (ValueID callee = calledOperandOf(call)) {
            CalleeCandidates candidates = calleeCandidates(program, function, callee);
            const Type& type = program.typeOf(function, candidates.value);
            for (FunctionID listed : type.functions)
                found.append({ FoundKind::Function, listed.index() });
            if (!mayBeUnlistedFunction(program, type))
                return;
            for (FunctionID listed : candidates.expected)
                found.append({ FoundKind::Function, listed.index() });
        }
        if (CalleeSetID stamp = call.callees())
            found.append({ FoundKind::Set, stamp.index() });
        AccessSiteID site = call.accessSite();
        if (!site ? mayBeCallFromSource(function, call) : !sites.hadCallByValue(site)) {
            found.append({ FoundKind::Anything, 0 });
            return;
        }
        if (!site)
            return;
        for (const CalleeSetOfSite& entry : sites.calleeSetsOf(site))
            found.append({ FoundKind::Set, entry.callees.index() });
        for (const CalleeOfSite& entry : sites.typedCalleesOf(site))
            found.append({ FoundKind::Function, entry.callee.index() });
    });
}

static bool neverReturns(const Function& function)
{
    return function.blocks.size() == 1 && function.at(entryBlock).terminator.kind != TerminatorKind::ReturnCall && function.at(entryBlock).terminator.kind != TerminatorKind::Return;
}

static void replaceBodyWithThrow(const AnalysisContext& context, TypeArena& types, Function& entrypoint)
{
    Vector<TypeID, 8> parameterTypes;
    for (ValueID parameter : entrypoint.at(entryBlock).parameters)
        parameterTypes.append(entrypoint.at(parameter).type);
    entrypoint.editAllBlocks().clear();
    entrypoint.editAllValues().clear();
    entrypoint.inlineFrames.clear();

    IRBuilder builder(context, entrypoint, types);
    builder.setOrigin({ entrypoint.position, 0 });
    BlockID block = builder.addBlock();
    builder.moveToEndOf(block);
    for (TypeID type : parameterTypes)
        builder.addParameter(block, type);
    Value error = IRBuilder::make(Opcode::NewRuntimeError);
    error.setRuntimeError(RuntimeErrorKind::LeftOutOfExecutable);
    error.setName(entrypoint.name);
    builder.emitThrow(builder.append(WTF::move(error)));
}

void closeEntrypointsThatNothingEnters(ProgramEditor& editor, const FlowResults& flow, const CalleesOfSites& sites, BuildContext& build)
{
    if (!sites.wasTaken() || !flow.hasResults())
        return;

    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    Vector<Found> found = collectInParallel<Found>(loops, count, [&](unsigned index, Vector<Found>& foundHere) {
        collectFunctionsThatCallsByValueFind(program, sites, FunctionID(index), foundHere);
    });

    BitVector mayBeEntered = findFunctionsEnteredBesideCallsInCode(program, flow, loops);
    BitVector isVisitedSet(program.types.numberOfCalleeSets());
    for (Found entry : found) {
        switch (entry.kind) {
        case FoundKind::Anything:
            return;
        case FoundKind::Function:
            mayBeEntered.set(entry.index);
            break;
        case FoundKind::Set:
            if (isVisitedSet.quickSet(entry.index))
                break;
            for (FunctionID function : program.at(CalleeSetID(entry.index)).functions)
                mayBeEntered.set(function.index());
            break;
        }
    }

    Vector<uint32_t> closed = findIndicesInParallel(loops, count, [&](unsigned index) {
        RecordFacts facts = recordFactsOf(program, FunctionID(index));
        if (facts.role != FunctionRole::GenericEntrypoint || !facts.hasBody || facts.mayBeCalledByHost())
            return false;
        const Function& function = program.functions[index];
        const auto* record = function.creator.tryAs<CreatorRecord::Entrypoint>();
        return record && function.role == FunctionRole::GenericEntrypoint && function.hasBody() && !function.mayBeCalledByHost()
            && !mayBeEntered.get(record->target.index()) && !neverReturns(function)
            && build.isEnabled(PassID::RemoveUnreachableFunctions, FunctionID(index));
    });
    Vector<FunctionID> entrypoints = closed.map([](uint32_t index) {
        return FunctionID(index);
    });
    AnalysisContext context { build, program, &flow, nullptr };
    editFunctionsInParallel(editor, build, entrypoints.span(), [&](unsigned, FunctionID, Function& entrypoint, TaskArenas& arenas) {
        replaceBodyWithThrow(context, arenas.types, entrypoint);
    });
    build.statistics().count(StatisticID::UnreachableFunctionsRemoved, entrypoints.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
