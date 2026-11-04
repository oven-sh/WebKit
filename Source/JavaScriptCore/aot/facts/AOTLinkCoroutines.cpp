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
    // Facts::held() gets no expectation of its own here: tried 11-04 (what is unseen taken out of the type for that reading): no row of 382 moved, and the application's image grew by 180 KB.
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
    auto entersGenerator = [&](const LinkedCall& call) {
        return program.at(call.callee).has(FunctionFlags::IsGenerator);
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

void writeCoroutinesOfLink(const Program& program, const LinkFlow& flow, TypeArena& types, LinkTables& tables)
{
    BitVector generatorsTakenOtherwise = findGeneratorsTakenOtherwiseInLinkCoroutines(program, flow, tables);
    // `return inner()`: the promise of the call is settled through the `then` that the promise finds, which a test run may replace. The flow takes the value of a promise without asking the table.
    const BuiltinTable& builtins = program.builtins();
    bool promisesFindThenOfTable = std::ranges::all_of(builtins.keysReadBy(Protocol::Thenable), [&](BuiltinName name) {
        BuiltinID member = builtins.findForInstance(BuiltinClasses::Promise, name).member;
        return member && !program.isUnlocked(member) && !program.isUnlocked(builtins.canonicalID(member));
    });

    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        const Function& function = program.at(id);
        // No call stands for what enters the body of a module, and the loader has its promise.
        if (!function.isCoroutine() || !function.hasBody() || function.isModuleBody() || function.results.size() != 1 || !flow.reached.get(index))
            continue;
        // A summary that has read the body leads the object of the coroutine into the result, and nothing else. Of any other summary a node that is missing says nothing.
        const LinkType& objects = flow.typeAt(flow.result(id, 0));
        if (objects.classes != LinkType::Classes::Site)
            continue;

        bool isGenerator = function.has(FunctionFlags::IsGenerator);
        NodesOfBodyInLinkCoroutines nodes = findNodesOfBodyInLinkCoroutines(flow.summaries.at(id));
        // A node that is missing says nothing: a summary made without the lines of the scan that lay it looks the same as a body that never yields.
        auto typeHeldAt = [&](uint32_t nodeInSummary) {
            return nodeInSummary == noSummaryNode ? TypeID() : typeHeldInLinkCoroutines(flow, types, flow.typeAt(flow.nodeOf(id, nodeInSummary)));
        };

        LinkedCoroutine coroutine;
        coroutine.function = id;
        coroutine.returned = typeHeldAt(nodes.returned);
        if (isGenerator)
            coroutine.yielded = typeHeldAt(nodes.yielded);

        // A family that has escaped holds anything in every slot that somebody has asked for.
        if (FlowNodeID value = isGenerator || !promisesFindThenOfTable ? FlowNodeID() : flow.findContentsOf(objects, ContentsSlot::Value))
            coroutine.fulfilledWith = typeHeldInLinkCoroutines(flow, types, flow.typeAt(value));

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
        tables.coroutines.append(coroutine);
    }
}

// What a family of sites holds, proven or seen, is written into types by one writer, which is not this file: the table by family of sites. So the default, which claims nothing of the contents.
TypeID typeOfInstancesWithContentsSeen(const LinkFlow&, TypeArena& types, const LinkType& instances)
{
    return types.builtinClass(instances.builtinClass);
}

static FunctionID functionWhoseClosuresAloneArriveInLinkCoroutines(const LinkType& type)
{
    bool holdsClosuresAlone = type.functions == LinkType::Functions::Closure && !type.primitives && type.objects == LinkType::Objects::None && type.classes == LinkType::Classes::None && !type.holdsUnseen && !type.builtin;
    return holdsClosuresAlone ? type.function : FunctionID();
}

// The operand that every row for the mode calls, once, in front of its return, and nothing else that any of them calls. `reduce` on an iterator has a record for the first call and one for each element: two records.
static std::optional<BuiltinOperand> findOperandCalledOnceInLinkCoroutines(const BuiltinTable& builtins, BuiltinID builtin, BuiltinCallMode mode)
{
    std::optional<BuiltinOperand> called;
    for (const BuiltinOverload& overload : builtins.overloadsOf(builtin)) {
        if (overload.mode != mode)
            continue;
        bool callsHandlers = std::ranges::any_of(overload.flow.handlerCalls, [](const BuiltinHandlerCall&) {
            return true;
        });
        if (overload.isUndeclared() || callsHandlers)
            return std::nullopt;
        unsigned numberOfCallbacks = 0;
        for (const BuiltinCallback& callback : overload.flow.callbacks) {
            if (++numberOfCallbacks > 1 || callback.times != BuiltinCallbackTimes::Once || callback.constructs || callback.callee.kind() != BuiltinOperand::Kind::Argument)
                return std::nullopt;
            if (called && *called != callback.callee)
                return std::nullopt;
            called = callback.callee;
        }
        if (!numberOfCallbacks)
            return std::nullopt;
    }
    return called;
}

