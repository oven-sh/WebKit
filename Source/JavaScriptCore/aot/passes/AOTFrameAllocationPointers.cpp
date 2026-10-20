/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameAllocationPointers.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTEdgeResults.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTSchemaRuntimeColumns.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <optional>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OperandUses unfollowedLeakingUses = usesLeakingFramePointers - usesLeakingContents - OperandUses { OperandUse::Retains, OperandUse::Returns };

bool hasFrameStorage(const Value& value)
{
    return Value::opcodesWithStorage.contains(value.opcode) && value.storage() != AllocationStorage::Heap;
}

bool isFrameCell(const Function& function, ValueID id)
{
    const Value& definition = function.at(sameObjectThroughCasts(function, id));
    return Value::opcodesWithStorage.contains(definition.opcode) && definition.storage() == AllocationStorage::Frame;
}

static bool canFollowResultOf(Opcode opcode)
{
    if (opcode == Opcode::MaterializeEnvironment)
        return false;
    switch (effectRuleOf(opcode)) {
    case EffectRule::Call:
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return false;
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool readsStoredValues(Opcode opcode)
{
    return declaredBoundOf(opcode).reads.containsAny(heapsHoldingValues);
}

bool propagatesFramePointers(Opcode opcode)
{
    return canFollowResultOf(opcode) && (readsFollowedByFramePointers.contains(opcode) || !readsStoredValues(opcode));
}

ValueID cellAccessedBy(const Value& followedAccess)
{
    ASSERT(storesFollowedByFramePointers.contains(followedAccess.opcode) || readsFollowedByFramePointers.contains(followedAccess.opcode));
    if (OperandPosition::closure(followedAccess.opcode))
        return followedAccess.closureOperand();
    if (OperandPosition::environment(followedAccess.opcode))
        return followedAccess.environmentOperand();
    return followedAccess.objectOperand();
}

void forEachReturnedOperand(const Function& function, const Value& value, NOESCAPE const ScopedLambda<void(ValueID)>& functor)
{
    if (value.opcode == Opcode::ResultAt) {
        ValueID results = value.resultsOperand();
        const Value& producer = function.at(results);
        functor(producer.opcode == Opcode::Results ? producer.operand(value.index()) : results);
        return;
    }
    if (!canFollowResultOf(value.opcode))
        return;
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operandUsesOf(value.opcode, operandIndex).contains(OperandUse::Returns))
            functor(operand);
    });
}

bool mayBeUnspecified(FunctionAnalyses& analyses, ValueID start)
{
    const Function& function = analyses.function();
    const CFG& cfg = analyses.cfg();
    const UseLists& uses = analyses.useLists();

    BitVector visited;
    Vector<ValueID, 8> worklist { start };
    while (!worklist.isEmpty()) {
        ValueID id = worklist.takeLast();
        if (!visited.add(id.index()))
            continue;

        const Value& value = function.at(id);
        if (value.opcode == Opcode::Unspecified)
            return true;
        if (value.opcode == Opcode::ResultAt && effectRuleOf(function.at(value.resultsOperand()).opcode) == EffectRule::Call)
            return true;
        if (value.opcode != Opcode::Parameter) {
            forEachReturnedOperand(function, value, [&](ValueID operand) {
                worklist.append(operand);
            });
            continue;
        }

        BlockID block = uses.blockOf(id);
        uint32_t parameterIndex = static_cast<uint32_t>(function.at(block).parameters.find(id));
        for (Edge edge : cfg.incomingEdges(block)) {
            if (ValueID passed = valuePassedOn(function, edge, parameterIndex))
                worklist.append(passed);
        }
    }
    return false;
}

FramePointers::FramePointers(const Function& function)
    : FramePointers(nullptr, function)
{
}

FramePointers::FramePointers(const Program& program, const Function& function)
    : FramePointers(&program, function)
{
}

FramePointers::FramePointers(const Program* program, const Function& function)
    : m_program(program)
{
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (!hasFrameStorage(function.at(id)))
                continue;
            m_allocations.append(id);
            m_values.ensureSizeAndSet(id.index(), function.values.size());
        }
    }
    if (m_allocations.isEmpty())
        return;
    std::ranges::sort(m_allocations);

    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            BlockID block(blockIndex);
            for (ValueID id : function.at(block).values)
                changed |= propagate(function, id);

            forEachOutgoingEdge(function, block, [&](Edge edge, const Target& target) {
                const CompactList<ValueID>& parameters = function.at(target.block).parameters;
                for (uint32_t argumentIndex = 0; argumentIndex < target.arguments.size(); ++argumentIndex) {
                    if (contains(target.arguments[argumentIndex]))
                        changed |= add(function, parameters[argumentIndex]);
                }
                if (edge.isHandlerEdge())
                    return;

                uint32_t firstParameter = firstEdgeResultParameter(function, block, edge.targetIndex);
                uint32_t numberOfResults = numberOfEdgeResults(function, block, edge.targetIndex);
                for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
                    EdgeResult result = edgeResultAt(function, { block, edge.targetIndex, resultIndex });
                    if (result.kind == EdgeResultKind::NarrowedOperand && contains(result.subject))
                        changed |= add(function, parameters[firstParameter + resultIndex]);
                }
            });
        }
    }
}

