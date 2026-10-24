/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCurrentWork.h"
#include "AOTEffects.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTValueFlowHeapTables.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

static bool writesOnlyObjects(const Effects& effects)
{
    return !effects.mayExitSideways() && effects.writes.containsOnly(perObjectHeaps | bookkeepingHeaps);
}

namespace {

// What becomes of a value that nothing uses. The row of most operations answers for all their values.
enum class UnusedValueRule : uint8_t {
    DependsOnEffects,
    IsRemoved,
    Stays,
    // A store into one of its operands, and nothing else.
    StaysWithWrittenObject,
};

struct OpcodeRules {
    UnusedValueRule unusedValue { UnusedValueRule::DependsOnEffects };
    bool writesOnlyNamedOperands { false };
    bool mayHaveFreshResult { true };
};

} // anonymous namespace

// freshnessOf() answers Fresh for a value of no other opcode.
static bool mayHaveFreshResult(Opcode opcode)
{
    switch (effectRuleOf(opcode)) {
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return true;
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
    case EffectRule::Call:
        break;
    }
    if (Value::opcodesWithOverload.contains(opcode) && resultRuleOf(opcode) == ResultRule::Declared)
        return true;
    if (boundToOf(opcode) == BoundTo::Site)
        return false;

    bool retainsOperandInResult = false;
    bool returnsOperand = false;
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        retainsOperandInResult |= info.uses.contains(OperandUse::Retains) && info.retainedIn == RetainedIn::Result;
        returnsOperand |= info.uses.contains(OperandUse::Returns);
    }
    return !returnsOperand && (resultRuleOf(opcode) == ResultRule::Allocation || retainsOperandInResult);
}

static UnusedValueRule unusedValueRuleOf(Opcode opcode, bool writesOnlyNamedOperands)
{
    const OpcodeEffectFacts& facts = effectFactsOf(opcode);
    Effects atMost = declaredBoundOf(opcode);
    if (facts.staysWithinDeclaredBound && atMost.isRemovableWhenUnused())
        return UnusedValueRule::IsRemoved;

    // The questions below ask which heaps are written and whether there is a way out, so one way out stands for all of them.
    Effects atLeast = Effects::none();
    atLeast.writes = facts.writtenHeaps;
    atLeast.mayThrow = facts.mayExitSideways;
    if (atLeast.isRemovableWhenUnused())
        return UnusedValueRule::DependsOnEffects;
    if (!writesOnlyNamedOperands || !writesOnlyObjects(atLeast))
        return UnusedValueRule::Stays;
    if (facts.staysWithinDeclaredBound && writesOnlyObjects(atMost))
        return UnusedValueRule::StaysWithWrittenObject;
    return UnusedValueRule::DependsOnEffects;
}

using RulesOfOpcodes = std::array<OpcodeRules, numberOfOpcodes>;

static const RulesOfOpcodes& rulesOfOpcodes()
{
    static const RulesOfOpcodes rules = [] {
        RulesOfOpcodes result { };
        for (Opcode member : allMembersOf<Opcode>) {
            bool writesOnlyNamedOperands = confinementOf(member) == OperandConfinement::WritesOnlyNamedOperands;
            result[static_cast<unsigned>(member)] = { unusedValueRuleOf(member, writesOnlyNamedOperands), writesOnlyNamedOperands, mayHaveFreshResult(member) };
        }
        return result;
    }();
    return rules;
}

namespace {

// One bit a value, in line for a function of up to 1,024 values.
class BitOfEachValue {
public:
    explicit BitOfEachValue(size_t numberOfValues)
        : m_words(FillWith { }, (numberOfValues + 63) / 64, 0)
    {
    }

    bool get(uint32_t index) const { return m_words[index >> 6] >> (index & 63) & 1; }
    void set(uint32_t index) { m_words[index >> 6] |= bitOf(index); }
    bool testAndSet(uint32_t index)
    {
        uint64_t& word = m_words[index >> 6];
        bool wasSet = word & bitOf(index);
        word |= bitOf(index);
        return wasSet;
    }

    bool hasBitThatIsClearIn(const BitOfEachValue& other) const
    {
        for (size_t wordIndex = 0; wordIndex < m_words.size(); ++wordIndex) {
            if (m_words[wordIndex] & ~other.m_words[wordIndex])
                return true;
        }
        return false;
    }

    void forEachSetBit(NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        for (size_t wordIndex = 0; wordIndex < m_words.size(); ++wordIndex) {
            for (uint64_t word = m_words[wordIndex]; word; word &= word - 1)
                functor(static_cast<uint32_t>(wordIndex * 64 + std::countr_zero(word)));
        }
    }

private:
    static uint64_t bitOf(uint32_t index) { return 1ull << (index & 63); }

