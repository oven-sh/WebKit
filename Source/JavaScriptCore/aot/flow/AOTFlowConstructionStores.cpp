/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowConstruction.h"

#if ENABLE(AOT)

#include "AOTForwardDataflow.h"
#include <algorithm>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ComposedCall {
    ThisLeak leak { ThisLeak::Unknown };
    Witness leakWitness { noWitness };
    BitVector storedBeforeLeak;
    BitVector storedAtExit;
    BitVector readBeforeStore;
    BitVector readNames;
    BitVector selfReferenceNames;
    BitVector possiblyStoredNames;
    BitVector definedNames;
    BitVector observedDefinitions;
    bool listsEveryStoredName { false };
};

struct StoredSets {
    ThisLeak leak { ThisLeak::Unknown };
    Witness leakWitness { noWitness };
    bool hasExit { false };
    BitVector beforeLeak;
    BitVector atExit;
    BitVector readEarly;
    BitVector read;
    BitVector selfReferences;
    BitVector possiblyStored;
    BitVector defined;
    BitVector observedDefinitions;
    bool listsEveryStoredName { true };
    BitVector eventsReachingSelfReference;
    BitVector readsAfterPossibleStore;
};

// A name is stored on every path to the point. It holds a self reference, is possibly stored, or holds what a field definition stored and nobody has seen, on some path to the point.
struct NamesAtPoint {
    BitVector stored;
    BitVector selfReferences;
    BitVector possiblyStored;
    BitVector unobservedDefinitions;
    // No path to the point lets code store into the object that the events do not list.
    bool listsEveryStoredName { false };
    bool hasLeaked { false };
};

bool overlaps(const BitVector& a, const BitVector& b)
{
    BitVector common = a;
    common.filter(b);
    return !common.isEmpty();
}

bool storesSelfReference(const FlowConstructionEvent& event, const FlowResolvedConstructionEvent& resolved)
{
    return event.storesSelfReference && !resolved.holderIsEmpty;
}

bool storesOnlyData(const FlowResolvedConstructionEvent& resolved)
{
    return resolved.doesNotLeak && resolved.runsOneCallee && resolved.callees.isEmpty();
}

class StoredNamesProblem {
    WTF_MAKE_NONCOPYABLE(StoredNamesProblem);
public:
    using State = NamesAtPoint;

    StoredNamesProblem(const FlowConstructionGraph& graph, const FlowConstructionNode& node, const FlowConstructionNames& universe)
        : m_node(node)
        , m_summary(*node.summary)
        , m_function(node.function->analyses.function())
        , m_universe(universe)
    {
        RELEASE_ASSERT(node.resolvedEvents.size() == m_summary.events.size());
        m_bitOfName = m_summary.names.map([&](const FlowConstructionName& name) {
            return bitOf(name);
        });
        computeComposedCalls(graph);
        findFieldDefinitions();

        size_t numberOfBits = m_universe.size();
        BitVector allBits(numberOfBits);
        for (size_t bit = 0; bit < numberOfBits; ++bit)
            allBits.set(bit);

        m_sets.leak = ThisLeak::DoesNotLeak;
        m_sets.beforeLeak = allBits;
        m_sets.atExit = WTF::move(allBits);
        m_sets.readEarly = BitVector(numberOfBits);
        m_sets.read = BitVector(numberOfBits);
        m_sets.selfReferences = BitVector(numberOfBits);
        m_sets.possiblyStored = BitVector(numberOfBits);
        m_sets.defined = BitVector(numberOfBits);
        m_sets.observedDefinitions = BitVector(numberOfBits);
    }

    // The caller of a function that takes the object as a parameter may have stored anything.
    State stateAtEntry() const
    {
        size_t numberOfBits = m_universe.size();
        return { BitVector(numberOfBits), BitVector(numberOfBits), BitVector(numberOfBits), BitVector(numberOfBits), m_node.root.kind != FlowConstructionRootKind::EntryParameter, false };
    }

