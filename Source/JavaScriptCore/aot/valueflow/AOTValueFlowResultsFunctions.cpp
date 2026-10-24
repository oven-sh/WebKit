/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowStamps.h"
#include "AOTPhaseTimes.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <optional>
#include <tuple>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// The group of a set of callees that a statement has asked for: the closures in the set are entered with what the statement hands to the group.
struct EnteredGroup {
    ValueSetID callees;
    ValueFlowCallKind kind { ValueFlowCallKind::Call };
};

struct GroupOfStatement {
    uint32_t statement { 0 };
    EnteredGroup group;
};

// Lists of numbers, by the number of their owner.
class ListsByOwner {
public:
    using Pair = std::pair<uint32_t, uint32_t>;

    ListsByOwner() = default;
    ListsByOwner(uint32_t numberOfOwners, std::span<const Pair> ownersAndMembers)
    {
        m_first.fill(0u, numberOfOwners + 1);
        for (const Pair& pair : ownersAndMembers)
            ++m_first[pair.first + 1];
        for (uint32_t owner = 0; owner < numberOfOwners; ++owner)
            m_first[owner + 1] += m_first[owner];
        m_members.grow(ownersAndMembers.size());
        Vector<uint32_t> next(m_first.span().first(numberOfOwners));
        for (const Pair& pair : ownersAndMembers)
            m_members[next[pair.first]++] = pair.second;
    }

    std::span<const uint32_t> of(uint32_t owner) const LIFETIME_BOUND { return m_members.subspan(m_first[owner], m_first[owner + 1] - m_first[owner]); }

private:
    Vector<uint32_t> m_first;
    Vector<uint32_t> m_members;
};

// The statements that rules have made, by their number among such statements.
class StatementsMadeByRules {
public:
    explicit StatementsMadeByRules(const ValueFlowResultInputs&);

    uint32_t size() const { return m_size; }
    uint32_t numberOf(ValueFlowStatementID statement) const { return statement.index() - m_numberOfStatementsOfBodies; }
    bool hasRun(uint32_t number) const { return m_hasRun[number]; }
    // Nobody can say for whom it acts: it stands for all that reaches an operand of native code anywhere, for native code itself, or for the calls of a placeholder.
    bool actsForNobodyKnown(uint32_t number) const { return m_actsForNobodyKnown[number]; }
    // It has asked for the group of some closure, or for a statement that leads to such a group.
    bool leadsToGroup(uint32_t number) const { return m_leadsToGroup.quickGet(number); }
    std::span<const GroupOfStatement> groupsOf(uint32_t number) const LIFETIME_BOUND { return m_groups.subspan(m_firstGroup[number], m_firstGroup[number + 1] - m_firstGroup[number]); }
    std::span<const uint32_t> statementsAskedForBy(uint32_t number) const LIFETIME_BOUND { return m_statementsAskedFor.of(number); }

    // From any thread.
    void noteAsker(uint32_t number) { AtomicRef(m_isAskedFor[number]).store(1, std::memory_order_relaxed); }
    bool isAskedFor(uint32_t number) const { return m_isAskedFor[number]; }

private:
    uint32_t m_numberOfStatementsOfBodies;
    uint32_t m_size;
    // The threads of a loop set these two, each statement on one thread.
    Vector<uint8_t> m_hasRun;
    Vector<uint8_t> m_actsForNobodyKnown;
    BitVector m_leadsToGroup;
    Vector<uint8_t> m_isAskedFor;
    // In the order of the statements.
    Vector<GroupOfStatement> m_groups;
    Vector<uint32_t> m_firstGroup;
    ListsByOwner m_statementsAskedFor;
};

// The entries into functions that no stamp lists.
struct EntriesWithoutStamp {
    // Ascending by caller, site, callee and kind.
    Vector<FlowIndirectCall> calls;
    BitVector functions;
    // Those among them that something enters for which nobody can name an operation.
    BitVector functionsEnteredByHost;
    // The group of almost everything was asked for, so nothing lists what it enters.
    bool mayBeIntoAnyFunction { false };
};

struct BindCall {
    FunctionID target;
    uint32_t numberOfBoundArguments { 0 };
    bool resultIsOnlyCalled { false };
};

} // anonymous namespace

// The solve enters the body of a module where something reached loads the module, and a function of the library where something reached uses its built-in.
static bool isEnteredOnDemand(const Function& function)
{
    if (function.role == FunctionRole::Library || function.role == FunctionRole::Model)
        return true;
    return function.kind == FunctionKind::Module || function.kind == FunctionKind::CommonJsModule;
}

static bool hasCalleeStamp(const ValueFlowStatement& statement)
{
    return statement.kind == ValueFlowStatementKind::Operation && PayloadWord::callees(statement.opcode);
}