    Vector<uint64_t, 16> m_words;
};

// ONE WALK BACKWARD, block by block against the reverse post order and value by value against the order in the block.
class DeadCodeEliminator {
public:
    explicit DeadCodeEliminator(FunctionPassContext& context)
        : m_context(context)
        , m_function(context.function)
        , m_analyses(context.analyses)
        , m_cfg(context.analyses.cfg())
        , m_rules(rulesOfOpcodes())
        , m_live(context.function.values.size())
        , m_needsAttentionWhenMarked(context.function.values.size())
        , m_checksRules(context.build.options().verifiesAfterEveryPass)
    {
    }

    // A store into an object that does not escape is live once a live value may be the object.
    PassOutcome run()
    {
        walkBackward();
        m_usedEscapeAnalysis = findObjectsOfWaitingStores();
        if (m_usedEscapeAnalysis)
            propagate();
        return sweep();
    }

    // With fewer objects left, the escape analysis may prove more on a second run.
    bool reachedFixpoint() const { return !m_usedEscapeAnalysis; }

    bool mayHaveGivenControlFlowWork() const { return m_madeEdgesAlikeOrBlocksEmpty || m_cfg.hasHandlers(); }

    PassSet passesLeftIdle() const
    {
        if (m_tookProofsFromEdges)
            return { };
        PassSet passes { PassID::PropagateBranchConditions };
        if (!m_context.foldingAskedForUses)
            passes.add(PassID::FoldConstants);
        if (!mayHaveGivenControlFlowWork())
            passes.add(PassID::SimplifyControlFlow);
        return passes;
    }

private:
    static constexpr uint32_t noWaitingStore = UINT32_MAX;

    struct WaitingStore {
        ValueID receiver;
        ValueID store;
        // With the same receiver.
        uint32_t previous { noWaitingStore };
    };

    struct Position {
        size_t rank { 0 };
        size_t index { 0 };
    };

    void mark(ValueID id)
    {
        if (!m_live.testAndSet(id.index()) && m_needsAttentionWhenMarked.get(id.index()))
            m_worklist.append(id);
    }

    void walkBackward()
    {
        for (ValueID parameter : m_function.at(m_cfg.root()).parameters)
            m_live.set(parameter.index());

        std::span<const BlockID> order = m_cfg.reversePostOrder();
        for (size_t rank = order.size(); rank--;) {
            const Block& block = m_function.at(order[rank]);
            block.forEachTarget([&](const Target& target) {
                markArgumentsOfLiveParameters(target);
            });
            block.terminator.Instruction::forEachChild([&](ValueID operand) {
                mark(operand);
            });
            if (block.subject)
                mark(block.subject);
            std::span<const ValueID> values = block.values.span();
            for (size_t index = values.size(); index--;)
                visit(values[index], { rank, index });
            if (!m_worklist.isEmpty())
                propagate();
        }
    }

    void markArgumentsOfLiveParameters(const Target& target)
    {
        std::span<const ValueID> parameters = m_function.at(target.block).parameters.span();
        std::span<const ValueID> arguments = target.arguments.span();
        for (size_t index = 0; index < parameters.size(); ++index) {
            ValueID parameter = parameters[index];
            if (!m_live.get(parameter.index()))
                m_needsAttentionWhenMarked.set(parameter.index());
            else if (index < arguments.size())
                mark(arguments[index]);
        }
    }

    void visit(ValueID id, Position position)
    {
        const Value& value = m_function.at(id);
        const OpcodeRules& rules = m_rules[static_cast<unsigned>(value.opcode)];
        if (m_checksRules) [[unlikely]]
            checkRules(id, rules);

        if (!m_live.get(id.index())) {
            if (!staysUnused(id, value, rules, position)) {
                m_needsAttentionWhenMarked.set(id.index());
                return;
            }
            m_live.set(id.index());
            if (m_needsAttentionWhenMarked.get(id.index()))
                markStoresThatWaitFor(id);
        }
        value.forEachOperand([&](ValueID operand, uint32_t) {
            mark(operand);
        });
    }