    bool merge(State& into, const State& from) const
    {
        size_t storedCount = into.stored.bitCount();
        size_t selfReferenceCount = into.selfReferences.bitCount();
        size_t possiblyStoredCount = into.possiblyStored.bitCount();
        size_t unobservedDefinitionCount = into.unobservedDefinitions.bitCount();
        bool listedEveryStoredName = into.listsEveryStoredName;
        bool hadLeaked = into.hasLeaked;
        into.stored.filter(from.stored);
        into.selfReferences.merge(from.selfReferences);
        into.possiblyStored.merge(from.possiblyStored);
        into.unobservedDefinitions.merge(from.unobservedDefinitions);
        into.listsEveryStoredName &= from.listsEveryStoredName;
        into.hasLeaked |= from.hasLeaked;
        return into.stored.bitCount() != storedCount || into.selfReferences.bitCount() != selfReferenceCount
            || into.possiblyStored.bitCount() != possiblyStoredCount || into.unobservedDefinitions.bitCount() != unobservedDefinitionCount
            || into.listsEveryStoredName != listedEveryStoredName || into.hasLeaked != hadLeaked;
    }

    void enterEdge(State&, Edge) const { }

    void execute(State& state, const DataflowStep& step) { applyEventsAt(state, { step.block, step.index }); }

    void executeTerminator(State& state, const DataflowTerminatorStep& step) { applyEventsAt(state, pointBeforeTerminatorOf(m_function, step.block)); }

    const StoredSets& sets() const LIFETIME_BOUND { return m_sets; }

private:
    uint32_t bitOf(FlowConstructionName name) const
    {
        auto found = std::ranges::lower_bound(m_universe, name);
        RELEASE_ASSERT(found != m_universe.end() && *found == name);
        return static_cast<uint32_t>(found - m_universe.begin());
    }

    uint32_t bitOf(const FlowConstructionEvent& event) const { return m_bitOfName[*event.name]; }

    BitVector bitsOf(const FlowConstructionNames& names) const
    {
        BitVector bits(m_universe.size());
        for (FlowConstructionName name : names)
            bits.set(bitOf(name));
        return bits;
    }

    void computeComposedCalls(const FlowConstructionGraph& graph)
    {
        m_composedCalls = m_node.callees.map([&](uint32_t callee) {
            const FlowSolvedConstruction& solved = graph.nodes[callee].solved;
            return ComposedCall { solved.leak, solved.leakWitness, bitsOf(solved.storedBeforeLeak), bitsOf(solved.storedAtExit), bitsOf(solved.readBeforeStore), bitsOf(solved.readNames), bitsOf(solved.selfReferenceNames), bitsOf(solved.possiblyStoredNames), bitsOf(solved.definedNames), bitsOf(solved.observedDefinitions), solved.listsEveryStoredName };
        });
    }

    void findFieldDefinitions()
    {
        if (m_node.function->fieldDefinitions.isEmpty() || m_node.root.kind != FlowConstructionRootKind::EntryParameter)
            return;
        for (uint32_t position = 0; position < m_summary.events.size(); ++position) {
            const FlowConstructionEvent& event = m_summary.events[position];
            if (event.kind == FlowConstructionEventKind::Store && event.isExact && m_node.function->findFieldDefinition(event.origin))
                m_isFieldDefinition.set(position);
        }
    }

    const ComposedCall& composedCallOf(uint32_t callee) const LIFETIME_BOUND
    {
        auto found = std::ranges::lower_bound(m_node.callees, callee);
        RELEASE_ASSERT(found != m_node.callees.end() && *found == callee);
        return m_composedCalls[found - m_node.callees.begin()];
    }

    void applyLeak(State& state, const BitVector& storedAtLeak, const BitVector& unobservedAtLeak, const Witness& witness)
    {
        m_sets.beforeLeak.filter(storedAtLeak);
        m_sets.observedDefinitions.merge(unobservedAtLeak);
        m_sets.leak = ThisLeak::Unknown;
        keepLowerWitness(m_sets.leakWitness, witness);
        state.hasLeaked = true;
        applyUnlistedStores(state);
    }

    void applyLeak(State& state, const State& stateBeforeOperation, const Witness& witness)
    {
        applyLeak(state, stateBeforeOperation.stored, stateBeforeOperation.unobservedDefinitions, witness);
    }