static const CalleeSet* findStamp(const ValueFlowResultInputs& inputs, const FlowApplication& application, FunctionID function, const ValueFlowStatement& statement)
{
    if (!hasCalleeStamp(statement))
        return nullptr;
    const FlowCallStamp* stamp = findByValue(application.bodies[function.index()].calls.span(), statement.site, &FlowCallStamp::value);
    return stamp ? &inputs.program.at(stamp->callees) : nullptr;
}

static bool holdsPlaceholder(const ValueSets& sets, ValueSetID set)
{
    return sets.contains(set, ValueFlowUniverse::unknownObject()) || sets.contains(set, ValueFlowUniverse::untrackedObject());
}

// Every rule that enters a closure asks for the group of a set that holds it.
static void forEachRequestOf(const ValueFlowResultInputs& inputs, ValueFlowStatementID id, NOESCAPE const Invocable<void(const EnteredGroup&)> auto& group, NOESCAPE const Invocable<void(ValueFlowStatementID)> auto& madeStatement)
{
    inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::CallGroup }), true, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
        // A statement asks for the nodes that it was made with.
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
        return ValueSets::isComplement(solver.keyOf(id).set) || holdsPlaceholder(solver.sets(), solver.keyOf(id).set);
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

// A group without a closure enters no function. What its statement asks for in turn is looked at by itself.
static bool entersFunctions(const ValueFlowResultInputs& inputs, const EnteredGroup& group)
{
    return inputs.sets().meet(group.callees, inputs.universe().closures());
}

