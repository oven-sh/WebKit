/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPropertyWalkInternal.h"

#if ENABLE(AOT)

#include "AOTFlowCallOperations.h"
#include "AOTFlowCallState.h"
#include "AOTFlowModules.h"
#include "AOTFlowOrder.h"
#include "AOTFlowPropertyState.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

std::array<FlowPrimitiveWrapper, 5> primitiveWrappers()
{
    return { {
        { Primitive::AnyString, BuiltinClasses::String },
        { Primitive::Number, BuiltinClasses::Number },
        { Primitive::Boolean, BuiltinClasses::Boolean },
        { Primitive::AnySymbol, BuiltinClasses::Symbol },
        { Primitive::BigInt, BuiltinClasses::BigInt },
    } };
}

static void noteStaticFieldAccesses(FlowSolver& solver, const FlowPropertyWalk& walk, std::span<const FlowPropertySource> sources)
{
    const FlowGraph& graph = solver.graph();
    FlowStaticFieldAccessKind kind = ensuresOwnLocations(walk.operation) ? FlowStaticFieldAccessKind::Store : FlowStaticFieldAccessKind::Read;
    for (const FlowPropertySource& source : sources) {
        if (source.kind != FlowPropertySourceKind::Value || graph.kindOf(source.node) != FlowNodeKind::Variable)
            continue;
        VariableID variable = graph.describe(source.node).variable;
        bool isStaticField = !!solver.program().at(variable).owner;
        if (isStaticField)
            noteStaticFieldAccess(solver, walk.waiter, variable, kind);
    }
}

static bool isAccessor(const FlowPropertySource& source)
{
    return source.kind == FlowPropertySourceKind::Getter || source.kind == FlowPropertySourceKind::Setter;
}

// A walk that looks for setters passes the data properties of the objects above the receiver. An accessor that it finds on the receiver is looked up too, and no target.
// The object at which a walk for another receiver starts is above that receiver.
static void noteMemberReads(FlowSolver& solver, const FlowPropertyWalk& walk, std::span<const FlowPropertySource> sources)
{
    const FlowGraph& graph = solver.graph();
    for (const FlowPropertySource& source : sources) {
        bool isDataProperty = source.kind == FlowPropertySourceKind::Value && graph.kindOf(source.node) == FlowNodeKind::Property;
        if ((!isDataProperty && !isAccessor(source)) || !source.key || !source.holder)
            continue;
        bool isOnReceiver = source.isOwn && !walk.startsAboveReceiver;
        if (isDataProperty && isOnReceiver && ensuresOwnLocations(walk.operation))
            continue;
        FlowMemberHolder kind = memberHolderKindOf(solver.objects(), source.holder);
        if (kind == FlowMemberHolder::Always || (kind == FlowMemberHolder::WhenPrototype && !isOnReceiver))
            noteMemberRead(solver, walk.waiter, source.holder, source.key);
    }
}

namespace {

struct FinishedWalk {
    size_t numberOfHolders { 0 };
    bool readsUnnamedKeysOfUnknownObject { false };
    bool passesHolderWithoutName { false };
    Vector<FlowObjectID, 2> visitedObjects;
    Vector<FlowPropertySource, 1> passedMembers;
};

} // anonymous namespace

static uint64_t numberOfReceiver(FlowObjectID receiver)
{
    return receiver.index();
}

static uint64_t numberOfReceiver(FlowPrimitiveReceiver receiver)
{
    return 1ull << 32 | receiver.primitives;
}

static FlowObjectID objectOfReceiver(FlowObjectID receiver)
{
    return receiver;
}

static FlowObjectID objectOfReceiver(FlowPrimitiveReceiver)
{
    return { };
}

