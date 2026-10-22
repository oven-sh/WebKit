/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayCapacities.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTKnownValue.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr Range plannedTermRange { -(int64_t { 1 } << 32), int64_t { 1 } << 32 };

static std::optional<AbstractHeap> heapMeasuredByGetter(const Program& program, const Value& call)
{
    if (!call.receiverOperand() || call.needsPristineReceiver())
        return std::nullopt;
    std::optional<InlineBuiltin> getter = inlineImplementationOf(program, call);
    if (getter == InlineBuiltin::ArrayLength)
        return AbstractHeap::ArrayStorage;
    if (getter == InlineBuiltin::TypedArrayLength)
        return AbstractHeap::TypedArrayStorage;
    if (getter == InlineBuiltin::MapSize || getter == InlineBuiltin::SetSize)
        return AbstractHeap::CollectionStorage;
    return std::nullopt;
}

std::optional<MeasuredStorage> storageMeasuredBy(const Program& program, const Value& value)
{
    switch (value.opcode) {
    case Opcode::GetArrayLength:
        return MeasuredStorage { value.arrayOperand(), value.guardOperand(), AbstractHeap::ArrayStorage };
    case Opcode::GetTypedArrayLength:
        return MeasuredStorage { value.viewOperand(), { }, AbstractHeap::TypedArrayStorage };
    case Opcode::CallBuiltin:
        if (std::optional<AbstractHeap> heap = heapMeasuredByGetter(program, value))
            return MeasuredStorage { value.receiverOperand(), { }, *heap };
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

static bool canMeasureAt(FunctionAnalyses& analyses, const MeasuredStorage& measured, ProgramPoint point)
{
    if (!analyses.dominates(measured.owner, point))
        return false;
    return !measured.guard || (analyses.dominates(measured.guard, point) && analyses.isGuardAvailable(measured.guard, point));
}

ValueID checkInFrontOf(const Function& function, const MeasuredStorage& measured)
{
    return !measured.guard && function.at(measured.owner).opcode == Opcode::RequireObjectCoercible ? measured.owner : ValueID();
}

// Nothing that anybody could tell lies between the point and the value, and no other way leads on from the point. So what the value does, a throw too, may as well be done at the point.
bool ArrayCapacityPlanner::followsWithoutVisibleEffect(ProgramPoint point, ValueID id)
{
    ValueLocation target = m_analyses.useLists().locationOf(id);
    const std::optional<Target>& handler = m_function.at(point.block).handler;
    BlockID block = point.block;
    uint32_t index = point.index + 1;
    for (unsigned numberOfBlocks = 0; numberOfBlocks < 4; ++numberOfBlocks) {
        const Block& current = m_function.at(block);
        if (current.handler.has_value() != handler.has_value() || (handler && current.handler->block != handler->block))
            return false;
        uint32_t end = block == target.block ? target.index : current.values.size();
        for (; index < end; ++index) {
            ValueID between = current.values[index];
            if (m_function.at(between).opcode != Opcode::LoopHint && !m_analyses.isUnobservable(between))
                return false;
        }
        if (block == target.block)
            return true;
        if (current.terminator.kind != TerminatorKind::Jump)
            return false;
        block = current.terminator.targets[0].block;
        index = 0;
    }
    return false;
}

bool ArrayCapacityPlanner::canMeasureBehindCheck(const MeasuredStorage& measured, ProgramPoint point)
{
    ValueID check = checkInFrontOf(m_function, measured);
    if (!check || m_function.at(point.block).handler)
        return false;
    return m_analyses.dominates(m_function.at(check).valueOperand(), point) && followsWithoutVisibleEffect(point, check);
}

static const NaturalLoop* findOutermostLoopExcluding(const NaturalLoops& loops, BlockID block, BlockID excludedBlock)
{
    const NaturalLoop* outermost = nullptr;
    for (const NaturalLoop* loop = loops.innerMostLoopOf(block); loop && !loops.belongsTo(excludedBlock, *loop); loop = loops.innerMostOuterLoop(*loop))
        outermost = loop;
    return outermost;
}

ValueID appendedArrayOf(const Value& append)
{
    switch (append.opcode) {
    case Opcode::CallBuiltin:
        return append.receiverOperand();
    case Opcode::ElemSet:
        return append.objectOperand();
    default:
        return append.arrayOperand();
    }
}

static bool mayWrite(const EffectsOnEdges& effects, AbstractHeap heap)
{
    return effects.normal.writes.contains(heap) || effects.handler.writes.contains(heap);
}

static bool mayChangeLengthIn(FunctionAnalyses& analyses, const NaturalLoop& loop, const MeasuredStorage& measured, ValueID literal, NOESCAPE const Invocable<bool(const Value&)> auto& isAppend)
{
    const Function& function = analyses.function();
    auto mayChangeLength = [&](ValueID id) {
        const EffectsOnEdges& effects = analyses.effectsOf(id);
        if (!mayWrite(effects, measured.heap))
            return false;

        // A capacity is a prediction: a callback is taken not to change the array that is walked for it.
        const Value& writer = function.at(id);
        if (effects.onAnyEdge().mayRunOtherCode())
            return false;
        if (writer.opcode == Opcode::MapNext || writer.opcode == Opcode::SetNext)
            return false;
        if (!isAppend(writer))
            return true;
        ValueID appended = appendedArrayOf(writer);
        if (!appended)
            return true;

        // The definition of the measured object dominates the literal, so the literal is a newer object.
        if (analyses.sameObjectThroughCasts(appended) == literal)
            return false;
        return analyses.escape().mayAlias(appended, measured.owner);
    };

    BitVector rareBlocks = blocksRarerThanHeader(analyses, loop);
    for (unsigned i = 0; i < loop.size(); ++i) {
        BlockID block = loop.at(i);
        if (rareBlocks.get(block.index()))
            continue;
        if (mayWrite(analyses.effectsOfTerminator(block), measured.heap) || std::ranges::any_of(function.at(block).values, mayChangeLength))
            return true;
    }
    return false;
}

// An exit outside the header that is rarer than the loop does not count: where a type test fails, the loop continues in its generic copy.
static bool hasOnlyRareSideExits(FunctionAnalyses& analyses, const NaturalLoop& loop)
{
    const Function& function = analyses.function();
    const NaturalLoops& loops = analyses.naturalLoops();
    const BlockFrequencies& frequencies = analyses.blockFrequencies();
    BlockFrequency frequencyOfLoop = frequencies.classOf(loop.header());
    for (unsigned i = 0; i < loop.size(); ++i) {
        BlockID block = loop.at(i);
        if (block == loop.header() || frequencyOfLoop < frequencies.classOf(block))
            continue;
        const Terminator& terminator = function.at(block).terminator;
        if (returnsToCaller(terminator.kind))
            return false;
        for (const Target& target : terminator.targets) {
            if (!loops.belongsTo(target.block, loop) && !(frequencyOfLoop < frequencies.classOf(target.block)))
                return false;
        }
    }
    return true;
}

static ValueID walkedCollectionOf(const Value& step)
{
    switch (step.opcode) {
    case Opcode::MapNext:
        return step.mapOperand();
    case Opcode::SetNext:
        return step.setOperand();
    default:
        return { };
    }
}

std::optional<Value> makeSizeReadOfWalkedCollection(FunctionPassContext& context, const Value& step)
{
    ValueID collection = walkedCollectionOf(step);
    if (!collection)
        return std::nullopt;
    return makeSizeRead(context, collection, step.opcode == Opcode::MapNext ? Builtins::MapPrototypeSize : Builtins::SetPrototypeSize);
}

std::optional<Value> makeSizeRead(FunctionPassContext& context, ValueID collection, BuiltinID getter)
{
    TypingContext typing { context.program, context.function, context.types };
    BuiltinOperandTypes operands = operandTypesOf(typing, collection, { });
    OverloadMatch match = matchOverload({ typing, getter, BuiltinCallMode::Get, operands, operands });
    if (match.state != BuiltinMatchState::Proven || !match.requirements.isEmpty())
        return std::nullopt;
    Value sizeRead = makeBuiltinCall(match.overload, BuiltinCallForm::Whole, { collection, { } }, { });
    if (!overloadTakesOperands(typing, sizeRead))
        return std::nullopt;
    return sizeRead;
}

static_assert(static_cast<uint32_t>(MapNextResult::HasEntry) == static_cast<uint32_t>(SetNextResult::HasEntry));

// The step through a Map or a Set in the header of a loop that is left only where the step finds no further entry.
static ValueID findStepControlling(FunctionAnalyses& analyses, const NaturalLoop& loop)
{
    const Function& function = analyses.function();
    const NaturalLoops& loops = analyses.naturalLoops();
    BlockID header = loop.header();
    const Terminator& terminator = function.at(header).terminator;
    if (terminator.kind != TerminatorKind::Branch || !hasOnlyRareSideExits(analyses, loop))
        return { };
    if (!loops.belongsTo(terminator.taken().block, loop) || loops.belongsTo(terminator.notTaken().block, loop))
        return { };

    const Value& hasEntry = function.at(terminator.conditionOperand());
    if (hasEntry.opcode != Opcode::ResultAt || hasEntry.index() != static_cast<uint32_t>(MapNextResult::HasEntry))
        return { };
    ValueID step = hasEntry.resultsOperand();
    if (!walkedCollectionOf(function.at(step)) || analyses.useLists().blockOf(step) != header)
        return { };
    return step;
}

TripCount ArrayCapacityPlanner::tripCountOfCollectionWalk(const PlannedLiteral& literal, const NaturalLoop& loop, const AppendSite& site)
{
    if (site.kind == AppendKind::StoreAtIndex)
        return { };
    ValueID step = findStepControlling(m_analyses, loop);
    if (!step || !makeSizeReadOfWalkedCollection(m_context, m_function.at(step)))
        return { };

    MeasuredStorage measured { walkedCollectionOf(m_function.at(step)), { }, AbstractHeap::CollectionStorage };
    auto isAppend = [&](const Value& value) {
        return appendKindOf(value).has_value();
    };
    if (!canMeasureAt(m_analyses, measured, literal.point) || mayChangeLengthIn(m_analyses, loop, measured, literal.value, isAppend))
        return { };
    return { .added = { CapacityTermKind::Integer, step } };
}

LoopCounter ArrayCapacityPlanner::findUnitStepCounter(const NaturalLoop& loop)
{
    for (const InductionVariable& variable : m_analyses.inductionVariables().of(loop).variables) {
        if (variable.step.kind != InductionStepKind::Constant || (variable.step.constant != 1 && variable.step.constant != -1))
            continue;
        for (const InductionTest& test : variable.tests) {
            if (test.tested != TestedValue::BeforeStep || !isBoundingRelation(test.relation, variable.direction))
                continue;
            if (test.exits == LoopExits::OnlyAtTest || (test.block == loop.header() && hasOnlyRareSideExits(m_analyses, loop)))
                return { &variable, &test };
        }
    }
    return { };
}

TripCount ArrayCapacityPlanner::tripCount(const PlannedLiteral& literal, const NaturalLoop& loop, const AppendSite& site)
{
    auto [counter, test] = findUnitStepCounter(loop);
    if (!counter)
        return tripCountOfCollectionWalk(literal, loop, site);

    CapacityTerm start = termOf(literal, counter->start);
    CapacityTerm bound = termOf(literal, test->bound);
    if (start.kind == CapacityTermKind::Number)
        return { };

    int64_t adjustment = test->relation == Opcode::LessEq || test->relation == Opcode::GreaterEq;
    if (site.kind != AppendKind::StoreAtIndex) {
        if (counter->step.constant > 0)
            return { bound, start, adjustment };
        return { start, bound, adjustment };
    }

    if (counter->step.constant < 0 || start.kind != CapacityTermKind::Constant || start.constant < 0)
        return { };
    ValueID key = m_analyses.identityOf(site.operand);
    if (key == counter->parameter)
        return { .added = bound, .adjustment = adjustment };
    if (isLengthOf(key, literal.value))
        return { bound, start, adjustment };
    return { };
}

// What a parameter of a block is given on the edges that are as likely as the block, where that is one value.
ValueID ArrayCapacityPlanner::valueOnLikelyEdges(ValueID id)
{
    if (m_function.at(id).opcode != Opcode::Parameter)
        return id;
    const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
    BlockID block = m_analyses.useLists().blockOf(id);
    uint32_t parameterIndex = static_cast<uint32_t>(m_function.at(block).parameters.find(id));
    ValueID given;
    for (Edge edge : m_analyses.cfg().incomingEdges(block)) {
        if (frequencies.classOf(block) < frequencies.classOf(edge.from))
            continue;
        ValueID passed = valuePassedOn(m_function, edge, parameterIndex);
        if (!passed || (given && given != passed))
            return id;
        given = passed;
    }
    return given ? given : id;
}

CapacityTerm ArrayCapacityPlanner::termOf(const PlannedLiteral& literal, ValueID id)
{
    if (!id)
        return { };
    id = valueOnLikelyEdges(m_analyses.sameObjectThroughCasts(id));

    KnownValue known = knownValueOf(m_context.program, m_function, id);
    if (known.kind() == KnownValue::Kind::Integer) {
        if (!plannedTermRange.contains(known.asInteger()))
            return { };
        return { CapacityTermKind::Constant, { }, known.asInteger() };
    }

    ValueID definition = m_analyses.sameObjectThroughCasts(id);
    const Type& type = m_context.program.typeOf(m_function, definition);
    bool isAvailable = m_analyses.dominates(definition, literal.point);
    if (isAvailable && type.isInteger() && type.integers.isSubsetOf(plannedTermRange))
        return { CapacityTermKind::Integer, definition };

    std::optional<MeasuredStorage> measured = storageMeasuredBy(m_context.program, m_function.at(definition));
    if (measured && (canMeasureAt(m_analyses, *measured, literal.point) || canMeasureBehindCheck(*measured, literal.point))) {
        const NaturalLoop* loop = findOutermostLoopExcluding(m_analyses.naturalLoops(), m_analyses.useLists().blockOf(definition), literal.point.block);
        auto isAppend = [&](const Value& value) {
            return appendKindOf(value).has_value();
        };
        if (!loop || !mayChangeLengthIn(m_analyses, *loop, *measured, literal.value, isAppend))
            return { CapacityTermKind::Integer, definition };
    }

    if (isAvailable && type.isNumber())
        return { CapacityTermKind::Number, definition };
    return { };
}

bool ArrayCapacityPlanner::isLengthOf(ValueID length, ValueID array) const
{
    std::optional<MeasuredStorage> measured = storageMeasuredBy(m_context.program, m_function.at(length));
    return measured && measured->heap == AbstractHeap::ArrayStorage && m_analyses.sameObjectThroughCasts(measured->owner) == array;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
