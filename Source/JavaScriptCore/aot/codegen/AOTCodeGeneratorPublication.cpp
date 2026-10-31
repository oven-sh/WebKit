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

static bool receivesMarkedValue(const Function& function, const BitVector& markedValues, Edge edge, uint32_t parameterIndex)
{
    IncomingValue incoming = incomingValueOf(function, edge, parameterIndex);
    switch (incoming.kind) {
    case IncomingValue::Kind::Argument:
        return markedValues.get(incoming.argument.index());
    case IncomingValue::Kind::EdgeResult:
        break;
    }

    EdgeResult result = edgeResultAt(function, incoming.result);
    switch (result.kind) {
    case EdgeResultKind::NarrowedOperand:
        return markedValues.get(result.subject.index());
    case EdgeResultKind::Guard:
    case EdgeResultKind::DeclaredResult:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool returnsMarkedOperand(const Function& function, const Value& value, const BitVector& markedValues)
{
    if (!hasRegister(value.representation))
        return false;
    bool returnsMarked = false;
    forEachReturnedOperand(function, value, [&](ValueID operand) {
        returnsMarked |= markedValues.get(operand.index());
    });
    return returnsMarked;
}

// A validating build marks a read that it moved to where the program does not read. The type does not say so: a pass that saw the mark would decide otherwise than in any other build.
static BitVector collectValuesValidatedWhereLookedAt(const Function& function, const CFG& cfg)
{
    BitVector markedValues;
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (function.at(id).opcode == Opcode::GetModuleVariable && function.at(id).isValidatedWhereLookedAt())
                markedValues.set(id.index());
        }
    }
    if (markedValues.isEmpty())
        return markedValues;

    bool changed = true;
    while (changed) {
        changed = false;
        for (BlockID id : cfg.reversePostOrder()) {
            const Block& block = function.at(id);
            for (uint32_t parameterIndex = 0; parameterIndex < block.parameters.size(); ++parameterIndex) {
                for (Edge edge : cfg.incomingEdges(id)) {
                    if (receivesMarkedValue(function, markedValues, edge, parameterIndex))
                        changed |= markedValues.add(block.parameters[parameterIndex].index());
                }
            }
            for (ValueID value : block.values) {
                if (returnsMarkedOperand(function, function.at(value), markedValues))
                    changed |= markedValues.add(value.index());
            }
        }
        changed &= cfg.hasCycle();
    }
    return markedValues;
}

bool CodeGenerator::isValidatedWhereLookedAt(ValueID id)
{
    if (typeOf(id).mayBeUnspecified())
        return true;
    if (!m_valuesValidatedWhereLookedAt)
        m_valuesValidatedWhereLookedAt = collectValuesValidatedWhereLookedAt(function(), analyses().cfg());
    return m_valuesValidatedWhereLookedAt->get(id.index());
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
    return effects.mayThrowOtherThanStackOverflow() || effects.mayBeTerminated || effects.mayRunOtherCode() || effects.mayCollect || !effects.writes.isEmpty();
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
        return !generator.typeOf(id).mayBeUnspecified() && (published == PublishedValue::Itself || !generator.hasTypeOnlyBehindTest(id));
    if (value.opcode == Opcode::ResultAt)
        return !generator.typeOf(id).mayBeUnspecified() && isPinnedWithOperation(generator, loweredValues[id.index()], value.resultsOperand());

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
        holdsForOperands &= value.operandKind(operandIndex) != OperandKind::Guard && value.operandKind(operandIndex) != OperandKind::Fact
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
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
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

    ASSERT(!(type.primitives & ~cellPrimitives));
    // The word is zero for a built-in that the realm lacks, or whose property held another value when it was first looked up.
    if (!type.builtins.isEmpty())
        return 0;
    if (type.environment)
        return placement.layout(type.environment).size;
    if (type.isAnyString() || type.isOpaque(OpaqueKind::FlatString))
        return sizeof(JSString);
    if (!type.hasOnlyLayouts())
        return headerSize;

    std::optional<unsigned> least;
    for (LayoutID member : membersOf(program, type)) {
        const PlannedLayout& layout = placement.layout(member);
        unsigned size = layout.holdsCompiledObjects() ? layout.size : headerSize;
        least = least ? std::min(*least, size) : size;
    }
    return least.value_or(0);
}

bool CodeGenerator::holdsTypeWhereverComputed(ValueID id)
{
    // A reference is used as it was lowered. Any other form has to be converted first, and B3 may do that earlier.
    PublishedValue published = function().at(id).representation == Representation::Cell ? PublishedValue::Itself : PublishedValue::DerivedFromIt;
    return !typeOf(id).mayBeUnspecified() && typeHoldsWhereverComputed(*this, m_values.span(), id, published);
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
    const Type& type = typeOf(id);
    if (lowered->isConstant() || type.mayBeUnspecified())
        return;

    Representation representation = operand.representation;
    std::optional<B3::IntRange> range = B3::mayHaveKnownRange(lowered->opcode()) ? knownRangeOf(type, representation) : std::nullopt;
    unsigned bytes = representation == Representation::Cell ? leastCellSizeOf(program(), m_inputs.program.placement, type) : 0;
    if (!range && !bytes)
        return;
    if (!typeHoldsWhereverComputed(*this, m_values.span(), id, isConverted ? PublishedValue::DerivedFromIt : PublishedValue::Itself))
        return;

    if (range && range->min() < range->max() && build().recordedOptions().isSabotaged(Sabotage::RangesEndOneEarly))
        range = B3::IntRange(range->min(), range->max() - 1);
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

// Once the other entries into such a cycle are removed, an edge that enters the loop today becomes a back edge, and it passes the counter unchanged.
static bool isOnCycleWithSeveralEntries(FunctionAnalyses& analyses, const LoopInduction& induction)
{
    const NaturalLoops& loops = analyses.naturalLoops();
    const NaturalLoop& loop = loops.loop(induction.loopIndex);
    auto enters = [&](Edge edge) {
        return !loops.belongsTo(edge.from, loop);
    };
    if (std::ranges::count_if(analyses.cfg().incomingEdges(induction.header), enters) <= 1)
        return false;

    const BlockCycles::Forest& forest = analyses.blockCycles().forest();
    for (unsigned cycle = forest.innermost(induction.header); cycle != BlockCycles::Forest::none; cycle = forest[cycle].parent) {
        if (!forest[cycle].isReducible())
            return true;
    }
    return false;
}

void CodeGenerator::publishInductionVariables()
{
    for (const LoopInduction& loop : analyses().inductionVariables().loops()) {
        if (loop.variables.isEmpty() || isOnCycleWithSeveralEntries(analyses(), loop))
            continue;
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