template<typename Receiver, size_t inlineCapacity>
static FinishedWalk collectSources(FlowSolver& solver, Receiver receiver, const FlowKeySet& keys, const FlowPropertyWalk& walk, Vector<FlowPropertySource, inlineCapacity>& sources)
{
    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    // At one holder each name, and each cause, leads to the same unknown source and to the same node of the unnamed keys. The sources of a holder follow each other.
    auto isRepeated = [&](const FlowPropertySource& source) {
        for (size_t index = sources.size(); index-- && sources[index].holder == source.holder;) {
            if (sources[index] == source)
                return true;
        }
        return false;
    };
    auto collect = [&](const FlowPropertySource& source) {
        if (source.key || !isRepeated(source))
            sources.append(source);
    };
    FlowPropertySourceFunctor collector(collect);
    FlowPropertyWalker walker(inputs, keys, walk, collector);
    walker.walkFrom(receiver);

    FinishedWalk finished { walker.numberOfHolders(), walker.readsUnnamedKeysOfUnknownObject(), walker.passesHolderWithoutName(), { }, { } };
    finished.passedMembers.append(walker.passedMembers());
    // Above a receiver the unknown object answers without a look at its locations.
    bool startsAtUnknownObject = numberOfReceiver(receiver) == unknownFlowObject.index();
    walker.forEachVisitedObject([&](FlowObjectID object) {
        if (object != unknownFlowObject || startsAtUnknownObject)
            finished.visitedObjects.append(object);
    });
    return finished;
}

static uint64_t numberOfKeys(FlowLookups& lookups, const FlowKeySet& keys)
{
    if (PropertyKeyID name = keys.onlyName())
        return static_cast<uint64_t>(name.index()) + 1;

    Hasher hasher;
    for (PropertyKeyID name : keys.names)
        add(hasher, name);
    add(hasher, keys.indexes.minimum, keys.indexes.maximum, keys.anyNumericName, keys.anyString, keys.anySymbol);
    uint32_t hash = hasher.hash();

    std::optional<uint32_t> number = lookups.numbersOfKeySets.find(hash, [&](uint32_t position) {
        return lookups.keySets[position] == keys;
    });
    if (!number) {
        number = lookups.keySets.size();
        lookups.keySets.append(keys);
        lookups.numbersOfKeySets.add(hash, *number);
    }
    return 1ull << 32 | *number;
}

static std::pair<uint64_t, uint64_t> keyOfLookup(uint64_t receiver, uint64_t keys, const FlowPropertyWalk& walk)
{
    uint64_t flags = static_cast<uint64_t>(walk.operation) << 5 | static_cast<uint64_t>(walk.builtinMembers) << 3 | walk.startsAtPrototype << 2 | walk.isOwnOnly << 1 | walk.isEnumerableOnly;
    return { receiver << 8 | flags, keys };
}

static FlowStatementSite statementOfLookup(const FlowGraph& graph, uint32_t lookup)
{
    FlowInstanceID host = graph.findInstance(graph.hostFunction(), FlowContext::Proven);
    RELEASE_ASSERT(host);
    return { host, { FlowStatementKind::Lookup, lookup } };
}

void forgetAnswersOfLookupsThrough(FlowSolver& solver, FlowObjectID object)
{
    solver.propertyState().lookups.didChange(object);
}

void forgetAnswersOfLookupsWaitingOnMutations(FlowSolver& solver, FlowObjectID object)
{
    solver.propertyState().lookups.didChangeMutations(object);
}

void forgetAnswersOfLookupsWaitingOnProperty(FlowSolver& solver, FlowObjectID object, PropertyKeyID key)
{
    solver.propertyState().lookups.didChangeProperty(object, key);
}

