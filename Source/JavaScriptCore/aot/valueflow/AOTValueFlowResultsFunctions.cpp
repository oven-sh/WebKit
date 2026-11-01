/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBuiltinIDs.h"
#include "AOTClaimsAboutClassesAndClosures.h"
#include "AOTFlowStamps.h"
#include "AOTHeatGraph.h"
#include "AOTLedgerOfClassesAndClosures.h"
#include "AOTPhaseTimes.h"
#include "AOTPrototypesAtCreation.h"
#include "AOTSharedMemo.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTValueFlowResultsFieldsInternal.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <functional>
#include <optional>
#include <tuple>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct EnteredGroup {
    ValueSetID callees;
    ValueFlowCallKind kind { ValueFlowCallKind::Call };
};

struct GroupOfStatement {
    uint32_t statement { 0 };
    EnteredGroup group;
};

// The callees of a large group that bring a row. n calls that each enter any of n functions ask the same of the same group.
struct CalleesThatBringRows {
    struct Key {
        ValueSetID callees;
        ValueFlowCallKind kind { ValueFlowCallKind::Call };
        const CalleeSet* stampThatListsCallees { nullptr };
        bool namesNoEscapedClosure { false };

        friend bool operator==(const Key&, const Key&) = default;
    };

    Key key;
    Vector<ValueFlowObjectID> callees;
};

struct ListsByOwner {
    using Pair = std::pair<uint32_t, uint32_t>;

    ListsByOwner() = default;
    ListsByOwner(uint32_t numberOfOwners, std::span<const Pair> ownersAndMembers)
    {
        first.fill(0u, numberOfOwners + 1);
        for (const Pair& pair : ownersAndMembers)
            ++first[pair.first + 1];
        for (uint32_t owner = 0; owner < numberOfOwners; ++owner)
            first[owner + 1] += first[owner];
        members.grow(ownersAndMembers.size());
        Vector<uint32_t> next(first.span().first(numberOfOwners));
        for (const Pair& pair : ownersAndMembers)
            members[next[pair.first]++] = pair.second;
    }

    std::span<const uint32_t> of(uint32_t owner) const LIFETIME_BOUND { return members.subspan(first[owner], first[owner + 1] - first[owner]); }

    Vector<uint32_t> first;
    Vector<uint32_t> members;
};

struct EntriesWithoutStamp {
    BitVector functionsEnteredByHost;
    bool mayBeIntoAnyFunction { false };
};

} // anonymous namespace

static bool isEnteredOnDemand(const Function& function)
{
    return function.role == FunctionRole::Library || function.role == FunctionRole::Model || function.kind == FunctionKind::Module || function.kind == FunctionKind::CommonJsModule;
}

static bool hasCalleeStamp(const ValueFlowStatement& statement)
{
    return statement.kind == ValueFlowStatementKind::Operation && PayloadWord::callees(statement.opcode);
}

static void forEachRequestOf(const ValueFlowResultInputs& inputs, ValueFlowStatementID id, NOESCAPE const Invocable<void(const EnteredGroup&)> auto& group, NOESCAPE const Invocable<void(ValueFlowStatementID)> auto& madeStatement)
{
    inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::CallGroup }), true, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
        if (made == id)
            return;
        if (key.kind == ValueFlowMadeNodeKind::CallGroup)
            group(EnteredGroup { key.set, key.callKind() });
        if (made)
            madeStatement(made);
    });
}

