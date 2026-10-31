/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEscapeAnalysis.h"
#include "AOTForwardDataflow.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTIRBuilder.h"
#include "AOTPlacement.h"
#include "AOTRepresentationRules.h"
#include "AOTSlotEmitter.h"
#include <algorithm>
#include <functional>
#include "AOTBitVector.h"
#include <wtf/IndexMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class BarrierDecision : uint8_t { Emit, OwnerCannotBeMarked, LaterBarrierCovers };

enum class OwnerKind : uint8_t { Unknown, FrameCell, HeapAllocation };

struct BarrierStore {
    BlockID block;
    uint32_t index { 0 };
    ValueID value;
    ValueID owner;
    uint32_t ownerNumber { 0 };
    bool alwaysRunsBarrier { false };
    bool canBeCovered { false };
    // RememberOwner stores nothing: it only runs the barrier.
    bool isStore { true };
    BarrierDecision decision { BarrierDecision::Emit };
};

struct BarrierBehindArms {
    BlockID join;
    ValueID owner;
};

struct BarrierOwner {
    ValueID value;
    OwnerKind kind { OwnerKind::Unknown };
};

struct StoreRange {
    uint32_t begin { 0 };
    uint32_t end { 0 };
};

using OwnerNumbers = IndexMap<ValueID, std::optional<uint32_t>>;

static_assert((storesThatMayAdd | Value::opcodesWithFieldAccess | Value::opcodesWithPlacedKeys) == (Value::opcodesWithFieldAccess | Value::opcodesWithPlacedKeys));

bool reachesSafepoint(const EffectsOnEdges& effects)
{
    return effects.normal.mayCollect || effects.normal.mayRunOtherCode() || effects.handler.mayCollect || effects.handler.mayRunOtherCode();
}

bool endsCoverage(const EffectsOnEdges& effects)
{
    return reachesSafepoint(effects) || effects.normal.mayExitSideways() || effects.handler.mayExitSideways();
}