bool FramePointers::propagate(const Function& function, ValueID id)
{
    const Value& value = function.at(id);
    bool changed = propagateStore(function, value);
    if (contains(id))
        return changed;

    bool mayBeFramePointer = readsPointer(function, value);
    forEachReturnedOperand(function, value, [&](ValueID operand) {
        mayBeFramePointer |= contains(operand);
    });
    if (mayBeFramePointer)
        changed |= add(function, id);
    return changed;
}

bool FramePointers::propagateStore(const Function&, const Value& value)
{
    bool changed = false;
    switch (value.opcode) {
    case Opcode::NewObject: {
        std::span<const ValueID> values = value.valuesOperands();
        std::span<const uint32_t> fields = value.fields();
        for (size_t index = 0; index < values.size(); ++index) {
            if (contains(values[index]))
                changed |= m_environmentFieldsWithPointers.appendIfNotContains(FieldReference { value.layout(), fields[index] });
        }
        return changed;
    }
    case Opcode::NewClosure: {
        std::span<const ValueID> captures = value.capturesOperands();
        for (uint32_t index = 0; index < captures.size(); ++index) {
            if (contains(captures[index]))
                changed |= m_capturesWithPointers.appendIfNotContains(CaptureReference { value.function(), index });
        }
        return changed;
    }
    case Opcode::PutEnvironmentField:
        return contains(value.valueOperand()) && m_environmentFieldsWithPointers.appendIfNotContains(FieldReference { value.layout(), value.field() });
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd:
        return contains(value.valueOperand()) && m_keysWithPointers.appendIfNotContains(value.key());
    case Opcode::PutSlots: {
        std::span<const ValueID> values = value.valuesOperands();
        PayloadListView<PlacedKey> placedKeys = value.placedKeys();
        for (size_t index = 0; index < values.size(); ++index) {
            if (contains(values[index]))
                changed |= m_keysWithPointers.appendIfNotContains(placedKeys[index].key);
        }
        return changed;
    }
    default:
        ASSERT(!storesFollowedByFramePointers.contains(value.opcode));
        return false;
    }
}

bool FramePointers::readsPointer(const Function&, const Value& value) const
{
    switch (value.opcode) {
    case Opcode::GetEnvironmentField:
        return contains(value.environmentOperand()) && m_environmentFieldsWithPointers.contains(FieldReference { value.layout(), value.field() });
    case Opcode::GetCapture:
        return contains(value.closureOperand()) && m_capturesWithPointers.contains(CaptureReference { value.function(), value.index() });
    case Opcode::GetSlot:
    case Opcode::GetSlotOrUndefined:
        return contains(value.objectOperand()) && m_keysWithPointers.contains(value.key());
    default:
        ASSERT(!readsFollowedByFramePointers.contains(value.opcode));
        return false;
    }
}

bool FramePointers::add(const Function& function, ValueID id)
{
    if (m_program && !mayBeReference(m_program->typeOf(function, id)) && !numberOfResultsOf(*m_program, function, function.at(id)))
        return false;
    return m_values.add(id.index());
}

bool FramePointers::mayHoldPointers(const Program& program, const Function& function, ValueID cell) const
{
    const Value& definition = function.at(sameObjectThroughCasts(function, cell));
    if (definition.opcode == Opcode::NewClosure) {
        return std::ranges::any_of(m_capturesWithPointers, [&](const CaptureReference& capture) {
            return capture.function == definition.function();
        });
    }
    if (definition.opcode != Opcode::NewObject)
        return !m_environmentFieldsWithPointers.isEmpty() || !m_keysWithPointers.isEmpty() || !m_capturesWithPointers.isEmpty();

    LayoutID layout = definition.layout();
    bool holdsPointerInField = std::ranges::any_of(m_environmentFieldsWithPointers, [&](const FieldReference& field) {
        return field.layout == layout;
    });
    return holdsPointerInField || std::ranges::any_of(m_keysWithPointers, [&](PropertyKeyID key) {
        return program.at(layout).findField(key).has_value();
    });
}