static bool actsForNobodyKnown(const ValueFlowSolver& solver, ValueFlowStatementID id)
{
    switch (solver.statementAt(id).kind) {
    case ValueFlowStatementKind::Lookup:
        return solver.keyOf(id).name().kind == ValueFlowNameKind::FoundThroughEscapedInstances;
    case ValueFlowStatementKind::CallGroup:
        return ValueSets::isComplement(solver.keyOf(id).set) || solver.sets().contains(solver.keyOf(id).set, ValueFlowUniverse::unknownObject()) || solver.sets().contains(solver.keyOf(id).set, ValueFlowUniverse::untrackedObject());
    case ValueFlowStatementKind::Behavior:
    case ValueFlowStatementKind::Escape:
        return true;
    case ValueFlowStatementKind::Operation:
    case ValueFlowStatementKind::ModuleEvaluation:
    case ValueFlowStatementKind::Coroutine:
    case ValueFlowStatementKind::HostRoots:
    case ValueFlowStatementKind::UsedBuiltins:
    case ValueFlowStatementKind::UnseenCode:
    case ValueFlowStatementKind::ModulesLoadedByName:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool throwsBeforeEntry(const Function& callee, ValueFlowCallKind kind)
{
    if (isConstruction(kind))
        return callee.constructorForm() == ConstructorForm::NotConstructor;
    return callee.constructorForm() != ConstructorForm::NotConstructor && callee.kind != FunctionKind::Plain;
}

static BuiltinCallbackTimes mergeTimes(BuiltinCallbackTimes first, BuiltinCallbackTimes second)
{
    if (first == second)
        return first;
    bool isCalledPerEvent = first == BuiltinCallbackTimes::PerEvent || second == BuiltinCallbackTimes::PerEvent;
    return isCalledPerEvent ? BuiltinCallbackTimes::PerEvent : BuiltinCallbackTimes::Repeatedly;
}

static Expected<BuiltinCallbackTimes> timesOfCallbacks(const BuiltinTable& builtins, const CalleeSet* stamp)
{
    std::optional<BuiltinCallbackTimes> times;
    auto add = [&](BuiltinCallbackTimes other) {
        times = times ? mergeTimes(*times, other) : other;
    };
    if (stamp) {
        for (BuiltinID builtin : stamp->seen().predicted().builtins) {
            for (const BuiltinOverload& overload : builtins.overloadsOf(builtin)) {
                for (const BuiltinCallback& callback : overload.flow.callbacks)
                    add(callback.times);
                for (const BuiltinHandlerCall& handlerCall : overload.flow.handlerCalls)
                    add(handlerCall.times);
            }
        }
    }
    return times.value_or(BuiltinCallbackTimes::Once);
}

// What the program itself stored in the holders, under one name or under every name.
static ValueSetID seenValuesStoredIn(const ValueFlowResultInputs& inputs, ValueSetMaker& maker, ValueSetID holders, BuiltinName name)
{
    ValueSetID found = ValueSets::nothing;
    auto take = [&](std::span<const ValueFlowHeapIndex::WrittenNode> nodes) {
        for (const ValueFlowHeapIndex::WrittenNode& node : nodes) {
            if (!ValueSets::isComplement(node.seenValue) && inputs.sets().meet(node.objects, holders))
                found = maker.unite(found, node.seenValue);
        }
    };
    if (name) {
        if (const ValueFlowHeapIndex::NodesWrittenUnderName* nodes = inputs.heapIndex->findNodesWrittenUnder(ValueFlowName::property(inputs.program, inputs.program.keyOf(name))))
            take(nodes->nodes.span());
        return found;
    }
    for (const ValueFlowHeapIndex::Stretch& stretch : inputs.heapIndex->stretchesOfNodesWrittenUnder([](ValueFlowName written) { return written.kind == ValueFlowNameKind::Property; }))
        take(stretch.nodes);
    return found;
}

static std::optional<ValueSetID> seenValuesAt(const ValueFlowResultInputs& inputs, ValueSetMaker& maker, const ValueFlowStatement& call, BuiltinOperand position)
{
    if (passesList(call.opcode))
        return std::nullopt;
    std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(call);
    ValueSetID seen = ValueSets::nothing;
    switch (position.kind()) {
    case BuiltinOperand::Kind::Receiver:
        return inputs.seenValuesOf(maker, operands[1]);
    case BuiltinOperand::Kind::Argument:
        return position.argumentIndex() + 2 < operands.size() ? inputs.seenValuesOf(maker, operands[position.argumentIndex() + 2]) : ValueSets::nothing;
    case BuiltinOperand::Kind::ArgumentsFrom:
        for (size_t index = position.argumentIndex() + 2; index < operands.size(); ++index)
            seen = maker.unite(seen, inputs.seenValuesOf(maker, operands[index]));
        return seen;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A handler in an options object has escaped once the call has kept the object. Where other code may then store over it, the unknown object stands for it and no group of callees names it.
// The stores of the program still say which functions were registered.
static std::optional<ValueSetID> handlersSeenAt(const ValueFlowResultInputs& inputs, ValueSetMaker& maker, const ValueFlowStatement& call, const BuiltinPlace& place)
{
    if (place.kind != BuiltinPlaceKind::Member)
        return std::nullopt;
    std::optional<ValueSetID> holders = seenValuesAt(inputs, maker, call, *place.operand);
    if (!holders)
        return std::nullopt;
    ValueSetID objectsOfProgram = maker.intersect(inputs.universe().knownObjects(), ValueSets::complement(inputs.universe().builtinObjects()));
    for (BuiltinName step : place.memberPath) {
        holders = maker.intersect(*holders, objectsOfProgram);
        if (*holders == ValueSets::nothing)
            return ValueSets::nothing;
        holders = seenValuesStoredIn(inputs, maker, *holders, step);
    }
    return maker.intersect(*holders, inputs.universe().closures());
}

namespace {

// Which closures each row of the built-ins at a call was seen to take, and how often the row calls them.
struct CallbacksOfCall {
    struct Row {
        ValueSetID closures;
        BuiltinCallbackTimes times;
        ValueFlowCallKind kind;
    };

    Vector<Row, 8> rows;
    bool everyRowIsListed { true };
};

} // anonymous namespace

static CallbacksOfCall findCallbacksOfCall(const ValueFlowResultInputs& inputs, ValueSetMaker& maker, const ValueFlowStatement& call, const CalleeSet& stamp)
{
    CallbacksOfCall callbacks;
    auto add = [&](std::optional<ValueSetID> seen, BuiltinCallbackTimes times, ValueFlowCallKind kind) {
        if (!seen || ValueSets::isComplement(*seen)) {
            callbacks.everyRowIsListed = false;
            return;
        }
        if (ValueSetID closures = maker.intersect(*seen, inputs.universe().closures()); closures != ValueSets::nothing)
            callbacks.rows.append({ closures, times, kind });
    };
    for (BuiltinID builtin : stamp.seen().predicted().builtins) {
        for (const BuiltinOverload& overload : inputs.program.builtins().overloadsOf(builtin)) {
            for (const BuiltinCallback& callback : overload.flow.callbacks)
                add(seenValuesAt(inputs, maker, call, callback.callee), callback.times, callback.constructs ? ValueFlowCallKind::ConstructByNativeCode : ValueFlowCallKind::CallByNativeCode);
            for (const BuiltinHandlerCall& handlerCall : overload.flow.handlerCalls)
                add(handlersSeenAt(inputs, maker, call, handlerCall.callee), handlerCall.times, ValueFlowCallKind::CallByNativeCode);
        }
    }
    return callbacks;
}

static auto callOrderOf(const FlowIndirectCall& call)
{
    return std::tuple(call.caller, call.site, call.callee, call.kind);
}

static void mergeIndirectCalls(Vector<FlowIndirectCall>& calls, size_t first)
{
    auto standsBehind = [](const FlowIndirectCall& earlier, const FlowIndirectCall& later) {
        return callOrderOf(earlier) < callOrderOf(later);
    };
    if (std::ranges::adjacent_find(calls.span().subspan(first), std::not_fn(standsBehind)) == calls.span().end())
        return;
    AOT::sortInLine(calls.mutableSpan().subspan(first), { }, callOrderOf);
    size_t count = first;
    for (size_t index = first; index < calls.size(); ++index) {
        FlowIndirectCall call = calls[index];
        if (count == first || callOrderOf(calls[count - 1]) != callOrderOf(call)) {
            calls[count++] = call;
            continue;
        }
        FlowIndirectCall& merged = calls[count - 1];
        merged.isInCycle |= call.isInCycle;
        merged.isAlwaysConstruction &= call.isAlwaysConstruction;
        merged.times = mergeTimes(merged.times.predicted(), call.times.predicted());
    }
    calls.shrink(count);
}

static FlowIndirectCallKind kindOfIndirectCall(const ValueFlowStatement& statement, ValueFlowCallKind kind)
{
    switch (kind) {
    case ValueFlowCallKind::Get:
    case ValueFlowCallKind::Set:
        return FlowIndirectCallKind::Accessor;
    case ValueFlowCallKind::Call:
    case ValueFlowCallKind::Construct:
    case ValueFlowCallKind::CallByNativeCode:
    case ValueFlowCallKind::ConstructByNativeCode:
    case ValueFlowCallKind::CallByHost:
        return hasCalleeStamp(statement) ? FlowIndirectCallKind::BuiltinCallback : FlowIndirectCallKind::Protocol;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

// The accessors that the program lays on objects under keys that nobody knows.
class AccessorsUnderUnknownKeys {
public:
    AccessorsUnderUnknownKeys(const ValueFlowResultInputs&, const ValueFlowHeapIndex&, ValueSetMaker&);

    // The closures among them that a read or a store under the name may find on the objects themselves: none where the program also lays a value that the name may find.
    ValueSetID findOn(ValueSetMaker&, ValueSetID knownObjects, PropertyKeyID, ValueFlowCallKind) const;

private:
    using WrittenNode = ValueFlowHeapIndex::WrittenNode;

    void forEachNodeWrittenUnder(ValueFlowName name, NOESCAPE const Invocable<void(const WrittenNode&)> auto& functor) const
    {
        if (const auto* written = m_index.findNodesWrittenUnder(name))
            std::ranges::for_each(written->nodes, functor);
    }
    // A descriptor without a value of its own describes whatever lies above it: a value that nobody knows says nothing. It stands for all that has escaped, listed beside it or not.
    bool laysValue(const WrittenNode& node) const
    {
        if (ValueSets::isComplement(node.value))
            return false;
        if (m_sets.contains(node.value, ValueFlowUniverse::unknownObject()))
            return m_sets.meet(node.value, m_objectsInKnownHands);
        return !m_sets.isSubset(node.value, m_universe.undefinedValue());
    }

    const Program& m_program;
    const ValueFlowHeapIndex& m_index;
    const ValueSets& m_sets;
    const ValueFlowUniverse& m_universe;
    ValueSetID m_objectsInKnownHands;
    ValueSetID m_holdersOfValuesUnderUnknownKey { ValueSets::nothing };
    ValueSetID m_holdersOfGettersUnderUnknownKey { ValueSets::nothing };
    ValueSetID m_holdersOfSettersUnderUnknownKey { ValueSets::nothing };
};

} // anonymous namespace

AccessorsUnderUnknownKeys::AccessorsUnderUnknownKeys(const ValueFlowResultInputs& inputs, const ValueFlowHeapIndex& index, ValueSetMaker& maker)
    : m_program(inputs.program)
    , m_index(index)
    , m_sets(inputs.sets())
    , m_universe(inputs.universe())
    , m_objectsInKnownHands(maker.intersect(inputs.universe().knownObjects(), ValueSets::complement(inputs.solver().possibleValuesOf(ValueFlowProgramNodes::escaped()))))
{
    forEachNodeWrittenUnder(ValueFlowName::of(ValueFlowNameKind::UnknownKey), [&](const WrittenNode& node) {
        if (laysValue(node))
            m_holdersOfValuesUnderUnknownKey = maker.unite(m_holdersOfValuesUnderUnknownKey, node.objects);
    });
    auto findHolders = [&](ValueFlowNameKind kind) {
        ValueSetID holders = ValueSets::nothing;
        forEachNodeWrittenUnder(ValueFlowName::of(kind), [&](const WrittenNode& node) {
            if (m_sets.meet(node.value, m_universe.closures()))
                holders = maker.unite(holders, node.objects);
        });
        return holders;
    };
    m_holdersOfGettersUnderUnknownKey = maker.unite(findHolders(ValueFlowNameKind::GetterUnderUnknownKey), findHolders(ValueFlowNameKind::GetterUnderNameOfEnumeration));
    m_holdersOfSettersUnderUnknownKey = findHolders(ValueFlowNameKind::SetterUnderUnknownKey);
}

ValueSetID AccessorsUnderUnknownKeys::findOn(ValueSetMaker& maker, ValueSetID objects, PropertyKeyID key, ValueFlowCallKind kind) const
{
    bool isRead = kind == ValueFlowCallKind::Get;
    if (!m_sets.meet(objects, isRead ? m_holdersOfGettersUnderUnknownKey : m_holdersOfSettersUnderUnknownKey) || m_sets.meet(objects, m_holdersOfValuesUnderUnknownKey))
        return ValueSets::nothing;
    bool findsValue = false;
    forEachNodeWrittenUnder(ValueFlowName::property(m_program, key), [&](const WrittenNode& node) {
        findsValue |= laysValue(node) && m_sets.meet(node.objects, objects);
    });
    if (findsValue)
        return ValueSets::nothing;
    ValueSetID accessors = ValueSets::nothing;
    auto take = [&](const WrittenNode& node) {
        if (m_sets.meet(node.objects, objects))
            accessors = maker.unite(accessors, maker.intersect(node.value, m_universe.closures()));
    };
    forEachNodeWrittenUnder(ValueFlowName::of(isRead ? ValueFlowNameKind::GetterUnderUnknownKey : ValueFlowNameKind::SetterUnderUnknownKey), take);
    if (isRead)
        forEachNodeWrittenUnder(ValueFlowName::of(ValueFlowNameKind::GetterUnderNameOfEnumeration), take);
    return accessors;
}

static ValueSetID findEscapedAccessors(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueSetMaker& maker, ValueSetID receivers, ValueFlowName name)
{
    const ValueFlowHeapIndex::NodesWrittenUnderName* written = heap.index().findNodesWrittenUnder(name);
    if (!written)
        return ValueSets::nothing;
    const ValueSets& sets = inputs.sets();
    ValueSetID escapedClosures = maker.intersect(inputs.universe().closures(), heap.escaped());
    std::optional<ValueSetID> chain;
    ValueSetID accessors = ValueSets::nothing;
    for (const ValueFlowHeapIndex::WrittenNode& node : written->nodes) {
        if (ValueSets::isComplement(node.value) || !sets.meet(node.value, escapedClosures))
            continue;
        if (!chain)
            chain = heap.chainOf(maker, receivers);
        if (sets.meet(node.objects, *chain))
            accessors = maker.unite(accessors, maker.intersect(node.value, escapedClosures));
    }
    return accessors;
}

enum class GroupSource : bool { Rule, FinalState };

static EntriesWithoutStamp findEntriesWithoutStamp(const ValueFlowResultInputs& inputs, ValueFlowStampedCalls& stampedCalls, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const ValueFlowSolver& solver = inputs.solver();
    const ValueFlowBodies& bodies = inputs.bodies();
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    const FlowApplication& application = *tables.application;
    unsigned numberOfFunctions = program.functions.size();
    auto idOf = [&](const ValueFlowStatement& statement) {
        return ValueFlowStatementID(static_cast<uint32_t>(&statement - bodies.statements().data()));
    };

    uint32_t numberOfStatementsOfBodies = solver.input().statements.size();
    uint32_t numberOfMadeStatements = solver.numberOfStatements() - numberOfStatementsOfBodies;
    Vector<uint8_t> hasRun(FillWith { }, numberOfMadeStatements, static_cast<uint8_t>(0));
    Vector<uint8_t> actsForNobody(FillWith { }, numberOfMadeStatements, static_cast<uint8_t>(0));
    BitVector leadsToGroup(numberOfMadeStatements);
    Vector<uint8_t> isAskedFor(FillWith { }, numberOfMadeStatements, static_cast<uint8_t>(0));
    Vector<GroupOfStatement> groups;
    Vector<uint32_t> firstGroup;
    ListsByOwner statementsAskedFor;
    auto numberOf = [&](ValueFlowStatementID statement) {
        return statement.index() - numberOfStatementsOfBodies;
    };
    auto groupsOf = [&](uint32_t number) {
        return groups.subspan(firstGroup[number], firstGroup[number + 1] - firstGroup[number]);
    };
    auto noteAsker = [&](uint32_t number) {
        AtomicRef(isAskedFor[number]).store(1, std::memory_order_relaxed);
    };
    {
        struct Requests {
            Vector<GroupOfStatement> groups;
            Vector<ListsByOwner::Pair> askersAndAsked;
        };
        constexpr uint32_t statementsPerTask = 256;
        Vector<Requests> requestsOfTask((numberOfMadeStatements + statementsPerTask - 1) / statementsPerTask);
        forEachIndexInParallel(inputs.loops(), requestsOfTask.size(), [&](unsigned task) {
            Vector<GroupOfStatement>& groupsOfTask = requestsOfTask[task].groups;
            Vector<ListsByOwner::Pair>& askersAndAskedOfTask = requestsOfTask[task].askersAndAsked;
            uint32_t end = std::min(numberOfMadeStatements, (task + 1) * statementsPerTask);
            for (uint32_t number = task * statementsPerTask; number < end; ++number) {
                ValueFlowStatementID id(numberOfStatementsOfBodies + number);
                if (solver.isAtRest(id))
                    continue;
                hasRun[number] = 1;
                if (actsForNobodyKnown(solver, id))
                    actsForNobody[number] = 1;
                size_t firstGroupOfStatement = groupsOfTask.size();
                size_t firstAskedByStatement = askersAndAskedOfTask.size();
                forEachRequestOf(inputs, id, [&](const EnteredGroup& group) {
                    if (sets.meet(group.callees, universe.closures()))
                        groupsOfTask.append(GroupOfStatement { number, group });
                }, [&](ValueFlowStatementID made) {
                    askersAndAskedOfTask.append({ number, numberOf(made) });
                    noteAsker(numberOf(made));
                });

                auto orderOfGroup = [](const GroupOfStatement& entry) {
                    return std::tuple(entry.group.callees, entry.group.kind);
                };
                auto groupsOfStatement = groupsOfTask.mutableSpan().subspan(firstGroupOfStatement);
                AOT::sort(groupsOfStatement, { }, orderOfGroup);
                groupsOfTask.shrink(groupsOfTask.size() - std::ranges::unique(groupsOfStatement, { }, orderOfGroup).size());
                auto asked = askersAndAskedOfTask.mutableSpan().subspan(firstAskedByStatement);
                AOT::sortInLine(asked);
                askersAndAskedOfTask.shrink(askersAndAskedOfTask.size() - std::ranges::unique(asked).size());
            }
        });

        size_t numberOfGroups = 0;
        size_t numberOfAsked = 0;
        for (const Requests& requests : requestsOfTask) {
            numberOfGroups += requests.groups.size();
            numberOfAsked += requests.askersAndAsked.size();
        }
        Vector<ListsByOwner::Pair> askersAndAsked;
        groups.reserveInitialCapacity(numberOfGroups);
        askersAndAsked.reserveInitialCapacity(numberOfAsked);
        for (Requests& requests : requestsOfTask) {
            groups.appendVector(WTF::move(requests.groups));
            askersAndAsked.appendVector(WTF::move(requests.askersAndAsked));
        }
        firstGroup.fill(0u, numberOfMadeStatements + 1);
        for (const GroupOfStatement& entry : groups)
            ++firstGroup[entry.statement + 1];
        for (uint32_t number = 0; number < numberOfMadeStatements; ++number)
            firstGroup[number + 1] += firstGroup[number];
        statementsAskedFor = ListsByOwner(numberOfMadeStatements, askersAndAsked.span());

        for (ListsByOwner::Pair& pair : askersAndAsked)
            std::swap(pair.first, pair.second);
        ListsByOwner askers(numberOfMadeStatements, askersAndAsked.span());
        Vector<uint32_t> worklist;
        for (const GroupOfStatement& entry : groups) {
            if (!leadsToGroup.quickSet(entry.statement))
                worklist.append(entry.statement);
        }
        while (!worklist.isEmpty()) {
            for (uint32_t asker : askers.of(worklist.takeLast())) {
                if (!leadsToGroup.quickSet(asker))
                    worklist.append(asker);
            }
        }
    }

    // Only an estimate of how often code runs reads these, and a very large program has none.
    const bool listsWhatWasSeen = !isVeryLargeProgram(program, inputs.build);
    ValueFlowHeapView heap(inputs);
    std::optional<AccessorsUnderUnknownKeys> accessorsUnderUnknownKeys;
    if (listsWhatWasSeen)
        accessorsUnderUnknownKeys.emplace(inputs, heap.index(), heap.maker());
    SharedMemo<CalleesThatBringRows> calleesThatBringRows;
    tables.functions.indirectCalls = collectInParallel<FlowIndirectCall>(inputs.loops(), numberOfFunctions, [&](unsigned index, Vector<FlowIndirectCall>& calls) {
        FunctionID id(index);
        if (!inputs.isReached(id))
            return;
        const Function& function = program.functions[index];
        size_t first = calls.size();
        bool hasPreparedSites = false;
        BitVector valuesInCycles;
        for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
            const FlowCallStamp* stampOfCall = hasCalleeStamp(statement) ? findByValue(application.bodies[index].calls.span(), statement.site, &FlowCallStamp::value) : nullptr;
            const CalleeSet* stamp = stampOfCall ? &program.at(stampOfCall->callees) : nullptr;
            std::optional<BuiltinCallbackTimes> timesOfCallbacksOfStatement;
            std::optional<CallbacksOfCall> callbacksOfStatement;
            ValueSetID escaped = solver.possibleValuesOf(ValueFlowProgramNodes::escaped());
            size_t firstOfStatement = calls.size();
            // A row says the same whichever group of one kind brings it, so a large group brings rows only for the callees that no large group of its kind has brought.
            AOT_THRESHOLD(size_t, fewestCalleesListedOnce, 8);
            std::array<ValueSetID, static_cast<size_t>(ValueFlowCallKind::CallByHost) + 1> listedByKind;
            listedByKind.fill(ValueSets::nothing);
            std::optional<AccessSiteID> siteOfStatement;
            // The accessors that one site enters count together, however many of its receivers find each of them.
            auto nameNoEscapedAccessorPastThoseToldApart = [&] {
                Vector<FunctionID, 8> accessors;
                for (const FlowIndirectCall& call : calls.span().subspan(firstOfStatement)) {
                    if (call.kind == FlowIndirectCallKind::Accessor && !accessors.contains(call.callee))
                        accessors.append(call.callee);
                }
                if (accessors.size() <= inputs.build.limit(LimitID::MaxValueFlowMembersToldApart))
                    return;
                calls.removeAllMatching([&](const FlowIndirectCall& call) {
                    return call.kind == FlowIndirectCallKind::Accessor && sets.contains(escaped, universe.closureOf(call.callee));
                }, firstOfStatement);
            };
            std::optional<ValueSetID> knownEscapedAccessors;
            auto escapedAccessorsAtSite = [&](ValueSetMaker& maker) {
                if (knownEscapedAccessors)
                    return *knownEscapedAccessors;
                knownEscapedAccessors = ValueSets::nothing;
                bool isRead = statement.opcode == Opcode::PropGet;
                if (!listsWhatWasSeen || statement.kind != ValueFlowStatementKind::Operation || !statement.site || (!isRead && statement.opcode != Opcode::PropSet))
                    return ValueSets::nothing;
                static_assert(!OperandIndex::PropGet::object && !OperandIndex::PropSet::object);
                ValueSetID receivers = std::as_const(heap).possibleValuesOf(maker, bodies.operandsOf(statement)[0]);
                if (ValueSets::isComplement(receivers))
                    return ValueSets::nothing;
                PropertyKeyID key = function.at(statement.site).key();
                // A read of a primitive starts at the prototype of its kind.
                Vector<ValueFlowObjectID, 8> prototypesOfPrimitives;
                sets.forEachMember(maker.intersect(receivers, maker.unite(universe.primitives(), universe.keys())), [&](ValueFlowObjectID primitive) {
                    forEachPrototypeAtCreation(program, universe, primitive, [&](ValueFlowObjectID prototype) {
                        prototypesOfPrimitives.append(prototype);
                    }, [](SilentAboutPrototype) { });
                });
                ValueSetID starts = maker.unite(maker.intersect(receivers, universe.knownObjects()), maker.of(prototypesOfPrimitives.span()));
                knownEscapedAccessors = findEscapedAccessors(inputs, heap, maker, starts, isRead ? ValueFlowName::getter(program, key) : ValueFlowName::setter(program, key));
                return *knownEscapedAccessors;
            };
            // A rule sees an escaped object only until the unknown object covers it, and is not run again when that happens, so which escaped closures its groups name depends on the order of the rules.
            // The final state answers instead, where it can: it lists no callbacks in a very large program, or of a call whose arguments it cannot tell apart.
            auto add = [&](EnteredGroup group, const CalleeSet* stampThatListsCallees, GroupSource source) {
                if (ValueSets::isComplement(group.callees))
                    return;
                bool finalStateAnswers = listsWhatWasSeen && (!callbacksOfStatement || callbacksOfStatement->everyRowIsListed);
                if (source == GroupSource::Rule && finalStateAnswers && sets.meet(group.callees, escaped)) {
                    BorrowedValueSetMaker maker(inputs.makerPool());
                    bool entersAccessors = group.kind == ValueFlowCallKind::Get || group.kind == ValueFlowCallKind::Set;
                    ValueSetID kept = maker.get().unite(ValueSets::complement(escaped), entersAccessors ? escapedAccessorsAtSite(maker.get()) : ValueSets::nothing);
                    group.callees = maker.get().intersect(group.callees, kept);
                }
                bool namesNoEscapedClosure = sets.size(group.callees) > inputs.build.limit(LimitID::MaxValueFlowMembersToldApart);
                ValueSetID callees = group.callees;
                if (sets.size(callees) >= fewestCalleesListedOnce) {
                    BorrowedValueSetMaker maker(inputs.makerPool());
                    ValueSetID& listed = listedByKind[static_cast<size_t>(group.kind)];
                    callees = maker.get().intersect(callees, ValueSets::complement(listed));
                    if (!stampThatListsCallees)
                        listed = maker.get().unite(listed, namesNoEscapedClosure ? maker.get().intersect(callees, ValueSets::complement(escaped)) : callees);
                }
                auto bringsRow = [&](ValueFlowObjectID callee) {
                    FunctionID entered = universe.closureFunction(callee);
                    if (!entered || throwsBeforeEntry(program.at(entered), group.kind))
                        return false;
                    if (namesNoEscapedClosure && sets.contains(escaped, callee))
                        return false;
                    return !stampThatListsCallees || !std::ranges::binary_search(stampThatListsCallees->followed().value(), entered);
                };
                auto forEachCalleeThatBringsRow = [&](const auto& functor) {
                    AOT_THRESHOLD(size_t, fewestCalleesAskedOnce, 16);
                    if (sets.size(callees) < fewestCalleesAskedOnce) {
                        sets.forEachMemberInLine(callees, [&](ValueFlowObjectID callee) {
                            if (bringsRow(callee))
                                functor(callee);
                        });
                        return;
                    }
                    CalleesThatBringRows::Key key { callees, group.kind, stampThatListsCallees, namesNoEscapedClosure };
                    uint64_t mixed = (static_cast<uint64_t>(callees.index()) << 8 | static_cast<uint64_t>(group.kind) << 1 | static_cast<uint64_t>(namesNoEscapedClosure)) * 0x9e3779b97f4a7c15ull;
                    uint32_t hash = static_cast<uint32_t>((mixed ^ std::bit_cast<uintptr_t>(stampThatListsCallees)) * 0x9e3779b97f4a7c15ull >> 32);
                    auto isWanted = [&](const CalleesThatBringRows& entry) {
                        return entry.key == key;
                    };
                    const CalleesThatBringRows* known = calleesThatBringRows.find(hash, isWanted);
                    if (!known) {
                        CalleesThatBringRows found { key, { } };
                        sets.forEachMember(callees, [&](ValueFlowObjectID callee) {
                            if (bringsRow(callee))
                                found.callees.append(callee);
                        });
                        known = &calleesThatBringRows.add(hash, WTF::move(found), isWanted);
                    }
                    for (ValueFlowObjectID callee : known->callees)
                        functor(callee);
                };
                forEachCalleeThatBringsRow([&](ValueFlowObjectID callee) {
                    FunctionID entered = universe.closureFunction(callee);
                    if (!std::exchange(hasPreparedSites, true)) {
                        valuesInCycles = BitVector(function.values.size());
                        CFG cfg(function);
                        BlockCycles cycles(cfg);
                        for (BlockID block : cfg.reversePostOrder()) {
                            if (!cycles.isInCycle(block))
                                continue;
                            for (ValueID value : function.at(block).values)
                                valuesInCycles.quickSet(value.index());
                        }
                    }
                    FlowIndirectCall call { .caller = id, .callee = entered, .kind = kindOfIndirectCall(statement, group.kind), .times = BuiltinCallbackTimes::Once, .isAlwaysConstruction = isConstruction(group.kind) };
                    if (statement.site) {
                        if (!siteOfStatement)
                            siteOfStatement = application.accessSites.siteOfValue(id, statement.site);
                        call.site = *siteOfStatement;
                        call.isInCycle = valuesInCycles.quickGet(statement.site.index());
                    }
                    if (call.kind == FlowIndirectCallKind::BuiltinCallback) {
                        std::optional<BuiltinCallbackTimes> timesOfRows;
                        if (callbacksOfStatement) {
                            for (const CallbacksOfCall::Row& row : callbacksOfStatement->rows) {
                                if (sets.contains(row.closures, callee))
                                    timesOfRows = timesOfRows ? mergeTimes(*timesOfRows, row.times) : row.times;
                            }
                        }
                        if (!timesOfRows && (!callbacksOfStatement || !callbacksOfStatement->everyRowIsListed)) {
                            if (!timesOfCallbacksOfStatement)
                                timesOfCallbacksOfStatement = timesOfCallbacks(program.builtins(), stamp).predicted();
                            timesOfRows = *timesOfCallbacksOfStatement;
                        }
                        call.times = timesOfRows.value_or(BuiltinCallbackTimes::Once);
                    }
                    calls.append(call);
                });
            };

            if (listsWhatWasSeen && stamp && !stamp->seen().predicted().builtins.empty()) {
                BorrowedValueSetMaker maker(inputs.makerPool());
                callbacksOfStatement = findCallbacksOfCall(inputs, maker.get(), statement, *stamp);
                for (const CallbacksOfCall::Row& row : callbacksOfStatement->rows)
                    add(EnteredGroup { row.closures, row.kind }, nullptr, GroupSource::FinalState);
            }

            Vector<uint32_t, 8> worklist;
            auto follow = [&](uint32_t number) {
                if (leadsToGroup.quickGet(number) && !actsForNobody[number])
                    worklist.append(number);
            };
            forEachRequestOf(inputs, idOf(statement), [&](const EnteredGroup& group) {
                bool isGroupOfCallees = (group.kind == ValueFlowCallKind::Call || group.kind == ValueFlowCallKind::Construct) && function.role != FunctionRole::Model;
                add(group, isGroupOfCallees ? stamp : nullptr, GroupSource::Rule);
            }, [&](ValueFlowStatementID made) {
                noteAsker(numberOf(made));
                follow(numberOf(made));
            });
            // Whoever holds an accessor that has escaped enters it, so no rule has to enter it from the read or the store that finds it. They run it all the same.
            auto addAccessorsLaidOnReceivers = [&] {
                bool isRead = statement.opcode == Opcode::PropGet;
                if (!accessorsUnderUnknownKeys || statement.kind != ValueFlowStatementKind::Operation || !statement.site || (!isRead && statement.opcode != Opcode::PropSet))
                    return false;
                static_assert(!OperandIndex::PropGet::object && !OperandIndex::PropSet::object);
                BorrowedValueSetMaker maker(inputs.makerPool());
                ValueSetID receivers = std::as_const(heap).possibleValuesOf(maker.get(), bodies.operandsOf(statement)[0]);
                if (ValueSets::isComplement(receivers) || !sets.meet(receivers, universe.knownObjects()))
                    return false;
                ValueFlowCallKind kind = isRead ? ValueFlowCallKind::Get : ValueFlowCallKind::Set;
                ValueSetID accessors = accessorsUnderUnknownKeys->findOn(maker.get(), maker.get().intersect(receivers, universe.knownObjects()), function.at(statement.site).key(), kind);
                if (accessors == ValueSets::nothing)
                    return false;
                add(EnteredGroup { accessors, kind }, nullptr, GroupSource::FinalState);
                return true;
            };
            if (!addAccessorsLaidOnReceivers() && worklist.isEmpty())
                continue;

            HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> seen;
            while (!worklist.isEmpty()) {
                uint32_t number = worklist.takeLast();
                if (!seen.add(number).isNewEntry)
                    continue;
                for (const GroupOfStatement& entry : groupsOf(number))
                    add(entry.group, nullptr, GroupSource::Rule);
                for (uint32_t asked : statementsAskedFor.of(number))
                    follow(asked);
            }
            nameNoEscapedAccessorPastThoseToldApart();
        }
        mergeIndirectCalls(calls, first);
    });

    EntriesWithoutStamp entries;
    BitVector& isEnteredByHost = entries.functionsEnteredByHost;
    isEnteredByHost = BitVector(numberOfFunctions);
    BitVector seen(numberOfMadeStatements);
    Vector<uint32_t> worklist;
    auto follow = [&](uint32_t number) {
        if (leadsToGroup.quickGet(number) && !seen.quickSet(number))
            worklist.append(number);
    };
    auto enterFromHost = [&](const EnteredGroup& group) {
        if (ValueSets::isComplement(group.callees))
            return;
        inputs.sets().forEachMember(group.callees, [&](ValueFlowObjectID callee) {
            FunctionID function = universe.closureFunction(callee);
            if (function && !throwsBeforeEntry(program.at(function), group.kind))
                isEnteredByHost.quickSet(function.index());
        });
    };
    for (const ValueFlowStatement& statement : bodies.statementsOf(bodies.hostBody())) {
        forEachRequestOf(inputs, idOf(statement), enterFromHost, [&](ValueFlowStatementID made) {
            noteAsker(numberOf(made));
            follow(numberOf(made));
        });
    }
    for (uint32_t number = 0; number < numberOfMadeStatements; ++number) {
        if (hasRun[number] && (actsForNobody[number] || !isAskedFor[number]))
            follow(number);
    }
    while (!worklist.isEmpty()) {
        uint32_t number = worklist.takeLast();
        for (const GroupOfStatement& entry : groupsOf(number))
            enterFromHost(entry.group);
        for (uint32_t asked : statementsAskedFor.of(number))
            follow(asked);
    }
    // The host hands the default export of an entry to Bun.serve. Its `fetch` then gets the requests, as that of a program that calls Bun.serve itself.
    BitVector getsRequests(numberOfFunctions);
    for (const Entry& entry : program.entries) {
        for (const HostRead& read : entry.hostReads) {
            if (!listsWhatWasSeen || !read.builtin || program.builtins().canonicalID(read.builtin) != Builtins::BunServe)
                continue;
            auto exported = std::ranges::find(program.at(entry.module).exports, read.exportName, &ModuleExport::name);
            if (exported == program.at(entry.module).exports.end() || !exported->variable)
                continue;
            ValueSetID configurations = solver.possibleValuesOf(bodies.nodes().variable(exported->variable));
            if (ValueSets::isComplement(configurations))
                continue;
            ValueSetID handlers = heap.writtenUnder(heap.maker().intersect(configurations, universe.knownObjects()), ValueFlowName::property(program, program.keyOf(BuiltinNames::fetch)));
            if (ValueSets::isComplement(handlers))
                continue;
            sets.forEachMember(handlers, [&](ValueFlowObjectID handler) {
                if (FunctionID function = universe.closureFunction(handler); function && inputs.isReached(function))
                    getsRequests.quickSet(function.index());
            });
        }
    }
    BitVector isCalledByHost = getsRequests;
    for (size_t function : isEnteredByHost) {
        if (!listsWhatWasSeen || !inputs.hasEscaped(universe.closureOf(FunctionID(static_cast<uint32_t>(function)))))
            isCalledByHost.quickSet(function);
    }
    for (size_t function : isCalledByHost)
        tables.functions.indirectCalls.append(FlowIndirectCall { .callee = FunctionID(static_cast<uint32_t>(function)), .kind = FlowIndirectCallKind::Host, .times = getsRequests.quickGet(function) ? BuiltinCallbackTimes::PerEvent : BuiltinCallbackTimes::Repeatedly });
    stampedCalls.functionsEnteredWithoutStamp = isEnteredByHost;
    for (const FlowIndirectCall& call : tables.functions.indirectCalls)
        stampedCalls.functionsEnteredWithoutStamp.quickSet(call.callee.index());

    std::atomic<bool> mayBeIntoAnyFunction { false };
    std::atomic<bool> callsUnknownObject { false };
    std::atomic<bool> callsUntrackedObject { false };
    auto raiseFlag = [](std::atomic<bool>& flag, NOESCAPE const Invocable<bool()> auto& holds) {
        if (!flag.load(std::memory_order_relaxed) && holds())
            flag.store(true, std::memory_order_relaxed);
    };
    RELEASE_ASSERT(inputs.heapIndex);
    std::span<const ValueFlowNodeID> firstNodesOfCallGroups = inputs.heapIndex->firstNodesOfCallGroups();
    forEachIndexInParallel(inputs.loops(), firstNodesOfCallGroups.size(), [&](unsigned index) {
        const ValueFlowNodeKey& key = *solver.keyOfMadeNode(firstNodesOfCallGroups[index]);
        raiseFlag(mayBeIntoAnyFunction, [&] { return ValueSets::isComplement(key.set); });
        raiseFlag(callsUnknownObject, [&] { return inputs.sets().contains(key.set, ValueFlowUniverse::unknownObject()); });
        raiseFlag(callsUntrackedObject, [&] { return inputs.sets().contains(key.set, ValueFlowUniverse::untrackedObject()); });
    });
    entries.mayBeIntoAnyFunction = mayBeIntoAnyFunction.load(std::memory_order_relaxed);
    stampedCalls.functionsEnteredWithoutRecord = BitVector(numberOfFunctions);
    auto enterClosuresIn = [&](ValueFlowNodeID covered) {
        sets.forEachMemberInLine(solver.possibleValuesOf(covered), [&](ValueFlowObjectID member) {
            if (FunctionID function = universe.closureFunction(member))
                stampedCalls.functionsEnteredWithoutRecord.quickSet(function.index());
        });
    };
    if (callsUnknownObject.load(std::memory_order_relaxed))
        enterClosuresIn(ValueFlowProgramNodes::escaped());
    if (callsUntrackedObject.load(std::memory_order_relaxed))
        enterClosuresIn(ValueFlowProgramNodes::lost());
    if (entries.mayBeIntoAnyFunction) {
        for (size_t index = 0; index < numberOfFunctions; ++index)
            stampedCalls.functionsEnteredWithoutRecord.quickSet(index);
    }
    stampedCalls.functionsEnteredWithoutStamp.merge(stampedCalls.functionsEnteredWithoutRecord);
    return entries;
}

static void forEachFunctionWithinReachOf(const ValueFlowResultInputs& inputs, FunctionID id, NOESCAPE const Invocable<void(FunctionID)> auto& functor)
{
    const Function& function = inputs.program.at(id);
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowProgramNodes& nodes = inputs.bodies().nodes();
    auto forEachClosureIn = [&](ValueFlowNodeID node) {
        ValueSetID held = inputs.solver().possibleValuesOf(node);
        if (ValueSets::isComplement(held) || !inputs.sets().meet(held, universe.closures()))
            return;
        inputs.sets().forEachMember(held, [&](ValueFlowObjectID member) {
            if (FunctionID closureFunction = universe.closureFunction(member))
                functor(closureFunction);
        });
    };
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        if (function.entryParameterKind(index) == EntryParameter::Capture)
            forEachClosureIn(nodes.entryParameter(id, index));
    }
    for (const Value& value : function.values) {
        switch (value.opcode) {
        case Opcode::CallDirect:
        case Opcode::NewClosure:
            functor(value.function());
            break;
        case Opcode::GetModuleVariable:
            forEachClosureIn(nodes.variable(value.variable()));
            break;
        case Opcode::GetEnvironmentField:
            forEachClosureIn(nodes.environmentField(value.layout(), value.field()));
            break;
        default:
            break;
        }
    }
}

static BitVector findFunctionsNamedByUnsolvedCode(const ValueFlowResultInputs& inputs, const FlowResultTables& tables)
{
    const Program& program = inputs.program;
    BitVector functions(program.functions.size());
    Vector<FunctionID> allNamed = collectInParallel<FunctionID>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
        if (!isUnsolvedCode(inputs, tables, FunctionID(index)))
            return;
        forEachFunctionWithinReachOf(inputs, FunctionID(index), [&](FunctionID named) {
            found.append(named);
        });
    });
    for (FunctionID named : allNamed)
        functions.quickSet(named.index());
    return functions;
}

static bool isOnlyCalled(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    return ClaimsAboutClassesAndClosures::useOfClosure(inputs.classesAndClosures->reachersOf(object).value_or(Reachers { { }, everyActor })) == ClosureUse::CalledOnly;
}

static bool mayHold(const ValueFlowResultInputs& inputs, const ValueFlowOperand& operand, ValueFlowObjectID object)
{
    return (operand.constant && inputs.sets().contains(operand.constant, object)) || (operand.node && inputs.sets().contains(inputs.solver().seenValuesOf(operand.node), object));
}

static Vector<FlowBoundFunctionUse> findBoundFunctionsOnlyCalled(const ValueFlowResultInputs& inputs)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    BuiltinID bindFunction = program.builtins().canonicalID(Builtins::FunctionPrototypeBind);
    if (!inputs.solver().usesOf(bindFunction).contains(BuiltinUse::Called))
        return { };

    struct BoundValues {
        ValueSetID all { ValueSets::nothing };
        ValueSetID withResultUsedOtherwise { ValueSets::nothing };
    };
    using BoundValuesByNumberOfArguments = Vector<BoundValues, 4>;
    auto add = [](ValueSetMaker& maker, BoundValuesByNumberOfArguments& values, uint32_t numberOfBoundArguments, const BoundValues& added) {
        if (values.size() <= numberOfBoundArguments)
            values.grow(numberOfBoundArguments + 1);
        BoundValues& bound = values[numberOfBoundArguments];
        bound.all = maker.unite(bound.all, added.all);
        bound.withResultUsedOtherwise = maker.unite(bound.withResultUsedOtherwise, added.withResultUsedOtherwise);
    };

    ValueFlowObjectID bind = universe.objectOf(bindFunction);
    ValueSetMakers makers(inputs);
    Vector<BoundValuesByNumberOfArguments> valuesOfWorker(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), program.functions.size(), [&](unsigned index, unsigned worker) {
        if (!inputs.isReached(FunctionID(index)))
            return;
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::Call)
                continue;
            std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(statement);
            if (!mayHold(inputs, operands[0], bind))
                continue;
            ValueSetMaker& maker = makers.of(worker);
            ValueSetID targets = inputs.seenValuesOf(maker, operands[1]);
            if (ValueSets::isComplement(targets))
                continue;
            ValueFlowObjectID result = universe.siteObject(statement);
            bool resultIsOnlyCalled = result && isOnlyCalled(inputs, result);
            uint32_t numberOfBoundArguments = operands.size() > 3 ? operands.size() - 3 : 0;
            add(maker, valuesOfWorker[worker], numberOfBoundArguments, { targets, resultIsOnlyCalled ? ValueSets::nothing : targets });
        }
    });

    ValueSetMaker& maker = makers.of(0);
    BoundValuesByNumberOfArguments values;
    for (const BoundValuesByNumberOfArguments& valuesOfOneWorker : valuesOfWorker) {
        for (uint32_t numberOfBoundArguments = 0; numberOfBoundArguments < valuesOfOneWorker.size(); ++numberOfBoundArguments)
            add(maker, values, numberOfBoundArguments, valuesOfOneWorker[numberOfBoundArguments]);
    }
    struct BindCall {
        FunctionID target;
        uint32_t numberOfBoundArguments { 0 };
        bool resultIsOnlyCalled { false };
    };
    Vector<BindCall> calls;
    for (uint32_t numberOfBoundArguments = 0; numberOfBoundArguments < values.size(); ++numberOfBoundArguments) {
        const BoundValues& bound = values[numberOfBoundArguments];
        inputs.sets().forEachMember(bound.all, [&](ValueFlowObjectID target) {
            if (FunctionID function = universe.closureFunction(target))
                calls.append(BindCall { function, numberOfBoundArguments, !inputs.sets().contains(bound.withResultUsedOtherwise, target) });
        });
    }
    auto callOrderOf = [](const BindCall& call) {
        return std::tuple(call.target, call.numberOfBoundArguments);
    };
    AOT::sort(calls, { }, callOrderOf);

    Vector<FlowBoundFunctionUse> uses;
    for (size_t first = 0; first < calls.size();) {
        size_t end = first;
        bool everyResultIsOnlyCalled = true;
        for (; end < calls.size() && callOrderOf(calls[end]) == callOrderOf(calls[first]); ++end)
            everyResultIsOnlyCalled &= calls[end].resultIsOnlyCalled;
        FunctionID target = calls[first].target;
        ValueFlowObjectID closure = universe.closureOf(target);
        bool isBoundOnlyHere = !inputs.hasEscaped(closure) && !inputs.isLost(closure) && !inputs.isHeldByNativeCode(closure);
        if (everyResultIsOnlyCalled && isBoundOnlyHere && inputs.build.isEnabled(FlowResultKind::ObjectUses, target.index()))
            uses.append(FlowBoundFunctionUse { target, calls[first].numberOfBoundArguments, ClosureUse::CalledOnly });
        first = end;
    }
    return uses;
}