bool endsCoverage(TerminatorKind kind)
{
    switch (kind) {
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
        return true;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct YoungOwnersProblem {
    using State = BitVector;

    const OwnerNumbers& ownerNumbers;
    std::span<const BarrierOwner> owners;

    State stateAtEntry() const { return BitVector(owners.size()); }

    // Effects can only empty the set, and the edge to a handler empties it whatever it holds.
    bool ignoresEffects(const State& young) const { return young.isEmpty(); }

    void execute(State& young, const DataflowStep& step) const
    {
        if (reachesSafepoint(step.effects))
            young.clearAll();
        if (std::optional<uint32_t> number = ownerNumbers[step.id]; number && owners[*number].kind == OwnerKind::HeapAllocation)
            young.set(*number);
    }

    void executeTerminator(State& young, const DataflowTerminatorStep& step) const
    {
        if (reachesSafepoint(step.effects))
            young.clearAll();
    }

    void enterEdge(State& young, Edge edge) const
    {
        bool allocatesThrownError = edge.isHandlerEdge();
        if (allocatesThrownError)
            young.clearAll();
    }

    bool merge(State& into, const State& from) const
    {
        size_t previousCount = into.bitCount();
        into.filter(from);
        return into.bitCount() != previousCount;
    }
};

class RedundantBarrierAnalysis {
public:
    explicit RedundantBarrierAnalysis(FunctionPassContext& pass)
        : m_pass(pass)
        , m_function(pass.function)
        , m_analyses(pass.analyses)
        , m_placement(*pass.inputs.placement)
        , m_order(pass.analyses.cfg().reversePostOrder())
    {
        RELEASE_ASSERT(pass.function.representationsAreChosen);
    }

    PassOutcome run()
    {
        decide();
        if (placeBarriersBehindArms())
            decide();
        for (const BarrierStore& store : m_stores) {
            if (store.isStore)
                setFlags(store.value, store.decision);
        }

        auto count = [&](StatisticID statistic, BarrierDecision decision) {
            m_pass.build.statistics().count(statistic, static_cast<uint64_t>(std::ranges::count(m_stores, decision, &BarrierStore::decision)));
        };
        count(StatisticID::BarriersOnUnmarkableOwners, BarrierDecision::OwnerCannotBeMarked);
        count(StatisticID::BarriersCoveredLater, BarrierDecision::LaterBarrierCovers);
        return m_outcome;
    }

private:
    void decide()
    {
        m_stores.clear();
        m_owners.clear();
        m_retentionHistory.reset();
        if (collectStores()) {
            findUnmarkableOwners();
            findCoveredStores();
        }
    }

    bool collectStores()
    {
        for (BlockID block : m_order) {
            const BlockValues& values = m_function.at(block).values;
            for (uint32_t index = 0; index < values.size(); ++index)
                collectStore(block, index, values[index]);
        }
        if (m_stores.isEmpty() || m_pass.exceeds(LimitID::MaxBarrierBlocks, m_function.blocks.size()))
            return false;

        m_ownerNumbers = OwnerNumbers(m_function.values.size());
        m_storeRanges = IndexMap<BlockID, StoreRange>(m_function.blocks.size());
        for (uint32_t storeIndex = 0; storeIndex < m_stores.size(); ++storeIndex) {
            BarrierStore& store = m_stores[storeIndex];
            std::optional<uint32_t>& number = m_ownerNumbers[store.owner];
            if (!number) {
                number = static_cast<uint32_t>(m_owners.size());
                m_owners.append(BarrierOwner { .value = store.owner });
            }
            store.ownerNumber = *number;

            StoreRange& range = m_storeRanges[store.block];
            if (range.begin == range.end)
                range.begin = storeIndex;
            range.end = storeIndex + 1;
        }
        return true;
    }

    // Where the code generator converts the operands, the operation names the value as it was defined.
    Representation formTakenBy(const Value& user, uint32_t operandIndex) const
    {
        RELEASE_ASSERT(m_pass.inputs.callingConventions);
        RepresentationInputs inputs { m_pass.program, m_pass.id, m_function, m_placement, *m_pass.inputs.callingConventions };
        Representation wanted = wantedRepresentationOf(inputs, user, operandIndex);
        return hasRegister(wanted) ? wanted : m_function.at(user.operand(operandIndex)).representation;
    }

    void collectStore(BlockID block, uint32_t index, ValueID id)
    {
        const Value& value = m_function.at(id);
        if (value.opcode == Opcode::RememberOwner) {
            m_stores.append(BarrierStore { .block = block, .index = index, .value = id, .owner = m_analyses.identityOf(value.objectOperand()), .alwaysRunsBarrier = true, .canBeCovered = true, .isStore = false });
            return;
        }
        if (!runsOwnerBarrierOf(value.opcode))
            return;

        std::optional<uint8_t> retainingOperand;
        bool hasOneOwner = true;
        bool storesCells = false;
        bool storesLiteralStringWord = false;
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (retainedInOf(value.opcode, operandIndex) != RetainedIn::Operand)
                return;
            std::optional<uint8_t> ownerIndex = retainingOperandOf(value.opcode, operandIndex);
            hasOneOwner &= !retainingOperand || retainingOperand == ownerIndex;
            retainingOperand = ownerIndex;
            storesCells |= m_pass.program.typeOf(m_function, operand).hasCells();
            storesLiteralStringWord |= formTakenBy(value, operandIndex) == Representation::LiteralString;
        });

        // A store that makes room for its value stores the Extension in the owner, whatever the value is.
        bool storesInOwner = storesCells || (storesThatMayAdd.contains(value.opcode) && storesInSomeSlotThat(value, &PlacedSlot::isOutOfLine));
        ValueID owner;
        if (storesInOwner && hasOneOwner && retainingOperand)
            owner = value.operand(*retainingOperand);
        if (!owner)
            return setFlags(id, BarrierDecision::Emit);
        bool alwaysRunsBarrier = storesCells && !storesLiteralStringWord && !storesInSomeSlotThat(value, [](const PlacedSlot& slot) {
            return slot.representation == SlotRepresentation::DeferredClosure;
        });
        m_stores.append(BarrierStore { .block = block, .index = index, .value = id, .owner = m_analyses.identityOf(owner), .alwaysRunsBarrier = alwaysRunsBarrier });
    }

    bool storesInSomeSlotThat(const Value& store, const auto& holds) const
    {
        auto mayHold = [&](FieldAccessID id) {
            return !id || std::invoke(holds, m_placement.fieldAccess(id).slot);
        };
        if (Value::opcodesWithFieldAccess.contains(store.opcode))
            return mayHold(store.fieldAccess());
        return Value::opcodesWithPlacedKeys.contains(store.opcode) && std::ranges::any_of(store.placedKeys(), [&](PlacedKey key) {
            return mayHold(key.fieldAccess);
        });
    }

    void setFlags(ValueID id, BarrierDecision decision)
    {
        const Value& value = m_function.at(id);
        bool ownerCannotBeMarked = decision == BarrierDecision::OwnerCannotBeMarked;
        bool laterBarrierCovers = decision == BarrierDecision::LaterBarrierCovers;
        if (value.ownerCannotBeMarked() == ownerCannotBeMarked && value.laterBarrierCovers() == laterBarrierCovers)
            return;
        Value& store = m_function.edit(id, ValueEdit::Definition);
        store.setOwnerCannotBeMarked(ownerCannotBeMarked);
        store.setLaterBarrierCovers(laterBarrierCovers);
        m_outcome = PassOutcome::Changed;
    }

    OwnerKind kindOf(ValueID owner) const
    {
        if (isFrameCell(m_function, owner))
            return OwnerKind::FrameCell;
        const Value& value = m_function.at(owner);
        if (!allocatesAfterLastSafepointOf(value.opcode))
            return OwnerKind::Unknown;
        if (value.opcode != Opcode::NewObject || value.storage() != AllocationStorage::Heap)
            return OwnerKind::HeapAllocation;
        bool allocatesUnmarkedCell = m_placement.layout(value.layout()).hasSlots() && !m_placement.freeCellOffset(value.layout()) && !value.prototypeOperand();
        return allocatesUnmarkedCell ? OwnerKind::HeapAllocation : OwnerKind::Unknown;
    }

    void findUnmarkableOwners()
    {
        bool hasHeapAllocation = false;
        for (BarrierOwner& owner : m_owners) {
            owner.kind = kindOf(owner.value);
            hasHeapAllocation |= owner.kind == OwnerKind::HeapAllocation;
        }
        for (BarrierStore& store : m_stores) {
            if (store.isStore && m_owners[store.ownerNumber].kind == OwnerKind::FrameCell)
                store.decision = BarrierDecision::OwnerCannotBeMarked;
        }
        if (!hasHeapAllocation)
            return;

        YoungOwnersProblem problem { m_ownerNumbers, m_owners.span() };
        Vector<std::optional<BitVector>> heads = m_analyses.solveForward(problem);
        for (BlockID block : m_order) {
            std::optional<BitVector>& young = heads[block.index()];
            if (!young)
                continue;
            const BlockValues& values = m_function.at(block).values;
            uint32_t index = 0;
            for (BarrierStore& store : storesIn(block)) {
                for (; index < store.index; ++index) {
                    ValueID id = values[index];
                    problem.execute(*young, DataflowStep { block, index, id, m_function.at(id), problem.ignoresEffects(*young) ? noEffectsOnEdges : m_analyses.effectsOf(id) });
                }
                if (store.isStore && isStoreToYoungOwner(store, *young))
                    store.decision = BarrierDecision::OwnerCannotBeMarked;
            }
        }
    }

    bool isStoreToYoungOwner(const BarrierStore& store, const BitVector& youngBeforeStore)
    {
        const BarrierOwner& owner = m_owners[store.ownerNumber];
        if (owner.kind != OwnerKind::HeapAllocation || !youngBeforeStore.get(store.ownerNumber))
            return false;
        if (reachesSafepoint(m_analyses.effectsOf(store.value)))
            return false;
        const EscapeAnalysis& escape = m_analyses.escape();
        std::optional<ObjectID> object = escape.objectDefinedBy(owner.value);
        if (!object || escape.at(*object).kind != ObjectKind::Allocation)
            return false;
        if (!m_retentionHistory)
            m_retentionHistory.emplace(m_analyses, UseEffectHistoryOptions { .uses = { OperandUse::Retains } });
        return m_retentionHistory->occurredBefore(*object, ProgramPoint { store.block, store.index }) == Occurrence::HasNotOccurred;
    }

    bool someOwnerHasTwoStoresThat(const auto& admits)
    {
        BitVector owners(m_owners.size());
        bool hasRepeatedOwner = false;
        for (BarrierStore& store : m_stores) {
            if (store.isStore && store.decision == BarrierDecision::Emit && remembersOwnerOf(m_function.at(store.value).opcode) && admits(store))
                hasRepeatedOwner |= owners.quickSet(store.ownerNumber);
        }
        return hasRepeatedOwner;
    }

    void findCoveredStores()
    {
        bool mayCover = someOwnerHasTwoStoresThat([](BarrierStore&) {
            return true;
        }) && someOwnerHasTwoStoresThat([&](BarrierStore& store) {
            store.canBeCovered = !reachesSafepoint(m_analyses.effectsOf(store.value));
            return store.canBeCovered;
        });
        if (!mayCover)
            return;

        BitVector allOwners(m_owners.size());
        for (size_t number = 0; number < m_owners.size(); ++number)
            allOwners.set(number);
        IndexMap<BlockID, BitVector> heads(m_function.blocks.size(), allOwners);
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t orderIndex = m_order.size(); orderIndex--;) {
                BlockID block = m_order[orderIndex];
                BitVector covered = coveredOwnersAtHead(block, heads, false);
                if (covered == heads[block])
                    continue;
                heads[block] = WTF::move(covered);
                changed = true;
            }
        }

        for (BlockID block : m_order) {
            if (!storesIn(block).empty())
                coveredOwnersAtHead(block, heads, true);
        }
    }

    BitVector coveredOwnersAtHead(BlockID id, const IndexMap<BlockID, BitVector>& heads, bool decides)
    {
        const Block& block = m_function.at(id);
        const Terminator& terminator = block.terminator;
        bool goesOn = !terminator.targets.isEmpty() && !endsCoverage(terminator.kind) && !endsCoverage(m_analyses.effectsOfTerminator(id));
        BitVector covered = goesOn ? heads[terminator.targets.first().block] : BitVector(m_owners.size());
        if (goesOn) {
            for (const Target& target : terminator.targets.span().subspan(1))
                covered.filter(heads[target.block]);
        }

        std::span<BarrierStore> stores = storesIn(id);
        for (size_t index = block.values.size(); index--;) {
            ValueID value = block.values[index];
            if (!stores.empty() && stores.back().index == index) {
                BarrierStore& store = stores.back();
                if (decides && store.isStore && store.canBeCovered && covered.get(store.ownerNumber))
                    store.decision = BarrierDecision::LaterBarrierCovers;
                if (store.canBeCovered && store.alwaysRunsBarrier)
                    covered.set(store.ownerNumber);
                stores = stores.first(stores.size() - 1);
            }
            if (!covered.isEmpty() && endsCoverage(m_analyses.effectsOf(value)))
                covered.clearAll();
            if (std::optional<uint32_t> number = m_ownerNumbers[value])
                covered.clear(*number);
        }
        RELEASE_ASSERT(stores.empty());
        for (ValueID parameter : block.parameters) {
            if (std::optional<uint32_t> number = m_ownerNumbers[parameter])
                covered.clear(*number);
        }
        return covered;
    }

    // The owners whose barrier runs in the block and may as well run behind its end: nothing behind the store ends the coverage, and the block goes on to one block.
    BitVector ownersWaitingAtEndOf(BlockID id)
    {
        BitVector waiting(m_owners.size());
        const Block& block = m_function.at(id);
        if (block.terminator.kind != TerminatorKind::Jump || endsCoverage(m_analyses.effectsOfTerminator(id)))
            return waiting;
        std::span<BarrierStore> stores = storesIn(id);
        for (size_t index = block.values.size(); index-- && !stores.empty();) {
            if (stores.back().index == index) {
                const BarrierStore& store = stores.back();
                if (store.isStore && store.canBeCovered && store.decision == BarrierDecision::Emit)
                    waiting.set(store.ownerNumber);
                stores = stores.first(stores.size() - 1);
            }
            if (endsCoverage(m_analyses.effectsOf(block.values[index])))
                break;
        }
        return waiting;
    }

    // A name of the owner that holds a pointer to an object wherever the block begins.
    ValueID nameOfOwnerAtHeadOf(BlockID join, uint32_t ownerNumber)
    {
        if (m_namesOfOwners.isEmpty()) {
            m_namesOfOwners.grow(m_owners.size());
            auto note = [&](ValueID id, BlockID block, bool isParameter) {
                if (m_function.at(id).representation != Representation::Cell || !m_pass.program.typeOf(m_function, id).isObject())
                    return;
                if (std::optional<uint32_t> number = m_ownerNumbers[m_analyses.identityOf(id)])
                    m_namesOfOwners[*number].append({ id, block, isParameter });
            };
            for (BlockID block : m_order) {
                for (ValueID parameter : m_function.at(block).parameters)
                    note(parameter, block, true);
                for (ValueID value : m_function.at(block).values)
                    note(value, block, false);
            }
        }
        for (const NameOfOwner& name : m_namesOfOwners[ownerNumber]) {
            if (name.block == join ? name.isParameter : m_analyses.dominators().dominates(name.block, join))
                return name.value;
        }
        return { };
    }

    // One store is often made in several arms, one for each place that the slot has in the layouts of the object, and the arms meet again behind it.
    // One barrier where they meet does for all of them. A block that is seldom run may come in beside them: there the barrier runs for nothing.
    bool placeBarriersBehindArms()
    {
        bool hasStoreThatCanWait = std::ranges::any_of(m_stores, [](const BarrierStore& store) {
            return store.isStore && store.canBeCovered && store.decision == BarrierDecision::Emit;
        });
        if (!hasStoreThatCanWait)
            return false;

        Vector<BarrierBehindArms> barriers;
        const CFG& cfg = m_analyses.cfg();
        for (BlockID join : m_order) {
            std::span<const BlockID> predecessors = cfg.predecessors(join);
            if (predecessors.size() < 2 || m_function.at(join).isHandlerBlock())
                continue;
            bool someArmStores = std::ranges::any_of(predecessors, [&](BlockID predecessor) {
                return !storesIn(predecessor).empty();
            });
            if (!someArmStores)
                continue;
            BitVector inOneArm(m_owners.size());
            BitVector inTwoArms(m_owners.size());
            std::optional<BitVector> inEveryOrdinaryArm;
            for (BlockID predecessor : predecessors) {
                BitVector waiting = ownersWaitingAtEndOf(predecessor);
                BitVector again = waiting;
                again.filter(inOneArm);
                inTwoArms.merge(again);
                inOneArm.merge(waiting);
                if (m_function.at(predecessor).frequency.predicted() != BlockFrequency::Normal)
                    continue;
                if (inEveryOrdinaryArm)
                    inEveryOrdinaryArm->filter(waiting);
                else
                    inEveryOrdinaryArm = WTF::move(waiting);
            }
            if (inEveryOrdinaryArm)
                inTwoArms.filter(*inEveryOrdinaryArm);
            for (size_t number : inTwoArms) {
                if (ValueID owner = nameOfOwnerAtHeadOf(join, static_cast<uint32_t>(number)))
                    barriers.append({ join, owner });
            }
        }
        if (barriers.isEmpty())
            return false;

        IRBuilder builder(m_analyses.context(), m_function, m_pass.types);
        for (const BarrierBehindArms& barrier : barriers) {
            ValueID made = builder.insertBefore(ProgramPoint::atHeadOf(barrier.join), IRBuilder::make(Opcode::RememberOwner, { barrier.owner }));
            m_function.edit(made, ValueEdit::Type).representation = Representation::Void;
        }
        builder.applyInsertions();
        m_analyses.didChangeValues();
        m_order = m_analyses.cfg().reversePostOrder();
        m_namesOfOwners.clear();
        m_outcome = PassOutcome::Changed;
        return true;
    }

    std::span<BarrierStore> storesIn(BlockID block)
    {
        StoreRange range = m_storeRanges[block];
        return m_stores.mutableSpan().subspan(range.begin, range.end - range.begin);
    }

    FunctionPassContext& m_pass;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    const Placement& m_placement;
    std::span<const BlockID> m_order;
    Vector<BarrierStore> m_stores;
    Vector<BarrierOwner> m_owners;
    IndexMap<BlockID, StoreRange> m_storeRanges;
    OwnerNumbers m_ownerNumbers;
    struct NameOfOwner {
        ValueID value;
        BlockID block;
        bool isParameter { false };
    };
    Vector<Vector<NameOfOwner, 2>> m_namesOfOwners;
    std::optional<UseEffectHistory> m_retentionHistory;
    PassOutcome m_outcome { PassOutcome::Unchanged };
};

} // anonymous namespace

PassOutcome removeRedundantBarriers(FunctionPassContext& pass)
{
    bool hasStoreOperation = std::ranges::any_of(pass.function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return runsOwnerBarrierOf(pass.function.at(id).opcode);
        });
    });
    if (!hasStoreOperation)
        return PassOutcome::Unchanged;
    RELEASE_ASSERT(pass.inputs.placement);
    return RedundantBarrierAnalysis(pass).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
