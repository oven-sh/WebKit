/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInductionVariables.h"
#include "AOTPlacement.h"
#include "AOTRepresentationRules.h"
#include "AOTVector.h"
#include "B3InductionRecords.h"
#include "B3KnownProperties.h"
#include "JSCell.h"
#include <algorithm>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

using DecorationMark = B3::AbstractHeapRepository::DecorationMark;

static bool receivesUnspecifiedValue(const Function& function, const BitVector& unspecifiedValues, Edge edge, uint32_t parameterIndex)
{
    IncomingValue incoming = incomingValueOf(function, edge, parameterIndex);
    switch (incoming.kind) {
    case IncomingValue::Kind::Argument:
        return unspecifiedValues.get(incoming.argument.index());
    case IncomingValue::Kind::EdgeResult:
        break;
    }

    EdgeResult result = edgeResultAt(function, incoming.result);
    switch (result.kind) {
    case EdgeResultKind::NarrowedOperand:
        return unspecifiedValues.get(result.subject.index());
    case EdgeResultKind::Guard:
    case EdgeResultKind::DeclaredResult:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A function that returns the parts of an object returns Unspecified values for the parts that the returned variant does not have.
static bool isUnspecifiedAtDefinition(const Function& function, const Value& value)
{
    if (value.opcode == Opcode::Unspecified)
        return true;
    return value.opcode == Opcode::ResultAt && effectRuleOf(function.at(value.resultsOperand()).opcode) == EffectRule::Call;
}

static bool returnsUnspecifiedOperand(const Function& function, const Value& value, const BitVector& unspecifiedValues)
{
    if (!hasRegister(value.representation))
        return false;
    bool returnsUnspecified = false;
    forEachReturnedOperand(function, value, [&](ValueID operand) {
        returnsUnspecified |= unspecifiedValues.get(operand.index());
    });
    return returnsUnspecified;
}

static BitVector collectUnspecifiedValues(const Function& function, const CFG& cfg)
{
    BitVector unspecifiedValues;
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (isUnspecifiedAtDefinition(function, function.at(id)))
                unspecifiedValues.set(id.index());
        }
    }
    if (unspecifiedValues.isEmpty())
        return unspecifiedValues;

    bool changed = true;
    while (changed) {
        changed = false;
        for (BlockID id : cfg.reversePostOrder()) {
            const Block& block = function.at(id);
            for (uint32_t parameterIndex = 0; parameterIndex < block.parameters.size(); ++parameterIndex) {
                for (Edge edge : cfg.incomingEdges(id)) {
                    if (receivesUnspecifiedValue(function, unspecifiedValues, edge, parameterIndex))
                        changed |= unspecifiedValues.add(block.parameters[parameterIndex].index());
                }
            }
            for (ValueID value : block.values) {
                if (returnsUnspecifiedOperand(function, function.at(value), unspecifiedValues))
                    changed |= unspecifiedValues.add(value.index());
            }
        }
        changed &= cfg.hasCycle();
    }
    return unspecifiedValues;
}

bool CodeGenerator::mayBeUnspecified(ValueID id)
{
    if (!m_unspecifiedValues)
        m_unspecifiedValues = collectUnspecifiedValues(function(), analyses().cfg());
    return m_unspecifiedValues->get(id.index());
}

// Decode declares the effects of creating a string constant, yet the decoded reference of a cell is plain arithmetic.
static ValueID valueBeforeConversions(const Function& function, ValueID id)
{
    while (familyOf(function.at(id).opcode) == OperationFamily::Representation)
        id = function.at(id).valueOperand();
    return id;
}