    bool staysUnused(ValueID id, const Value& value, const OpcodeRules& rules, Position position)
    {
        // The host makes such a member when it is first read, with code of the library, and what that code does can be seen.
        if (value.opcode == Opcode::Builtin && isMadeWithCodeOfLibrary(m_context.program.builtins(), value.builtin()))
            return true;

        switch (rules.unusedValue) {
        case UnusedValueRule::IsRemoved:
            return false;
        case UnusedValueRule::Stays:
            return true;
        case UnusedValueRule::StaysWithWrittenObject:
            return !waitsForWrittenObject(id, value, position);
        case UnusedValueRule::DependsOnEffects:
            break;
        }

        // The effects of a value only matter while nothing live uses it.
        if (m_analyses.isRemovableWhenUnused(id))
            return false;
        if (!rules.writesOnlyNamedOperands)
            return true;
        const EffectsOnEdges& effects = m_analyses.effectsOf(id);
        if (writesOnlyObjects(effects.normal) && writesOnlyObjects(effects.handler))
            return !waitsForWrittenObject(id, value, position);
        return true;
    }

    void checkRules(ValueID id, const OpcodeRules& rules)
    {
        RELEASE_ASSERT(rules.mayHaveFreshResult || freshnessOf(m_analyses.effectContext(), m_function, m_function.at(id)) != ResultFreshness::Fresh);

        Effects effects = m_analyses.effectsOf(id).onAnyEdge();
        bool isStoreIntoOperand = rules.writesOnlyNamedOperands && writesOnlyObjects(effects);
        switch (rules.unusedValue) {
        case UnusedValueRule::DependsOnEffects:
            return;
        case UnusedValueRule::IsRemoved:
            RELEASE_ASSERT(effects.isRemovableWhenUnused());
            return;
        case UnusedValueRule::Stays:
            RELEASE_ASSERT(!effects.isRemovableWhenUnused() && !isStoreIntoOperand);
            return;
        case UnusedValueRule::StaysWithWrittenObject:
            RELEASE_ASSERT(!effects.isRemovableWhenUnused() && isStoreIntoOperand);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool waitsForWrittenObject(ValueID store, const Value& value, Position position)
    {
        // In front of the first allocation there is no object that a store could be alone in knowing.
        if (!isBehindFreshAllocation(position))
            return false;
        std::optional<uint32_t> writtenOperand = findOnlyWrittenOperand(value);
        ValueID receiver = writtenOperand ? value.operand(*writtenOperand) : ValueID();
        if (!receiver || m_live.get(receiver.index()))
            return false;

        auto last = m_lastWaitingStoreInto.add(receiver.index(), noWaitingStore);
        m_waitingStores.append({ receiver, store, last.iterator->value });
        last.iterator->value = m_waitingStores.size() - 1;
        m_needsAttentionWhenMarked.set(receiver.index());
        return true;
    }

    // At the position or in front of it, in reverse post order. Looked for when the first store asks.
    bool isBehindFreshAllocation(Position position)
    {
        if (!m_lookedForFreshAllocation) {
            m_lookedForFreshAllocation = true;
            m_firstFreshAllocation = findFirstFreshAllocation();
        }
        if (!m_firstFreshAllocation)
            return false;
        return m_firstFreshAllocation->rank < position.rank || (m_firstFreshAllocation->rank == position.rank && m_firstFreshAllocation->index <= position.index);
    }

    std::optional<Position> findFirstFreshAllocation() const
    {
        std::span<const BlockID> order = m_cfg.reversePostOrder();
        for (size_t rank = 0; rank < order.size(); ++rank) {
            std::span<const ValueID> values = m_function.at(order[rank]).values.span();
            for (size_t index = 0; index < values.size(); ++index) {
                const Value& value = m_function.at(values[index]);
                if (m_rules[static_cast<unsigned>(value.opcode)].mayHaveFreshResult && freshnessOf(m_analyses.effectContext(), m_function, value) == ResultFreshness::Fresh)
                    return Position { rank, index };
            }
        }
        return std::nullopt;
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
            if (!m_waitingStores.isEmpty())
                markStoresThatWaitFor(id);
            if (escape)
                activateObjects(escape->pointsTo(id));
        }
    }

    void markStoresThatWaitFor(ValueID receiver)
    {
        auto last = m_lastWaitingStoreInto.find(receiver.index());
        if (last == m_lastWaitingStoreInto.end())
            return;
        for (uint32_t index = last->value; index != noWaitingStore; index = m_waitingStores[index].previous)
            mark(m_waitingStores[index].store);
    }

    bool findObjectsOfWaitingStores()
    {
        bool anyWaits = false;
        for (const WaitingStore& waiting : m_waitingStores)
            anyWaits |= !m_live.get(waiting.store.index());
        if (!anyWaits)
            return false;

        const EscapeAnalysis& escape = m_analyses.escape();
        for (const WaitingStore& waiting : m_waitingStores) {
            if (m_live.get(waiting.store.index()))
                continue;
            if (std::optional<ObjectID> object = findStoreTarget(escape, waiting.receiver))
                deferStore(*object, waiting.store);
            else
                mark(waiting.store);
        }
        if (!m_storesInto.isEmpty()) {
            m_live.forEachSetBit([&](uint32_t index) {
                activateObjects(escape.pointsTo(ValueID(index)));
            });
        }
        return true;
    }

    void markInputsOf(ValueID id)
    {
        const Value& value = m_function.at(id);
        if (value.opcode == Opcode::Parameter) {
            markArgumentsOf(locationOfParameter(id));
            return;
        }
        value.forEachOperand([&](ValueID operand, uint32_t) {
            mark(operand);
        });
    }

    ValueLocation locationOfParameter(ValueID id)
    {
        if (m_parameterLocations.isEmpty()) {
            if (m_analyses.computed().contains(FunctionAnalysis::UseLists))
                return m_analyses.useLists().locationOf(id);
            m_parameterLocations.grow(m_function.values.size());
            for (BlockID block : m_cfg.reversePostOrder()) {
                const auto& parameters = m_function.at(block).parameters;
                for (uint32_t index = 0; index < parameters.size(); ++index)
                    m_parameterLocations[parameters[index].index()] = { ValuePlacement::Parameter, block, index };
            }
        }
        return m_parameterLocations[id.index()];
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
            if (!m_live.get(list[index].index()))
                deadEntries.ensureSizeAndSet(index, list.size());
        }
        return deadEntries;
    }