// The waiter of the kept walk is a statement of the lookup's own, in the instance of the host, which is charged for nothing.
template<typename Receiver>
static const FlowLookup& currentLookup(FlowSolver& solver, Receiver receiver, const FlowKeySet& keys, const FlowPropertyWalk& walk)
{
    FlowLookups& lookups = solver.propertyState().lookups;
    // A walk of the fixpoint check makes no prediction.
    if (std::exchange(lookups.answersAreOfFixpointCheck, solver.isCheckingFixpoint()) != solver.isCheckingFixpoint())
        lookups.forgetAnswers();

    uint32_t index = lookups.indexes.ensure(keyOfLookup(numberOfReceiver(receiver), numberOfKeys(lookups, keys), walk), [&] {
        lookups.all.append(FlowLookup { });
        lookups.all.last().receiver = objectOfReceiver(receiver);
        return static_cast<uint32_t>(lookups.all.size() - 1);
    }).iterator->value;

    FlowLookup& lookup = lookups.all[index];
    FlowPropertyWalk walkOfLookup = walk;
    walkOfLookup.waiter = statementOfLookup(solver.graph(), index);
    if (lookup.sumOfVersions != lookups.sumOfVersions(lookup))
        lookup.isCurrent = false;
    while (!std::exchange(lookup.isCurrent, true)) {
        Vector<FlowPropertySource, 8> sources;
        FlowLookupWaits waits;
        uint64_t numberOfChangesBefore = lookups.numberOfChanges;
        solver.listWaitsOfLookup(&waits);
        lookups.lookupBeingWalked = &lookup;
        FinishedWalk finished = collectSources(solver, receiver, keys, walkOfLookup, sources);
        lookups.lookupBeingWalked = nullptr;
        solver.listWaitsOfLookup(nullptr);
        lookup.sources = Vector<FlowPropertySource>(sources.span());
        lookup.passedMembers = Vector<FlowPropertySource>(finished.passedMembers.span());
        lookup.waits = Vector<FlowLookupWait>(waits.span());
        // The next walk finds the locations that this one created.
        if (lookups.numberOfChanges != numberOfChangesBefore)
            lookup.isCurrent = false;
        lookup.numberOfHolders = static_cast<uint32_t>(finished.numberOfHolders);
        lookup.visitedObjects = Vector<FlowObjectID>(finished.visitedObjects.span());
        lookup.sumOfVersions = lookups.sumOfVersions(lookup);
        lookup.readsUnnamedKeysOfUnknownObject = finished.readsUnnamedKeysOfUnknownObject;
        lookup.passesHolderWithoutName = finished.passesHolderWithoutName;
        ++lookups.numberOfWalks;
    }
    ++lookups.numberOfAnswersTaken;
    if (!lookup.reliesOn.isEmpty())
        lookups.noteAnswerRelyingOnStartUp(index, walk.receivers);
    return lookup;
}

// Beside the unknown object a receiver that has escaped is a prediction: the unknown source answers for it, so nothing has to be fixed for its sake.
// Whether it was looked at before it escaped, or before the unknown object came, depends on the order of the solve. What start-up fixes goes by the sets at the end.
void keepWhatProvenLookupsRelyOn(FlowSolver& solver)
{
    FlowPropertyState& state = solver.propertyState();
    BitVector members;
    HashSet<FlowAbsentName> absentNames;
    BitVector prototypeLinks;
    for (uint64_t answer : state.lookups.answersRelyingOnStartUp) {
        const FlowLookup& lookup = state.lookups.all[static_cast<uint32_t>(answer >> 32) - 1];
        FlowNodeID receivers(static_cast<uint32_t>(answer));
        if (receivers && lookup.receiver && solver.hasReachedNativeCode(lookup.receiver) && solver.valueOf(receivers).hasUnknownObject())
            continue;
        for (BuiltinID member : lookup.reliesOn.members)
            members.set(member.index());
        for (const FlowAbsentName& name : lookup.reliesOn.absentNames)
            absentNames.add(name);
        for (BuiltinID holder : lookup.reliesOn.prototypeLinks)
            prototypeLinks.set(holder.index());
    }
    state.membersTakenForConstants = WTF::move(members);
    state.namesTakenForAbsent = WTF::move(absentNames);
    state.holdersWithLinkTakenForConstant = WTF::move(prototypeLinks);
}

// A walk leaves out a member that native code calls, and nothing wakes a statement when a member becomes one.
static void removeMembersLeftToNativeCode(FlowSolver& solver, FlowWalkOperation operation, Vector<FlowPropertySource, 8>& sources)
{
    if (operation == FlowWalkOperation::Set)
        return;
    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    sources.removeAllMatching([&](const FlowPropertySource& source) {
        return source.isFoundThroughUnknownObject && (inputs.records.mayRunUnknownJavaScript() || isCalledByNativeCode(inputs, source.builtin));
    });
}

// A class of the host prints itself with a native method that calls a function of a start-up module. The table lists few of those classes, so the method is what an object
// that nobody describes answers under the name, or what a built-in object of the host does. Nobody stores the method anywhere else, so whoever calls it by hand has read it there.
// Only a key that is the name counts. Under a key that the program computes the method is found like anything that no source names: its call says that the function is not in the executable.
static bool findsHostMethodCallingStartupLoads(FlowSolver& solver, const FlowKeySet& keys, std::span<const FlowPropertySource> sources)
{
    const Program& program = solver.program();
    const BuiltinTable& builtins = solver.builtins();
    bool namesMethod = std::ranges::any_of(builtins.namesOfHostMethodsCallingStartupLoads(), [&](BuiltinName name) {
        return keys.names.contains(program.keyOf(name));
    });
    return namesMethod && std::ranges::any_of(sources, [&](const FlowPropertySource& source) {
        return source.kind == FlowPropertySourceKind::Unknown || (source.builtin && builtins.at(source.builtin).origin == BuiltinOrigin::Host);
    });
}

