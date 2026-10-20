/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static bool writesOnlyObjects(const Effects& effects)
{
    return !effects.mayExitSideways() && effects.writes.containsOnly(perObjectHeaps | bookkeepingHeaps);
}

namespace {

class DeadCodeEliminator {
public:
    explicit DeadCodeEliminator(FunctionPassContext& context)
        : m_context(context)
        , m_function(context.function)
        , m_analyses(context.analyses)
        , m_cfg(context.analyses.cfg())
        , m_live(context.function.values.size())
        , m_isReceiverOfWaitingStore(context.function.values.size())
    {
    }

    // A store into an object that does not escape is live once a live value may be the object. Its receiver is such a value, so most stores are live
    // before anybody has to know which object they write. Only those whose receiver stays dead wait for the escape analysis.
    PassOutcome run()
    {
        markRoots();
        std::ranges::sort(m_waitingStores, { }, &WaitingStore::receiver);
        propagate();
        if (findObjectsOfWaitingStores())
            propagate();
        return sweep();
    }

private:
    void mark(ValueID id)
    {
        if (!m_live.quickSet(id.index()))
            m_worklist.append(id);
    }

    void markRoots()
    {
        for (ValueID parameter : m_function.at(m_cfg.root()).parameters)
            m_live.quickSet(parameter.index());

        for (BlockID id : m_cfg.reversePostOrder()) {
            const Block& block = m_function.at(id);
            block.terminator.Instruction::forEachChild([&](ValueID operand) {
                mark(operand);
            });
            if (block.subject)
                mark(block.subject);
            for (ValueID value : block.values)
                markIfRoot(value);
        }
    }

    void markIfRoot(ValueID id)
    {
        const Value& value = m_function.at(id);
        if (!m_hasFreshAllocation)
            m_hasFreshAllocation = freshnessOf(m_analyses.effectContext(), m_function, value) == ResultFreshness::Fresh;

        Effects effects = m_analyses.effectsOf(id).onAnyEdge();
        if (effects.isRemovableWhenUnused())
            return;
        ValueID receiver = findOnlyWrittenObject(value, effects);
        if (!receiver || m_live.quickGet(receiver.index())) {
            mark(id);
            return;
        }
        m_waitingStores.append({ receiver, id });
        m_isReceiverOfWaitingStore.quickSet(receiver.index());
    }

    ValueID findOnlyWrittenObject(const Value& store, const Effects& effects)
    {
        if (!m_hasFreshAllocation || confinementOf(store.opcode) != OperandConfinement::WritesOnlyNamedOperands || !writesOnlyObjects(effects))
            return ValueID();
        std::optional<uint32_t> writtenOperand = findOnlyWrittenOperand(store);
        return writtenOperand ? store.operand(*writtenOperand) : ValueID();
    }

    static std::optional<ObjectID> findStoreTarget(const EscapeAnalysis& escape, ValueID receiver)
    {
        if (escape.gaveUp())
            return std::nullopt;
        std::optional<ObjectID> object = escape.singleObject(receiver);
        if (!object)
            return std::nullopt;
        const TrackedObject& tracked = escape.at(*object);
        if (tracked.kind != ObjectKind::Allocation || tracked.state != EscapeState::DoesNotEscape)
            return std::nullopt;
        return object;
    }

    std::optional<uint32_t> findOnlyWrittenOperand(const Value& store) const
    {
        std::optional<uint32_t> writtenOperand;
        bool mayWriteOtherObjects = false;
        store.forEachOperand([&](ValueID, uint32_t operandIndex) {
            OperandUses uses = summarizeOperand(m_analyses.effectContext(), m_function, store, operandIndex).uses;
            uint32_t declaredIndex = std::min(operandIndex, store.numberOfNamedOperands());
            if (!isWrittenOperand(store.opcode, declaredIndex) && !uses.containsAny(operandWritingUses))
                return;
            mayWriteOtherObjects |= writtenOperand.has_value() || uses.containsAny(unconfinedUses);
            writtenOperand = operandIndex;
        });
        if (mayWriteOtherObjects)
            return std::nullopt;
        return writtenOperand;
    }

