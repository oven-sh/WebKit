/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBuiltinIDs.h"
#include "AOTFlowStamps.h"
#include "AOTPhaseTimes.h"
#include "AOTSort.h"
#include "AOTValueFlowResultsFieldsInternal.h"
#include <atomic>
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
            auto add = [&](const EnteredGroup& group, const CalleeSet* stampThatListsCallees) {
                if (ValueSets::isComplement(group.callees))
                    return;
                bool namesNoEscapedClosure = sets.size(group.callees) > inputs.build.limit(LimitID::MaxValueFlowMembersToldApart);
                ValueSetID escaped = solver.possibleValuesOf(ValueFlowProgramNodes::escaped());
                sets.forEachMember(group.callees, [&](ValueFlowObjectID callee) {
                    FunctionID entered = universe.closureFunction(callee);
                    if (!entered || throwsBeforeEntry(program.at(entered), group.kind))
                        return;
                    if (namesNoEscapedClosure && sets.contains(escaped, callee))
                        return;
                    if (stampThatListsCallees && std::ranges::binary_search(stampThatListsCallees->functions, entered))
                        return;

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
                    FlowIndirectCall call { .caller = id, .callee = entered, .kind = kindOfIndirectCall(statement, group.kind), .times = BuiltinCallbackTimes::Once };
                    if (statement.site) {
                        call.site = application.accessSites.siteOfValue(id, statement.site);
                        call.isInCycle = valuesInCycles.quickGet(statement.site.index());
                    }
                    if (call.kind == FlowIndirectCallKind::BuiltinCallback) {
                        if (!timesOfCallbacksOfStatement)
                            timesOfCallbacksOfStatement = timesOfCallbacks(program.builtins(), stamp);
                        call.times = *timesOfCallbacksOfStatement;
                    }
                    calls.append(call);
                });
            };

            Vector<uint32_t, 8> worklist;
            auto follow = [&](uint32_t number) {
                if (leadsToGroup.quickGet(number) && !actsForNobody[number])
                    worklist.append(number);
            };
            forEachRequestOf(inputs, idOf(statement), [&](const EnteredGroup& group) {
                bool isGroupOfCallees = (group.kind == ValueFlowCallKind::Call || group.kind == ValueFlowCallKind::Construct) && function.role != FunctionRole::Model;
                add(group, isGroupOfCallees ? stamp : nullptr);
            }, [&](ValueFlowStatementID made) {
                noteAsker(numberOf(made));
                follow(numberOf(made));
            });
            if (worklist.isEmpty())
                continue;

            HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> seen;
            while (!worklist.isEmpty()) {
                uint32_t number = worklist.takeLast();
                if (!seen.add(number).isNewEntry)
                    continue;
                for (const GroupOfStatement& entry : groupsOf(number))
                    add(entry.group, nullptr);
                for (uint32_t asked : statementsAskedFor.of(number))
                    follow(asked);
            }
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
    for (size_t function : isEnteredByHost)
        tables.functions.indirectCalls.append(FlowIndirectCall { .callee = FunctionID(static_cast<uint32_t>(function)), .kind = FlowIndirectCallKind::Host, .times = BuiltinCallbackTimes::Repeatedly });
    stampedCalls.functionsEnteredWithoutStamp = BitVector(numberOfFunctions);
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
        sets.forEachMember(solver.possibleValuesOf(covered), [&](ValueFlowObjectID member) {
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
    case ValueFlowNameKind::FoundThroughEscapedInstances:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static ValueSetID findCallablesLookedInto(const ValueFlowResultInputs& inputs, ValueSetMaker& maker)
{
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

static bool isOnlyCalled(const ValueFlowResultInputs& inputs, ValueSetID callablesLookedInto, ValueFlowObjectID object)
{
    if (inputs.hasEscaped(object) || inputs.isLost(object) || inputs.isHeldByNativeCode(object))
        return false;
    return !inputs.sets().contains(callablesLookedInto, object) && markedMutationsOf(inputs, object).isEmpty();
}

static bool mayHold(const ValueFlowResultInputs& inputs, const ValueFlowOperand& operand, ValueFlowObjectID object)
{
    return (operand.constant && inputs.sets().contains(operand.constant, object)) || (operand.node && inputs.sets().contains(inputs.solver().possibleValuesOf(operand.node), object));
}

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
            ValueSetID targets = inputs.possibleValuesOf(maker, operands[1]);
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
            ValueSetID targets = inputs.possibleValuesOf(maker, operands[1]);
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

static Vector<Witness> findWitnessesOfEscapes(const ValueFlowResultInputs& inputs, ValueSetMaker& maker)
{
    const ValueFlowSolver& solver = inputs.solver();
    const ValueFlowUniverse& universe = inputs.universe();
    if (!solver.input().remembersWhoLoggedEachEdge)
        return { };
    Vector<Witness> witnesses(FillWith { }, inputs.program.functions.size(), noWitness);

    using SetOfNode = HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    ValueSetID remaining = maker.intersect(solver.possibleValuesOf(escaped), universe.closures());
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
                ValueSetID brought = maker.intersect(solver.possibleValuesOf(predecessor), lookedFor);
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

    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();
    ValueSetID callablesLookedInto = findCallablesLookedInto(inputs, maker);
    times.didFinish("functions: callables looked into"_s);
    tables.functions.boundFunctionUses = findBoundFunctionsOnlyCalled(inputs, callablesLookedInto);
    tables.sites.boundFunctionsAtSites = findBoundFunctionsAtSites(inputs, application.accessSites);

    BitVector isNamedByTable(numberOfFunctions);
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (entry.function)
            isNamedByTable.quickSet(entry.function.index());
    }
    Vector<Witness> witnessesOfEscapes = findWitnessesOfEscapes(inputs, maker);
    times.didFinish("functions: bound functions, named by table"_s);

    // A read of a name finds a value that was stored, on the closure or above it, or nothing.
    constexpr OptionSet<ObjectMutation> mutationsThatLetReadsRunCode { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype };
    bool objectsAboveFunctionsAreReadWithoutCode = std::ranges::none_of(std::array { Builtins::FunctionPrototype, Builtins::ObjectPrototype }, [&](BuiltinID above) {
        return markedMutationsOf(inputs, universe.objectOf(above)).containsAny(mutationsThatLetReadsRunCode);
    });

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
        const Function& function = inputs.program.functions[index];
        bool inheritsFromFunctionPrototype = !function.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) && (!function.constructedClass || inputs.program.at(function.constructedClass).has(LayoutFlags::IsConstructorFunction));
        if (objectsAboveFunctionsAreReadWithoutCode && inheritsFromFunctionPrototype && inputs.build.isEnabled(FlowResultKind::HolderMutations, index) && !markedMutationsOf(inputs, closure).containsAny(mutationsThatLetReadsRunCode))
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

void computeClosuresNeverCompared(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    const BuiltinTable& builtins = program.builtins();
    const ParallelLoops& loops = inputs.loops();
    ValueSetID closures = universe.closures();
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