static void exposeStartupModulesToReader(FlowSolver& solver, const FlowKeySet& keys, const FlowPropertyWalk& walk, std::span<const FlowPropertySource> sources)
{
    // The method is not enumerable, so a spread passes it by.
    if (walk.operation != FlowWalkOperation::Get || !walk.waiter.instance || solver.callState().unseenCode.startupModulesAreExposed)
        return;
    if (findsHostMethodCallingStartupLoads(solver, keys, sources) && isInSeenCode(solver, walk.waiter))
        exposeStartupModules(solver, solver.witnessAt(walk.waiter, ValueID()));
}

// But for its waiter, a walk is the same for every statement that asks: one walk answers all of them, and each makes the waits of that walk in its own name.
template<typename Receiver>
static void walkWhileSolving(FlowSolver& solver, Receiver receiver, const FlowKeySet& keys, const FlowPropertyWalk& walk, const FlowPropertySourceFunctor& functor)
{
    Vector<FlowPropertySource, 8> sources;
    Vector<FlowPropertySource, 1> passedMembers;
    bool readsUnnamedKeysOfUnknownObject;
    if (walk.waiter.instance) {
        const FlowLookup& lookup = currentLookup(solver, receiver, keys, walk);
        if (lookup.passesHolderWithoutName && !solver.mayRelyOn(FlowAbsence::PropertyOnHolder, walk.waiter))
            return;
        sources.append(lookup.sources.span());
        passedMembers.append(lookup.passedMembers.span());
        removeMembersLeftToNativeCode(solver, walk.operation, sources);
        readsUnnamedKeysOfUnknownObject = lookup.readsUnnamedKeysOfUnknownObject;
        for (const FlowLookupWait& wait : lookup.waits)
            solver.waitOn(wait, walk.waiter);
        solver.charge(walk.waiter, lookup.numberOfHolders);
    } else {
        FinishedWalk finished = collectSources(solver, receiver, keys, walk, sources);
        passedMembers = WTF::move(finished.passedMembers);
        readsUnnamedKeysOfUnknownObject = finished.readsUnnamedKeysOfUnknownObject;
    }
    if (readsUnnamedKeysOfUnknownObject)
        exposeMembersOfEscapedBuiltins(solver, walk.waiter.instance ? solver.witnessAt(walk.waiter, ValueID()) : noWitness);
    exposeStartupModulesToReader(solver, keys, walk, sources.span());

    noteStaticFieldAccesses(solver, walk, sources.span());
    noteMemberReads(solver, walk, sources.span());
    noteMemberReads(solver, walk, passedMembers.span());
    for (const FlowPropertySource& source : sources)
        functor(source);
}

static void waitOnKeys(FlowSolver& solver, FlowObjectID object, const FlowKeySet& keys, FlowStatementSite waiter)
{
    for (PropertyKeyID name : keys.names)
        solver.waitOnProperty(object, name, waiter);
    if (keys.hasUnnamedKeys() || keys.mayNameIndex())
        solver.waitOnProperties(object, waiter);
}

static bool isImmutableUnder(const FlowWalkInputs& inputs, const FlowHolder& holder, const FlowKeySet& keys)
{
    if (mutabilityOf(inputs, holder) == FlowHolderMutability::Immutable)
        return true;
    if (keys.hasUnnamedKeys())
        return false;
    if ((keys.mayNameIndex() || keys.anyNumericName) && mutabilityOfIndexes(inputs, holder) == FlowHolderMutability::Ordinary)
        return false;
    return std::ranges::all_of(keys.names, [&](PropertyKeyID name) {
        return mutabilityOf(inputs, holder, name) == FlowHolderMutability::Immutable;
    });
}

// What a walk, or whoever takes its sources, reads from the records of a holder. A new location and the prototype link have waits of their own.
// A holder with all of these has been reached by unseen JavaScript, so it answers with a prediction whatever is stored under unnamed keys later.
static constexpr OptionSet<ObjectMutation> mutationsChangingLookups {
    ObjectMutation::ReachNativeCode,
    ObjectMutation::ReachUnknownJavaScript,
    ObjectMutation::DeleteField,
    ObjectMutation::RedefineProperty,
    ObjectMutation::StoreOfOtherType,
};