// Finds the calls of bind whose receiver was seen to be a closure of one function, and the calls that their results reach as the callee.
static Vector<FlowBoundFunctionAtSite> findBoundFunctionsAtSites(const ValueFlowResultInputs& inputs, const FlowAccessSiteNumbering& accessSites)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    BuiltinID bindFunction = program.builtins().canonicalID(Builtins::FunctionPrototypeBind);
    if (!inputs.solver().usesOf(bindFunction).contains(BuiltinUse::Called))
        return { };

    struct MadeByBind {
        ValueFlowObjectID object;
        FlowBoundFunctionAtSite made;
    };
    auto forEachCallIn = [&](unsigned index, NOESCAPE const Invocable<void(const ValueFlowStatement&)> auto& functor) {
        if (!inputs.isReached(FunctionID(index)) || !inputs.build.isEnabled(FlowResultKind::CalleeSets, index))
            return;
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (statement.kind == ValueFlowStatementKind::Operation && statement.opcode == Opcode::Call)
                functor(statement);
        }
    };
    ValueFlowObjectID bind = universe.objectOf(bindFunction);
    Vector<MadeByBind> made = collectInParallel<MadeByBind>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<MadeByBind>& found) {
        forEachCallIn(index, [&](const ValueFlowStatement& statement) {
            std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(statement);
            if (!mayHold(inputs, operands[0], bind))
                return;
            BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
            ValueSetMaker& maker = borrowedMaker.get();
            ValueSetID targets = inputs.seenValuesOf(maker, operands[1]);
            ValueFlowObjectID result = universe.siteObject(statement);
            if (!result || ValueSets::isComplement(targets))
                return;
            FunctionID onlyTarget;
            bool hasSeveral = false;
            inputs.sets().forEachMember(maker.intersect(targets, universe.closures()), [&](ValueFlowObjectID target) {
                FunctionID function = universe.closureFunction(target);
                hasSeveral |= onlyTarget && onlyTarget != function;
                onlyTarget = function;
            });
            if (!onlyTarget || hasSeveral)
                return;
            uint32_t numberOfBoundArguments = operands.size() > 3 ? operands.size() - 3 : 0;
            found.append({ result, { accessSites.siteOfValue(FunctionID(index), statement.site), FlowBoundFunctionUseAtSite::Made, onlyTarget, numberOfBoundArguments } });
        });
    });
    if (made.isEmpty())
        return { };

    AOT::sort(made, { }, &MadeByBind::object);
    Vector<ValueFlowObjectID> objects(made.size(), [&](size_t index) {
        return made[index].object;
    });
    ValueSetID everyObject = BorrowedValueSetMaker(inputs.makerPool()).get().of(objects.span());
    Vector<FlowBoundFunctionAtSite> result = collectInParallel<FlowBoundFunctionAtSite>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<FlowBoundFunctionAtSite>& found) {
        forEachCallIn(index, [&](const ValueFlowStatement& statement) {
            const ValueFlowOperand& operand = inputs.bodies().operandsOf(statement)[0];
            if (!operand.node || !inputs.sets().meet(inputs.solver().seenValuesOf(operand.node), everyObject))
                return;
            BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
            ValueSetMaker& maker = borrowedMaker.get();
            ValueSetID callees = inputs.seenValuesOf(maker, operand);
            // A call that may get anything would otherwise expect the program's one bound function.
            if (ValueSets::isComplement(callees))
                return;
            AccessSiteID site = accessSites.siteOfValue(FunctionID(index), statement.site);
            inputs.sets().forEachMember(maker.intersect(callees, everyObject), [&](ValueFlowObjectID callee) {
                const MadeByBind* entry = std::ranges::lower_bound(made, callee, { }, &MadeByBind::object);
                found.append({ site, FlowBoundFunctionUseAtSite::Called, entry->made.target, entry->made.numberOfBoundArguments });
            });
        });
    });
    for (const MadeByBind& entry : made)
        result.append(entry.made);
    AOT::sort(result, { }, [](const FlowBoundFunctionAtSite& bound) {
        return std::tuple(bound.site, bound.use, bound.target, bound.numberOfBoundArguments);
    });
    result.shrink(std::ranges::unique(result).begin() - result.begin());
    return result;
}