    void deferStore(ObjectID object, ValueID store)
    {
        if (m_storesInto.isEmpty()) {
            unsigned numberOfObjects = m_analyses.escape().numberOfObjects();
            m_storesInto.grow(numberOfObjects);
            m_isActive.ensureSize(numberOfObjects);
        }
        m_storesInto[object.index()].append(store);
    }

    void propagate()
    {
        const EscapeAnalysis* escape = m_storesInto.isEmpty() ? nullptr : &m_analyses.escape();
        while (!m_worklist.isEmpty()) {
            ValueID id = m_worklist.takeLast();
            markInputsOf(id);
            if (m_isReceiverOfWaitingStore.quickGet(id.index())) {
                for (const WaitingStore& waiting : std::ranges::equal_range(m_waitingStores, id, { }, &WaitingStore::receiver))
                    mark(waiting.store);
            }
            if (escape)
                activateObjects(escape->pointsTo(id));
        }
    }

    bool findObjectsOfWaitingStores()
    {
        m_waitingStores.removeAllMatching([&](const WaitingStore& waiting) {
            return m_live.quickGet(waiting.store.index());
        });
        if (m_waitingStores.isEmpty())
            return false;

        const EscapeAnalysis& escape = m_analyses.escape();
        for (const WaitingStore& waiting : m_waitingStores) {
            if (std::optional<ObjectID> object = findStoreTarget(escape, waiting.receiver))
                deferStore(*object, waiting.store);
            else
                mark(waiting.store);
        }
        if (!m_storesInto.isEmpty()) {
            for (size_t index : m_live)
                activateObjects(escape.pointsTo(ValueID(index)));
        }
        return true;
    }

    void markInputsOf(ValueID id)
    {
        const Value& value = m_function.at(id);
        if (value.opcode == Opcode::Parameter) {
            markArgumentsOf(m_analyses.useLists().locationOf(id));
            return;
        }
        value.forEachOperand([&](ValueID operand, uint32_t) {
            mark(operand);
        });
    }

    void markArgumentsOf(ValueLocation parameter)
    {
        if (parameter.placement != ValuePlacement::Parameter)
            return;
        for (Edge edge : m_cfg.incomingEdges(parameter.block)) {
            IncomingValue incoming = incomingValueOf(m_function, edge, parameter.index);
            switch (incoming.kind) {
            case IncomingValue::Kind::Argument:
                mark(incoming.argument);
                break;
            case IncomingValue::Kind::EdgeResult:
                break;
            }
        }
    }

    void activateObjects(PointsToSet pointsTo)
    {
        for (ObjectID object : pointsTo.objects) {
            if (m_isActive.quickSet(object.index()))
                continue;
            for (ValueID store : m_storesInto[object.index()])
                mark(store);
        }
    }

    template<typename List>
    BitVector findDeadEntries(const List& list) const
    {
        BitVector deadEntries;
        for (size_t index = 0; index < list.size(); ++index) {
            if (!m_live.quickGet(list[index].index()))
                deadEntries.ensureSizeAndSet(index, list.size());
        }
        return deadEntries;
    }

    PassOutcome sweep()
    {
        IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
        PassOutcome outcome = PassOutcome::Unchanged;
        for (BlockID id : m_cfg.reversePostOrder()) {
            const Block& block = m_function.at(id);
            BitVector deadValues = findDeadEntries(block.values);
            if (!deadValues.isEmpty()) {
                builder.eraseValues(id, deadValues);
                outcome = PassOutcome::Changed;
            }
            BitVector deadParameters = findDeadEntries(block.parameters);
            if (!deadParameters.isEmpty()) {
                builder.removeParameters(id, deadParameters);
                outcome = PassOutcome::Changed;
            }
        }
        return outcome;
    }

    FunctionPassContext& m_context;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    const CFG& m_cfg;
    struct WaitingStore {
        ValueID receiver;
        ValueID store;
    };

    BitVector m_live;
    Vector<ValueID, 64> m_worklist;
    Vector<WaitingStore, 16> m_waitingStores;
    BitVector m_isReceiverOfWaitingStore;
    Vector<Vector<ValueID, 2>> m_storesInto;
    BitVector m_isActive;
    bool m_hasFreshAllocation { false };
};

} // anonymous namespace

PassOutcome eliminateDeadCode(FunctionPassContext& context)
{
    return DeadCodeEliminator(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