void forEachPropertySource(FlowSolver& solver, FlowObjectID receiver, const FlowKeySet& keys, const FlowPropertyWalk& walk, NOESCAPE const FlowPropertySourceFunctor& functor)
{
    if (keys.isEmpty() || !receiver)
        return;
    walkWhileSolving(solver, receiver, keys, walk, functor);
}

void forEachPropertySource(const FlowSolution& solution, FlowObjectID receiver, const FlowKeySet& keys, const FlowPropertyWalk& walk, NOESCAPE const FlowPropertySourceFunctor& functor)
{
    if (keys.isEmpty() || !receiver)
        return;
    FlowWalkInputs inputs = FlowWalkInputs::of(solution);
    FlowPropertyWalker(inputs, keys, walk, functor).walkFrom(receiver);
}

void forEachPropertySource(FlowSolver& solver, FlowPrimitiveReceiver receiver, const FlowKeySet& keys, const FlowPropertyWalk& walk, NOESCAPE const FlowPropertySourceFunctor& functor)
{
    if (keys.isEmpty())
        return;
    walkWhileSolving(solver, receiver, keys, walk, functor);
}

FlowPropertyWalker::FlowPropertyWalker(const FlowWalkInputs& inputs, const FlowKeySet& keys, const FlowPropertyWalk& walk, const FlowPropertySourceFunctor& functor)
    : m_inputs(inputs)
    , m_keys(keys)
    , m_walk(walk)
    , m_functor(functor)
{
}

void FlowPropertyWalker::walkFrom(FlowObjectID receiver)
{
    // What the body of the module makes lies beside such a built-in in every value, and it is that object that has the properties.
    if (m_inputs.objects.kindOf(receiver) == FlowObjectKind::Builtin && findCompiledModuleWithObject(m_inputs.program, m_inputs.objects.at(receiver).builtin))
        return;

    m_receiver = receiver;
    m_receiverShowsEveryKey = !m_keys.hasUnlistedNames() && !m_walk.startsAtPrototype;
    enqueue(holderOf(m_inputs.objects, receiver), FlowCertainty::Proven);
    drain();
    reportAbsentIfChainEnds();
}

void FlowPropertyWalker::walkFrom(FlowPrimitiveReceiver receiver)
{
    if (!followsPrototypes(m_walk.operation))
        return;
    if (m_walk.isOwnOnly) {
        reportUnknown({ });
        return;
    }

    std::array<FlowPrimitiveWrapper, 5> wrappers = primitiveWrappers();
    auto wrapsReceiver = [&](const FlowPrimitiveWrapper& wrapper) {
        return !!(receiver.primitives & wrapper.primitives);
    };
    bool hasOneChain = std::ranges::count_if(wrappers, wrapsReceiver) == 1;
    FlowCertainty certainty = hasOneChain ? FlowCertainty::Proven : FlowCertainty::Predicted;

    // A read under a key nobody can name finds each member of each prototype above the primitive. Where unseen JavaScript has reached one of them that is not locked, the unknown source is among the answers,
    // and it stands for the others: that code names the global object, so each member has reached native code, and an accessor up there answers a primitive with a primitive or with one of these prototypes.
    auto isOpened = [&](const FlowPrimitiveWrapper& wrapper) {
        BuiltinID prototype = m_inputs.builtins().at(wrapper.builtinClass).prototype;
        if (!wrapsReceiver(wrapper) || !prototype)
            return false;
        FlowHolder holder = holderOf(m_inputs, prototype);
        return holder.object && m_inputs.hasReachedUnknownJavaScript(holder.object) && mutabilityOf(m_inputs, holder) == FlowHolderMutability::Ordinary;
    };
    bool findsEveryMember = m_keys.hasUnnamedKeys() && m_walk.operation == FlowWalkOperation::Get && m_walk.builtinMembers != FlowBuiltinMemberLookup::None;
    if (m_inputs.isSolving() && findsEveryMember && std::ranges::any_of(wrappers, isOpened)) {
        reportUnknown({ });
        return;
    }

    for (const FlowPrimitiveWrapper& wrapper : wrappers) {
        if (!wrapsReceiver(wrapper))
            continue;
        if (BuiltinID prototype = m_inputs.builtins().at(wrapper.builtinClass).prototype)
            enqueue(holderOf(m_inputs, prototype), certainty);
        else
            reportUnknown({ });
    }
    drain();
    reportAbsentIfChainEnds();
}