// Finds the calls of bind that can only bind undefined and no argument to one built-in function, and the calls that no other bound function can reach as the callee.
static Vector<FlowBuiltinBoundAtSite> findBuiltinsBoundAtSites(const ValueFlowResultInputs& inputs, const FlowAccessSiteNumbering& accessSites)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    BuiltinID bindFunction = program.builtins().canonicalID(Builtins::FunctionPrototypeBind);
    if (!inputs.solver().usesOf(bindFunction).contains(BuiltinUse::Called))
        return { };

    struct MadeByBind {
        ValueFlowObjectID object;
        BuiltinID target;
    };
    auto forEachCallIn = [&](unsigned index, NOESCAPE const Invocable<void(const ValueFlowStatement&)> auto& functor) {
        if (!inputs.isReached(FunctionID(index)) || !inputs.build.isEnabled(FlowResultKind::CalleeSets, index))
            return;
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (statement.kind == ValueFlowStatementKind::Operation && statement.opcode == Opcode::Call)
                functor(statement);
        }
    };
    ValueFlowObjectID bind = universe.objectOf(bindFunction);
    Vector<MadeByBind> made = collectInParallel<MadeByBind>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<MadeByBind>& found) {
        forEachCallIn(index, [&](const ValueFlowStatement& statement) {
            std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(statement);
            ValueFlowObjectID result = universe.siteObject(statement);
            if (!result || operands.size() > 3 || !mayHold(inputs, operands[0], bind) || !bindsNoArgument(universe, result))
                return;
            BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
            ValueSetMaker& maker = borrowedMaker.get();
            if (inputs.sets().onlyMember(inputs.possibleValuesOf(maker, operands[0])) != bind)
                return;
            ValueFlowObjectID target = inputs.sets().onlyMember(inputs.possibleValuesOf(maker, operands[1]));
            if (!target || !inputs.sets().contains(universe.builtinFunctions(), target))
                return;
            if (operands.size() == 3 && inputs.sets().meet(inputs.possibleValuesOf(maker, operands[2]), ValueSets::complement(universe.undefinedValue())))
                return;
            found.append({ result, universe.builtinOf(target) });
        });
    });
    if (made.isEmpty())
        return { };

    AOT::sort(made, { }, &MadeByBind::object);
    Vector<ValueFlowObjectID> objects(made.size(), [&](size_t index) {
        return made[index].object;
    });
    ValueSetID everyObject = BorrowedValueSetMaker(inputs.makerPool()).get().of(objects.span());
    ValueSetID otherBoundFunctions = BorrowedValueSetMaker(inputs.makerPool()).get().intersect(universe.objectsThatMayBeMadeAs(BuiltinClasses::BoundFunction), ValueSets::complement(everyObject));
    ValueFlowObjectID apply = universe.objectOf(program.builtins().canonicalID(Builtins::FunctionPrototypeApply));
    ValueFlowObjectID call = universe.objectOf(program.builtins().canonicalID(Builtins::FunctionPrototypeCall));
    Vector<FlowBuiltinBoundAtSite> result = collectInParallel<FlowBuiltinBoundAtSite>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<FlowBuiltinBoundAtSite>& found) {
        forEachCallIn(index, [&](const ValueFlowStatement& statement) {
            std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(statement);
            BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
            ValueSetMaker& maker = borrowedMaker.get();
            // `f.apply(..)` and `f.call(..)` become calls of f where they stand.
            ValueFlowObjectID method = inputs.sets().onlyMember(inputs.possibleValuesOf(maker, operands[0]));
            const ValueFlowOperand& operand = operands[method && (method == apply || method == call) ? 1 : 0];
            if (!operand.node || !inputs.sets().meet(inputs.solver().possibleValuesOf(operand.node), everyObject))
                return;
            ValueSetID callees = inputs.possibleValuesOf(maker, operand);
            if (ValueSets::isComplement(callees) || inputs.sets().meet(callees, otherBoundFunctions))
                return;
            BuiltinID onlyTarget;
            bool hasSeveral = false;
            inputs.sets().forEachMember(maker.intersect(callees, everyObject), [&](ValueFlowObjectID callee) {
                BuiltinID target = std::ranges::lower_bound(made, callee, { }, &MadeByBind::object)->target;
                hasSeveral |= onlyTarget && onlyTarget != target;
                onlyTarget = target;
            });
            if (onlyTarget && !hasSeveral)
                found.append({ accessSites.siteOfValue(FunctionID(index), statement.site), onlyTarget });
        });
    });
    AOT::sort(result, { }, &FlowBuiltinBoundAtSite::site);
    result.shrink(std::ranges::unique(result).begin() - result.begin());
    return result;
}

