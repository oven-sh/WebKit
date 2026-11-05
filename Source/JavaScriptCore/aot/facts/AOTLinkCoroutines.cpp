/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// What Link says of coroutines: LinkTables::coroutines. One object stands for all that the calls of a coroutine answer with (ObjectTable::coroutineResultOf()), and one site of the flow.
// What the promise of an async function is fulfilled with hangs on the family of that site, so it widens where the promise escapes or meets another. What the body yields and returns hangs on the body alone.
// It adds no node, no edge and no observer: it reads the flow when that has settled. Withheld: the table is empty, and an empty table claims nothing.

#include "config.h"
#include "AOTLinkCoroutines.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTypesOnAllThreads.h"
#include "AOTObjects.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct NodesOfBodyInLinkCoroutines {
    uint32_t yielded { noSummaryNode };
    uint32_t returned { noSummaryNode };
};

} // anonymous namespace

const LinkedCoroutine* LinkTables::findCoroutine(FunctionID function) const
{
    auto found = std::ranges::lower_bound(coroutines, function, { }, &LinkedCoroutine::function);
    return found != coroutines.end() && found->function == function ? &*found : nullptr;
}

// As WriterOfLink::typeIDOf() writes a type out of a node, but for a node at which nothing has arrived. The summary lays down every yield and every return of the text, and the run time fills these places with nothing else.
static TypeID typeHeldInLinkCoroutines(const LinkFlow& flow, TypeArena& types, LinkType held)
{
    if (held.isNothing())
        return TypeTable::never;
    if (held.isAnything())
        return { };
    // Facts::held() gets no expectation of its own here: tried (what is unseen taken out of the type for that reading): no row of 382 moved, and the application's image grew by 180 KB.
    held = flow.withoutObjectsThatHaveEscaped(held);

    TypeID written = flow.toTypeID(types, held);
    if (!written || holdsOnlyPrimitives(written))
        return written;
    return types.at(written).isAnything() ? TypeID() : written;
}

// Only the object of the coroutine has such a slot: ScanOfBody::retain(), returnFromCoroutine().
static NodesOfBodyInLinkCoroutines findNodesOfBodyInLinkCoroutines(const ThinSummary& summary)
{
    NodesOfBodyInLinkCoroutines found;
    for (uint32_t index = 1; index < summary.nodes.size(); ++index) {
        const SummaryNode& node = summary.nodes[index];
        if (node.kind != SummaryNode::Kind::Contents)
            continue;
        if (node.second == SlotOfContents::yieldedAlone().number())
            found.yielded = index;
        else if (node.second == SlotOfContents::returnedAlone().number())
            found.returned = index;
    }
    return found;
}

// By function: some listed call enters the generator function, and what it answers goes elsewhere than into one loop.
// Each caller that enters a generator function has its summary looked through once, whatever the number of its calls.
static BitVector findGeneratorsTakenOtherwiseInLinkCoroutines(const Program& program, const LinkFlow& flow, const LinkTables& tables)
{
    BitVector takenOtherwise(program.functions.size());
    // A record of the program lies in a line of memory of its own, and there are more calls than functions.
    BitVector isGenerator(program.functions.size());
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (program.functions[index].has(FunctionFlags::IsGenerator))
            isGenerator.quickSet(index);
    }
    auto entersGenerator = [&](const LinkedCall& call) {
        return isGenerator.quickGet(call.callee.index());
    };

    Vector<ValueID, 8> sitesOnlyIterated;
    size_t end = 0;
    for (size_t first = 0; first < tables.calls.size(); first = end) {
        FunctionID caller = tables.calls[first].caller;
        while (end < tables.calls.size() && tables.calls[end].caller == caller)
            ++end;
        std::span<const LinkedCall> listed = tables.calls.span().subspan(first, end - first);
        if (std::ranges::none_of(listed, entersGenerator))
            continue;

        sitesOnlyIterated.shrink(0);
        for (const SummaryCall& call : flow.summaries.at(caller).calls) {
            if (call.resultIsOnlyIterated)
                sitesOnlyIterated.append(call.site);
        }
        std::ranges::sort(sitesOnlyIterated);
        for (const LinkedCall& call : listed) {
            if (entersGenerator(call) && !std::ranges::binary_search(sitesOnlyIterated, call.site))
                takenOtherwise.quickSet(call.callee.index());
        }
    }
    return takenOtherwise;
}

static thread_local TypesOfThreadInLink s_typesOfThreadInLinkCoroutines;