bool FlowPropertyWalker::isEnqueued(const FlowHolder& holder) const
{
    if (m_pending.size() > maxScannedHolders)
        return holder.object ? m_visitedObjects.contains(holder.object) : m_visitedBuiltins.get(holder.builtin.index());
    return std::ranges::any_of(m_pending.span().subspan(m_numberOfForgottenHolders), [&](const PendingHolder& pending) {
        return pending.holder == holder;
    });
}

void FlowPropertyWalker::addToVisitedSets(const FlowHolder& holder)
{
    if (holder.object)
        m_visitedObjects.add(holder.object);
    else
        m_visitedBuiltins.set(holder.builtin.index());
}

// A chain is short. The sets are for a link that holds many prototypes.
void FlowPropertyWalker::enqueue(const FlowHolder& holder, FlowCertainty certainty)
{
    RELEASE_ASSERT(holder.object || holder.builtin);
    if (isEnqueued(holder))
        return;

    m_pending.append(PendingHolder { holder, certainty });
    if (m_pending.size() <= maxScannedHolders)
        return;
    if (m_pending.size() > maxScannedHolders + 1) {
        addToVisitedSets(holder);
        return;
    }
    for (const PendingHolder& pending : m_pending.span().subspan(m_numberOfForgottenHolders))
        addToVisitedSets(pending.holder);
}

void FlowPropertyWalker::drain()
{
    for (size_t index = 0; index < m_pending.size(); ++index)
        visit(PendingHolder(m_pending[index]));
    if (m_inputs.isSolving() && m_walk.waiter.instance)
        m_inputs.solver->charge(m_walk.waiter, m_pending.size());
}

void FlowPropertyWalker::visit(const PendingHolder& pending)
{
    const FlowHolder& holder = pending.holder;
    m_holderIsImmutable = false;
    m_holderShowsConstantMembers = false;
    m_holderHasClosedAbsentName = false;
    m_holderMayHaveUnseenAccessor = false;
    m_unknownSourceAnswersForHolder = false;
    if (holder.object == unknownFlowObject) {
        visitUnknownObject();
        return;
    }
    if (isOpenedByUnseenJavaScript(holder)) {
        visitOpenedHolder(holder);
        return;
    }

    m_holderIsImmutable = holder.object && isImmutableUnder(m_inputs, holder, m_keys);
    m_holderShowsConstantMembers = !m_holderIsImmutable && findsOnlyConstantMembers(holder);
    m_holderMayHaveUnseenAccessor = holder.object && !m_holderIsImmutable && !m_holderShowsConstantMembers && isReachedByUnseenCode(holder);
    // A built-in holder still lists the members of the table and what the program put under a name. The unknown source stands for whatever else it holds or inherits, now and later, so nothing is waited on.
    m_unknownSourceAnswersForHolder = m_holderMayHaveUnseenAccessor && m_inputs.isSolving() && isAnsweredAtOpenedHolders(m_walk.operation);
    if (m_unknownSourceAnswersForHolder)
        reportUnknown(holder);
    else
        registerWaits(holder);
    // What is listed beside the unknown source is a prediction, and running every rule again makes none.
    if (m_unknownSourceAnswersForHolder && m_inputs.solver->isCheckingFixpoint()) {
        m_receiverShowsEveryKey = false;
        m_chainMayEnd = true;
        m_certaintyOfAbsence = FlowCertainty::Predicted;
        return;
    }
    FlowCertainty certainty = certaintyAt(holder, pending.certainty);
    if (isReceiver(holder) && m_walk.startsAtPrototype) {
        // A flow object stands for many objects and can be its own prototype: there it is a holder like any other.
        RELEASE_ASSERT(m_pending.size() == 1);
        m_numberOfForgottenHolders = 1;
        m_receiver = { };
    } else
        reportOwnSources(holder, certainty);
    if (std::exchange(m_lookupEndsAtHolder, false))
        return;
    if (m_walk.isOwnOnly) {
        m_chainMayEnd = true;
        m_certaintyOfAbsence = weakerOf(m_certaintyOfAbsence, certainty);
        return;
    }
    if (followsPrototypes(m_walk.operation))
        enqueuePrototypesOf(holder, certainty);
}