// `on("close", listener)` stores under a parameter, and `emit("close")` calls what it reads under one. Flow follows the first once for each key that its callers spell, so it knows what lies under "close".
// The call in the second has one stamp for all keys. A copy of it in a caller that spells the key asks here.
static Vector<FlowClosureStoredUnderKey> findClosuresStoredUnderKeys(const ValueFlowResultInputs& inputs)
{
    const ValueFlowSolver& solver = inputs.solver();
    const ValueFlowInstancedFunctions* instanced = solver.input().instancedFunctions;
    if (!instanced || instanced->isEmpty())
        return { };
    const ValueFlowUniverse& universe = inputs.universe();
    BitVector isSpelled(inputs.program.types.numberOfPropertyKeys());
    for (uint32_t index = 0; index < inputs.program.functions.size(); ++index) {
        std::optional<ValueFlowNodeID> markers = instanced->find(FunctionID(index)) && inputs.isReached(FunctionID(index)) ? solver.markersOf(FunctionID(index)) : std::nullopt;
        if (!markers)
            continue;
        inputs.sets().forEachMember(solver.possibleValuesOf(*markers), [&](ValueFlowObjectID marker) {
            if (universe.kindOf(marker) == ValueFlowObjectKind::Key)
                isSpelled.set(universe.keyOf(marker).index());
        });
    }
    if (isSpelled.isEmpty())
        return { };

    // A call tests for no more.
    AOT_THRESHOLD(unsigned, mostClosuresUnderKey, 2);
    struct Stored {
        uint32_t key;
        // None: something that nobody knows, or more than a call tests for.
        FunctionID function;
    };
    uint32_t firstMadeNode = solver.input().numberOfNodes;
    ValueSetMakers makers(inputs);
    Vector<Stored> stored = collectWithMakers<Stored>(inputs, makers, solver.graph().numberOfNodes() - firstMadeNode, [&](unsigned position, ValueSetMaker& maker, Vector<Stored>& found) {
        ValueFlowNodeID node(firstMadeNode + position);
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(node);
        if (key->kind != ValueFlowMadeNodeKind::Written || key->name().kind != ValueFlowNameKind::Property || !isSpelled.get(key->name().payload))
            return;
        // Which closures stand listed beside the unknown object hangs on the order of the rules.
        ValueSetID value = solver.seenValuesOf(node);
        if (ValueSets::isComplement(value) || inputs.sets().contains(value, ValueFlowUniverse::unknownObject())) {
            found.append({ key->name().payload, { } });
            return;
        }
        unsigned number = 0;
        inputs.sets().forEachMember(maker.intersect(value, universe.closures()), [&](ValueFlowObjectID closure) {
            if (++number <= mostClosuresUnderKey + 1)
                found.append({ key->name().payload, universe.closureFunction(closure) });
        });
    });
    AOT::sort(stored, { }, [](const Stored& entry) {
        return std::pair(entry.key, entry.function);
    });
    Vector<FlowClosureStoredUnderKey> result;
    for (size_t first = 0; first < stored.size();) {
        size_t end = first;
        size_t sizeBefore = result.size();
        bool isKnown = true;
        for (; end < stored.size() && stored[end].key == stored[first].key; ++end) {
            isKnown &= stored[end].function && inputs.build.isEnabled(FlowResultKind::CalleeSets, stored[end].function.index());
            if (isKnown && (result.size() == sizeBefore || result.last().function != stored[end].function))
                result.append({ PropertyKeyID(stored[end].key), stored[end].function });
        }
        if (!isKnown || result.size() - sizeBefore > mostClosuresUnderKey)
            result.shrink(sizeBefore);
        first = end;
    }
    return result;
}