StatementsMadeByRules::StatementsMadeByRules(const ValueFlowResultInputs& inputs)
    : m_numberOfStatementsOfBodies(inputs.solver().input().statements.size())
    , m_size(inputs.solver().numberOfStatements() - m_numberOfStatementsOfBodies)
    , m_hasRun(FillWith { }, m_size, static_cast<uint8_t>(0))
    , m_actsForNobodyKnown(FillWith { }, m_size, static_cast<uint8_t>(0))
    , m_leadsToGroup(m_size)
    , m_isAskedFor(FillWith { }, m_size, static_cast<uint8_t>(0))
{
    struct Requests {
        Vector<GroupOfStatement> groups;
        Vector<ListsByOwner::Pair> askersAndAsked;
    };
    constexpr uint32_t statementsPerTask = 256;

    const ValueFlowSolver& solver = inputs.solver();
    Vector<Requests> requestsOfTask((m_size + statementsPerTask - 1) / statementsPerTask);
    forEachIndexInParallel(inputs.loops(), requestsOfTask.size(), [&](unsigned task) {
        Vector<GroupOfStatement>& groupsOfTask = requestsOfTask[task].groups;
        Vector<ListsByOwner::Pair>& askersAndAskedOfTask = requestsOfTask[task].askersAndAsked;
        uint32_t end = std::min(m_size, (task + 1) * statementsPerTask);
        for (uint32_t number = task * statementsPerTask; number < end; ++number) {
            ValueFlowStatementID id(m_numberOfStatementsOfBodies + number);
            if (solver.isAtRest(id))
                continue;
            m_hasRun[number] = 1;
            if (Typed::actsForNobodyKnown(solver, id))
                m_actsForNobodyKnown[number] = 1;
            size_t firstGroup = groupsOfTask.size();
            size_t firstAsked = askersAndAskedOfTask.size();
            forEachRequestOf(inputs, id, [&](const EnteredGroup& group) {
                if (entersFunctions(inputs, group))
                    groupsOfTask.append(GroupOfStatement { number, group });
            }, [&](ValueFlowStatementID made) {
                askersAndAskedOfTask.append({ number, numberOf(made) });
                noteAsker(numberOf(made));
            });

            // A statement that is shown only what is new in a node keeps what all its runs have asked for, the same thing many times over.
            auto orderOfGroup = [](const GroupOfStatement& entry) {
                return std::tuple(entry.group.callees, entry.group.kind);
            };
            auto groups = groupsOfTask.mutableSpan().subspan(firstGroup);
            AOT::sort(groups, { }, orderOfGroup);
            groupsOfTask.shrink(groupsOfTask.size() - std::ranges::unique(groups, { }, orderOfGroup).size());
            auto asked = askersAndAskedOfTask.mutableSpan().subspan(firstAsked);
            AOT::sort(asked);
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
    m_groups.reserveInitialCapacity(numberOfGroups);
    askersAndAsked.reserveInitialCapacity(numberOfAsked);
    for (Requests& requests : requestsOfTask) {
        m_groups.appendVector(WTF::move(requests.groups));
        askersAndAsked.appendVector(WTF::move(requests.askersAndAsked));
    }
    m_firstGroup.fill(0u, m_size + 1);
    for (const GroupOfStatement& entry : m_groups)
        ++m_firstGroup[entry.statement + 1];
    for (uint32_t number = 0; number < m_size; ++number)
        m_firstGroup[number + 1] += m_firstGroup[number];
    m_statementsAskedFor = ListsByOwner(m_size, askersAndAsked.span());

    for (ListsByOwner::Pair& pair : askersAndAsked)
        std::swap(pair.first, pair.second);
    ListsByOwner askers(m_size, askersAndAsked.span());
    Vector<uint32_t> worklist;
    for (const GroupOfStatement& entry : m_groups) {
        if (!m_leadsToGroup.quickSet(entry.statement))
            worklist.append(entry.statement);
    }
    while (!worklist.isEmpty()) {
        for (uint32_t asker : askers.of(worklist.takeLast())) {
            if (!m_leadsToGroup.quickSet(asker))
                worklist.append(asker);
        }
    }
}

// As the rule that enters a closure decides it.
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

// The solve does not keep which row of the table has entered which closure, so every function that a call of built-ins enters counts as called as often as the rows of those built-ins say together.
static BuiltinCallbackTimes timesOfCallbacks(const BuiltinTable& builtins, const CalleeSet* stamp)
{
    std::optional<BuiltinCallbackTimes> times;
    auto add = [&](BuiltinCallbackTimes other) {
        times = times ? mergeTimes(*times, other) : other;
    };
    if (stamp) {
        for (BuiltinID builtin : stamp->builtins) {
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

static auto callOrderOf(const FlowIndirectCall& call)
{
    return std::tuple(call.caller, call.site, call.callee, call.kind);
}

// Sorts the calls from `first` on and leaves one for each caller, site, callee and kind.
static void mergeIndirectCalls(Vector<FlowIndirectCall>& calls, size_t first)
{
    AOT::sort(calls.mutableSpan().subspan(first), { }, callOrderOf);
    size_t count = first;
    for (size_t index = first; index < calls.size(); ++index) {
        FlowIndirectCall call = calls[index];
        if (count == first || callOrderOf(calls[count - 1]) != callOrderOf(call)) {
            calls[count++] = call;
            continue;
        }
        FlowIndirectCall& merged = calls[count - 1];
        merged.isInCycle |= call.isInCycle;
        merged.times = mergeTimes(merged.times, call.times);
    }
    calls.shrink(count);
}

namespace {

// The statements of one reached function, and what they enter beside what their stamps list.
class EntriesFromFunction {
public:
    EntriesFromFunction(const ValueFlowResultInputs& inputs, const FlowApplication& application, StatementsMadeByRules& madeStatements, FunctionID id, Vector<FlowIndirectCall>& calls)
        : m_inputs(inputs)
        , m_application(application)
        , m_madeStatements(madeStatements)
        , m_id(id)
        , m_function(inputs.program.at(id))
        , m_calls(calls)
    {
    }

    void collect(ValueFlowStatementID, const ValueFlowStatement&);

private:
    void prepareSites();
    void add(const ValueFlowStatement&, const EnteredGroup&, const CalleeSet* stampThatListsCallees);
    FlowIndirectCallKind kindOf(const ValueFlowStatement&, ValueFlowCallKind) const;

    const ValueFlowResultInputs& m_inputs;
    const FlowApplication& m_application;
    StatementsMadeByRules& m_madeStatements;
    FunctionID m_id;
    const Function& m_function;
    Vector<FlowIndirectCall>& m_calls;
    // Made with the first entry.
    bool m_hasPreparedSites { false };
    BitVector m_valuesInCycles;
    // Of the statement that is looked at.
    const CalleeSet* m_stamp { nullptr };
    std::optional<BuiltinCallbackTimes> m_timesOfCallbacks;
};

} // anonymous namespace

void EntriesFromFunction::prepareSites()
{
    if (std::exchange(m_hasPreparedSites, true))
        return;
    m_valuesInCycles = BitVector(m_function.values.size());
    CFG cfg(m_function);
    BlockCycles cycles(cfg);
    for (BlockID block : cfg.reversePostOrder()) {
        if (!cycles.isInCycle(block))
            continue;
        for (ValueID value : m_function.at(block).values)
            m_valuesInCycles.quickSet(value.index());
    }
}

// A call enters what a built-in among its callees calls, the target of a bound function among them, and the function of the library that a built-in among them is. Any other operation runs a protocol of the language.
FlowIndirectCallKind EntriesFromFunction::kindOf(const ValueFlowStatement& statement, ValueFlowCallKind kind) const
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

void EntriesFromFunction::add(const ValueFlowStatement& statement, const EnteredGroup& group, const CalleeSet* stampThatListsCallees)
{
    const ValueFlowUniverse& universe = m_inputs.universe();
    if (ValueSets::isComplement(group.callees))
        return;
    // A group is what the statement was shown when it last ran, and a statement runs again only when it is shown more.
    bool namesNoEscapedClosure = m_inputs.sets().size(group.callees) > m_inputs.build.limit(LimitID::MaxValueFlowMembersToldApart);
    ValueSetID escaped = m_inputs.solver().valueOf(ValueFlowProgramNodes::escaped());
    m_inputs.sets().forEachMember(group.callees, [&](ValueFlowObjectID callee) {
        FunctionID function = universe.closureFunction(callee);
        if (!function || throwsBeforeEntry(m_inputs.program.at(function), group.kind))
            return;
        if (namesNoEscapedClosure && m_inputs.sets().contains(escaped, callee))
            return;
        if (stampThatListsCallees && std::ranges::binary_search(stampThatListsCallees->functions, function))
            return;

        prepareSites();
        FlowIndirectCall call { .caller = m_id, .callee = function, .kind = kindOf(statement, group.kind), .times = BuiltinCallbackTimes::Once };
        if (statement.site) {
            call.site = m_application.accessSites.siteOfValue(m_id, statement.site);
            call.isInCycle = m_valuesInCycles.quickGet(statement.site.index());
        }
        if (call.kind == FlowIndirectCallKind::BuiltinCallback) {
            if (!m_timesOfCallbacks)
                m_timesOfCallbacks = timesOfCallbacks(m_inputs.program.builtins(), m_stamp);
            call.times = *m_timesOfCallbacks;
        }
        m_calls.append(call);
    });
}

void EntriesFromFunction::collect(ValueFlowStatementID id, const ValueFlowStatement& statement)
{
    const ValueFlowSolver& solver = m_inputs.solver();
    m_stamp = findStamp(m_inputs, m_application, m_id, statement);
    m_timesOfCallbacks = std::nullopt;

    Vector<uint32_t, 8> worklist;
    forEachRequestOf(m_inputs, id, [&](const EnteredGroup& group) {
        // A call lists in its stamp what it enters as its callee. Nothing emits a model: native code makes the calls that it spells out, so nothing in the image lists what they enter.
        bool isGroupOfCallees = (group.kind == ValueFlowCallKind::Call || group.kind == ValueFlowCallKind::Construct) && m_function.role != FunctionRole::Model;
        add(statement, group, isGroupOfCallees ? m_stamp : nullptr);
    }, [&](ValueFlowStatementID made) {
        uint32_t number = m_madeStatements.numberOf(made);
        m_madeStatements.noteAsker(number);
        if (m_madeStatements.leadsToGroup(number) && !m_madeStatements.actsForNobodyKnown(number))
            worklist.append(number);
    });
    if (worklist.isEmpty())
        return;

    HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> seen;
    while (!worklist.isEmpty()) {
        uint32_t number = worklist.takeLast();
        if (!seen.add(number).isNewEntry)
            continue;
        for (const GroupOfStatement& entry : m_madeStatements.groupsOf(number))
            add(statement, entry.group, nullptr);
        for (uint32_t asked : m_madeStatements.statementsAskedForBy(number)) {
            if (m_madeStatements.leadsToGroup(asked) && !m_madeStatements.actsForNobodyKnown(asked))
                worklist.append(asked);
        }
    }
}

static void setFunctionsOfClosuresIn(const ValueFlowResultInputs& inputs, ValueFlowNodeID covered, BitVector& functions)
{
    inputs.sets().forEachMember(inputs.solver().valueOf(covered), [&](ValueFlowObjectID member) {
        if (FunctionID function = inputs.universe().closureFunction(member))
            functions.quickSet(function.index());
    });
}

// An operation names its function and its site as the caller.
static EntriesWithoutStamp findEntriesWithoutStamp(const ValueFlowResultInputs& inputs, const FlowApplication& application)
{
    const Program& program = inputs.program;
    const ValueFlowSolver& solver = inputs.solver();
    const ValueFlowBodies& bodies = inputs.bodies();
    const ValueFlowUniverse& universe = inputs.universe();
    unsigned numberOfFunctions = program.functions.size();
    auto idOf = [&](const ValueFlowStatement& statement) {
        return ValueFlowStatementID(static_cast<uint32_t>(&statement - bodies.statements().data()));
    };

    StatementsMadeByRules madeStatements(inputs);
    EntriesWithoutStamp entries;
    entries.functions = BitVector(numberOfFunctions);
    entries.calls = collectInParallel<FlowIndirectCall>(inputs.loops(), numberOfFunctions, [&](unsigned index, Vector<FlowIndirectCall>& calls) {
        if (!inputs.isReached(FunctionID(index)))
            return;
        size_t first = calls.size();
        EntriesFromFunction fromFunction(inputs, application, madeStatements, FunctionID(index), calls);
        for (const ValueFlowStatement& statement : bodies.statementsOf(index))
            fromFunction.collect(idOf(statement), statement);
        mergeIndirectCalls(calls, first);
    });

    // A big set of callees hands on to the groups of its parts, which no statement has asked for.
    BitVector isEnteredByHost(numberOfFunctions);
    BitVector seen(madeStatements.size());
    Vector<uint32_t> worklist;
    auto follow = [&](uint32_t number) {
        if (madeStatements.leadsToGroup(number) && !seen.quickSet(number))
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
            madeStatements.noteAsker(madeStatements.numberOf(made));
            follow(madeStatements.numberOf(made));
        });
    }
    for (uint32_t number = 0; number < madeStatements.size(); ++number) {
        if (madeStatements.hasRun(number) && (madeStatements.actsForNobodyKnown(number) || !madeStatements.isAskedFor(number)))
            follow(number);
    }
    while (!worklist.isEmpty()) {
        uint32_t number = worklist.takeLast();
        for (const GroupOfStatement& entry : madeStatements.groupsOf(number))
            enterFromHost(entry.group);
        for (uint32_t asked : madeStatements.statementsAskedForBy(number))
            follow(asked);
    }
    for (size_t function : isEnteredByHost)
        entries.calls.append(FlowIndirectCall { .callee = FunctionID(static_cast<uint32_t>(function)), .kind = FlowIndirectCallKind::Host, .times = BuiltinCallbackTimes::Repeatedly });
    for (const FlowIndirectCall& call : entries.calls)
        entries.functions.quickSet(call.callee.index());
    entries.functionsEnteredByHost = WTF::move(isEnteredByHost);

    // Whoever calls a placeholder has asked for a group that holds it.
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
    if (callsUnknownObject.load(std::memory_order_relaxed))
        setFunctionsOfClosuresIn(inputs, ValueFlowProgramNodes::escaped(), entries.functions);
    if (callsUntrackedObject.load(std::memory_order_relaxed))
        setFunctionsOfClosuresIn(inputs, ValueFlowProgramNodes::lost(), entries.functions);
    if (entries.mayBeIntoAnyFunction) {
        for (size_t index = 0; index < numberOfFunctions; ++index)
            entries.functions.quickSet(index);
    }
    return entries;
}

// The functions that the code of one function can enter if nobody has followed that code: those it calls or makes closures of, and those whose closures lie where it reads, in a variable, in a field of an environment or in a capture of its own.
static void forEachFunctionWithinReachOf(const ValueFlowResultInputs& inputs, FunctionID id, NOESCAPE const Invocable<void(FunctionID)> auto& functor)
{
    const Function& function = inputs.program.at(id);
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowProgramNodes& nodes = inputs.bodies().nodes();
    auto forEachClosureIn = [&](ValueFlowNodeID node) {
        ValueSetID held = inputs.solver().valueOf(node);
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

// Unsolved code hands whatever it likes to what it enters.
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

// Whether a read or a lookup under a name of the kind looks at what the program can see of an object. The others are cells of native code: a call of a bound function reads its target out of one.
static constexpr bool looksIntoObject(ValueFlowNameKind kind)
{
    switch (kind) {
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
    case ValueFlowNameKind::EveryProperty:
    case ValueFlowNameKind::EveryGetter:
    case ValueFlowNameKind::EverySetter:
    case ValueFlowNameKind::PrototypeLink:
    case ValueFlowNameKind::CopiedFrom:
    case ValueFlowNameKind::DeletedMember:
    case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
    case ValueFlowNameKind::RedefinedMember:
    case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
    case ValueFlowNameKind::PrivateField:
    case ValueFlowNameKind::OpenedIterators:
    case ValueFlowNameKind::IteratedValues:
    case ValueFlowNameKind::IteratorResults:
    case ValueFlowNameKind::AwaitedValue:
    case ValueFlowNameKind::MemberWritten:
    case ValueFlowNameKind::EveryMemberWritten:
    case ValueFlowNameKind::EveryMemberRead:
        return true;
    case ValueFlowNameKind::Slot:
    case ValueFlowNameKind::HiddenSlot:
    case ValueFlowNameKind::InternalFields:
    case ValueFlowNameKind::SteppedValues:
    case ValueFlowNameKind::ReturnedValue:
    case ValueFlowNameKind::SentValues:
    case ValueFlowNameKind::OpenedBy:
    case ValueFlowNameKind::ResolvedPromises:
    case ValueFlowNameKind::RejectedPromises:
    case ValueFlowNameKind::AllocatedAs:
    case ValueFlowNameKind::RestArgument:
    case ValueFlowNameKind::AddedNames:
    // Its statement asks under other names for what it looks into.
    case ValueFlowNameKind::FoundThroughEscapedInstances:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// What can be called and has a property, its prototype or a protocol asked of it. Whatever a store or a definition does to an object is marked on the object.
static ValueSetID findCallablesLookedInto(const ValueFlowResultInputs& inputs, ValueSetMaker& maker)
{
    // A set that several threads meet is tested by each of them, and adds the same.
    struct FoundByWorker {
        ValueSetsMet seenSets;
        Vector<ValueSetID> callablesInSets;
    };
    constexpr uint32_t nodesPerTask = 4096;

    const ValueFlowSolver& solver = inputs.solver();
    ValueSetID callables = inputs.universe().callables();
    uint32_t firstMadeNode = solver.input().numberOfNodes;
    uint32_t numberOfMadeNodes = solver.graph().numberOfNodes() - firstMadeNode;
    ValueSetMakers makers(inputs);
    Vector<FoundByWorker> foundByWorker(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), (numberOfMadeNodes + nodesPerTask - 1) / nodesPerTask, [&](unsigned task, unsigned worker) {
        FoundByWorker& ofWorker = foundByWorker[worker];
        uint32_t end = std::min(numberOfMadeNodes, (task + 1) * nodesPerTask);
        for (uint32_t index = task * nodesPerTask; index < end; ++index) {
            const ValueFlowNodeKey& key = *solver.keyOfMadeNode(ValueFlowNodeID(firstMadeNode + index));
            switch (key.kind) {
            case ValueFlowMadeNodeKind::Read:
            case ValueFlowMadeNodeKind::Lookup:
            case ValueFlowMadeNodeKind::ReadUnderKind:
                break;
            case ValueFlowMadeNodeKind::Constant:
            case ValueFlowMadeNodeKind::Written:
            case ValueFlowMadeNodeKind::CallGroup:
            case ValueFlowMadeNodeKind::Hub:
            case ValueFlowMadeNodeKind::Masked:
            case ValueFlowMadeNodeKind::Marked:
            case ValueFlowMadeNodeKind::WrittenUnderKind:
            case ValueFlowMadeNodeKind::Instance:
                continue;
            }
            // A read through the untracked object is answered by name, out of all that anybody has written under the name. It looks into what is lost and into nothing else.
            if (key.set == inputs.universe().knownObjects())
                continue;
            if (looksIntoObject(key.name().kind) && ofWorker.seenSets.add(key.set) && inputs.sets().meet(key.set, callables))
                ofWorker.callablesInSets.append(makers.of(worker).intersect(key.set, callables));
        }
    });
    Vector<ValueSetID> callablesInSets;
    for (FoundByWorker& ofWorker : foundByWorker)
        callablesInSets.appendVector(WTF::move(ofWorker.callablesInSets));
    sortAndRemoveDuplicates(inputs.loops(), callablesInSets);
    return maker.uniteAll(callablesInSets.span());
}

// The solve has followed the object to every place that it reaches, and nothing happens to it there but that it is handed on and called.
static bool isOnlyCalled(const ValueFlowResultInputs& inputs, ValueSetID callablesLookedInto, ValueFlowObjectID object)
{
    if (inputs.hasEscaped(object) || inputs.isLost(object) || inputs.isHeldByNativeCode(object))
        return false;
    return !inputs.sets().contains(callablesLookedInto, object) && markedMutationsOf(inputs, object).isEmpty();
}

static bool mayHold(const ValueFlowResultInputs& inputs, const ValueFlowOperand& operand, ValueFlowObjectID object)
{
    return (operand.constant && inputs.sets().contains(operand.constant, object)) || (operand.node && inputs.sets().contains(inputs.solver().valueOf(operand.node), object));
}

// A pass turns a call of bind that spells out its arguments into the creation of a closure, of one function for each target and number of bound arguments.
static Vector<FlowBoundFunctionUse> findBoundFunctionsOnlyCalled(const ValueFlowResultInputs& inputs, ValueSetID callablesLookedInto)
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
            ValueSetID targets = inputs.valueOf(maker, operands[1]);
            if (ValueSets::isComplement(targets))
                continue;
            ValueFlowObjectID result = universe.siteObject(statement);
            bool resultIsOnlyCalled = result && isOnlyCalled(inputs, callablesLookedInto, result);
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

// By function: the statement that has handed a closure of it, or something that holds one, to code that nobody compiles.
static Vector<Witness> findWitnessesOfEscapes(const ValueFlowResultInputs& inputs, ValueSetMaker& maker)
{
    const ValueFlowSolver& solver = inputs.solver();
    const ValueFlowUniverse& universe = inputs.universe();
    if (!solver.input().remembersWhoLoggedEachEdge)
        return { };
    Vector<Witness> witnesses(FillWith { }, inputs.program.functions.size(), noWitness);

    using SetOfNode = HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    ValueSetID remaining = maker.intersect(solver.valueOf(escaped), universe.closures());
    SetOfNode seen;
    seen.add(escaped.index(), remaining);
    Vector<std::pair<uint32_t, ValueSetID>> level { { escaped.index(), remaining } };
    Vector<ValueFlowNodeID> predecessors;
    while (!level.isEmpty() && remaining != ValueSets::nothing) {
        SetOfNode next;
        for (auto [node, lookedForAtNode] : level) {
            ValueSetID lookedFor = maker.intersect(lookedForAtNode, remaining);
            if (lookedFor == ValueSets::nothing)
                continue;
            predecessors.shrink(0);
            solver.graph().forEachPredecessor(ValueFlowNodeID(node), [&](ValueFlowNodeID predecessor) {
                predecessors.append(predecessor);
            });
            AOT::sort(predecessors);
            for (ValueFlowNodeID predecessor : predecessors) {
                ValueSetID brought = maker.intersect(solver.valueOf(predecessor), lookedFor);
                if (brought == ValueSets::nothing)
                    continue;
                Witness witness = noWitness;
                if (ValueFlowStatementID logger = solver.statementThatLogged({ predecessor, ValueFlowNodeID(node) }))
                    witness = witnessOfStatement(inputs.program, solver.statementAt(logger));
                if (hasFunction(witness)) {
                    inputs.sets().forEachMember(brought, [&](ValueFlowObjectID closure) {
                        if (FunctionID function = universe.closureFunction(closure))
                            witnesses[function.index()] = witness;
                    });
                    remaining = maker.intersect(remaining, ValueSets::complement(brought));
                    lookedFor = maker.intersect(lookedFor, ValueSets::complement(brought));
                    if (lookedFor == ValueSets::nothing)
                        break;
                    continue;
                }
                auto seenThere = seen.add(predecessor.index(), ValueSets::nothing);
                ValueSetID fresh = maker.intersect(brought, ValueSets::complement(seenThere.iterator->value));
                if (fresh == ValueSets::nothing)
                    continue;
                seenThere.iterator->value = maker.unite(seenThere.iterator->value, fresh);
                auto entry = next.add(predecessor.index(), ValueSets::nothing);
                entry.iterator->value = maker.unite(entry.iterator->value, fresh);
            }
        }
        level.shrink(0);
        for (auto& [node, lookedFor] : next)
            level.append({ node, lookedFor });
        AOT::sort(level, { }, [](const auto& entry) { return entry.first; });
    }
    return witnesses;
}

// The table names these.
static BitVector findFunctionsNamedByTable(const Program& program)
{
    BitVector functions(program.functions.size());
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (entry.function)
            functions.quickSet(entry.function.index());
    }
    return functions;
}

void computeFunctionResults(const ValueFlowResultInputs& inputs, ValueFlowStampedCalls& calls, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    FlowApplication& application = *tables.application;
    unsigned numberOfFunctions = program.functions.size();
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    BitVector isCalledByHost = functionsCalledByHost(program, inputs.loops());
    EntriesWithoutStamp entries = findEntriesWithoutStamp(inputs, application);
    times.didFinish("functions: entries without stamp"_s);
    calls.functionsEnteredWithoutStamp = WTF::move(entries.functions);
    tables.functions.indirectCalls = WTF::move(entries.calls);

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

    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();
    ValueSetID callablesLookedInto = findCallablesLookedInto(inputs, maker);
    times.didFinish("functions: callables looked into"_s);
    tables.functions.boundFunctionUses = findBoundFunctionsOnlyCalled(inputs, callablesLookedInto);

    BitVector isNamedByTable = findFunctionsNamedByTable(program);
    Vector<Witness> witnessesOfEscapes = findWitnessesOfEscapes(inputs, maker);
    times.didFinish("functions: bound functions, named by table"_s);

    // Every entry into a function whose callers are all known is a call that names the function, a call whose stamp lists it, or an indirect call out of an operation.
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index) {
        FunctionFlow& flow = tables.functions.functions[index];
        ValueFlowObjectID closure = universe.closureOf(FunctionID(index));
        if (isCalledByHost.get(index) || isNamedByTable.get(index) || entries.functionsEnteredByHost.get(index) || calls.functionsNamedByUnsolvedCode.get(index) || inputs.isHeldByNativeCode(closure)) {
            flow.cause = UnprovenCause::ReachesNativeCode;
            return;
        }
        if (inputs.hasEscaped(closure)) {
            flow.cause = UnprovenCause::ReachesNativeCode;
            if (!witnessesOfEscapes.isEmpty())
                flow.witness = witnessesOfEscapes[index];
            return;
        }
        // A call of the untracked object may enter it, and the stamp of such a call is not complete.
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
        if (inputs.build.isEnabled(FlowResultKind::ObjectUses, index) && isOnlyCalled(inputs, callablesLookedInto, closure))
            flow.closureUse = ClosureUse::CalledOnly;
        OptionSet<FunctionCapabilities>& capabilities = application.signatures[index].provenCapabilities;
        capabilities.add(FunctionCapabilities::IsNeverCalledByHost);
        if (!calls.functionsInStamps.get(index) && !calls.functionsEnteredWithoutStamp.get(index))
            capabilities.add(FunctionCapabilities::HasOnlyDirectCallers);
    });
}

// What a built-in does with an operand that cannot tell one closure from another of the same function: it calls it, keeps it, hands it back.
static bool mayTellClosuresApart(const BuiltinParameter& parameter)
{
    constexpr OperandUses usesThatDoNotLook { OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly, OperandUse::Retains, OperandUse::Returns };
    constexpr OperandUses callsOrKeeps { OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly, OperandUse::Retains };
    OperandUses others = parameter.uses - usesThatDoNotLook;
    return !others.isEmpty() && (others != OperandUses { OperandUse::Inspects } || !parameter.uses.containsAny(callsOrKeeps));
}

// Two closures of one function are two objects.
void computeClosuresNeverCompared(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    const BuiltinTable& builtins = program.builtins();
    const ParallelLoops& loops = inputs.loops();
    ValueSetID closures = universe.closures();
    // Under the phase log: who can tell, part by part.
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

    // Without the masks of the operands: what a type would cut off stays, and the bit says "compared" of a closure too many at the worst.
    constexpr size_t comparisonsPerTask = 1024;
    std::span<const ValueFlowComparedValues> comparedValues = inputs.bodies().comparedValues();
    forEachIndexInParallel(loops, (comparedValues.size() + comparisonsPerTask - 1) / comparisonsPerTask, [&](unsigned task, unsigned worker) {
        for (const ValueFlowComparedValues& compared : comparedValues.subspan(task * comparisonsPerTask, std::min(comparisonsPerTask, comparedValues.size() - task * comparisonsPerTask))) {
            if (!inputs.isReached(compared.function))
                continue;
            auto sidesOf = [&](const ValueFlowOperand& operand) {
                return std::array { closuresIn(worker, operand.node ? inputs.solver().valueOf(operand.node) : ValueSets::nothing), closuresIn(worker, operand.constant) };
            };
            for (ValueSetID left : sidesOf(compared.left)) {
                if (left == ValueSets::nothing)
                    continue;
                for (ValueSetID right : sidesOf(compared.right)) {
                    if (right == ValueSets::nothing)
                        continue;
                    auto [low, high] = std::minmax(left.index(), right.index());
                    if (lookers[worker].pairsMet.add(static_cast<uint64_t>(low) << 32 | high).isNewEntry)
                        keep(worker, left == right ? left : makers.of(worker).intersect(left, right), { compared.function, { }, { } });
                }
            }
        }
    });

    BitVector builtinsThatLook = builtinsWithOverload(builtins, [](const BuiltinOverload& overload) {
        return (overload.receiver && mayTellClosuresApart(*overload.receiver)) || std::ranges::any_of(overload.parameters, mayTellClosuresApart);
    });
    // What a new WeakRef is born with is written under no slot.
    const std::array watchingBuiltins { builtins.canonicalID(Builtins::WeakRef), builtins.canonicalID(Builtins::FinalizationRegistryPrototypeRegister), builtins.canonicalID(Builtins::FinalizationRegistryPrototypeUnregister) };
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
            // A function of a protocol comes once more with anything at every operand, for what the language may hand it.
            if (value != ValueSets::anything)
                keep(worker, closuresIn(worker, value), { application.function, application.builtin, { } });
        };
        // They may land at any position.
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

    auto comparesAccessors = [](BuiltinPropertyActionKind kind) {
        return kind == BuiltinPropertyActionKind::TakesDescriptor || kind == BuiltinPropertyActionKind::TakesDescriptorMap || kind == BuiltinPropertyActionKind::DefinesGetter || kind == BuiltinPropertyActionKind::DefinesSetter;
    };
    BitVector builtinsThatCompareAccessors = builtinsWithOverload(builtins, [&](const BuiltinOverload& overload) {
        return std::ranges::any_of(overload.flow.propertyActions, comparesAccessors, &BuiltinPropertyAction::kind);
    });
    forEachBuiltinApplication(inputs, heap, builtinsThatCompareAccessors, [&](const ValueFlowBuiltinApplication& application) {
        Teller teller { application.function, application.builtin, { } };
        for (const BuiltinOverload& overload : builtins.overloadsOf(application.builtin)) {
            if (overload.mode != application.mode())
                continue;
            for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
                if (!comparesAccessors(action.kind) || !action.value.operand)
                    continue;
                ValueSetID handed = application.valueAt(*action.value.operand, heap.maker());
                if (action.kind == BuiltinPropertyActionKind::DefinesGetter || action.kind == BuiltinPropertyActionKind::DefinesSetter) {
                    if (handed != ValueSets::anything)
                        keep(0, closuresIn(0, handed), teller);
                    continue;
                }
                ValueSetID descriptors = action.kind == BuiltinPropertyActionKind::TakesDescriptor ? handed : ValueSets::anything;
                for (BuiltinName member : { BuiltinNames::get, BuiltinNames::set, BuiltinNames::value })
                    keep(0, closuresIn(0, heap.writtenUnder(descriptors, ValueFlowName::property(program.keyOf(member)))), teller);
            }
        }
    });

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
}

// The rule for Symbol answers the key of the call, which stands for every symbol that the call makes. Where the call makes one symbol, that key is the value.
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
            ValueFlowObjectID result = inputs.sets().onlyMember(inputs.solver().valueOf(statement.result));
            PropertyKeyID key = result ? inputs.universe().keyOf(result) : PropertyKeyID();
            if (!key || !inputs.symbolsCreatedOnce->get(key.index()))
                continue;
            const PropertyKey& symbol = program.at(key);
            if (symbol.kind == PropertyKeyKind::CreatedSymbol && symbol.module == function.module && symbol.position == function.at(statement.site).position)
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