// Unseen code names the global object, and from there every built-in object of the realm.
bool FlowPropertyWalker::isReachedByUnseenCode(const FlowHolder& holder) const
{
    if (m_inputs.hasReachedUnknownJavaScript(holder.object))
        return true;
    return holder.builtin && m_inputs.records.unseenCodeCanRun() && m_inputs.builtins().at(holder.builtin).scope == BuiltinScope::Realm;
}

// A member of the table may be locked on a built-in holder whoever has reached it.
bool FlowPropertyWalker::isOpenedByUnseenJavaScript(const FlowHolder& holder) const
{
    return m_inputs.isSolving() && !holder.builtin && isAnsweredAtOpenedHolders(m_walk.operation)
        && m_inputs.hasReachedUnknownJavaScript(holder.object) && !isImmutableUnder(m_inputs, holder, m_keys);
}

// Each location of the holder holds the unknown value, its link the unknown object, and all that it inherits from has been reached too.
// So the unknown source stands for whatever a lookup finds here or above, now and later: nothing is waited on, and what is reported beside it, under a name or an index, is a prediction.
void FlowPropertyWalker::visitOpenedHolder(const FlowHolder& holder)
{
    // Running every rule again makes no prediction.
    bool reportsPredictions = !m_inputs.solver->isCheckingFixpoint();
    if (isReceiver(holder) && m_walk.startsAtPrototype) {
        RELEASE_ASSERT(m_pending.size() == 1);
        m_numberOfForgottenHolders = 1;
        m_receiver = { };
    } else if (reportsPredictions) {
        for (PropertyKeyID name : m_keys.names) {
            FlowOwnProperty own = findOwnProperty(m_inputs, holder, name);
            own.key = name;
            if (own.lookup != FlowOwnLookup::Absent)
                reportProperty(holder, own, FlowCertainty::Predicted);
        }
        if (isReceiver(holder) && m_keys.mayNameIndex() && !m_keys.hasUnnamedKeys()) {
            forEachIndexedLocation(m_inputs, holder.object, m_keys.indexes, [&](const FlowIndexedLocation& location) {
                reportNode(FlowPropertySourceKind::Value, holder, location.key, location.node, FlowCertainty::Predicted);
            });
        }
    }

    reportUnknown(holder);
    m_receiverShowsEveryKey = false;
    m_chainMayEnd = true;
    m_certaintyOfAbsence = FlowCertainty::Predicted;
    if (m_walk.isOwnOnly || !followsPrototypes(m_walk.operation) || !reportsPredictions)
        return;

    reportMembersOfBuiltinObjects();
    if (!m_keys.hasOnlyNames())
        return;
    FlowPrototypeSet prototypes = prototypesOf(m_inputs, holder, FlowStatementSite(), &m_visitedObjects);
    for (const FlowHolder& prototype : prototypes.holders)
        enqueue(prototype, FlowCertainty::Predicted);
}