void computeFunctionResults(const ValueFlowResultInputs& inputs, ValueFlowStampedCalls& calls, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    FlowApplication& application = *tables.application;
    unsigned numberOfFunctions = program.functions.size();
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    BitVector isCalledByHost = functionsCalledByHost(program, inputs.loops());
    EntriesWithoutStamp entries = findEntriesWithoutStamp(inputs, calls, tables);
    times.didFinish("functions: entries without stamp"_s);

    Vector<uint32_t> unreachedFunctions = findIndicesInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index) {
        if (inputs.isReached(FunctionID(index)) || !inputs.build.isEnabled(FlowResultKind::ReachedFunctions, index))
            return false;
        return !isCalledByHost.get(index) || isEnteredOnDemand(program.functions[index]);
    });
    for (uint32_t index : unreachedFunctions)
        tables.functions.functions[index].reachability = FunctionReachability::Unreached;
    calls.functionsNamedByUnsolvedCode = findFunctionsNamedByUnsolvedCode(inputs, tables);
    times.didFinish("functions: unreached, named by unsolved code"_s);
    inputs.build.statistics().count(StatisticID::FlowUnreachedFunctions, std::ranges::count(tables.functions.functions, FunctionReachability::Unreached, &FunctionFlow::reachability));

    tables.functions.boundFunctionUses = findBoundFunctionsOnlyCalled(inputs);
    tables.sites.boundFunctionsAtSites = findBoundFunctionsAtSites(inputs, application.accessSites);
    tables.sites.builtinsBoundAtSites = findBuiltinsBoundAtSites(inputs, application.accessSites);
    tables.sites.closuresStoredUnderKeys = findClosuresStoredUnderKeys(inputs);

    BitVector isNamedByTable(numberOfFunctions);
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (entry.function && !isEnteredByCallsAlone(program, entry.id))
            isNamedByTable.quickSet(entry.function.index());
    }
    times.didFinish("functions: bound functions, named by table"_s);

    // A read of a name finds a value that was stored on the closure, or goes on to Function.prototype.
    constexpr OptionSet<ObjectMutation> mutationsThatLetReadsRunCode { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype };

    if (inputs.birthsOfUnknownObject) {
        tables.functions.otherEscapeOriginsOfFunctions.fill(0, numberOfFunctions);
        for (uint32_t index = 0; index < numberOfFunctions; ++index) {
            if (inputs.birthsOfUnknownObject->hasOtherEscapeOrigins(universe.closureOf(FunctionID(index))))
                tables.functions.functionsWithOtherEscapeOrigins.set(index);
        }
    }
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        if (markedMutationsOf(inputs, universe.closureOf(FunctionID(index))).contains(ObjectMutation::ChangeIntegrity))
            tables.functions.functionsWhoseClosuresChangeIntegrity.set(index);
    }
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index) {
        FunctionFlow& flow = tables.functions.functions[index];
        ValueFlowObjectID closure = universe.closureOf(FunctionID(index));
        if (isCalledByHost.get(index) || isNamedByTable.get(index) || entries.functionsEnteredByHost.get(index) || calls.functionsNamedByUnsolvedCode.get(index) || inputs.isHeldByNativeCode(closure)) {
            flow.cause = UnprovenCause::ReachesNativeCode;
            return;
        }
        if (inputs.hasEscaped(closure)) {
            flow.cause = UnprovenCause::ReachesNativeCode;
            if (inputs.birthsOfUnknownObject) {
                flow.witness = inputs.birthsOfUnknownObject->escapeOriginOf(closure);
                tables.functions.otherEscapeOriginsOfFunctions[index] = inputs.birthsOfUnknownObject->otherEscapeOriginsOf(closure);
            }
            return;
        }
        if (inputs.isLost(closure)) {
            flow.cause = UnprovenCause::Untracked;
            return;
        }
        if (!inputs.build.isEnabled(FlowResultKind::CalleeSets, index)) {
            flow.cause = UnprovenCause::ResultDisabled;
            return;
        }
        if (entries.mayBeIntoAnyFunction)
            return;
        flow.callers = CallerState::AllKnown;
        if (inputs.build.isEnabled(FlowResultKind::ObjectUses, index) && isOnlyCalled(inputs, closure) && !inputs.universe().builtinTables().entryDescribing(closure))
            flow.closureUse = ClosureUse::CalledOnly;
        OptionSet<FunctionCapabilities>& capabilities = application.signatures[index].provenCapabilities;
        capabilities.add(FunctionCapabilities::IsNeverCalledByHost);
        if (!calls.functionsInStamps.get(index) && !calls.functionsEnteredWithoutStamp.get(index))
            capabilities.add(FunctionCapabilities::HasOnlyDirectCallers);
        const Function& function = inputs.program.functions[index];
        bool inheritsFromFunctionPrototype = !function.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) && (!function.constructedClass || inputs.program.at(function.constructedClass).has(LayoutFlags::IsConstructorFunction));
        if (inheritsFromFunctionPrototype && inputs.build.isEnabled(FlowResultKind::HolderMutations, index) && !markedMutationsOf(inputs, closure).containsAny(mutationsThatLetReadsRunCode))
            capabilities.add(FunctionCapabilities::ClosuresAreReadWithoutCode);
    });
}