void writeCoroutinesOfLink(const Program& program, const LinkFlow& flow, TypeArena& types, LinkTables& tables)
{
    BitVector generatorsTakenOtherwise = findGeneratorsTakenOtherwiseInLinkCoroutines(program, flow, tables);
    // `return inner()`: the promise of the call is settled through the `then` that the promise finds, which a test run may replace. The flow takes the value of a promise without asking the table.
    const BuiltinTable& builtins = program.builtins();
    bool promisesFindThenOfTable = std::ranges::all_of(builtins.keysReadBy(Protocol::Thenable), [&](BuiltinName name) {
        BuiltinID member = builtins.findForInstance(BuiltinClasses::Promise, name).member;
        return member && !program.isUnlocked(member) && !program.isUnlocked(builtins.canonicalID(member));
    });

    // All threads look for the coroutines, and each is written by itself: AOTLinkTypesOnAllThreads.h.
    LinkFlow::ViewOfHeldReading* view = LinkFlow::ViewAtWork::current();
    Vector<uint32_t> coroutines = findIndicesInParallel(flow.loopsOfReading(), static_cast<unsigned>(program.functions.size()), [&](unsigned index) {
        LinkFlow::ViewAtWork viewAtWork(view);
        FunctionID id(index);
        const Function& function = program.at(id);
        // No call stands for what enters the body of a module, and the loader has its promise.
        if (!function.isCoroutine() || !function.hasBody() || function.isModuleBody() || function.results.size() != 1 || !flow.reached.get(index))
            return false;
        // A summary that has read the body leads the object of the coroutine into the result, and nothing else. Of any other summary a node that is missing says nothing.
        return flow.typeAt(flow.result(id, 0)).classes == LinkType::Classes::Site;
    });
    auto typesOfThread = [&]() -> TypeArena& {
        return s_typesOfThreadInLinkCoroutines.types ? *s_typesOfThreadInLinkCoroutines.types : types;
    };
    tables.coroutines.fill(LinkedCoroutine { }, coroutines.size());
    writeTypedPositionsOnAllThreads(flow, flow.program.types, static_cast<unsigned>(coroutines.size()), [](TypesOfThreadInLink typesHanded) {
        s_typesOfThreadInLinkCoroutines = typesHanded;
    }, [&](unsigned place) {
        uint32_t index = coroutines[place];
        FunctionID id(index);
        const Function& function = program.at(id);
        const LinkType& objects = flow.typeAt(flow.result(id, 0));
        bool isGenerator = function.has(FunctionFlags::IsGenerator);
        NodesOfBodyInLinkCoroutines nodes = findNodesOfBodyInLinkCoroutines(flow.summaries.at(id));
        // A node that is missing says nothing: a summary made without the lines of the scan that lay it looks the same as a body that never yields.
        auto typeHeldAt = [&](uint32_t nodeInSummary) {
            return nodeInSummary == noSummaryNode ? TypeID() : typeHeldInLinkCoroutines(flow, typesOfThread(), flow.typeAt(flow.nodeOf(id, nodeInSummary)));
        };

        LinkedCoroutine coroutine;
        coroutine.function = id;
        coroutine.returned = typeHeldAt(nodes.returned);
        if (isGenerator)
            coroutine.yielded = typeHeldAt(nodes.yielded);

        // A family that has escaped holds anything in every slot that somebody has asked for.
        if (FlowNodeID value = isGenerator || !promisesFindThenOfTable ? FlowNodeID() : flow.findContentsOf(objects, ContentsSlot::Value))
            coroutine.fulfilledWith = typeHeldInLinkCoroutines(flow, typesOfThread(), flow.typeAt(value));

        // A store or a definition under a name lands in a slot that flows into this one. A deletion, a definition by a descriptor, another prototype, and a look at the prototype let the object escape: AOTLinkLayouts.cpp, CallInLinkCalls::addPrototypes().
        // The object under `prototype` of a generator function is come by through the function, which then escapes and its results with it (AccessInLinkAccesses::resolveOnClosure(), StateOfLinkReach::openFunction()), or through a generator.
        FlowNodeID underNames = flow.findContentsOf(objects, SlotOfContents::underAnyName());
        coroutine.objectsStayAsMade = !underNames || flow.typeAt(underNames).isNothing();
        if (flow.hasEscaped(objects.site))
            coroutine.objectsStayAsMade = false;

        // Link calls unseen code where a protocol looks a name up on a generator, so a generator that is stepped has escaped as far as the flow goes. The summaries of the callers say more:
        // each object goes from its call into one loop, which has the only reference, steps it with next() and closes it with return(), and hands neither of them a value.
        bool onlyListedCallsEnter = tables.function(id).everyCallHasOneCallee;
        if (isGenerator && !function.has(FunctionFlags::IsAsync) && onlyListedCallsEnter && !generatorsTakenOtherwise.get(index)) {
            coroutine.objectsAreOnlyIterated = true;
            coroutine.objectsStayAsMade = true;
            coroutine.sent = TypeTable::undefined;
        }
        tables.coroutines[place] = coroutine;
        return isOfProgramInLink(coroutine.returned) && isOfProgramInLink(coroutine.yielded) && isOfProgramInLink(coroutine.fulfilledWith) && isOfProgramInLink(coroutine.sent);
    });
}

void forEachFunctionNamedByCoroutinesCalledBy(const LinkTables& tables, TypeReader reader, FunctionID referrer, const ScopedLambda<void(FunctionID)>& functor)
{
    if (tables.coroutines.isEmpty())
        return;
    for (const LinkedCall& call : std::ranges::equal_range(tables.calls, referrer, { }, &LinkedCall::caller)) {
        const LinkedCoroutine* coroutine = tables.findCoroutine(call.callee);
        if (!coroutine)
            continue;
        for (TypeID type : { coroutine->fulfilledWith, coroutine->returned, coroutine->yielded, coroutine->sent }) {
            if (type && !holdsOnlyPrimitives(type))
                forEachFunctionOf(reader, reader.at(type), functor);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