// The result of a call stays at the call. Anything else that is computed may be computed earlier than it stands: a load in front of the guard that it rests on.
static bool hasTypeWhereverSourceIsComputed(const Function& function, const BitVector& typedOnlyBehindTest, ValueID source)
{
    const Value& value = function.at(source);
    if (value.opcode == Opcode::Parameter)
        return !typedOnlyBehindTest.get(source.index());
    if (value.opcode == Opcode::ResultAt)
        return effectRuleOf(function.at(value.resultsOperand()).opcode) == EffectRule::Call;

    switch (resultRuleOf(value.opcode)) {
    case ResultRule::Constant:
    case ResultRule::Allocation:
        return true;
    case ResultRule::Natural:
    case ResultRule::Declared:
    case ResultRule::Unknown:
        return effectRuleOf(value.opcode) == EffectRule::Call;
    case ResultRule::None:
    case ResultRule::Cast:
    case ResultRule::Results:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool arrivesTypedOnlyBehindTest(const Function& function, const BitVector& typedOnlyBehindTest, Edge edge, uint32_t parameterIndex)
{
    IncomingValue incoming = incomingValueOf(function, edge, parameterIndex);
    switch (incoming.kind) {
    case IncomingValue::Kind::Argument:
        return !hasTypeWhereverSourceIsComputed(function, typedOnlyBehindTest, valueBeforeConversions(function, incoming.argument));
    case IncomingValue::Kind::EdgeResult:
        // The operand that the edge of a test has narrowed, and what an operation answers on one of its edges.
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A block parameter is a Phi, and stays in its block.
static BitVector collectParametersTypedOnlyBehindTest(const Function& function, const CFG& cfg)
{
    BitVector typedOnlyBehindTest;
    bool changed = true;
    while (changed) {
        changed = false;
        for (BlockID id : cfg.reversePostOrder()) {
            if (id == entryBlock)
                continue;
            const Block& block = function.at(id);
            for (uint32_t parameterIndex = 0; parameterIndex < block.parameters.size(); ++parameterIndex) {
                if (typedOnlyBehindTest.get(block.parameters[parameterIndex].index()))
                    continue;
                for (Edge edge : cfg.incomingEdges(id)) {
                    if (arrivesTypedOnlyBehindTest(function, typedOnlyBehindTest, edge, parameterIndex)) {
                        typedOnlyBehindTest.set(block.parameters[parameterIndex].index());
                        changed = true;
                        break;
                    }
                }
            }
        }
        changed &= cfg.hasCycle();
    }
    return typedOnlyBehindTest;
}

bool CodeGenerator::hasTypeOnlyBehindTest(ValueID parameter)
{
    if (!m_parametersTypedOnlyBehindTest)
        m_parametersTypedOnlyBehindTest = collectParametersTypedOnlyBehindTest(function(), analyses().cfg());
    return m_parametersTypedOnlyBehindTest->get(parameter.index());
}

static bool isPinnedByEffects(const Effects& effects)
{
    return effects.mayExitSideways() || effects.mayCollect || !effects.writes.isEmpty();
}

static bool isCallOrPhi(B3::Opcode opcode)
{
    return opcode == B3::Patchpoint || opcode == B3::CCall || opcode == B3::Phi;
}

// The effects pin the call, not the result: it may be an operand's B3 value, or a load that B3 hoists above the guards of the operation.
static bool isPinnedWithOperation(CodeGenerator& generator, LValue lowered, ValueID operation)
{
    if (!lowered || valueOfOrigin(lowered->origin()) != operation)
        return false;
    LValue call = lowered->opcode() == B3::Extract ? lowered->child(0) : lowered;
    return isCallOrPhi(call->opcode()) && isPinnedByEffects(generator.analyses().effectsOf(operation).onAnyEdge());
}

enum class PublishedValue : uint8_t { Itself, DerivedFromIt };

static bool hasUnconditionalType(CodeGenerator& generator, std::span<const LValue> loweredValues, ValueID id, PublishedValue published)
{
    const Value& value = generator.function().at(id);
    if (value.opcode == Opcode::Parameter)
        return !generator.mayBeUnspecified(id) && (published == PublishedValue::Itself || !generator.hasTypeOnlyBehindTest(id));
    if (value.opcode == Opcode::ResultAt)
        return !generator.mayBeUnspecified(id) && isPinnedWithOperation(generator, loweredValues[id.index()], value.resultsOperand());

    switch (resultRuleOf(value.opcode)) {
    case ResultRule::Constant:
    case ResultRule::Allocation:
        return true;
    case ResultRule::Natural:
    case ResultRule::Declared:
    case ResultRule::Unknown:
        return isPinnedWithOperation(generator, loweredValues[id.index()], id);
    case ResultRule::None:
    case ResultRule::Cast:
    case ResultRule::Results:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool typeHoldsWhereverComputed(CodeGenerator& generator, std::span<const LValue> loweredValues, ValueID id, PublishedValue published)
{
    const Function& function = generator.function();
    ValueID converted = std::exchange(id, valueBeforeConversions(function, id));
    if (hasUnconditionalType(generator, loweredValues, id, converted == id ? published : PublishedValue::DerivedFromIt))
        return true;

    const Value& value = function.at(id);
    if (resultRuleOf(value.opcode) != ResultRule::Natural || !generator.analyses().hasNoEffects(id))
        return false;

    bool holdsForOperands = true;
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        holdsForOperands &= value.operandKind(operandIndex) != OperandKind::Guard
            && hasUnconditionalType(generator, loweredValues, valueBeforeConversions(function, operand), PublishedValue::DerivedFromIt);
    });
    return holdsForOperands;
}

static std::optional<B3::IntRange> integerRangeWithin(const Type& type, Range widest)
{
    if (!type.isInteger())
        return std::nullopt;
    Range range = type.integers.intersectWith(widest);
    if (range.isEmpty() || range == widest)
        return std::nullopt;
    return B3::IntRange(range.minimum, range.maximum);
}

static std::optional<B3::IntRange> knownRangeOf(const Type& type, Representation representation)
{
    switch (representation) {
    case Representation::Boolean:
        return B3::IntRange(0, 1);
    case Representation::Int32:
        return integerRangeWithin(type, Range::int32());
    case Representation::Int64:
        return integerRangeWithin(type, Range::full());
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static unsigned leastCellSizeOf(const Program& program, const Placement& placement, const Type& type)
{
    constexpr unsigned headerSize = sizeof(JSCell);

    bool hasOnlyCells = !(type.primitives & ~cellPrimitives);
    ASSERT(hasOnlyCells);
    if (!hasOnlyCells)
        return 0;
    if (type.environment)
        return placement.layout(type.environment).size;
    if (type.isAnyString() || type.isOpaque(OpaqueKind::FlatString))
        return sizeof(JSString);
    if (!type.hasOnlyLayouts(program))
        return headerSize;

    std::optional<unsigned> least;
    for (LayoutID member : membersOf(program, type)) {
        const PlannedLayout& layout = placement.layout(member);
        unsigned size = layout.hasSlots() ? layout.size : headerSize;
        least = least ? std::min(*least, size) : size;
    }
    return least.value_or(0);
}

void CodeGenerator::publishKnownProperties(ValueID id, LValue lowered)
{
    publishKnownProperties(id, Operand(function().at(id).representation, lowered), false);
}

void CodeGenerator::publishKnownPropertiesOfConvertedValue(ValueID id, Operand converted)
{
    publishKnownProperties(id, converted, true);
}

void CodeGenerator::publishKnownProperties(ValueID id, Operand operand, bool isConverted)
{
    LValue lowered = operand.value;
    if (lowered->isConstant())
        return;

    const Type& type = typeOf(id);
    Representation representation = operand.representation;
    std::optional<B3::IntRange> range = B3::mayHaveKnownRange(lowered->opcode()) ? knownRangeOf(type, representation) : std::nullopt;
    unsigned bytes = representation == Representation::Cell ? leastCellSizeOf(program(), m_inputs.program.placement, type) : 0;
    if (!range && !bytes)
        return;
    if (!typeHoldsWhereverComputed(*this, m_values.span(), id, isConverted ? PublishedValue::DerivedFromIt : PublishedValue::Itself))
        return;

    if (range)
        m_proc.setKnownRange(lowered, *range);
    if (bytes) {
        m_proc.setKnownDereferenceableBytes(lowered, bytes);
        build().statistics().count(StatisticID::ReferencesPublishedAsDereferenceable, 1);
    }
}

static std::optional<int64_t> constantStepOf(const InductionStep& step, Representation representation)
{
    if (step.kind != InductionStepKind::Constant)
        return std::nullopt;
    CheckedInt64 amount = step.isSubtracted ? CheckedInt64(0) - step.constant : CheckedInt64(step.constant);
    if (amount.hasOverflowed() || !amount.value())
        return std::nullopt;
    if (representation == Representation::Int32 && !isRepresentableAs<int32_t>(amount.value()))
        return std::nullopt;
    return amount.value();
}

void CodeGenerator::publishInductionVariables()
{
    for (const LoopInduction& loop : analyses().inductionVariables().loops()) {
        for (const InductionVariable& counter : loop.variables) {
            if (counter.steppedValues.size() != 1)
                continue;
            const InductionTest* test = counter.boundingTest();
            if (!test)
                continue;

            ValueID next = counter.steppedValues.first();
            Representation representation = representationOf(counter.parameter);
            if (!integerRepresentations.contains(representation) || representationOf(next) != representation)
                continue;
            std::optional<int64_t> step = constantStepOf(counter.step, representation);
            if (!step)
                continue;

            LValue phi = m_values[counter.parameter.index()];
            LValue nextValue = m_values[next.index()];
            LValue testValue = m_values[test->comparison.index()];
            if (!phi || !nextValue || !testValue || phi->opcode() != B3::Phi)
                continue;

            m_proc.setKnownInduction(B3::InductionRecord {
                .phi = phi,
                .next = nextValue,
                .test = testValue,
                .step = *step,
                .doesNotWrap = true,
                .relationHoldsOnEntry = test->entryRelation == EntryRelation::Holds,
            });
        }
    }
}

void CodeGenerator::checkInlineStores(const OperationContext& context, const DecorationMark& mark)
{
    struct StorePositions {
        unsigned begin;
        unsigned end;
    };

    DecorationMark end = m_heaps.markDecorations();
    auto forEachStoreSince = [&](const DecorationMark& begin, const auto& functor) {
        m_heaps.forEachWriteDecoratedSince(DecorationMark { begin.memory, end.cCallWrites, end.patchpointWrites }, functor);
    };
    auto numberOfStoresSince = [&](const DecorationMark& begin) {
        unsigned count = 0;
        forEachStoreSince(begin, [&](const B3::AbstractHeap&, B3::Value*) {
            ++count;
        });
        return count;
    };

    unsigned numberOfStores = numberOfStoresSince(mark);
    if (!numberOfStores)
        return;

    // A mark counts loads too, so a boundary is located by the number of stores that follow it.
    auto positionOf = [&](const DecorationMark& boundary) {
        return numberOfStores - std::min(numberOfStores, numberOfStoresSince(boundary));
    };
    Vector<StorePositions, 2> exemptPositions;
    for (const ExemptStores& exempt : m_exemptStores)
        exemptPositions.append({ positionOf(exempt.begin), positionOf(exempt.end) });

    LocationMasks declared = context.effects().onAnyEdge().writes;
    unsigned position = 0;
    forEachStoreSince(mark, [&](const B3::AbstractHeap& heap, B3::Value*) {
        unsigned current = position++;
        bool isExempt = exemptPositions.containsIf([&](const StorePositions& exempt) {
            return exempt.begin <= current && current < exempt.end;
        });
        if (!isExempt && !m_heaps.isCoveredBy(heap, declared))
            fail(context, InternalErrorKind::StoreOutsideDeclaredEffects);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