static bool mayTellClosuresApart(const BuiltinParameter& parameter)
{
    constexpr OperandUses usesThatDoNotLook { OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly, OperandUse::Retains, OperandUse::Returns };
    constexpr OperandUses callsOrKeeps { OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly, OperandUse::Retains };
    OperandUses others = parameter.uses - usesThatDoNotLook;
    return !others.isEmpty() && (others != OperandUses { OperandUse::Inspects } || !parameter.uses.containsAny(callsOrKeeps));
}

static std::array<BuiltinID, 3> builtinsThatWatchTheirArguments(const BuiltinTable& builtins)
{
    return { builtins.canonicalID(Builtins::WeakRef), builtins.canonicalID(Builtins::FinalizationRegistryPrototypeRegister), builtins.canonicalID(Builtins::FinalizationRegistryPrototypeUnregister) };
}

void computeLayoutsNeverHeldWeakly(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, const ValueFlowUsesOfHeap& usesOfHeap, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const BuiltinTable& builtins = program.builtins();
    Vector<ValueSetID> parts;
    // An unknown value is an object that has reached code outside the program, which its layout says by itself.
    auto note = [&](ValueSetID value) {
        if (value && value != ValueSets::nothing && value != ValueSets::anything)
            parts.append(value);
    };
    for (uint32_t index = 0; index < builtins.slotCount(); ++index) {
        BuiltinClassID owner = builtins.at(BuiltinSlotID(index)).owner;
        if (owner == BuiltinClasses::WeakRef || owner == BuiltinClasses::FinalizationRegistry || owner == BuiltinClasses::WeakMap || owner == BuiltinClasses::WeakSet)
            note(heap.writtenUnder(ValueSets::anything, ValueFlowName { ValueFlowNameKind::Slot, index }));
    }
    const std::array watchingBuiltins = builtinsThatWatchTheirArguments(builtins);
    BitVector watches(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (std::ranges::contains(watchingBuiltins, builtins.canonicalID(BuiltinID(index))))
            watches.quickSet(index);
    }
    forEachBuiltinApplication(inputs, heap, watches, [&](const ValueFlowBuiltinApplication& application) {
        note(application.otherArguments);
        for (ValueSetID argument : application.arguments)
            note(argument);
    });
    ValueSetID held = parts.isEmpty() ? ValueSets::nothing : heap.maker().uniteAll(parts.span());

    tables.sites.layoutsNeverHeldWeakly = BitVector(program.layouts.size());
    if (ValueSets::isComplement(held))
        return;
    BitVector isHeldWeakly(program.layouts.size());
    usesOfHeap.layoutsOfObjects->forEachLayoutOfMembers(held, [&](LayoutID layout) {
        isHeldWeakly.quickSet(layout.index());
    });
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (!isHeldWeakly.quickGet(index) && inputs.build.isEnabled(FlowResultKind::HolderMutations, index))
            tables.sites.layoutsNeverHeldWeakly.quickSet(index);
    }
}