    // Any code may run between a definition on an object that has leaked and the next store.
    void applyDefinitions(State& state, const BitVector& defined)
    {
        m_sets.defined.merge(defined);
        if (state.hasLeaked)
            m_sets.observedDefinitions.merge(defined);
        else
            state.unobservedDefinitions.merge(defined);
    }

    void applyUnlistedStores(State& state)
    {
        state.listsEveryStoredName = false;
        m_sets.listsEveryStoredName = false;
    }

    static void addPossibleStores(State& state, const State& stateAfterCallee)
    {
        state.selfReferences.merge(stateAfterCallee.selfReferences);
        state.possiblyStored.merge(stateAfterCallee.possiblyStored);
        state.unobservedDefinitions.merge(stateAfterCallee.unobservedDefinitions);
        state.listsEveryStoredName &= stateAfterCallee.listsEveryStoredName;
        state.hasLeaked |= stateAfterCallee.hasLeaked;
    }

    void applyCompose(const ComposedCall& call, State& state, const Witness& witness)
    {
        BitVector readEarly = call.readBeforeStore;
        readEarly.exclude(state.stored);
        m_sets.readEarly.merge(readEarly);
        m_sets.read.merge(call.readNames);
        m_sets.selfReferences.merge(call.selfReferenceNames);
        m_sets.possiblyStored.merge(call.possiblyStoredNames);
        BitVector observedByReads = state.unobservedDefinitions;
        observedByReads.filter(call.readBeforeStore);
        m_sets.observedDefinitions.merge(observedByReads);
        m_sets.observedDefinitions.merge(call.observedDefinitions);
        if (call.leak == ThisLeak::Unknown) {
            BitVector storedAtLeak = state.stored;
            storedAtLeak.merge(call.storedBeforeLeak);
            BitVector unobservedAtLeak = state.unobservedDefinitions;
            unobservedAtLeak.exclude(call.storedBeforeLeak);
            applyLeak(state, storedAtLeak, unobservedAtLeak, hasFunction(call.leakWitness) ? call.leakWitness : witness);
        }
        if (!call.listsEveryStoredName)
            applyUnlistedStores(state);
        state.stored.merge(call.storedAtExit);
        state.selfReferences.merge(call.selfReferenceNames);
        state.possiblyStored.merge(call.possiblyStoredNames);
        state.unobservedDefinitions.exclude(call.storedAtExit);
        applyDefinitions(state, call.definedNames);
    }

    void applyCallees(State& state, const State& stateBeforeOperation, const FlowResolvedConstructionEvent& resolved)
    {
        if (!resolved.doesNotLeak)
            applyLeak(state, stateBeforeOperation, resolved.witness);
        if (resolved.callees.isEmpty())
            return;

        State stateAfterCall = stateBeforeOperation;
        applyCompose(composedCallOf(resolved.callees[0]), stateAfterCall, resolved.witness);
        for (uint32_t callee : resolved.callees.span().subspan(1)) {
            State stateAfterCallee = stateBeforeOperation;
            applyCompose(composedCallOf(callee), stateAfterCallee, resolved.witness);
            merge(stateAfterCall, stateAfterCallee);
        }
        state.stored.merge(stateAfterCall.stored);
        BitVector replacedDefinitions = stateBeforeOperation.unobservedDefinitions;
        replacedDefinitions.exclude(stateAfterCall.unobservedDefinitions);
        state.unobservedDefinitions.exclude(replacedDefinitions);
        addPossibleStores(state, stateAfterCall);
    }

    // An access links the accessors of every receiver, an operand that is not exact may be another object, and other code may run in place of a callee: such a callee's stores prove nothing.
    void applyPossibleCallees(State& state, const State& stateBeforeOperation, const FlowResolvedConstructionEvent& resolved)
    {
        if (!resolved.doesNotLeak)
            applyLeak(state, stateBeforeOperation, resolved.witness);
        for (uint32_t callee : resolved.callees) {
            State stateAfterCallee = stateBeforeOperation;
            applyCompose(composedCallOf(callee), stateAfterCallee, resolved.witness);
            addPossibleStores(state, stateAfterCallee);
        }
    }