static OperandSummary summarizePublishedParameter(const Program& program, const FlowResults& flow, const Value& call, FunctionID callee, std::optional<uint32_t> entryParameter)
{
    if (!entryParameter)
        return { };

    OperandSummary summary = flow.framePointerSummary(callee, *entryParameter);
    if (summary.retainedIn != RetainedIn::Operand)
        return summary;

    if (std::optional<uint32_t> retainingOperand = findOperandPassedFor(program.at(callee), call, summary.retainingOperand)) {
        summary.retainingOperand = *retainingOperand;
        return summary;
    }
    summary.retainedIn = RetainedIn::UnknownMemory;
    summary.retainingOperand = 0;
    return summary;
}

static OperandSummary summarizeCallOperand(const Program& program, const FlowResults& flow, const Value& call, uint32_t operandIndex)
{
    CalleeList callees = calleesOfCall(program, call);
    if (!callees.isComplete || !callees.builtins.isEmpty() || maySuspend(call.opcode))
        return { };

    OperandUses declaredUses = operandUsesOf(call.opcode, operandIndex);
    bool isCalledValue = declaredUses.containsAny(callingUses);
    OperandSummary summary { isCalledValue ? declaredUses : OperandUses { OperandUse::Inspects }, RetainedIn::UnknownMemory, 0 };
    for (FunctionID id : callees.functions) {
        const Function& callee = program.at(id);
        if (!isCalledValue) {
            summary = joinOf(summary, summarizePublishedParameter(program, flow, call, id, entryParameterOf(callee, call, operandIndex)));
            continue;
        }
        for (uint32_t parameter : entryParametersOfCalledValue(callee, call))
            summary = joinOf(summary, summarizePublishedParameter(program, flow, call, id, parameter));
    }
    return summary;
}

OperandSummary summarizeFramePointerUse(const Program& program, const FlowResults* flow, const Function& function, const Value& user, uint32_t operandIndex)
{
    if (effectRuleOf(user.opcode) == EffectRule::Call)
        return flow ? summarizeCallOperand(program, *flow, user, operandIndex) : OperandSummary();

    EffectSummaries unknownSummaries;
    return summarizeOperand(EffectContext { program, unknownSummaries }, function, user, operandIndex);
}

OperandSummary summarizeFramePointerUse(const Program& program, const FlowResults*, const Function& function, const Terminator& user, uint32_t operandIndex)
{
    EffectSummaries unknownSummaries;
    return summarizeOperand(EffectContext { program, unknownSummaries }, function, user, operandIndex);
}

// Where the stub of a call with a list cannot copy the list to the stack (a hole, a long list, another storage), the run-time function of the operation makes the call.
static bool isCalledByRuntimeFunction(const Value& user, uint32_t operandIndex)
{
    return runtimeOf(user.opcode) && operandUsesOf(user.opcode, operandIndex).containsAny(callingUses);
}

OperandUses usesOfFrameCellsAllowedIn(const Program&, const Value&)
{
    return nativeUsesOfFrameCells;
}

bool mayLeakFramePointer(const Program& program, const FlowResults* flow, const Function& function, const Value& user, uint32_t operandIndex)
{
    OperandSummary summary = summarizeFramePointerUse(program, flow, function, user, operandIndex);
    if (summary.uses.containsAny(unfollowedLeakingUses))
        return true;
    bool isRetained = summary.uses.contains(OperandUse::Retains);
    if (isRetained && summary.retainedIn == RetainedIn::UnknownMemory)
        return true;
    if (summary.uses.contains(OperandUse::Returns) && !propagatesFramePointers(user.opcode))
        return true;

    switch (effectRuleOf(user.opcode)) {
    case EffectRule::Call:
        return isRetained || isCalledByRuntimeFunction(user, operandIndex);
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return !summary.uses.containsOnly(usesOfFrameCellsAllowedIn(program, user));
    case EffectRule::Access:
        return provenAccessOf(program, function, user).isLookupByKey;
    case EffectRule::Declared:
    case EffectRule::Operator:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool mayLeakContents(const Program& program, const FlowResults* flow, const Function& function, const Value& user, uint32_t operandIndex)
{
    return summarizeFramePointerUse(program, flow, function, user, operandIndex).uses.containsAny(usesLeakingContents);
}

bool mayLeakFramePointer(const Program& program, const FlowResults* flow, const Function& function, const Terminator& user, uint32_t operandIndex)
{
    return summarizeFramePointerUse(program, flow, function, user, operandIndex).uses.containsAny(usesLeakingFramePointers);
}

bool mayLeakCapture(const FlowResults* flow, const Value& closure, uint32_t captureIndex)
{
    return !flow || flow->framePointerSummary(closure.function(), captureIndex).uses.containsAny(usesLeakingFramePointers);
}

// Without flow no other function gets the environment: every capture and every operand of a call counts as leaked.
bool mayLeakStoredValue(const FlowResults* flow, FieldReference environmentField)
{
    return flow && flow->storedFramePointerUses(environmentField).containsAny(usesLeakingFramePointers);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