ValueSetID computeClosuresNeverCompared(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    const BuiltinTable& builtins = program.builtins();
    const ParallelLoops& loops = inputs.loops();
    ValueSetID lists = universe.sureInstancesOf(BuiltinClasses::Array);
    ValueSetID closures = heap.maker().unite(universe.closures(), lists);
    struct Teller {
        FunctionID function;
        BuiltinID builtin;
        BuiltinSlotID slot;
    };
    const bool logsTellers = inputs.build.options().logging.logsPhaseTimes;

    struct Looker {
        HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> closuresOfSet;
        HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> partsKept;
        HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> pairsMet;
        Vector<ValueSetID> parts;
        Vector<Teller> tellers;
    };
    ValueSetMakers makers(inputs);
    Vector<Looker> lookers(loops.width());
    auto closuresIn = [&](unsigned worker, ValueSetID value) {
        if (!value || value == ValueSets::nothing)
            return ValueSets::nothing;
        auto result = lookers[worker].closuresOfSet.add(value.index(), ValueSets::nothing);
        if (result.isNewEntry)
            result.iterator->value = makers.of(worker).intersect(value, closures);
        return result.iterator->value;
    };
    auto keep = [&](unsigned worker, ValueSetID part, const Teller& teller) {
        if (part == ValueSets::nothing || !lookers[worker].partsKept.add(part.index()).isNewEntry)
            return;
        lookers[worker].parts.append(part);
        if (logsTellers) [[unlikely]]
            lookers[worker].tellers.append(teller);
    };

    constexpr size_t comparisonsPerTask = 1024;
    std::span<const ValueFlowComparedValues> comparedValues = inputs.bodies().comparedValues();
    forEachIndexInParallel(loops, (comparedValues.size() + comparisonsPerTask - 1) / comparisonsPerTask, [&](unsigned task, unsigned worker) {
        for (const ValueFlowComparedValues& compared : comparedValues.subspan(task * comparisonsPerTask, std::min(comparisonsPerTask, comparedValues.size() - task * comparisonsPerTask))) {
            if (!inputs.isReached(compared.function))
                continue;
            auto sidesOf = [&](const ValueFlowOperand& operand) {
                return std::array { closuresIn(worker, operand.node ? inputs.solver().possibleValuesOf(operand.node) : ValueSets::nothing), closuresIn(worker, operand.constant) };
            };
            for (ValueSetID left : sidesOf(compared.left)) {
                if (left == ValueSets::nothing)
                    continue;
                for (ValueSetID right : sidesOf(compared.right)) {
                    if (right == ValueSets::nothing)
                        continue;
                    auto [low, high] = std::minmax({ left.index(), right.index() });
                    if (!compared.mayBeFunctions) {
                        keep(worker, makers.of(worker).intersect(makers.of(worker).intersect(left, right), lists), { compared.function, { }, { } });
                        continue;
                    }
                    if (lookers[worker].pairsMet.add(static_cast<uint64_t>(low) << 32 | high).isNewEntry)
                        keep(worker, left == right ? left : makers.of(worker).intersect(left, right), { compared.function, { }, { } });
                }
            }
        }
    });

    BitVector builtinsThatLook = builtinsWithOverload(builtins, [](const BuiltinOverload& overload) {
        return (overload.receiver && mayTellClosuresApart(*overload.receiver)) || std::ranges::any_of(overload.parameters, mayTellClosuresApart);
    });
    const std::array watchingBuiltins = builtinsThatWatchTheirArguments(builtins);
    auto watchesItsArguments = [&](BuiltinID builtin) {
        return std::ranges::contains(watchingBuiltins, builtins.canonicalID(builtin));
    };
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (watchesItsArguments(BuiltinID(index)))
            builtinsThatLook.quickSet(index);
    }
    forEachBuiltinApplicationOnAllThreads(inputs, heap, makers, builtinsThatLook, closures, [&](unsigned worker, const ValueFlowBuiltinApplication& application) {
        ValueSetMaker& maker = makers.of(worker);
        auto note = [&](ValueSetID value) {
            if (value != ValueSets::anything)
                keep(worker, closuresIn(worker, value), { application.function, application.builtin, { } });
        };
        note(application.otherArguments);
        if (watchesItsArguments(application.builtin)) {
            for (ValueSetID argument : application.arguments)
                note(argument);
        }
        for (const BuiltinOverload& overload : builtins.overloadsOf(application.builtin)) {
            if (overload.mode != application.mode())
                continue;
            if (overload.receiver && mayTellClosuresApart(*overload.receiver))
                note(application.valueAt(overload.receiver->operand, maker));
            for (const BuiltinParameter& parameter : overload.parameters) {
                if (mayTellClosuresApart(parameter))
                    note(application.valueAt(parameter.operand, maker));
            }
        }
    });

    const ValueFlowBuiltinTables& behaviors = universe.builtinTables();
    Vector<BuiltinID> builtinThatLooksThrough(FillWith { }, behaviors.numberOfBehaviors(), BuiltinID());
    for (uint32_t index = 0; index < builtins.overloadCount(); ++index) {
        const BuiltinOverload& overload = builtins.at(BuiltinOverloadID(index));
        auto noteBehaviorOf = [&](const BuiltinParameter& parameter) {
            if (!mayTellClosuresApart(parameter) || !parameter.uses.containsAny({ OperandUse::AppliesTransitively, OperandUse::AppliesToContents }))
                return;
            if (ValueFlowBehaviorID behavior = behaviors.behaviorOf(parameter); behavior && !builtinThatLooksThrough[behavior.index()])
                builtinThatLooksThrough[behavior.index()] = overload.builtin;
        };
        if (overload.receiver)
            noteBehaviorOf(*overload.receiver);
        std::ranges::for_each(overload.parameters, noteBehaviorOf);
    }
    for (uint32_t index = 0; index < builtinThatLooksThrough.size(); ++index) {
        if (!builtinThatLooksThrough[index] || inputs.build.isSabotaged(Sabotage::BuiltinsLookAtTheirOperandsAlone))
            continue;
        if (ValueSetID reached = inputs.solver().possibleValuesOf(inputs.bodies().nodes().hub(ValueFlowBehaviorID(index))); reached != ValueSets::anything)
            keep(0, closuresIn(0, reached), { { }, builtinThatLooksThrough[index], { } });
    }

    keep(0, inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::ClosuresThatDefinitionsCompare)), { { }, builtins.canonicalID(Builtins::ObjectDefineProperty), { } });

    for (uint32_t index = 0; index < builtins.slotCount(); ++index) {
        const BuiltinSlot& slot = builtins.at(BuiltinSlotID(index));
        bool isSet = slot.owner == BuiltinClasses::Set || slot.owner == BuiltinClasses::WeakSet;
        bool watchesWhatItHolds = slot.owner == BuiltinClasses::WeakRef || slot.owner == BuiltinClasses::FinalizationRegistry;
        if (!watchesWhatItHolds && slot.contentsSlot != ContentsSlot::Key && !(isSet && slot.contentsSlot == ContentsSlot::Element))
            continue;
        keep(0, closuresIn(0, heap.writtenUnder(ValueSets::anything, ValueFlowName { ValueFlowNameKind::Slot, index })), { { }, { }, BuiltinSlotID(index) });
    }

    Vector<ValueSetID> parts;
    Vector<Teller> tellers;
    for (Looker& looker : lookers) {
        parts.appendVector(WTF::move(looker.parts));
        tellers.appendVector(WTF::move(looker.tellers));
    }
    ValueSetID compared = heap.maker().uniteAll(parts.span());
    ValueSetID listsToldApart = heap.maker().intersect(compared, lists);
    BitVector& neverCompared = tables.functions.functionsWhoseClosuresAreNeverCompared;
    neverCompared = BitVector(program.functions.size());
    Vector<uint32_t> functionsNeverCompared = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        return inputs.build.isEnabled(FlowResultKind::ObjectUses, index) && !inputs.sets().contains(compared, universe.closureOf(FunctionID(index)));
    });
    for (uint32_t index : functionsNeverCompared)
        neverCompared.quickSet(index);
    if (logsTellers) [[unlikely]] {
        auto describe = [&](FunctionID id) {
            if (!id)
                return makeString("no operation"_s);
            const Function& function = program.at(id);
            auto textOf = [&](StringID text) {
                return text ? String::fromUTF8(program.string(text)) : String();
            };
            return makeString('f', id.index(), " '"_s, textOf(function.name), "' "_s, function.module ? textOf(program.at(function.module).displayPath) : String(), ':', function.position);
        };
        unsigned onlyCalled = 0, toldApart = 0;
        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            if (tables.functions.functions[index].closureUse != ClosureUse::CalledOnly)
                continue;
            ++onlyCalled;
            if (neverCompared.quickGet(index))
                continue;
            ++toldApart;
            constexpr unsigned mostTellersPrinted = 3;
            unsigned found = 0;
            StringBuilder line;
            for (size_t part = 0; part < parts.size(); ++part) {
                if (!inputs.sets().contains(parts[part], universe.closureOf(FunctionID(index))) || ++found > mostTellersPrinted)
                    continue;
                const Teller& teller = tellers[part];
                if (teller.slot)
                    line.append(" | a key of, or watched by, "_s, builtins.textOf(builtins.at(builtins.at(teller.slot).owner).constructor));
                else if (teller.builtin)
                    line.append(" | looked at by "_s, builtins.textOf(teller.builtin), " in "_s, describe(teller.function));
                else
                    line.append(" | on both sides of a comparison in "_s, describe(teller.function));
                line.append(" (with "_s, inputs.sets().size(parts[part]), " closures)"_s);
            }
            dataLogLn("AOT closures compared: ", describe(FunctionID(index)), " | ", found, " can tell", line.toString());
        }
        dataLogLn("AOT closures compared: functions whose closures are only called ", onlyCalled, " | of them told apart ", toldApart, " | sets of closures that somebody can tell apart ", parts.size());
    }
    return listsToldApart;
}

void computeSymbolConstantResults(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    if (!inputs.symbolsCreatedOnce || inputs.symbolsCreatedOnce->isEmpty())
        return;

    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    forEachIndexInParallel(inputs.loops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        if (!inputs.isReached(FunctionID(index)) || !function.module)
            return;

        FlowBodyUpdate& update = application.bodies[index];
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::Call || !statement.result)
                continue;
            ValueFlowObjectID result = inputs.sets().onlyMember(inputs.solver().possibleValuesOf(statement.result));
            PropertyKeyID key = result ? inputs.universe().keyOf(result) : PropertyKeyID();
            if (!key || !inputs.symbolsCreatedOnce->get(key.index()))
                continue;
            const PropertyKey& symbol = program.at(key);
            if (symbol.kind == PropertyKeyKind::CreatedSymbol && symbol.module == function.module && symbol.callNumber == function.at(statement.site).callNumber())
                update.symbolConstants.append(FlowSymbolConstant { statement.site, key });
        }
        if (update.symbolConstants.isEmpty())
            return;
        AOT::sort(update.symbolConstants, { }, &FlowSymbolConstant::call);
        update.proofs.removeAllMatching([&](const FlowValueProof& proof) {
            return !!findByValue(update.symbolConstants.span(), proof.value, &FlowSymbolConstant::call);
        });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)