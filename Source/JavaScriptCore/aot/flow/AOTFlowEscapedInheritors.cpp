/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowEscapedInheritors.h"

#if ENABLE(AOT)

#include "AOTFlowCallState.h"
#include "AOTFlowPrototypes.h"
#include <algorithm>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ChainHolders {
    Vector<BuiltinID, 4> builtins;
    bool mayIncludeUnknownObject { false };
};

// Many objects have the same prototype, so the holders from a prototype up are collected once.
class PrototypeChains {
public:
    explicit PrototypeChains(const FlowWalkInputs& inputs)
        : m_inputs(inputs)
    {
    }

    // The answer is valid until the next call.
    const ChainHolders& from(const FlowHolder& first) LIFETIME_BOUND
    {
        unsigned next = m_chains.size();
        unsigned index = first.object ? m_indexByObject.add(first.object, next).iterator->value : m_indexByBuiltin.add(first.builtin, next).iterator->value;
        if (index == next)
            m_chains.append(collect(first));
        return m_chains[index];
    }

private:
    ChainHolders collect(const FlowHolder& first) const
    {
        ChainHolders chain;
        Vector<FlowHolder, 8> worklist;
        FlowObjectSet visitedObjects;
        BitVector visitedBuiltins;
        auto enqueue = [&](const FlowHolder& holder) {
            bool isNew = holder.object ? visitedObjects.add(holder.object) : visitedBuiltins.add(holder.builtin.index());
            if (!isNew)
                return;
            worklist.append(holder);
            if (holder.builtin)
                chain.builtins.append(m_inputs.builtins().canonicalID(holder.builtin));
        };

        enqueue(first);
        for (size_t index = 0; index < worklist.size(); ++index) {
            FlowPrototypeSet prototypes = prototypesOf(m_inputs, FlowHolder(worklist[index]), { });
            chain.mayIncludeUnknownObject |= !prototypes.isNeverUnknown;
            for (const FlowHolder& prototype : prototypes.holders)
                enqueue(prototype);
        }
        return chain;
    }

    const FlowWalkInputs& m_inputs;
    Vector<ChainHolders> m_chains;
    HashMap<FlowObjectID, unsigned> m_indexByObject;
    HashMap<BuiltinID, unsigned> m_indexByBuiltin;
};

} // anonymous namespace

static FlowNodeID nodeOf(FlowSolver& solver, BuiltinID prototype)
{
    return solver.temporaryFor(callTemporaryKey(nativeCodeSite(solver), FlowCallTemporaryRole::EscapedInheritors, prototype.index()));
}

FlowNodeID escapedInheritorsOf(FlowSolver& solver, BuiltinID prototype)
{
    prototype = solver.builtins().canonicalID(prototype);
    FlowEscapedInheritorState& state = solver.callState().escapedInheritors;
    if (!state.isRequested.set(prototype.index()))
        state.prototypes.append(prototype);
    return nodeOf(solver, prototype);
}

FlowProgress refineEscapedInheritors(FlowSolver& solver)
{
    Vector<BuiltinID> requested = solver.callState().escapedInheritors.prototypes;
    if (requested.isEmpty() || solver.records().mayRunUnknownJavaScript())
        return FlowProgress::Finished;

    // The links are read as they are now. A link that gets another prototype queues work, and this runs again once the queues are empty.
    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    inputs.solver = nullptr;

    PrototypeChains chains(inputs);

    FlowObjectSet escapedObjects = solver.valueOf(solver.graph().nativeCode()).objects;
    escapedObjects.forEach([&](FlowObjectID object) {
        FlowHolder holder = holderOf(inputs.objects, object);
        if (object == unknownFlowObject || mutabilityOf(inputs, holder) == FlowHolderMutability::Immutable)
            return;

        FlowPrototypeSet prototypes = prototypesOf(inputs, holder, { });
        for (BuiltinID requestedPrototype : requested) {
            bool mayInherit = !prototypes.isNeverUnknown || std::ranges::any_of(prototypes.holders, [&](const FlowHolder& prototype) {
                const ChainHolders& chain = chains.from(prototype);
                return chain.mayIncludeUnknownObject || chain.builtins.contains(requestedPrototype);
            });
            if (mayInherit)
                solver.joinObject(nodeOf(solver, requestedPrototype), object);
        }
    });
    return solver.hasQueuedWork() ? FlowProgress::QueuedWork : FlowProgress::Finished;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
