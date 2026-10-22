/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRemoveRedundantBarriers.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEscapeAnalysis.h"
#include "AOTForwardDataflow.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTPass.h"
#include "AOTPlacement.h"
#include "AOTRepresentationRules.h"
#include "AOTSlotEmitter.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
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
    bool hasOwnerBarrier { false };
    BarrierDecision decision { BarrierDecision::Emit };
};

struct BarrierOwner {
    ValueID value;
    OwnerKind kind { OwnerKind::Unknown };
};

struct StoreRange {
    uint32_t begin { 0 };
    uint32_t end { 0 };
};

using OwnerSets = IndexMap<BlockID, BitVector>;
using OwnerNumbers = IndexMap<ValueID, std::optional<uint32_t>>;

// It stores into a cell that is as old as the inner object.
bool takesCell(const Value& value)
{
    return value.opcode == Opcode::NewObject && value.takesCellOfInnerObject();
}

bool hasStoreOperation(const Function& function)
{
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (operationsThatMayRunBarrier.contains(value.opcode) && (runsOwnerBarrierOf(value.opcode) || takesCell(value)))
                return true;
        }
    }
    return false;
}

bool reachesSafepoint(const Effects& effects)
{
    return effects.mayCollect || effects.mayRunOtherCode();
}

bool reachesSafepoint(const EffectsOnEdges& effects)
{
    return reachesSafepoint(effects.normal) || reachesSafepoint(effects.handler);
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

class YoungOwnersProblem {
public:
    using State = BitVector;

    YoungOwnersProblem(const OwnerNumbers& ownerNumbers, std::span<const BarrierOwner> owners)
        : m_ownerNumbers(ownerNumbers)
        , m_owners(owners)
    {
    }

    State stateAtEntry() const { return BitVector(m_owners.size()); }

    // Effects can only empty the set, and the edge to a handler empties it whatever it holds.
    bool ignoresEffects(const State& young) const { return young.isEmpty(); }

    void execute(State& young, const DataflowStep& step) const
    {
        if (reachesSafepoint(step.effects))
            young.clearAll();
        if (std::optional<uint32_t> number = m_ownerNumbers[step.id]; number && m_owners[*number].kind == OwnerKind::HeapAllocation)
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

private:
    const OwnerNumbers& m_ownerNumbers;
    std::span<const BarrierOwner> m_owners;
};

class RedundantBarrierAnalysis {
public:
    explicit RedundantBarrierAnalysis(FunctionPassContext& pass)
        : m_pass(pass)
        , m_program(pass.program)
        , m_function(pass.function)
        , m_analyses(pass.analyses)
        , m_cfg(pass.analyses.cfg())
    {
        RELEASE_ASSERT(pass.inputs.placement);
        RELEASE_ASSERT(pass.function.representationsAreChosen);
    }

    PassOutcome run()
    {
        if (collectStores()) {
            findUnmarkableOwners();
            findCoveredStores();
        }
        for (const BarrierStore& store : m_stores)
            setFlags(store.value, store.decision);
        countRemovedBarriers();
        return m_outcome;
    }

private:
    const Placement& placement() const { return *m_pass.inputs.placement; }

    bool collectStores()
    {
        for (BlockID block : m_cfg.reversePostOrder()) {
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
        Representation defined = m_function.at(user.operand(operandIndex)).representation;
        if (!m_function.operandsAreConvertedAtFetch)
            return defined;
        RELEASE_ASSERT(m_pass.inputs.callingConventions);
        RepresentationInputs inputs { m_program, m_pass.id, m_function, placement(), *m_pass.inputs.callingConventions };
        Representation wanted = wantedRepresentationOf(inputs, user, operandIndex);
        return hasRegister(wanted) ? wanted : defined;
    }

    void collectStore(BlockID block, uint32_t index, ValueID id)
    {
        const Value& value = m_function.at(id);
        if (takesCell(value)) {
            collectTakenCell(block, index, id);
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
            storesCells |= m_program.typeOf(m_function, operand).hasCells();
            storesLiteralStringWord |= formTakenBy(value, operandIndex) == Representation::LiteralString;
        });

        ValueID owner;
        if ((storesCells || mayStoreExtension(value)) && hasOneOwner && retainingOperand)
            owner = value.operand(*retainingOperand);
        if (!owner) {
            setFlags(id, BarrierDecision::Emit);
            return;
        }
        m_stores.append(BarrierStore {
            .block = block,
            .index = index,
            .value = id,
            .owner = m_analyses.sameObjectThroughCasts(owner),
            .alwaysRunsBarrier = storesCells && !storesLiteralStringWord && !mayStoreInDeferredClosureSlot(value),
        });
    }

    void collectTakenCell(BlockID block, uint32_t index, ValueID id)
    {
        const Value& value = m_function.at(id);
        std::span<const uint32_t> fields = value.fields();
        for (size_t position = 0; position < fields.size(); ++position) {
            if (!placement().layout(value.layout()).innerObjectIn(fields[position]))
                continue;
            m_stores.append(BarrierStore { .block = block, .index = index, .value = id, .owner = m_analyses.sameObjectThroughCasts(value.valuesOperands()[position]) });
            return;
        }
        setFlags(id, BarrierDecision::Emit);
    }

    bool mayLieOutOfLine(FieldAccessID id) const
    {
        return !id || placement().fieldAccess(id).slot.isOutOfLine();
    }

    // A store that makes room for its value stores the Extension in the owner, whatever the value is.
    bool mayStoreExtension(const Value& store) const
    {
        if (!storesThatMayAdd.contains(store.opcode))
            return false;
        if (Value::opcodesWithFieldAccess.contains(store.opcode))
            return mayLieOutOfLine(store.fieldAccess());
        bool result = false;
        for (PlacedKey key : store.placedKeys())
            result |= mayLieOutOfLine(key.fieldAccess);
        return result;
    }

    bool mayBeDeferredClosureSlot(FieldAccessID id) const
    {
        return !id || placement().fieldAccess(id).slot.representation == SlotRepresentation::DeferredClosure;
    }

    bool mayStoreInDeferredClosureSlot(const Value& store) const
    {
        if (Value::opcodesWithFieldAccess.contains(store.opcode))
            return mayBeDeferredClosureSlot(store.fieldAccess());
        if (!Value::opcodesWithPlacedKeys.contains(store.opcode))
            return false;
        bool result = false;
        for (PlacedKey key : store.placedKeys())
            result |= mayBeDeferredClosureSlot(key.fieldAccess);
        return result;
    }

    void setFlags(ValueID id, BarrierDecision decision)
    {
        const Value& value = m_function.at(id);
        bool ownerCannotBeMarked = decision == BarrierDecision::OwnerCannotBeMarked;
        bool laterBarrierCovers = decision == BarrierDecision::LaterBarrierCovers;
        if (takesCell(value)) {
            if (value.cellCannotBeMarked() != ownerCannotBeMarked) {
                m_function.edit(id, ValueEdit::Definition).setCellCannotBeMarked(ownerCannotBeMarked);
                m_outcome = PassOutcome::Changed;
            }
            return;
        }
        if (value.ownerCannotBeMarked() == ownerCannotBeMarked && value.laterBarrierCovers() == laterBarrierCovers)
            return;
        Value& store = m_function.edit(id, ValueEdit::Definition);
        store.setOwnerCannotBeMarked(ownerCannotBeMarked);
        store.setLaterBarrierCovers(laterBarrierCovers);
        m_outcome = PassOutcome::Changed;
    }

    void countRemovedBarriers()
    {
        auto numberOfStoresWith = [&](BarrierDecision decision) {
            return static_cast<uint64_t>(std::ranges::count(m_stores, decision, &BarrierStore::decision));
        };
        Statistics& statistics = m_pass.build.statistics();
        statistics.count(StatisticID::BarriersOnUnmarkableOwners, numberOfStoresWith(BarrierDecision::OwnerCannotBeMarked));
        statistics.count(StatisticID::BarriersCoveredLater, numberOfStoresWith(BarrierDecision::LaterBarrierCovers));
    }

    void findUnmarkableOwners()
    {
        bool hasHeapAllocation = false;
        for (BarrierOwner& owner : m_owners) {
            classifyOwner(owner);
            hasHeapAllocation |= owner.kind == OwnerKind::HeapAllocation;
        }
        for (BarrierStore& store : m_stores) {
            if (m_owners[store.ownerNumber].kind == OwnerKind::FrameCell)
                store.decision = BarrierDecision::OwnerCannotBeMarked;
        }
        if (!hasHeapAllocation)
            return;

        YoungOwnersProblem problem(m_ownerNumbers, m_owners.span());
        Vector<std::optional<BitVector>> heads = m_analyses.solveForward(problem);
        for (BlockID block : m_cfg.reversePostOrder()) {
            if (std::optional<BitVector>& young = heads[block.index()])
                findStoresToYoungOwners(problem, block, *young);
        }
    }

    void classifyOwner(BarrierOwner& owner)
    {
        if (isFrameCell(m_function, owner.value)) {
            owner.kind = OwnerKind::FrameCell;
            return;
        }
        const Value& value = m_function.at(owner.value);
        if (!allocatesAfterLastSafepointOf(value.opcode))
            return;
        if (value.opcode == Opcode::NewObject && !allocatesUnmarkedCell(value))
            return;
        owner.kind = OwnerKind::HeapAllocation;
    }

    bool allocatesUnmarkedCell(const Value& newObject) const
    {
        if (newObject.storage() != AllocationStorage::Heap)
            return true;
        LayoutID layout = newObject.layout();
        return placement().layout(layout).hasSlots() && !placement().freeCellOffset(layout) && !newObject.prototypeOperand() && !newObject.takesCellOfInnerObject();
    }

    void findStoresToYoungOwners(const YoungOwnersProblem& problem, BlockID block, BitVector& young)
    {
        const BlockValues& values = m_function.at(block).values;
        uint32_t index = 0;
        for (BarrierStore& store : storesIn(block)) {
            for (; index < store.index; ++index) {
                ValueID id = values[index];
                problem.execute(young, DataflowStep { block, index, id, m_function.at(id), problem.ignoresEffects(young) ? noEffectsOnEdges : m_analyses.effectsOf(id) });
            }
            if (isStoreToYoungOwner(store, young))
                store.decision = BarrierDecision::OwnerCannotBeMarked;
        }
    }

    bool isStoreToYoungOwner(const BarrierStore& store, const BitVector& youngBeforeStore)
    {
        const BarrierOwner& owner = m_owners[store.ownerNumber];
        if (owner.kind != OwnerKind::HeapAllocation || !youngBeforeStore.get(store.ownerNumber))
            return false;
        // An operation that takes a cell allocates none before it has filled the cell.
        bool reachesSafepointBeforeStore = !takesCell(m_function.at(store.value)) && reachesSafepoint(m_analyses.effectsOf(store.value));
        if (reachesSafepointBeforeStore)
            return false;
        const EscapeAnalysis& escape = m_analyses.escape();
        std::optional<ObjectID> object = escape.objectDefinedBy(owner.value);
        if (!object || escape.at(*object).kind != ObjectKind::Allocation)
            return false;
        return retentionHistory().occurredBefore(*object, ProgramPoint { store.block, store.index }) == Occurrence::HasNotOccurred;
    }

    // A store is covered by a later one to the same owner.
    bool someOwnerIsRememberedTwice() const
    {
        BitVector rememberedOwners(m_owners.size());
        for (const BarrierStore& store : m_stores) {
            if (store.decision == BarrierDecision::Emit && remembersOwnerOf(m_function.at(store.value).opcode) && rememberedOwners.quickSet(store.ownerNumber))
                return true;
        }
        return false;
    }

    void findCoveredStores()
    {
        if (!someOwnerIsRememberedTwice())
            return;

        BitVector ownersOfCoverableStores(m_owners.size());
        bool hasRepeatedOwner = false;
        for (BarrierStore& store : m_stores) {
            if (store.decision != BarrierDecision::Emit)
                continue;
            store.canBeCovered = remembersOwnerOf(m_function.at(store.value).opcode) && !reachesSafepoint(m_analyses.effectsOf(store.value));
            store.hasOwnerBarrier = store.canBeCovered && store.alwaysRunsBarrier;
            if (store.canBeCovered)
                hasRepeatedOwner |= ownersOfCoverableStores.set(store.ownerNumber);
        }
        if (!hasRepeatedOwner)
            return;

        std::span<const BlockID> order = m_cfg.reversePostOrder();
        OwnerSets heads(m_function.blocks.size(), allOwners());
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t orderIndex = order.size(); orderIndex--;) {
                BlockID block = order[orderIndex];
                BitVector covered = coveredOwnersAtTail(block, heads);
                transferCoveredOwners(block, covered, [](BarrierStore&, const BitVector&) { });
                if (covered == heads[block])
                    continue;
                heads[block] = WTF::move(covered);
                changed = true;
            }
        }

        for (BlockID block : order) {
            if (storesIn(block).empty())
                continue;
            BitVector covered = coveredOwnersAtTail(block, heads);
            transferCoveredOwners(block, covered, [](BarrierStore& store, const BitVector& coveredAfterStore) {
                if (store.canBeCovered && coveredAfterStore.get(store.ownerNumber))
                    store.decision = BarrierDecision::LaterBarrierCovers;
            });
        }
    }

    BitVector coveredOwnersAtTail(BlockID block, const OwnerSets& heads)
    {
        const Terminator& terminator = m_function.at(block).terminator;
        if (terminator.targets.isEmpty() || endsCoverage(terminator.kind) || endsCoverage(m_analyses.effectsOfTerminator(block)))
            return BitVector(m_owners.size());

        BitVector covered = heads[terminator.targets.first().block];
        for (const Target& target : terminator.targets.span().subspan(1))
            covered.filter(heads[target.block]);
        return covered;
    }

    void transferCoveredOwners(BlockID blockID, BitVector& covered, NOESCAPE const Invocable<void(BarrierStore&, const BitVector&)> auto& visitStore)
    {
        const Block& block = m_function.at(blockID);
        std::span<BarrierStore> stores = storesIn(blockID);
        for (size_t index = block.values.size(); index--;) {
            ValueID value = block.values[index];
            if (!stores.empty() && stores.back().index == index) {
                BarrierStore& store = stores.back();
                visitStore(store, covered);
                if (store.hasOwnerBarrier)
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
    }

    const UseEffectHistory& retentionHistory()
    {
        if (!m_retentionHistory)
            m_retentionHistory.emplace(m_analyses, UseEffectHistoryOptions { .uses = { OperandUse::Retains } });
        return *m_retentionHistory;
    }

    std::span<BarrierStore> storesIn(BlockID block)
    {
        StoreRange range = m_storeRanges[block];
        return m_stores.mutableSpan().subspan(range.begin, range.end - range.begin);
    }

    BitVector allOwners() const
    {
        BitVector owners(m_owners.size());
        for (size_t number = 0; number < m_owners.size(); ++number)
            owners.set(number);
        return owners;
    }

    FunctionPassContext& m_pass;
    const Program& m_program;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    const CFG& m_cfg;
    Vector<BarrierStore> m_stores;
    Vector<BarrierOwner> m_owners;
    IndexMap<BlockID, StoreRange> m_storeRanges;
    OwnerNumbers m_ownerNumbers;
    std::optional<UseEffectHistory> m_retentionHistory;
    PassOutcome m_outcome { PassOutcome::Unchanged };
};

} // anonymous namespace

PassOutcome removeRedundantBarriers(FunctionPassContext& pass)
{
    if (!hasStoreOperation(pass.function))
        return PassOutcome::Unchanged;
    return RedundantBarrierAnalysis(pass).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