    PassOutcome sweep()
    {
        // Whatever is dead at the end was left behind as dead by the walk, or found dead by an edge.
        if (!m_needsAttentionWhenMarked.hasBitThatIsClearIn(m_live))
            return PassOutcome::Unchanged;

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
                noteEdgesThatLoseArguments(id);
                builder.removeParameters(id, deadParameters);
                outcome = PassOutcome::Changed;
            }
            if (deadValues.isEmpty() && deadParameters.isEmpty())
                continue;
            m_madeEdgesAlikeOrBlocksEmpty |= block.values.isEmpty() && block.parameters.isEmpty() && block.terminator.kind == TerminatorKind::Jump;
        }
        RELEASE_ASSERT(outcome == PassOutcome::Changed);
        return outcome;
    }

    void noteEdgesThatLoseArguments(BlockID block)
    {
        std::span<const Edge> edges = m_cfg.incomingEdges(block);
        for (size_t index = 0; index < edges.size(); ++index) {
            Edge edge = edges[index];
            if (edge.isHandlerEdge())
                continue;
            m_tookProofsFromEdges |= !!numberOfEdgeResults(m_function, edge.from, edge.targetIndex);
            m_madeEdgesAlikeOrBlocksEmpty |= index && edges[index - 1].from == edge.from;
        }
    }

    FunctionPassContext& m_context;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    const CFG& m_cfg;
    const RulesOfOpcodes& m_rules;
    BitOfEachValue m_live;
    BitOfEachValue m_needsAttentionWhenMarked;
    Vector<ValueID, 64> m_worklist;
    Vector<WaitingStore, 16> m_waitingStores;
    UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_lastWaitingStoreInto;
    std::optional<Position> m_firstFreshAllocation;
    Vector<ValueLocation> m_parameterLocations;
    Vector<Vector<ValueID, 2>> m_storesInto;
    BitVector m_isActive;
    bool m_lookedForFreshAllocation { false };
    bool m_usedEscapeAnalysis { false };
    bool m_tookProofsFromEdges { false };
    bool m_madeEdgesAlikeOrBlocksEmpty { false };
    const bool m_checksRules;
};

} // anonymous namespace

PassOutcome eliminateDeadCode(FunctionPassContext& context)
{
    if (context.build.recordedOptions().sabotage) [[unlikely]]
        stopWhereSabotaged(context.build, context.program, context.function, PassID::EliminateDeadCode);
    DeadCodeEliminator eliminator(context);
    PassOutcome outcome = eliminator.run();
    if (outcome == PassOutcome::Changed) {
        context.keptAnalyses = allAnalyses;
        context.reachedFixpoint = eliminator.reachedFixpoint();
        context.passesLeftIdle = eliminator.passesLeftIdle();
        if (!eliminator.mayHaveGivenControlFlowWork())
            context.passesThatCanWait.add(PassID::SimplifyControlFlow);
    }
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