FunctionID findCallbackCalledOnce(const Program& program, const LinkFlow& flow, FunctionID caller, const SummaryCall& call, const LinkFlow::CalleesOfCall& callees)
{
    if (callees.mayEnterLost || callees.mayEnterUnseen || !callees.shared.isEmpty() || call.passesList || call.callee == SummaryCall::Callee::Binding)
        return { };

    const BuiltinTable& builtins = program.builtins();
    BuiltinCallMode mode = call.isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
    std::optional<BuiltinOperand> called;
    auto isCalledOnceBy = [&](BuiltinID builtin) {
        std::optional<BuiltinOperand> calledByBuiltin = findOperandCalledOnceInLinkCoroutines(builtins, builtin, mode);
        if (!calledByBuiltin || (called && *called != *calledByBuiltin))
            return false;
        called = calledByBuiltin;
        return true;
    };
    if (call.callee == SummaryCall::Callee::Builtin && !isCalledOnceBy(call.builtin))
        return { };
    if (!std::ranges::all_of(callees.builtins, isCalledOnceBy))
        return { };
    if (!called || called->argumentIndex() >= call.numberOfArguments)
        return { };

    uint32_t operand = flow.summaries.at(caller).arguments[call.firstArgument + called->argumentIndex()];
    if (operand == noSummaryNode)
        return { };
    FunctionID callback = functionWhoseClosuresAloneArriveInLinkCoroutines(flow.typeAt(flow.nodeOf(caller, operand)));
    // Whatever else runs because of the call would share the site with it.
    if (!callback || callees.listed.size() != 1 || callees.listed[0] != callback || !flow.tablesMayName(callback))
        return { };
    return callback;
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

// The objects of coroutines lie side by side in the table of objects, the coroutines in front, far from the sites. So they have a list of their own, which is as long as there are coroutines.
// FactTables has that list once the two blocks for AOTFacts.h are in, which stand under ASK. Until then this compiles, writes nothing, and nothing is claimed of any such object.
void fillFactsOfObjectsOfCoroutines(const Program& program, const LinkTables& linked, FactTables& tables)
{
    auto fill = [&](auto& tablesWithList) {
        if constexpr (requires { tablesWithList.objectsOfCoroutines; }) {
            const ObjectTable& objects = program.types.objects();
            uint32_t first = objects.firstOf(ObjectKind::CoroutineResult);
            uint32_t numberOfPlaces = 0;
            for (const LinkedCoroutine& coroutine : linked.coroutines)
                numberOfPlaces = std::max(numberOfPlaces, objects.coroutineResultOf(coroutine.function).index() - first + 1);
            tablesWithList.firstObjectOfCoroutines = first;
            tablesWithList.objectsOfCoroutines.fill(ObjectFacts { }, numberOfPlaces);
            for (const LinkedCoroutine& coroutine : linked.coroutines) {
                if (coroutine.objectsStayAsMade && program.at(coroutine.function).has(FunctionFlags::IsGenerator))
                    tablesWithList.objectsOfCoroutines[objects.coroutineResultOf(coroutine.function).index() - first].facts.add(ObjectFact::IsPristine);
            }
        }
    };
    fill(tables);
}

bool callRunsWholeBodyOfCoroutine(const Program& program, const ThinSummary& summary, FunctionID id)
{
    const Function& function = program.at(id);
    if (!function.has(FunctionFlags::IsAsync) || function.has(FunctionFlags::IsGenerator) || function.isModuleBody() || summary.has(SummaryTrait::Suspends))
        return false;
    // The promise is resolved in front of the caller's next operation, and an object that it is resolved with is asked for `then`: a getter runs, on a promise too, of which the summary has no lookup.
    // Where the scan has seen that primitives alone are returned it has left no such record: ScanOfBody::settle(). A terminator has no site.
    return std::ranges::none_of(summary.accesses, [](const SummaryAccess& access) {
        return !access.site && access.whatHoldsNothing != noSummaryNode;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