    // Whoever reads a name that holds a self reference, and an accessor that takes one as the stored value, has the object.
    bool reachesSelfReference(const State& stateBeforeOperation, const FlowConstructionEvent& event, const FlowResolvedConstructionEvent& resolved) const
    {
        bool reaches = false;
        switch (event.kind) {
        case FlowConstructionEventKind::Store:
            reaches = storesSelfReference(event, resolved) && !storesOnlyData(resolved);
            break;
        case FlowConstructionEventKind::Read:
            reaches = stateBeforeOperation.selfReferences.get(bitOf(event));
            break;
        case FlowConstructionEventKind::Leak:
        case FlowConstructionEventKind::Call:
        case FlowConstructionEventKind::Exit:
            break;
        }
        return reaches || std::ranges::any_of(resolved.callees, [&](uint32_t callee) {
            return overlaps(composedCallOf(callee).readNames, stateBeforeOperation.selfReferences);
        });
    }

    void applyStore(State& state, uint32_t position)
    {
        const FlowConstructionEvent& event = m_summary.events[position];
        const FlowResolvedConstructionEvent& resolved = m_node.resolvedEvents[position];
        uint32_t bit = bitOf(event);
        state.possiblyStored.set(bit);
        m_sets.possiblyStored.set(bit);
        if (!storesOnlyData(resolved)) {
            if (m_isFieldDefinition.get(position))
                m_sets.observedDefinitions.set(bit);
            return;
        }
        if (event.isExact) {
            state.stored.set(bit);
            state.unobservedDefinitions.clear(bit);
        }
        if (m_isFieldDefinition.get(position)) {
            BitVector defined(m_universe.size());
            defined.set(bit);
            applyDefinitions(state, defined);
        }
        if (storesSelfReference(event, resolved)) {
            state.selfReferences.set(bit);
            m_sets.selfReferences.set(bit);
        }
    }

    // A test for the name, and a store to a private name, see that the field exists and not what it holds.
    bool readsValue(const FlowConstructionEvent& event) const
    {
        return accessOf(m_function.at(event.origin).opcode) == Access::Read;
    }

    // A read that no store under the name can have reached finds no own property, so it observes nothing about the field.
    void applyRead(const State& stateBeforeOperation, uint32_t position)
    {
        uint32_t bit = bitOf(m_summary.events[position]);
        m_sets.read.set(bit);
        if (stateBeforeOperation.unobservedDefinitions.get(bit) && readsValue(m_summary.events[position]))
            m_sets.observedDefinitions.set(bit);
        if (stateBeforeOperation.listsEveryStoredName && !stateBeforeOperation.possiblyStored.get(bit) && m_node.resolvedEvents[position].startsAtPrototype)
            return;
        m_sets.readsAfterPossibleStore.set(position);
        if (!stateBeforeOperation.stored.get(bit))
            m_sets.readEarly.set(bit);
    }

    void applyEvent(State& state, const State& stateBeforeOperation, uint32_t position)
    {
        const FlowConstructionEvent& event = m_summary.events[position];
        const FlowResolvedConstructionEvent& resolved = m_node.resolvedEvents[position];
        if (reachesSelfReference(stateBeforeOperation, event, resolved)) {
            applyLeak(state, stateBeforeOperation, resolved.witness);
            m_sets.eventsReachingSelfReference.set(position);
        }

        switch (event.kind) {
        case FlowConstructionEventKind::Leak:
            if (!resolved.doesNotLeak)
                applyLeak(state, stateBeforeOperation, resolved.witness);
            return;
        case FlowConstructionEventKind::Store:
            applyPossibleCallees(state, stateBeforeOperation, resolved);
            applyStore(state, position);
            break;
        case FlowConstructionEventKind::Read:
            applyPossibleCallees(state, stateBeforeOperation, resolved);
            applyRead(stateBeforeOperation, position);
            break;
        case FlowConstructionEventKind::Call:
            if (event.isExact && resolved.runsOneCallee)
                applyCallees(state, stateBeforeOperation, resolved);
            else
                applyPossibleCallees(state, stateBeforeOperation, resolved);
            break;
        case FlowConstructionEventKind::Exit:
            m_sets.beforeLeak.filter(state.stored);
            m_sets.atExit.filter(state.stored);
            m_sets.hasExit = true;
            if (m_node.root.kind != FlowConstructionRootKind::EntryParameter)
                m_sets.observedDefinitions.merge(state.unobservedDefinitions);
            return;
        }
        if (!resolved.storesOnlyThroughCallees)
            applyUnlistedStores(state);
    }