void FlowPropertyWalker::visitUnknownObject()
{
    FlowHolder holder { unknownFlowObject, { } };
    reportUnknown(holder);
    if (followsPrototypes(m_walk.operation))
        reportMembersOfBuiltinObjects();
    if (!isReceiver(holder))
        return;

    switch (m_walk.operation) {
    case FlowWalkOperation::Set:
    case FlowWalkOperation::DefineOwnProperty:
    case FlowWalkOperation::Delete:
        reportOwnSources(holder, FlowCertainty::Predicted);
        // An accessor that is defined through the unknown object under one of the names after this walk is found by another walk. At most three locations are made under a name.
        if (m_inputs.isSolving() && m_walk.waiter.instance && m_walk.operation != FlowWalkOperation::DefineOwnProperty && !std::exchange(m_waitsOnNamesOfUnknownObject, true)) {
            for (PropertyKeyID name : m_keys.names)
                m_inputs.solver->waitOnProperty(unknownFlowObject, name, m_walk.waiter);
        }
        return;
    case FlowWalkOperation::Get:
    case FlowWalkOperation::HasProperty:
    case FlowWalkOperation::CopyDataProperties:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowPropertyWalker::enqueuePrototypesOf(const FlowHolder& holder, FlowCertainty certainty)
{
    FlowStatementSite waiter = m_unknownSourceAnswersForHolder ? FlowStatementSite() : m_walk.waiter;
    FlowPrototypeSet prototypes = m_holderHasClosedAbsentName ? prototypesFixedAtStartUp(m_inputs, holder, waiter, &m_visitedObjects) : prototypesOf(m_inputs, holder, waiter, &m_visitedObjects);
    if (!prototypes.isNeverNull) {
        m_chainMayEnd = true;
        m_certaintyOfAbsence = weakerOf(m_certaintyOfAbsence, certainty);
    }
    if (!prototypes.isNeverUnknown || m_holderMayHaveUnseenAccessor)
        reportUnknown(holder);
    if (!prototypes.isNeverUnknown)
        reportMembersOfBuiltinObjects();

    bool hasOneChain = prototypes.numberOfHolders == 1 && prototypes.isNeverUnknown;
    FlowCertainty above = hasOneChain ? certainty : FlowCertainty::Predicted;
    for (const FlowHolder& prototype : prototypes.holders)
        enqueue(prototype, above);
}

void FlowPropertyWalker::registerWaits(const FlowHolder& holder)
{
    if (!m_inputs.isSolving() || !m_walk.waiter.instance || !holder.object)
        return;
    if (m_walk.operation == FlowWalkOperation::DefineOwnProperty || m_holderIsImmutable)
        return;

    FlowSolver& solver = *m_inputs.solver;
    // The first write to the members of a built-in holder under a key nobody can name wakes whoever waits on its mutations, whichever it has.
    if (m_holderShowsConstantMembers || !m_inputs.records.possibleMutations(holder.object).containsAll(mutationsChangingLookups))
        solver.waitOnMutations(holder.object, m_walk.waiter, mutationsChangingLookups);
    waitOnKeys(solver, holder.object, m_keys, m_walk.waiter);
    bool answersWithNamesOfUnknownObject = holder.object == unknownFlowObject ? m_inputs.hasReachedNativeCode(holder.object) : sharesLocationsWithUnknownObject(m_inputs, holder.object);
    if (m_holderShowsConstantMembers || !answersWithNamesOfUnknownObject || std::exchange(m_waitsOnNamesOfUnknownObject, true))
        return;

    // Every escape creates entries on the unknown object. An access under unnamed keys answers by its mutations and waits on those.
    for (PropertyKeyID name : m_keys.names)
        solver.waitOnProperty(unknownFlowObject, name, m_walk.waiter);
}

FlowCertainty FlowPropertyWalker::certaintyAt(const FlowHolder& holder, FlowCertainty incoming) const
{
    if (incoming == FlowCertainty::Predicted || !holder.object || m_holderIsImmutable || m_holderShowsConstantMembers)
        return incoming;
    if (m_holderMayHaveUnseenAccessor)
        return FlowCertainty::Predicted;

    auto hasStoresIn = [&](FlowOpenPart part) {
        return hasUnnamedKeyStores(m_inputs, holder.object, part);
    };
    bool hasStoresForNames = std::ranges::any_of(m_keys.names, [&](PropertyKeyID name) {
        std::optional<FlowOpenPart> part = unnamedKeyPartOf(classOf(m_inputs.program, name));
        return part && hasStoresIn(*part);
    });
    bool hasStoresForOtherKeys = (m_keys.anyString && hasStoresIn(FlowOpenPart::StringProperties))
        || (m_keys.anySymbol && hasStoresIn(FlowOpenPart::SymbolProperties))
        || (m_keys.mayNameIndex() && hasStoresIn(FlowOpenPart::IndexedProperties));
    return hasStoresForNames || hasStoresForOtherKeys ? FlowCertainty::Predicted : FlowCertainty::Proven;
}

void FlowPropertyWalker::reportAbsentIfChainEnds()
{
    if (!m_chainMayEnd || m_receiverShowsEveryKey)
        return;

    switch (m_walk.operation) {
    case FlowWalkOperation::Get:
    case FlowWalkOperation::HasProperty:
        m_functor(FlowPropertySource { .kind = FlowPropertySourceKind::Absent, .certainty = m_certaintyOfAbsence });
        return;
    case FlowWalkOperation::Set:
    case FlowWalkOperation::DefineOwnProperty:
    case FlowWalkOperation::CopyDataProperties:
    case FlowWalkOperation::Delete:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