    // What an operation stores through one operand is not stored yet for what it reads or leaks through another.
    void applyEventsAt(State& state, ProgramPoint point)
    {
        std::span<const FlowConstructionEvent> events = m_summary.eventsAt(point);
        if (events.empty())
            return;
        uint32_t first = m_summary.indexOfFirstEventAt(point);
        State stateBeforeOperation = state;
        for (uint32_t offset = 0; offset < events.size(); ++offset)
            applyEvent(state, stateBeforeOperation, first + offset);
    }

    const FlowConstructionNode& m_node;
    const FlowConstructionSummary& m_summary;
    const Function& m_function;
    const FlowConstructionNames& m_universe;
    Vector<uint32_t> m_bitOfName;
    Vector<ComposedCall> m_composedCalls;
    BitVector m_isFieldDefinition;
    StoredSets m_sets;
};

} // namespace

static FlowConstructionNames collectUniverse(const FlowConstructionGraph& graph, const FlowConstructionNode& node)
{
    Vector<FlowConstructionName> names = node.summary->names;
    for (uint32_t callee : node.callees) {
        const FlowSolvedConstruction& solved = graph.nodes[callee].solved;
        names.append(solved.storedBeforeLeak.span());
        names.append(solved.storedAtExit.span());
        names.append(solved.readBeforeStore.span());
        names.append(solved.readNames.span());
        names.append(solved.selfReferenceNames.span());
        names.append(solved.possiblyStoredNames.span());
        names.append(solved.definedNames.span());
        names.append(solved.observedDefinitions.span());
    }
    return FlowConstructionNames(names.span());
}

static FlowConstructionNames namesOf(const FlowConstructionNames& universe, const BitVector& bits)
{
    FlowConstructionNames names;
    for (size_t bit : bits)
        names.add(universe[bit]);
    return names;
}

static void writeStoredSets(FlowSolvedConstruction& solved, const FlowConstructionNames& universe, const StoredSets& sets)
{
    solved.leak = sets.leak;
    solved.leakWitness = sets.leakWitness;
    // A function that never returns has still read: its caller may catch what it throws and go on.
    solved.readBeforeStore = namesOf(universe, sets.readEarly);
    solved.readNames = namesOf(universe, sets.read);
    solved.selfReferenceNames = namesOf(universe, sets.selfReferences);
    solved.possiblyStoredNames = namesOf(universe, sets.possiblyStored);
    solved.definedNames = namesOf(universe, sets.defined);
    solved.observedDefinitions = namesOf(universe, sets.observedDefinitions);
    solved.listsEveryStoredName = sets.listsEveryStoredName;
    if (!sets.hasExit)
        return;

    BitVector beforeLeak = sets.beforeLeak;
    beforeLeak.exclude(sets.readEarly);
    solved.storedBeforeLeak = namesOf(universe, beforeLeak);
    solved.storedAtExit = namesOf(universe, sets.atExit);
}

void solveStoredNames(FlowConstructionGraph& graph, uint32_t index)
{
    FlowConstructionNode& node = graph.nodes[index];
    if (!node.summary) {
        Witness leakWitness = node.solved.leakWitness;
        node.solved = { };
        node.solved.leakWitness = leakWitness;
        return;
    }

    FlowConstructionNames universe = collectUniverse(graph, node);
    StoredNamesProblem problem(graph, node, universe);
    node.function->analyses.solveForward(problem);
    writeStoredSets(node.solved, universe, problem.sets());
    // What is left at the exit of a constructor has been observed there.
    if (node.root.kind != FlowConstructionRootKind::EntryParameter)
        node.solved.definedNames = { };
    for (size_t position : problem.sets().eventsReachingSelfReference)
        node.resolvedEvents[position].doesNotLeak = false;
    for (size_t position : problem.sets().readsAfterPossibleStore)
        node.resolvedEvents[position].startsAtPrototype = false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
