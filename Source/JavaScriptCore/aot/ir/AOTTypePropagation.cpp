/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypePropagation.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFieldValueNarrowing.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGrouping.h"
#include "AOTObservableOperations.h"
#include "AOTPredictedValues.h"
#include "AOTResultRulesInternal.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <ranges>
#include <tuple>
#include <utility>
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t updatesBeforeJoiningBound = 8;
static constexpr uint32_t updatesBeforeRangeStays = 16;
static constexpr uint32_t notVisited = UINT32_MAX;
static constexpr uint32_t noComponent = UINT32_MAX - 1;

static_assert(!TypeTable::never.index());

static void forEachValueOf(const Block& block, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    std::ranges::for_each(block.parameters, functor);
    std::ranges::for_each(block.values, functor);
}

static void forEachListedValue(const Function& function, const CFG& cfg, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    for (BlockID id : cfg.reversePostOrder())
        forEachValueOf(function.at(id), functor);
}

static bool isSeldomTaken(const Function& function, Edge edge)
{
    return function.at(edge.from).frequency.predicted() != BlockFrequency::Normal;
}

static bool isEveryEdgeSeldomTaken(const Function& function, std::span<const Edge> edges)
{
    return std::ranges::all_of(edges, [&](Edge edge) {
        return isSeldomTaken(function, edge);
    });
}

TypePropagation::TypePropagation(const TypePropagationInputs& inputs)
    : m_inputs(inputs)
    , m_cfg(inputs.analyses.cfg())
    , m_verifies(inputs.build.options().verifiesAfterEveryPass)
{
    RELEASE_ASSERT(!inputs.memo || !inputs.knownPositions);
    if (!m_inputs.memo)
        m_inputs.memo = &m_ownMemo;
    if (const ChangeLog* log = inputs.function.changeLog())
        m_logAtStart = log->end();
}

TypePropagation::~TypePropagation()
{
    reportRetypedValues(false);
}

void TypePropagation::willTypeValues()
{
    if (!m_statesBeforeRestart.isEmpty())
        return;
    m_marks.grow(m_inputs.function.values.size());
    m_statesBeforeRestart.grow(m_inputs.function.values.size());
}

void TypePropagation::willMarkRestartedValues()
{
    if (std::exchange(m_marksRestartedValues, true))
        return;
    willTypeValues();
    m_isRestarted.ensureSize(m_inputs.function.values.size());
}

void TypePropagation::willRestartValues()
{
    willMarkRestartedValues();
    uses();
}

const UseLists& TypePropagation::uses()
{
    if (!m_uses)
        m_uses = &m_inputs.analyses.useLists();
    return *m_uses;
}

// A rule reads where a parameter stands, and of no other value.
ValueLocation TypePropagation::locationForRule(ValueID id) const
{
    if (m_uses)
        return m_uses->locationOf(id);
    if (m_inputs.function.at(id).opcode != Opcode::Parameter)
        return { ValuePlacement::InBlock, { }, 0 };
    PlaceOfParameter place = m_placesOfParameters[id.index()];
    return { ValuePlacement::Parameter, BlockID(place.block), place.index };
}

static void reportTypeOutOfDate(BuildContext& build, const InternalError& error)
{
    if (build.options().keepsGoingBehindFailedVerifications)
        build.diagnostics().keepFirstFailureOfFunction(error, error.kind);
    else
        build.diagnostics().add(error);
}

void TypePropagation::addTypeOutOfDate(ValueID id, ASCIILiteral detail)
{
    reportTypeOutOfDate(m_inputs.build, InternalError { .kind = InternalErrorKind::TypeOutOfDate, .function = m_inputs.analyses.subject().function, .value = id, .detail = detail });
}

void TypePropagation::reportRetypedValues(bool countsThem)
{
    RELEASE_ASSERT(!countsThem || !m_numberOfReportedValues);
    for (ValueID id : m_rememberedValues.span().subspan(m_numberOfReportedValues)) {
        const Value& value = m_inputs.function.at(id);
        const StateBeforeRestart& previous = m_statesBeforeRestart[id.index()];
        bool isRetyped = value.type != previous.type;
        bool isExpectedToBeOther = value.expected().predicted() != previous.expected();
        if (countsThem) {
            m_outcome.retypedValues += isRetyped;
            m_outcome.changedExpectedTypes += isExpectedToBeOther;
        }
        if (isRetyped || isExpectedToBeOther)
            m_inputs.function.didEdit(id, ValueEdit::Type, previous.type, previous.expected());
    }
    m_numberOfReportedValues = m_rememberedValues.size();
}

void TypePropagation::rememberState(ValueID id)
{
    MarksOfValue& marks = m_marks[id.index()];
    if (marks.isRemembered)
        return;
    marks.isRemembered = true;
    const Value& value = m_inputs.function.at(id);
    m_statesBeforeRestart[id.index()] = { value.type, value.expectedForTyping().predicted(), value.expectedOrTypeForTyping().predicted(), 0 };
    m_rememberedValues.append(id);
}

bool TypePropagation::isEveryValueTypedBehindItsSources() const
{
    const Function& function = m_inputs.function;
    BitVector isDefined;
    isDefined.ensureSize(function.values.size());
    bool isUsedTooEarly = false;
    auto use = [&](ValueID id, uint32_t = 0) {
        isUsedTooEarly |= id && !isDefined.quickGet(id.index());
    };
    for (BlockID blockID : m_cfg.reversePostOrder()) {
        const Block& block = function.at(blockID);
        for (ValueID parameter : block.parameters)
            isDefined.quickSet(parameter.index());
        for (ValueID id : block.values) {
            function.at(id).forEachOperand(use);
            isDefined.quickSet(id.index());
        }
        block.terminator.forEachOperand(use);
        for (const Target& target : block.terminator.targets)
            std::ranges::for_each(target.arguments, use);
        if (block.handler)
            std::ranges::for_each(block.handler->arguments, use);
    }
    return !isUsedTooEarly;
}

void TypePropagation::typeAllValues()
{
    m_didTypeAllValues = true;
    if (m_cfg.hasCycle() || !isEveryValueTypedBehindItsSources()) {
        restartAllValues();
        solveTypes();
        solveExpectedTypes();
        return;
    }

    willTypeValues();
    m_sweepFlags.fill(SweepFlag::IsChangedValue, m_inputs.function.values.size());
    for (const Block& block : m_inputs.function.blocks)
        forEachValueOf(block, [&](ValueID id) { restart(id); });
    sweep(TypeLevel::Proven);
    sweep(TypeLevel::Expected);
}

void TypePropagation::typeDependentsOf(std::span<const ValueID> changedValues, bool everyOtherValueIsAsTyped)
{
    m_everyOtherValueIsAsTyped = everyOtherValueIsAsTyped;
    if (m_cfg.hasCycle()) {
        restartDependentsOf(changedValues);
        solveTypes();
        solveExpectedTypes();
        return;
    }

    willTypeValues();
    m_sweepFlags.fill({ }, m_inputs.function.values.size());
    for (ValueID id : changedValues)
        m_sweepFlags[id.index()].add(SweepFlag::IsChangedValue);
    sweep(TypeLevel::Proven);
    sweep(TypeLevel::Expected);
}

void TypePropagation::sweep(TypeLevel level)
{
    const Function& function = m_inputs.function;
    TypingContext context = typingContext(level);
    bool isProven = level == TypeLevel::Proven;
    SweepFlag hasOtherType = isProven ? SweepFlag::HasOtherType : SweepFlag::HasOtherExpectedType;
    SweepFlag isTyped = isProven ? SweepFlag::IsTyped : SweepFlag::IsExpectedTyped;
    auto hasOperandTypedOtherwise = [&](const Instruction& user) {
        bool found = false;
        user.forEachOperand([&](ValueID operand, uint32_t) {
            OptionSet<SweepFlag> flags = m_sweepFlags[operand.index()];
            found |= flags.contains(hasOtherType) || (flags.contains(isTyped) && opcodesWithIndirectUsers.contains(function.at(operand).opcode));
        });
        return found;
    };
    auto hasArgumentTypedOtherwise = [&](BlockID block, uint32_t index) {
        return std::ranges::any_of(m_cfg.incomingEdges(block), [&](Edge edge) {
            const Target& target = targetOf(function, edge);
            if (index < target.arguments.size())
                return m_sweepFlags[target.arguments[index].index()].contains(hasOtherType);
            return !edge.isHandlerEdge() && hasOperandTypedOtherwise(function.at(edge.from).terminator);
        });
    };
    bool hasReachedChangedValue = false;
    auto isToSweep = [&](ValueID id, NOESCAPE const Invocable<bool()> auto& hasSourceTypedOtherwise) {
        OptionSet<SweepFlag> flags = m_sweepFlags[id.index()];
        hasReachedChangedValue |= flags.contains(SweepFlag::IsChangedValue);
        return hasReachedChangedValue && (flags.contains(SweepFlag::IsChangedValue) || (!isProven && flags.contains(SweepFlag::IsTyped)) || hasSourceTypedOtherwise());
    };

    for (BlockID blockID : m_cfg.reversePostOrder()) {
        const Block& block = function.at(blockID);
        for (uint32_t index = 0; index < block.parameters.size(); ++index) {
            ValueID id = block.parameters[index];
            if (isToSweep(id, [&] { return hasArgumentTypedOtherwise(blockID, index); }))
                storeSweptType(level, id, computeParameter(context, id, blockID, index));
        }
        for (ValueID id : block.values) {
            if (isToSweep(id, [&] { return hasOperandTypedOtherwise(function.at(id)); }))
                storeSweptType(level, id, computeValue(context, id));
        }
    }
}

TypeID TypePropagation::expectedTypeToStore(ValueID id, const Value& value, TypeID computed)
{
    TypeID expected = mayAllocateOtherLayout(m_inputs.program, value) ? computed : meetWithPreviousExpectation(id, computed);
    if (expected == TypeTable::never)
        return expected;
    expected = m_inputs.types.intersectionOf(value.type, expected);
    return expected == TypeTable::never ? TypeID() : narrowerTypeOrNull(expected, value.type);
}

void TypePropagation::storeSweptType(TypeLevel level, ValueID id, TypeID computed)
{
    rememberState(id);
    Value& value = m_inputs.function.editAndReportLater(id);
    const StateBeforeRestart& before = m_statesBeforeRestart[id.index()];
    OptionSet<SweepFlag>& flags = m_sweepFlags[id.index()];
    bool isChangedValue = !m_everyOtherValueIsAsTyped || flags.contains(SweepFlag::IsChangedValue);
    if (level == TypeLevel::Proven) {
        value.type = computed;
        flags.add(SweepFlag::IsTyped);
        if (isChangedValue || computed != before.type)
            flags.add(SweepFlag::HasOtherType);
        return;
    }
    value.setExpected(ExpectedType(expectedTypeToStore(id, value, computed)));
    flags.add(SweepFlag::IsExpectedTyped);
    if (isChangedValue || value.expectedOrTypeForTyping().predicted() != before.expectedOrType)
        flags.add(SweepFlag::HasOtherExpectedType);
}

void TypePropagation::restart(ValueID id)
{
    rememberState(id);
    Value& value = m_inputs.function.editAndReportLater(id);
    value.type = TypeTable::never;
    value.setExpected({ });
    if (m_marksRestartedValues)
        m_isRestarted.quickSet(id.index());
}

void TypePropagation::restartAllValues()
{
    willRestartValues();
    for (const Block& block : m_inputs.function.blocks)
        forEachValueOf(block, [&](ValueID id) { restart(id); });
}

void TypePropagation::restartDependentsOf(std::span<const ValueID> values)
{
    willRestartValues();
    for (ValueID id : values)
        enqueue(id);
    for (size_t index = 0; index < m_worklist.size(); ++index)
        enqueueUsersOf(m_worklist[index]);

    for (ValueID id : m_worklist) {
        m_marks[id.index()].isQueued = false;
        restart(id);
    }
    m_worklist.shrink(0);
}

void TypePropagation::restartSourcesOf(std::span<const ValueID> values, const BitVector& listed)
{
    // The users of a value are asked for where a value is typed from itself.
    willMarkRestartedValues();
    const Function& function = m_inputs.function;
    m_placesOfParameters.grow(function.values.size());
    for (BlockID block : m_cfg.reversePostOrder()) {
        const CompactList<ValueID>& parameters = function.at(block).parameters;
        for (uint32_t index = 0; index < parameters.size(); ++index)
            m_placesOfParameters[parameters[index].index()] = { block.index(), index };
    }
    m_queuedValues = QueuedValues::OnlyListed;
    m_listedValues = &m_isRestarted;
    auto visit = [&](ValueID id) {
        if (!id || !listed.get(id.index()) || m_isRestarted.quickGet(id.index()))
            return;
        restart(id);
        m_worklist.append(id);
    };
    std::ranges::for_each(values, visit);
    while (!m_worklist.isEmpty()) {
        ValueID id = m_worklist.takeLast();
        ValueLocation location = locationForRule(id);
        if (location.placement == ValuePlacement::Parameter)
            forEachSourceOfParameter(location.block, location.index, visit);
        else
            forEachSourceOf(id, visit);
    }
}

void TypePropagation::restoreTypes()
{
    for (ValueID id : m_rememberedValues) {
        Value& value = m_inputs.function.editAndReportLater(id);
        value.type = m_statesBeforeRestart[id.index()].type;
        value.setExpected(ExpectedType(m_statesBeforeRestart[id.index()].expectedForTyping));
    }
}

static uint64_t positionOf(const CFG& cfg, const ValueLocation& location)
{
    uint64_t isInBlock = location.placement == ValuePlacement::InBlock;
    return static_cast<uint64_t>(cfg.reversePostOrderIndex(location.block)) << 33 | isInBlock << 32 | location.index;
}

void TypePropagation::listFromLastToFirst(std::span<const ValueID> values)
{
    Vector<std::pair<uint64_t, ValueID>, 64> positions;
    for (ValueID id : values) {
        ValueLocation location = m_uses->locationOf(id);
        if (location.placement == ValuePlacement::Detached)
            continue;
        positions.append({ positionOf(m_cfg, location), id });
    }
    AOT::sortInLine(positions, std::ranges::greater { }, &std::pair<uint64_t, ValueID>::first);
    for (auto [position, id] : positions)
        m_restartedValuesFromLastToFirst.append(id);
}

void TypePropagation::listRestartedValuesInOrder()
{
    if (std::exchange(m_didListRestartedValues, true))
        return;

    const Function& function = m_inputs.function;
    constexpr size_t valuesWalkedForOneSorted = 8;
    if (m_uses && m_rememberedValues.size() * valuesWalkedForOneSorted < function.values.size()) {
        listFromLastToFirst(m_rememberedValues.span());
        return;
    }

    auto appendIfRestarted = [&](ValueID id) {
        if (m_isRestarted.quickGet(id.index()))
            m_restartedValuesFromLastToFirst.append(id);
    };
    for (BlockID id : m_cfg.reversePostOrder() | std::views::reverse) {
        std::ranges::for_each(function.at(id).values.span() | std::views::reverse, appendIfRestarted);
        std::ranges::for_each(function.at(id).parameters.span() | std::views::reverse, appendIfRestarted);
    }
}

void TypePropagation::enqueue(ValueID id)
{
    RELEASE_ASSERT(id.index() < m_statesBeforeRestart.size());
    if (m_queuedValues != QueuedValues::All && m_listedValues->quickGet(id.index()) != (m_queuedValues == QueuedValues::OnlyListed))
        return;
    MarksOfValue& marks = m_marks[id.index()];
    if (marks.isQueued)
        return;
    marks.isQueued = true;
    m_worklist.append(id);
}

void TypePropagation::enqueueEdgeResultParameters(BlockID from)
{
    if (passesNothing(from))
        return;
    const Function& function = m_inputs.function;
    const Terminator& terminator = function.at(from).terminator;
    for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
        const CompactList<ValueID>& parameters = function.at(terminator.targets[targetIndex].block).parameters;
        uint32_t first = firstEdgeResultParameter(function, from, targetIndex);
        uint32_t end = first + numberOfEdgeResults(function, from, targetIndex);
        for (uint32_t index = first; index < end && index < parameters.size(); ++index)
            enqueue(parameters[index]);
    }
}

void TypePropagation::enqueueIndirectUsersOf(ValueID id)
{
    const Function& function = m_inputs.function;
    const Value& value = function.at(id);
    if (!opcodesWithIndirectUsers.contains(value.opcode))
        return;
    bool isTest = provesOf(value.opcode) != Proof::None;
    bool hasMultipleResults = numberOfResultsOf(m_inputs.program, function, value).has_value();
    if (!isTest && !hasMultipleResults)
        return;

    for (const Use& use : uses().usesOf(id)) {
        if (hasMultipleResults && use.kind == Use::Kind::Operand)
            enqueue(use.user);
        if (isTest && use.kind == Use::Kind::TerminatorOperand && function.at(use.block).terminator.isConditionalBranch())
            enqueueEdgeResultParameters(use.block);
    }
}

void TypePropagation::enqueueUsersOf(ValueID id)
{
    const Function& function = m_inputs.function;
    for (const Use& use : uses().usesOf(id)) {
        switch (use.kind) {
        case Use::Kind::Operand:
            enqueue(use.user);
            enqueueIndirectUsersOf(use.user);
            break;
        case Use::Kind::TerminatorOperand:
            enqueueEdgeResultParameters(use.block);
            break;
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument: {
            if (passesNothing(use.block))
                break;
            const CompactList<ValueID>& parameters = function.at(targetOf(function, use.edge()).block).parameters;
            if (use.operandIndex < parameters.size())
                enqueue(parameters[use.operandIndex]);
            break;
        }
        case Use::Kind::BlockSubject:
            break;
        }
    }
}

// To put a parameter back costs a walk over what is queued, more than to join a few edges again: with 4 the build of the TypeScript compiler rises by 0.03%, with 2 by 0.26%.
AOT_THRESHOLD(size_t, fewestEdgesOfWideJoin, 16);

// A block may have as many incoming edges as the function has statements. Taken behind each source that moves, its parameter would be joined over all of them that many times:
// it goes back below the queued values that lie in front of the last of its sources. It waits for a value that is typed when it is taken, never for another that waits as it does:
// so it is not taken again before that value is.
NEVER_INLINE bool TypePropagation::letParameterOfWideJoinWait(ValueID parameter, BlockID block)
{
    uint32_t lastSource = 0;
    for (Edge edge : m_cfg.incomingEdges(block))
        lastSource = std::max(lastSource, m_cfg.reversePostOrderIndex(edge.from));
    bool waitsForValueThatIsTyped = false;
    size_t index = m_worklist.size();
    for (; index; --index) {
        ValueLocation location = locationForRule(m_worklist[index - 1]);
        if (location.placement != ValuePlacement::Detached && m_cfg.isReachable(location.block) && m_cfg.reversePostOrderIndex(location.block) > lastSource)
            break;
        waitsForValueThatIsTyped |= location.placement != ValuePlacement::Parameter || m_cfg.incomingEdges(location.block).size() < fewestEdgesOfWideJoin;
    }
    if (!waitsForValueThatIsTyped)
        return false;
    m_worklist.insert(index, parameter);
    m_marks[parameter.index()].isQueued = true;
    return true;
}

void TypePropagation::propagate(TypeLevel level, Direction direction)
{
    TypingContext context = typingContext(level);
    auto markIfNotTaken = [&](ValueID source) {
        if (!source)
            return;
        MarksOfValue& marks = m_marks[source.index()];
        if (marks.lastWalkThatTookIt != m_numberOfWalks)
            marks.hasUserThatWasTakenFirst = true;
    };
    while (!m_worklist.isEmpty()) {
        ValueID id = m_worklist.takeLast();
        MarksOfValue& marks = m_marks[id.index()];
        marks.isQueued = false;
        ValueLocation location = locationForRule(id);
        bool isTakenAgain = marks.lastWalkThatTookIt == m_numberOfWalks;
        if (!isTakenAgain && location.placement == ValuePlacement::Parameter)
            forEachSourceOfParameter(location.block, location.index, markIfNotTaken);
        else if (!isTakenAgain)
            forEachSourceOf(id, markIfNotTaken);
        else if (location.placement == ValuePlacement::Parameter && m_cfg.incomingEdges(location.block).size() >= fewestEdgesOfWideJoin && letParameterOfWideJoinWait(id, location.block))
            continue;
        marks.lastWalkThatTookIt = m_numberOfWalks;
        TypeID computed = compute(context, id, location);
        if (level == TypeLevel::Expected)
            computed = meetWithPreviousExpectation(id, computed);

        if (!update(level, direction, id, computed))
            continue;
        if (isTakenAgain || marks.hasUserThatWasTakenFirst) {
            enqueueUsersOf(id);
            continue;
        }
        if (m_verifies) [[unlikely]] {
            size_t numberOfWaitingValues = m_worklist.size();
            enqueueUsersOf(id);
            if (m_worklist.size() != numberOfWaitingValues)
                addTypeOutOfDate(id, "a user of the value was not waiting to be typed again"_s);
        }
    }
}

void TypePropagation::willTakeValuesFromWorklist()
{
    const Function& function = m_inputs.function;
    ++m_numberOfWalks;
    RELEASE_ASSERT(m_numberOfWalks < 1u << 29);
    if (std::exchange(m_didMarkSourcesOfUnreachableValues, true))
        return;

    if (m_cfg.reversePostOrder().size() == function.blocks.size())
        return;
    auto mark = [&](ValueID id, uint32_t = 0) {
        if (id)
            m_marks[id.index()].hasUserThatWasTakenFirst = true;
    };
    auto markWithOperands = [&](ValueID id, uint32_t) {
        mark(id);
        function.at(id).forEachOperand(mark);
    };
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        if (m_cfg.isReachable(BlockID(blockIndex)))
            continue;
        const Block& block = function.blocks[blockIndex];
        for (ValueID id : block.values)
            function.at(id).forEachOperand(markWithOperands);
        for (const Target& target : block.terminator.targets)
            std::ranges::for_each(target.arguments, mark);
        if (block.handler)
            std::ranges::for_each(block.handler->arguments, mark);
        block.terminator.forEachOperand(markWithOperands);
    }
}

void TypePropagation::solve(TypeLevel level)
{
    if (!m_marksRestartedValues)
        willRestartValues();
    willTakeValuesFromWorklist();
    listRestartedValuesInOrder();
    for (ValueID id : m_restartedValuesFromLastToFirst)
        enqueue(id);
    propagate(level, Direction::Ascending);

    for (ValueID value : m_valuesAboveTheirRules)
        enqueue(value);
    m_valuesAboveTheirRules.shrink(0);
    propagate(level, Direction::Descending);
    for (ValueID id : m_restartedValuesFromLastToFirst)
        m_statesBeforeRestart[id.index()].numberOfUpdates = 0;
}

void TypePropagation::solveExpectedTypes()
{
    willRestartValues();
    listRestartedValuesInOrder();
    for (ValueID id : m_restartedValuesFromLastToFirst)
        editTypeOf(id).setExpected(ExpectedType(TypeTable::never));
    solve(TypeLevel::Expected);
}

TypeID TypePropagation::meetWithPreviousExpectation(ValueID id, TypeID computed)
{
    TypeID previous = m_statesBeforeRestart[id.index()].expected();
    if (!previous || previous == computed || computed == TypeTable::never)
        return computed;
    const TypeArena& types = m_inputs.types;
    TypeID met = m_inputs.types.intersectionOf(previous, computed);
    if (met == TypeTable::never)
        return computed;
    bool disagreeAboutObjects = !types.at(met).hasObjects() && types.at(previous).hasObjects() && types.at(computed).hasObjects();
    return disagreeAboutObjects ? computed : met;
}

TypeID TypePropagation::compute(const TypingContext& context, ValueID id, const ValueLocation& location)
{
    return location.placement == ValuePlacement::Parameter ? computeParameter(context, id, location.block, location.index) : computeValue(context, id);
}

TypeID TypePropagation::computeValue(const TypingContext& context, ValueID id)
{
    const Value& value = context.function.at(id);
    TypeID type = computeResultTypeID(context, value);
    if (m_verifies && type != context.types.intern(context.types.adopt(computeResultType(context, value)))) [[unlikely]]
        addTypeOutOfDate(id, "the rule that answers with a type id disagrees with the rule that answers with a type"_s);
    if (type == TypeTable::never && context.level == TypeLevel::Expected && value.opcode == Opcode::AssumeType && context.typeIDOf(value.valueOperand()) != TypeTable::never)
        return value.type;
    return type;
}

ALWAYS_INLINE TypeID TypePropagation::computeIncomingType(const TypingContext& context, Edge edge, uint32_t index)
{
    const Target& target = targetOf(context.function, edge);
    if (index < target.arguments.size())
        return context.typeIDOf(target.arguments[index]);
    if (!edge.isHandlerEdge()) {
        EdgeResultPosition position { edge.from, edge.targetIndex, index - static_cast<uint32_t>(target.arguments.size()) };
        if (std::optional<EdgeResult> result = tryGetEdgeResult(context.function, position))
            return computeTypeOnEdge(context, position, *result);
    }
    return TypeTable::anything;
}

TypeID TypePropagation::computeParameter(const TypingContext& context, ValueID id, BlockID block, uint32_t index)
{
    const Function& function = context.function;
    if (block == entryBlock)
        return index < function.numberOfEntryParameters() ? declaredEntryTypeOf(context, function, index) : TypeTable::anything;

    auto incomingEdges = m_cfg.incomingEdges(block);
    bool skipsSeldomTakenEdges = context.level == TypeLevel::Expected && !isEveryEdgeSeldomTaken(function, incomingEdges);
    TypeID joined = TypeTable::never;
    for (Edge edge : incomingEdges) {
        if (passesNothing(edge.from) || (skipsSeldomTakenEdges && isSeldomTaken(function, edge)))
            continue;
        joined = joinIntoParameter(context, id, joined, computeIncomingType(context, edge, index));
    }
    return joined;
}

// Every cycle of values passes through a back edge. Growth before a parameter has already been widened there, so it terminates on its own.
bool TypePropagation::growsOnBackEdge(TypeLevel level, ValueID parameter, TypeID old, TypeID grown)
{
    ValueLocation location = locationForRule(parameter);
    if (location.placement != ValuePlacement::Parameter)
        return false;
    auto incomingEdges = m_cfg.incomingEdges(location.block);
    uint32_t position = m_cfg.reversePostOrderIndex(location.block);
    auto leadsBack = [&](Edge edge) {
        return m_cfg.reversePostOrderIndex(edge.from) >= position;
    };
    if (incomingEdges.size() < 2 || std::ranges::none_of(incomingEdges, leadsBack))
        return false;
    TypeArena& types = m_inputs.types;
    TypingContext context = typingContext(level);
    TypeID withForwardEdgeTypes = old;
    for (Edge edge : incomingEdges) {
        if (!leadsBack(edge) && !passesNothing(edge.from))
            withForwardEdgeTypes = types.unionOf(withForwardEdgeTypes, computeIncomingType(context, edge, location.index));
    }
    return types.unionOf(withForwardEdgeTypes, grown) != withForwardEdgeTypes;
}

TypeID TypePropagation::computeTypeOnEdge(const TypingContext& context, EdgeResultPosition position, const EdgeResult& result)
{
    auto computeByRule = [&] {
        return context.types.intern(context.types.adopt(Typed::computeTypeOnEdge(context, position)));
    };
    if (result.kind == EdgeResultKind::Guard)
        return TypeTable::guard;
    if (result.kind != EdgeResultKind::NarrowedOperand || !result.test)
        return computeByRule();

    constexpr uint32_t keyOfNarrowedOperand = UINT32_MAX - 16;
    const Value& test = context.function.at(result.test);
    auto addWords = [&](auto& hasher) {
        hasher.addNonIDWords(keyOfNarrowedOperand, static_cast<uint32_t>(test.opcode));
        hasher.add(result.operandIndex << 2 | static_cast<uint32_t>(result.side) << 1 | static_cast<uint32_t>(context.level), test.numberOfOperands());
        hasher.addPayloadOf(test);
        hasher.addForEachOperandOf(test, [&](ValueID operand) {
            return (operand ? context.typeIDOf(operand) : TypeID()).index();
        });
    };
    WordHasher hasher;
    addWords(hasher);

    TypeID known = context.memo->results.find(hasher.hash());
    if (known && !m_verifies) [[likely]]
        return known;
    TypeID type = sharedOrComputedType(context, SharedAnswers::AboutTypes, hasher.hash(), addWords, computeByRule);
    if (!known)
        context.memo->results.add(hasher.hash(), type);
    else if (known != type)
        addTypeOutOfDate(result.test, "the type remembered for the edge is not what the rule gives now"_s);
    return type;
}

TypeID TypePropagation::joinIntoParameter(const TypingContext& context, ValueID parameter, TypeID current, TypeID incoming)
{
    if (current == incoming || incoming == TypeTable::never)
        return current;
    if (current == TypeTable::never)
        return incoming;

    uint64_t key = TypeMemo::keyOf(current, incoming);
    if (TypeID known = context.memo->joins.find(key))
        return known;

    TypeArena& types = context.types;
    bool wasRestarted = parameter.index() < m_marks.size() && isRemembered(parameter);
    TypeID before = wasRestarted ? m_statesBeforeRestart[parameter.index()].type : context.function.at(parameter).type;
    // An answer that a limit decided belongs to this parameter alone.
    bool didWiden = false;
    bool didKeepListsOfTypeBefore = false;
    TypeID joined = types.sharedAnswer(SharedAnswers::AboutProgram, TypeArena::sharedKeyOf(SharedTypeMemo::Relation::Join, current, incoming), [&] {
        WideningContext widening { context.program, types, m_inputs.build, m_inputs.analyses.subject(parameter), &didWiden, before ? &types.at(before) : nullptr, &didKeepListsOfTypeBefore };
        TypeID computed = types.intern(types.at(current).unionWideningWith(types.at(incoming), widening));
        if (didWiden || didKeepListsOfTypeBefore)
            types.markCurrentAnswersUnshareable();
        return computed;
    });
    if (!didWiden && !didKeepListsOfTypeBefore)
        context.memo->joins.add(key, joined);
    return joined;
}

TypeID TypePropagation::widenGrownParameter(ValueID parameter, TypeID old, TypeID grown)
{
    TypeArena& types = m_inputs.types;
    const StateBeforeRestart& state = m_statesBeforeRestart[parameter.index()];
    TypeID bound = state.type;
    const Type& type = types.at(grown);

    Range widened = type.integers.widenedFrom(types.at(old).integers);
    if (bound && !widened.isEmpty() && type.integers.isSubsetOf(types.at(bound).integers))
        widened = widened.intersectWith(types.at(bound).integers);
    if (widened != type.integers) {
        Type withWidenedRange = type;
        withWidenedRange.integers = widened;
        grown = types.intern(WTF::move(withWidenedRange));
    }
    bool joinsBound = bound && state.numberOfUpdates >= updatesBeforeJoiningBound;
    return joinsBound ? types.unionOf(grown, bound) : grown;
}

// Round a cycle, a bound that was widened far out comes back by a step a round.
TypeID TypePropagation::keepRangeOfNarrowedParameter(ValueID parameter, TypeID old, TypeID narrowed)
{
    TypeArena& types = m_inputs.types;
    const Type& type = types.at(narrowed);
    Range before = types.at(old).integers;
    if (m_statesBeforeRestart[parameter.index()].numberOfUpdates < updatesBeforeRangeStays || type.integers.isEmpty() || type.integers == before)
        return narrowed;
    m_outcome.keptRangesOfNarrowedParameters = true;
    Type withRangeBefore = type;
    withRangeBefore.integers = before;
    return types.intern(WTF::move(withRangeBefore));
}

bool TypePropagation::update(TypeLevel level, Direction direction, ValueID id, TypeID computed)
{
    TypeArena& types = m_inputs.types;
    const Value& value = m_inputs.function.at(id);
    TypeID old = level == TypeLevel::Proven ? value.type : value.expectedOrTypeForTyping().predicted();

    auto isJoin = [&] {
        ValueLocation location = locationForRule(id);
        return location.placement == ValuePlacement::Parameter && m_cfg.incomingEdges(location.block).size() > 1;
    };
    TypeID next = computed;
    if (direction == Direction::Ascending) {
        next = types.unionOf(old, computed);
        if (next != old && old != TypeTable::never && growsOnBackEdge(level, id, old, next))
            next = widenGrownParameter(id, old, next);
        if (next != computed)
            m_valuesAboveTheirRules.append(id);
    } else if (next != old && isJoin())
        next = keepRangeOfNarrowedParameter(id, old, next);
    if (level == TypeLevel::Expected && next != old) {
        next = types.intersectionOf(value.type, next);
        if (next == TypeTable::never && (direction == Direction::Descending || computed != TypeTable::never))
            next = value.type;
    }
    if (next == old)
        return false;

    if (level == TypeLevel::Proven)
        editTypeOf(id).type = next;
    else
        editTypeOf(id).setExpected(ExpectedType(next));
    if (++m_statesBeforeRestart[id.index()].numberOfUpdates > m_inputs.build.limit(LimitID::MaxTypeValuesRounds)) {
        giveUpOnRounds(level, direction);
        return false;
    }
    return true;
}

void TypePropagation::giveUpOnRounds(TypeLevel level, Direction direction)
{
    Function& function = m_inputs.function;
    TypeArena& types = m_inputs.types;
    m_inputs.build.gaveUp(LimitID::MaxTypeValuesRounds, m_inputs.analyses.subject());
    m_outcome.gaveUpOnRounds = true;

    Vector<ValueID, 64> pendingValues = std::exchange(m_worklist, { });
    for (ValueID id : pendingValues)
        m_marks[id.index()].isQueued = false;
    m_valuesAboveTheirRules.clear();

    if (level == TypeLevel::Expected) {
        auto expectAsBefore = [&](ValueID id) {
            Value& value = editTypeOf(id);
            TypeID previous = m_statesBeforeRestart[id.index()].expected();
            value.setExpected(ExpectedType(previous ? types.intersectionOf(value.type, previous) : TypeID()));
        };
        if (direction == Direction::Ascending)
            std::ranges::for_each(m_restartedValuesFromLastToFirst | std::views::reverse, expectAsBefore);
        else
            std::ranges::for_each(pendingValues, expectAsBefore);
        return;
    }

    m_outcome.leftTypesAboveTheirRules = true;
    m_didTypeAllValues = true;
    forEachListedValue(function, m_cfg, [&](ValueID id) { rememberState(id); });

    TypingContext context = typingContext(TypeLevel::Proven);
    auto retype = [&](ValueID id, TypeID type) {
        if (function.at(id).type == type)
            return false;
        editTypeOf(id).type = type;
        return true;
    };
    auto widenParameter = [&](ValueID parameter, BlockID block, uint32_t index) {
        TypeID old = function.at(parameter).type;
        TypeID grown = types.unionOf(old, computeParameter(context, parameter, block, index));
        if (grown == old)
            return false;
        TypeID bound = m_statesBeforeRestart[parameter.index()].type;
        if (const Type& type = types.at(grown); type.isJavaScriptValue())
            grown = bound && type.isSubsetOf(types.at(bound), m_inputs.program) ? bound : types.withPrimitives(TypeTable::anything, type.primitives & Primitive::Absent);
        return retype(parameter, grown);
    };

    for (bool didChange = true; didChange;) {
        didChange = false;
        for (BlockID id : m_cfg.reversePostOrder()) {
            const Block& block = function.at(id);
            for (uint32_t index = 0; index < block.parameters.size(); ++index) {
                ValueID parameter = block.parameters[index];
                didChange |= id == entryBlock ? retype(parameter, computeParameter(context, parameter, id, index)) : widenParameter(parameter, id, index);
            }
            for (ValueID value : block.values)
                didChange |= retype(value, computeValue(context, value));
        }
    }
}

TypeID TypePropagation::narrowerTypeOrNull(TypeID narrower, TypeID type)
{
    if (narrower == TypeTable::never || narrower == type)
        return { };

    TypeMemo& memo = m_inputs.memo->narrowerTypes;
    uint64_t key = TypeMemo::keyOf(narrower, type);
    if (TypeID known = memo.find(key))
        return known == TypeTable::never ? TypeID() : known;
    const Program& program = m_inputs.program;
    bool isNarrower = program.at(narrower).isSubsetOf(program.at(type), program);
    memo.add(key, isNarrower ? narrower : TypeTable::never);
    return isNarrower ? narrower : TypeID();
}

void TypePropagation::dropExpectedTypesThatSayNothing()
{
    auto dropIfItSaysNothing = [&](ValueID id) {
        const Value& value = m_inputs.function.at(id);
        TypeID expected = value.expected().predicted();
        if (!expected)
            return;
        TypeID narrower = narrowerTypeOrNull(expected, value.type);
        if (narrower != expected)
            editTypeOf(id).setExpected(ExpectedType(narrower));
        if (!narrower && !isRemembered(id))
            ++m_outcome.changedExpectedTypes;
    };
    if (m_dropsExpectedTypesAtOnce)
        return;
    if (m_everyOtherValueIsAsTyped)
        std::ranges::for_each(m_rememberedValues, dropIfItSaysNothing);
    else
        forEachListedValue(m_inputs.function, m_cfg, dropIfItSaysNothing);
}

TypePropagationOutcome TypePropagation::finish()
{
    Vector<ValueID> parametersBehindRemovedEdges;
    finishWithoutCompacting(parametersBehindRemovedEdges);
    if (m_outcome.cutBlocks) {
        compactFunction(m_inputs.function, m_inputs.renumbering);
        m_inputs.analyses.didChangeControlFlow();
    }
    return m_outcome;
}

void TypePropagation::replaceUpcastsOfWidenedValues()
{
    Function& function = m_inputs.function;
    const TypeArena& types = m_inputs.types;
    auto mayBeWiderThanBefore = [&](ValueID id) {
        TypeID before = m_statesBeforeRestart[id.index()].type;
        TypeID now = function.at(id).type;
        if (!before || before == now)
            return false;
        const Type& typeBefore = types.at(before);
        const Type& type = types.at(now);
        // What a test leaves of a type need not lie inside the type: beside a class that no test tells, a class part loses its proofs.
        return (type.anyObject && !typeBefore.anyObject) || (type.anyFunction && !typeBefore.anyFunction) || (type.primitives & ~typeBefore.primitives) || !type.integers.isSubsetOf(typeBefore.integers)
            || type.classes != typeBefore.classes || type.layouts != typeBefore.layouts;
    };
    // A cast hands on what stands in front of its operand whichever cast it is, so the marks hold while casts are replaced.
    std::optional<PredictedValues> predicted;
    auto replaceIfItCutsValue = [&](ValueID upcast, ValueID id) {
        TypeID castType = function.at(upcast).castType();
        if (types.at(function.at(id).type).isSubsetOf(types.at(castType), m_inputs.program))
            return;
        if (!predicted)
            predicted.emplace(function, m_cfg);
        function.edit(upcast).becomeAssumeType(id, Proven::byRulesBeforeWidening(castType, predicted->isBehindCheck(id) ? ProofScope::BehindChecks : ProofScope::EveryVersion));
        ++m_outcome.replacedUpcasts;
    };
    const UseLists* useLists = m_inputs.analyses.useListsIfUpToDate();
    for (ValueID id : m_rememberedValues) {
        if (!mayBeWiderThanBefore(id))
            continue;
        if (!useLists) {
            // One walk finds the casts of every such value.
            for (BlockID block : m_cfg.reversePostOrder()) {
                for (ValueID upcast : function.at(block).values) {
                    if (function.at(upcast).opcode != Opcode::Upcast)
                        continue;
                    ValueID operand = function.at(upcast).valueOperand();
                    if (isRemembered(operand) && mayBeWiderThanBefore(operand))
                        replaceIfItCutsValue(upcast, operand);
                }
            }
            break;
        }
        for (const Use& use : useLists->usesOf(id)) {
            if (use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::Upcast)
                replaceIfItCutsValue(use.user, id);
        }
    }
    if (m_outcome.replacedUpcasts)
        m_inputs.analyses.didChangeValues();
}

TypePropagationOutcome TypePropagation::finishWithoutCompacting(Vector<ValueID>& parametersBehindRemovedEdges)
{
    replaceUpcastsOfWidenedValues();
    dropExpectedTypesThatSayNothing();
    reportRetypedValues(true);
    if (mayHaveCodeToCut()) {
        parametersBehindRemovedEdges = findParametersLeftAtNever();
        m_outcome.foundParametersLeftAtNever = !parametersBehindRemovedEdges.isEmpty();
        // All that was computed from them is never too. Nothing is cut before they are typed.
        if (!m_outcome.foundParametersLeftAtNever)
            cutAfterNeverValues();
    }

    if (!m_outcome.cutBlocks) {
        if (m_outcome.didChange())
            m_inputs.analyses.didRetypeValues();
        return m_outcome;
    }

    const Function& function = m_inputs.function;
    Vector<uint32_t, 32> numberOfIncomingEdgesBefore(FillWith { }, function.blocks.size(), 0u);
    for (BlockID id : m_cfg.reversePostOrder())
        numberOfIncomingEdgesBefore[id.index()] = m_cfg.incomingEdges(id).size();

    m_inputs.analyses.didChangeControlFlow();
    const CFG& cfg = m_inputs.analyses.cfg();
    for (BlockID id : cfg.reversePostOrder()) {
        if (cfg.incomingEdges(id).size() != numberOfIncomingEdgesBefore[id.index()])
            parametersBehindRemovedEdges.append(function.at(id).parameters.span());
    }
    return m_outcome;
}

bool waitsForGenericVersion(const Program& program, const Function& function)
{
    if (program.form() == IRForm::AsRead && !program.hasGuardedReads())
        return true;
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            Opcode opcode = function.at(id).opcode;
            return opcode == Opcode::Check || opcode == Opcode::Convert;
        });
    });
}

bool TypePropagation::mayHaveCodeToCut() const
{
    if (m_statesBeforeRestart.isEmpty() || m_rememberedValues.isEmpty())
        return !m_everyOtherValueIsAsTyped;
    const Function& function = m_inputs.function;
    return std::ranges::any_of(m_rememberedValues, [&](ValueID id) {
        return function.at(id).type == TypeTable::never || function.at(id).opcode == Opcode::NarrowByField;
    });
}

// Nothing comes to a block whose parameter is never: by what arrives now. A parameter may have been typed when only code that is never reached led to its block, a copy of a loop for integers of 32 bits
// behind `i > 2147483649`, and edges from code that runs have been led there since. Whoever did that has not said so if the parameter is still never here.
Vector<ValueID> TypePropagation::findParametersLeftAtNever()
{
    const Function& function = m_inputs.function;
    TypingContext context = typingContext(TypeLevel::Proven);
    Vector<ValueID> found;
    for (BlockID id : m_cfg.reversePostOrder()) {
        if (id == entryBlock)
            continue;
        const CompactList<ValueID>& parameters = function.at(id).parameters;
        for (uint32_t index = 0; index < parameters.size(); ++index) {
            if (function.at(parameters[index]).type == TypeTable::never && computeParameter(context, parameters[index], id, index) != TypeTable::never)
                found.append(parameters[index]);
        }
    }
    return found;
}

void TypePropagation::cutAfterNeverValues()
{
    const Function& function = m_inputs.function;
    std::optional<bool> waits;
    m_outcome.didLookForCodeToCut = true;
    auto isNever = [&](ValueID id) {
        return function.at(id).type == TypeTable::never;
    };
    TypingContext typing { m_inputs.program, function, m_inputs.types };
    auto isNeverReached = [&](ValueID id) {
        return narrowsHolderToNothing(typing, function.at(id));
    };

    for (BlockID id : m_cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        bool endsInUnreachable = block.terminator.kind == TerminatorKind::Unreachable;
        bool isNeverEntered = id != entryBlock && std::ranges::any_of(block.parameters, isNever);
        size_t index = isNeverEntered ? 0 : block.values.findIf([&](ValueID value) {
            return (isNever(value) && resultRuleOf(function.at(value).opcode) != ResultRule::None) || isNeverReached(value);
        });
        if (index == notFound)
            continue;
        bool keepsValue = !isNeverEntered && !isNeverReached(block.values[index]);
        if (endsInUnreachable && (isNeverEntered ? block.values.isEmpty() && !block.handler : keepsValue && index + 1 == block.values.size()))
            continue;
        if (!waits)
            waits = waitsForGenericVersion(m_inputs.program, function);
        if (*waits) {
            m_outcome.waitsToCutCode = true;
            return;
        }
        Origin origin = isNeverEntered ? block.terminator.origin() : function.at(block.values[index]).origin();
        for (ValueID value : block.values.span().subspan(index + keepsValue))
            explainOperationThatGoes(function, value, InPlaceOfOperation::NothingBecauseNothingComesHere);
        // The call stays in tail position, as in IRBuilder::demoteReturnCall(): its function is gone from the stack once the callee runs.
        if (keepsValue && block.terminator.kind == TerminatorKind::ReturnCall && block.terminator.callOperand() == block.values[index])
            m_inputs.function.edit(block.values[index], ValueEdit::Definition).isTailCall = true;
        // A read that only asks the tables runs nothing, so it does not throw, and the read that does stands behind its failing side, which goes. It becomes that read.
        if (keepsValue && function.at(block.values[index]).opcode == Opcode::PropGet && function.at(block.values[index]).asksOnlyTables())
            m_inputs.function.edit(block.values[index], ValueEdit::Definition).setAsksOnlyTables(false);
        Block& cut = m_inputs.function.edit(id);
        cut.values.shrink(index + keepsValue);
        // A handler sees the parameters of the block that it covers. Nothing is left here that throws.
        if (cut.values.isEmpty())
            cut.handler = std::nullopt;
        cut.terminator.become(TerminatorKind::Unreachable);
        cut.terminator.setCutBy(m_inputs.build.passToReportForUnreachableCode());
        cut.terminator.setOrigin(origin);
        ++m_outcome.cutBlocks;
    }
}

bool TypePropagation::hasTypesOfEveryArgument(BlockID block, uint32_t index, const Value& parameter) const
{
    const Function& function = m_inputs.function;
    std::span<const Edge> edges = m_cfg.incomingEdges(block);
    bool countsSeldomTakenEdges = isEveryEdgeSeldomTaken(function, edges);
    TypeID expected = parameter.expectedOrTypeForTyping().predicted();
    for (Edge edge : edges) {
        const Target& target = targetOf(function, edge);
        if (index >= target.arguments.size())
            return false;
        const Value& argument = function.at(target.arguments[index]);
        if (argument.type != parameter.type)
            return false;
        if ((countsSeldomTakenEdges || !isSeldomTaken(function, edge)) && argument.expectedOrTypeForTyping().predicted() != expected)
            return false;
    }
    return !edges.empty();
}

// A block that is reached no longer hands nothing on: what the log says of it is said of the blocks that it leads to through blocks which are not reached either. Each of those has lost an edge.
static void forEachReachedBlockBehind(const Function& function, const CFG& cfg, BlockID unreachedBlock, BitVector& didVisit, NOESCAPE const Invocable<void(BlockID)> auto& functor)
{
    Vector<BlockID, 8> unreachedBlocks { unreachedBlock };
    while (!unreachedBlocks.isEmpty()) {
        function.at(unreachedBlocks.takeLast()).forEachTarget([&](const Target& target) {
            if (!target.block)
                return;
            if (cfg.isReachable(target.block))
                functor(target.block);
            else if (!didVisit.set(target.block.index()))
                unreachedBlocks.append(target.block);
        });
    }
}

void TypePropagation::enqueueParametersOf(BlockID blockID)
{
    if (m_didEnqueueParametersOf.set(blockID.index()))
        return;
    const Function& function = m_inputs.function;
    if (!m_cfg.isReachable(blockID)) {
        forEachReachedBlockBehind(function, m_cfg, blockID, m_didEnqueueParametersOf, [&](BlockID reached) {
            enqueueParametersOf(reached);
        });
        return;
    }
    const CompactList<ValueID>& parameters = function.at(blockID).parameters;
    bool mayBeTypedFromItself = m_cycles && m_cycles->isInCycle(blockID);
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        ValueID id = parameters[index];
        if (!mayBeTypedFromItself && blockID != entryBlock && hasTypesOfEveryArgument(blockID, index, function.at(id)))
            continue;
        m_placesOfParameters[id.index()] = { blockID.index(), index };
        enqueue(id);
    }
}

void TypePropagation::enqueuePendingValues(PendingTyping& pending)
{
    const Function& function = m_inputs.function;
    size_t numberOfValues = function.values.size();
    willTypeValues();
    m_placesOfParameters.grow(numberOfValues);
    m_cycles = m_cfg.hasCycle() ? &m_inputs.analyses.blockCycles() : nullptr;

    if (pending.m_valuesTypedFromRecordsArePending) {
        enqueueParametersOf(entryBlock);
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            if (opcodesTypedFromNarrowedRecords().contains(function.values[index].opcode))
                enqueue(ValueID(index));
        }
    }

    auto enqueueIfTypedFromItself = [&](ValueID id) {
        if (!m_cycles)
            return;
        willRestartValues();
        ValueLocation location = m_uses->locationOf(id);
        if (location.placement == ValuePlacement::Detached || !m_cfg.isReachable(location.block))
            return;
        if (std::optional<unsigned> cycle = m_cycles->outermostCycleOf(location.block); cycle && componentOf(id, *cycle) != noComponent)
            enqueue(id);
    };

    BitVector hasEarlierEntry(numberOfValues);
    for (const ValueChange& change : pending.m_values) {
        if (change.edits.contains(ValueEdit::TypedByRule))
            continue;
        if (change.edits.contains(ValueEdit::OperandsOfSameTypes)) {
            enqueueIfTypedFromItself(change.value);
            continue;
        }
        const Value& value = function.at(change.value);
        if (value.opcode == Opcode::Parameter)
            willRestartValues();
        enqueue(change.value);
        if (change.edits.contains(ValueEdit::Definition) && opcodesWithIndirectUsers.contains(value.opcode)) {
            willRestartValues();
            enqueueIndirectUsersOf(change.value);
        }
        if (hasEarlierEntry.quickSet(change.value.index()) || change.edits.contains(ValueEdit::Added))
            continue;
        if (change.typeBefore != value.type || change.expectedBefore != value.expectedForTyping().predicted()) {
            willRestartValues();
            enqueueUsersOf(change.value);
        }
    }

    constexpr OptionSet<BlockEdit> editsOfOwnParameters { BlockEdit::Added, BlockEdit::Parameters, BlockEdit::Incoming };
    constexpr OptionSet<BlockEdit> editsOfWhatIsPassedOn { BlockEdit::Added, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments, BlockEdit::Rest };
    BitVector didLookAtTargetsOf;
    for (const BlockChange& change : pending.m_blocks) {
        if (change.edits.containsAny(editsOfOwnParameters))
            enqueueParametersOf(change.block);
        bool hasOperandsOfSameTypes = change.edits.contains(BlockEdit::OperandsOfSameTypes);
        if (hasOperandsOfSameTypes ? !m_cycles : !change.edits.containsAny(editsOfWhatIsPassedOn) || didLookAtTargetsOf.set(change.block.index()))
            continue;
        function.at(change.block).forEachTarget([&](const Target& target) {
            if (!target.block)
                return;
            if (!hasOperandsOfSameTypes)
                enqueueParametersOf(target.block);
            else if (m_cfg.isReachable(target.block) && m_cycles->isInCycle(target.block))
                std::ranges::for_each(function.at(target.block).parameters, enqueueIfTypedFromItself);
        });
    }
}

void TypePropagation::typePendingValues(PendingTyping& pending)
{
    const Function& function = m_inputs.function;
    m_everyOtherValueIsAsTyped = true;
    m_dropsExpectedTypesAtOnce = true;
    enqueuePendingValues(pending);
    if (m_worklist.isEmpty())
        return;
    if (m_cycles)
        willRestartValues();
    std::ranges::reverse(m_worklist);

    using ValueAtPosition = std::pair<uint64_t, ValueID>;
    Vector<ValueAtPosition, 16> valuesFromFirstToLast;
    auto takeNextValue = [&] {
        if (!m_uses)
            return m_worklist.isEmpty() ? ValueID() : m_worklist.takeLast();
        while (!m_worklist.isEmpty()) {
            ValueID id = m_worklist.takeLast();
            ValueLocation location = m_uses->locationOf(id);
            bool isListed = location.placement != ValuePlacement::Detached && m_cfg.isReachable(location.block);
            valuesFromFirstToLast.append({ isListed ? positionOf(m_cfg, location) : 0, id });
            std::ranges::push_heap(valuesFromFirstToLast, std::ranges::greater { }, &ValueAtPosition::first);
        }
        if (valuesFromFirstToLast.isEmpty())
            return ValueID();
        std::ranges::pop_heap(valuesFromFirstToLast, std::ranges::greater { }, &ValueAtPosition::first);
        return valuesFromFirstToLast.takeLast().second;
    };

    uint64_t numberOfValuesTyped = 0;
    auto countValuesTyped = makeScopeExit([&] {
        m_inputs.build.statistics().count(StatisticID::ValuesTypedFromChanges, numberOfValuesTyped);
    });
    while (true) {
        while (ValueID id = takeNextValue()) {
            m_marks[id.index()].isQueued = false;
            ValueLocation location;
            if (m_uses) {
                location = m_uses->locationOf(id);
                if (location.placement == ValuePlacement::Detached || !m_cfg.isReachable(location.block))
                    continue;
            } else if (function.at(id).opcode == Opcode::Parameter)
                location = { ValuePlacement::Parameter, BlockID(m_placesOfParameters[id.index()].block), m_placesOfParameters[id.index()].index };

            std::optional<unsigned> cycle = m_cycles ? m_cycles->outermostCycleOf(location.block) : std::nullopt;
            if (uint32_t component = cycle ? componentOf(id, *cycle) : noComponent; component != noComponent) {
                if (!m_isWaitingComponent.set(component)) {
                    m_waitingComponents.append(component);
                    std::ranges::push_heap(m_waitingComponents, std::greater<>());
                }
                continue;
            }

            ++numberOfValuesTyped;
            rememberState(id);
            Value& value = m_inputs.function.editAndReportLater(id);
            TypeID type = value.type;
            TypeID expected = value.expectedOrTypeForTyping().predicted();
            value.type = compute(typingContext(TypeLevel::Proven), id, location);
            TypeID expectedNow = expectedTypeToStore(id, value, compute(typingContext(TypeLevel::Expected), id, location));
            value.setExpected(ExpectedType(expectedNow == value.type ? TypeID() : expectedNow));
            if (value.type != type || value.expectedOrTypeForTyping().predicted() != expected) {
                willRestartValues();
                enqueueUsersOf(id);
            }
        }
        if (m_waitingComponents.isEmpty())
            return;

        std::ranges::pop_heap(m_waitingComponents, std::greater<>());
        uint32_t component = m_waitingComponents.takeLast();
        m_isWaitingComponent.quickClear(component);
        typeComponent(component);
    }
}

uint32_t TypePropagation::componentOf(ValueID id, unsigned cycle)
{
    if (m_componentOfValue.isEmpty())
        m_componentOfValue.fill(notVisited, m_inputs.function.values.size());
    if (!m_isSearchedCycle.set(cycle))
        findComponentsOf(cycle);
    return std::min(m_componentOfValue[id.index()], noComponent);
}

void TypePropagation::findComponentsOf(unsigned cycle)
{
    const Function& function = m_inputs.function;

    struct Frame {
        ValueID value;
        uint32_t number;
        uint32_t lowest;
        uint32_t nextSource;
        uint32_t endOfSources;
        bool isTypedFromItselfAlone;
    };
    Vector<Frame, 32> frames;
    Vector<ValueID, 64> sources;
    Vector<ValueID, 32> stack;
    constexpr uint32_t isOnStack = 1u << 31;
    uint32_t nextNumber = 0;

    auto addSource = [&](ValueID source) {
        if (!source)
            return;
        ValueLocation location = m_uses->locationOf(source);
        if (location.placement != ValuePlacement::Detached && m_cfg.isReachable(location.block) && m_cycles->outermostCycleOf(location.block) == cycle)
            sources.append(source);
    };
    auto push = [&](ValueID id) {
        uint32_t number = nextNumber++;
        m_componentOfValue[id.index()] = number | isOnStack;
        stack.append(id);
        uint32_t firstSource = sources.size();
        ValueLocation location = m_uses->locationOf(id);
        if (location.placement != ValuePlacement::Parameter)
            std::ranges::for_each(function.at(id).operands(), addSource);
        else {
            for (Edge edge : m_cfg.incomingEdges(location.block)) {
                const Target& target = targetOf(function, edge);
                if (location.index < target.arguments.size())
                    addSource(target.arguments[location.index]);
                else if (!edge.isHandlerEdge())
                    std::ranges::for_each(function.at(edge.from).terminator.operands(), addSource);
            }
        }
        frames.append({ id, number, number, firstSource, static_cast<uint32_t>(sources.size()), false });
    };

    auto search = [&](ValueID root) {
        if (m_componentOfValue[root.index()] != notVisited)
            return;
        push(root);
        while (!frames.isEmpty()) {
            Frame& frame = frames.last();
            if (frame.nextSource < frame.endOfSources) {
                ValueID source = sources[frame.nextSource++];
                uint32_t state = m_componentOfValue[source.index()];
                if (state == notVisited)
                    push(source);
                else if (state != noComponent && (state & isOnStack)) {
                    frame.lowest = std::min(frame.lowest, state & ~isOnStack);
                    frame.isTypedFromItselfAlone |= source == frame.value;
                }
                continue;
            }

            Frame finished = frames.takeLast();
            sources.shrink(frames.isEmpty() ? 0 : frames.last().endOfSources);
            if (!frames.isEmpty())
                frames.last().lowest = std::min(frames.last().lowest, finished.lowest);
            if (finished.lowest != finished.number)
                continue;
            if (stack.last() == finished.value && !finished.isTypedFromItselfAlone) {
                m_componentOfValue[stack.takeLast().index()] = noComponent;
                continue;
            }
            uint32_t component = m_firstMemberOfComponent.size();
            m_firstMemberOfComponent.append(m_membersOfComponents.size());
            do {
                m_componentOfValue[stack.last().index()] = component;
                m_membersOfComponents.append(stack.last());
            } while (stack.takeLast() != finished.value);
        }
    };

    if (m_firstBlockOfCycle.isEmpty()) {
        unsigned numberOfCycles = 0;
        for (BlockID blockID : m_cfg.reversePostOrder())
            numberOfCycles = std::max(numberOfCycles, m_cycles->outermostCycleOf(blockID).value_or(0) + 1);
        groupByIndex<BlockID>(numberOfCycles, m_firstBlockOfCycle, m_blocksOfCycles, [&](const auto& add) {
            for (BlockID blockID : m_cfg.reversePostOrder()) {
                if (std::optional<unsigned> outermost = m_cycles->outermostCycleOf(blockID))
                    add(*outermost, blockID);
            }
        });
    }
    for (BlockID blockID : entriesOfGroup(m_firstBlockOfCycle, m_blocksOfCycles, cycle)) {
        bool isHeader = std::ranges::any_of(m_cfg.incomingEdges(blockID), [&](Edge edge) {
            return m_cfg.reversePostOrderIndex(edge.from) >= m_cfg.reversePostOrderIndex(blockID);
        });
        if (isHeader)
            std::ranges::for_each(function.at(blockID).parameters, search);
    }
}

void TypePropagation::typeComponent(uint32_t component)
{
    const Function& function = m_inputs.function;
    size_t first = m_firstMemberOfComponent[component];
    size_t end = component + 1 < m_firstMemberOfComponent.size() ? m_firstMemberOfComponent[component + 1] : m_membersOfComponents.size();

    m_restartedValuesFromLastToFirst.shrink(0);
    listFromLastToFirst(m_membersOfComponents.span().subspan(first, end - first));
    m_didListRestartedValues = true;
    const auto& members = m_restartedValuesFromLastToFirst;

    Vector<std::pair<TypeID, TypeID>, 32> typesBefore;
    m_isMemberOfTypedComponent.ensureSize(function.values.size());
    m_listedValues = &m_isMemberOfTypedComponent;
    for (ValueID id : members) {
        typesBefore.append({ function.at(id).type, function.at(id).expectedOrTypeForTyping().predicted() });
        restart(id);
        m_isMemberOfTypedComponent.quickSet(id.index());
    }
    m_queuedValues = QueuedValues::OnlyListed;
    solveTypes();
    solveExpectedTypes();

    for (ValueID id : members) {
        const Value& value = function.at(id);
        if (TypeID expected = value.expected().predicted())
            editTypeOf(id).setExpected(ExpectedType(narrowerTypeOrNull(expected, value.type)));
        else if (value.type == TypeTable::never)
            editTypeOf(id).setExpected({ });
    }
    m_queuedValues = QueuedValues::AllButListed;
    for (size_t index = 0; index < members.size(); ++index) {
        const Value& value = function.at(members[index]);
        if (value.type != typesBefore[index].first || value.expectedOrTypeForTyping().predicted() != typesBefore[index].second)
            enqueueUsersOf(members[index]);
    }
    m_queuedValues = QueuedValues::All;
    for (ValueID id : members)
        m_isMemberOfTypedComponent.quickClear(id.index());
}

void TypePropagation::tellWhatIsTyped()
{
    PendingTyping* pending = m_inputs.memo->pendingTyping;
    const ChangeLog* log = m_inputs.function.changeLog();
    if (!pending || !log)
        return;
    reportRetypedValues(false);
    if (log->hasForgottenSince(m_logAtStart))
        return;
    if (m_didTypeAllValues) {
        pending->didTypeEverythingBefore(m_logAtStart);
        pending->takeEntries(*log);
        pending->forgetChangesOfTypesAlone();
        return;
    }
    if (pending->isEverythingPending(*log) || m_everyOtherValueIsAsTyped)
        return;

    const Function& function = m_inputs.function;
    auto isTyped = [&](ValueID id) {
        return id.index() < m_marks.size() && isRemembered(id);
    };
    const CFG& cfg = m_inputs.analyses.cfg();
    auto areParametersTyped = [&](BlockID block) {
        if (cfg.isReachable(block))
            return std::ranges::all_of(function.at(block).parameters, isTyped);
        bool areAllTyped = true;
        BitVector didVisit;
        didVisit.set(block.index());
        forEachReachedBlockBehind(function, cfg, block, didVisit, [&](BlockID reached) {
            areAllTyped &= std::ranges::all_of(function.at(reached).parameters, isTyped);
        });
        return areAllTyped;
    };
    auto isTypedWithItsTargets = [&](BlockID block) {
        bool areAllTyped = areParametersTyped(block);
        function.at(block).forEachTarget([&](const Target& target) {
            areAllTyped &= !target.block || areParametersTyped(target.block);
        });
        return areAllTyped;
    };

    size_t numberOfOwnValueEntries = log->valuesSince(m_logAtStart).size();
    size_t numberOfOwnBlockEntries = log->blocksSince(m_logAtStart).size();
    pending->takeEntries(*log);
    size_t firstOwnValueEntry = pending->m_values.size() - numberOfOwnValueEntries;
    size_t firstOwnBlockEntry = pending->m_blocks.size() - numberOfOwnBlockEntries;
    size_t index = 0;
    pending->m_values.removeAllMatching([&](const ValueChange& change) {
        return index++ < firstOwnValueEntry ? isTyped(change.value) : !change.edits.containsAny({ ValueEdit::Added, ValueEdit::Definition });
    });
    enum class Answer : uint8_t { NotAsked, No, Yes };
    Vector<Answer, 128> answers;
    answers.fill(Answer::NotAsked, function.blocks.size());
    index = 0;
    pending->m_blocks.removeAllMatching([&](const BlockChange& change) {
        if (index++ >= firstOwnBlockEntry)
            return false;
        Answer& answer = answers[change.block.index()];
        if (answer == Answer::NotAsked)
            answer = isTypedWithItsTargets(change.block) ? Answer::Yes : Answer::No;
        return answer == Answer::Yes;
    });
}

static PendingTyping* pendingTypingOf(const TypePropagationInputs& inputs)
{
    return inputs.memo && inputs.function.changeLog() ? inputs.memo->pendingTyping : nullptr;
}

TypePropagationOutcome typeUntilNothingIsCut(const TypePropagationInputs& inputs, TypedFirst typedFirst, std::span<const ValueID> changedValues, bool everyOtherValueIsAsTyped)
{
    PendingTyping* pending = pendingTypingOf(inputs);
    TypePropagationOutcome total;
    Vector<ValueID> parametersBehindRemovedEdges;
    bool typesAllValues = typedFirst == TypedFirst::AllValues;
    while (true) {
        TypePropagation propagation(inputs);
        switch (typedFirst) {
        case TypedFirst::Nothing:
            break;
        case TypedFirst::DependentsOfChangedValues:
            propagation.typeDependentsOf(changedValues, everyOtherValueIsAsTyped);
            break;
        case TypedFirst::AllValues:
            propagation.typeAllValues();
            break;
        case TypedFirst::PendingValues:
            pending->takeEntries(*inputs.function.changeLog());
            propagation.typePendingValues(*pending);
            break;
        }

        parametersBehindRemovedEdges.shrink(0);
        TypePropagationOutcome outcome = propagation.finishWithoutCompacting(parametersBehindRemovedEdges);
        if (typedFirst == TypedFirst::PendingValues) {
            pending->didTypePendingValues();
            pending->takeEntries(*inputs.function.changeLog());
            pending->forgetChangesOfTypesAlone();
        } else if (typedFirst != TypedFirst::Nothing)
            propagation.tellWhatIsTyped();

        total += outcome;
        if ((!outcome.cutBlocks && !outcome.foundParametersLeftAtNever) || parametersBehindRemovedEdges.isEmpty())
            break;
        if (typedFirst == TypedFirst::PendingValues && !outcome.foundParametersLeftAtNever)
            continue;
        typedFirst = TypedFirst::DependentsOfChangedValues;
        changedValues = parametersBehindRemovedEdges.span();
    }

    if (total.cutBlocks) {
        if (pending)
            pending->compact(inputs.function, inputs.renumbering);
        else
            compactFunction(inputs.function, inputs.renumbering);
        inputs.analyses.didChangeControlFlow();
    }
    if (pending && total.didLookForCodeToCut)
        pending->setWaitsToCutCode(total.waitsToCutCode);
    if (pending && (total.leftTypesAboveTheirRules || typesAllValues))
        pending->setHoldsTypesLeftWhenRoundsRanOut(total.leftTypesAboveTheirRules);
    return total;
}

bool hasTypedValues(const Function& function)
{
    for (const Block& block : function.blocks) {
        if (!block.values.isEmpty())
            return !!function.at(block.values[0]).type;
    }
    return false;
}

void typeValuesWithoutCutting(const TypePropagationInputs& inputs)
{
    TypePropagation propagation(inputs);
    propagation.typeAllValues();
    propagation.tellWhatIsTyped();
}

void PendingTyping::takeEntries(const ChangeLog& log)
{
    if (isEverythingPending(log))
        return;
    m_values.append(log.valuesSince(m_seen));
    m_blocks.append(log.blocksSince(m_seen));
    m_seen = log.end();
}

void PendingTyping::forgetChangesOfTypesAlone()
{
    m_values.removeAllMatching([](const ValueChange& change) {
        return change.edits == ValueEdit::Type;
    });
}

void PendingTyping::didTypePendingValues()
{
    m_values.shrink(0);
    m_blocks.shrink(0);
    m_valuesTypedFromRecordsArePending = false;
}

void PendingTyping::didTypeEverythingBefore(const ChangeLog::Cursor& cursor)
{
    didTypePendingValues();
    m_isEverythingPending = false;
    m_seen = cursor;
}

void PendingTyping::didEnterScheduleTyped(const ChangeLog& log)
{
    didTypeEverything(log);
    m_valuesTypedFromRecordsArePending = true;
}

// An unreachable block is removed, and its entry with it. The reachable blocks that it led to have lost an edge.
void PendingTyping::moveEntriesOutOfUnreachableBlocks(const Function& function)
{
    if (m_blocks.isEmpty())
        return;
    BitVector isReachable(function.blocks.size());
    Vector<BlockID, 16> worklist { entryBlock };
    isReachable.quickSet(entryBlock.index());
    while (!worklist.isEmpty()) {
        function.at(worklist.takeLast()).forEachTarget([&](const Target& target) {
            if (target.block && !isReachable.quickSet(target.block.index()))
                worklist.append(target.block);
        });
    }

    BitVector didVisit(function.blocks.size());
    for (const BlockChange& change : m_blocks) {
        if (!isReachable.quickGet(change.block.index()) && !didVisit.quickSet(change.block.index()))
            worklist.append(change.block);
    }
    while (!worklist.isEmpty()) {
        function.at(worklist.takeLast()).forEachTarget([&](const Target& target) {
            if (!target.block || didVisit.quickSet(target.block.index()))
                return;
            if (isReachable.quickGet(target.block.index()))
                m_blocks.append(BlockChange { target.block, BlockEdit::Incoming });
            else
                worklist.append(target.block);
        });
    }
}

bool PendingTyping::compact(Function& function, Renumbering* renumberingForCaller)
{
    const ChangeLog& log = *function.changeLog();
    takeEntries(log);
    bool wasEverythingPending = isEverythingPending(log);
    if (!wasEverythingPending)
        moveEntriesOutOfUnreachableBlocks(function);
    Renumbering renumbering;
    bool didMove = compactFunction(function, &renumbering);
    if (didMove && !wasEverythingPending) {
        m_values.removeAllMatching([&](ValueChange& change) {
            return !(change.value = renumbering.newIDOf(change.value));
        });
        m_blocks.removeAllMatching([&](BlockChange& change) {
            return !(change.block = renumbering.newIDOf(change.block));
        });
        m_seen = log.end();
        for (const ValueChange& change : m_values) {
            if (change.edits.contains(ValueEdit::Type) && !change.edits.contains(ValueEdit::Added))
                function.changeLog()->markTypeAsUnpropagated(change.value);
        }
    }
    if (renumberingForCaller)
        *renumberingForCaller = WTF::move(renumbering);
    return didMove;
}

PendingTyping PendingTyping::forCopyWith(const ChangeLog& logOfCopy, const ChangeLog& log) const
{
    PendingTyping copy = *this;
    copy.takeEntries(log);
    copy.m_isEverythingPending = isEverythingPending(log);
    copy.m_seen = logOfCopy.end();
    return copy;
}

static bool listsSameValuesInSameBlocks(const Function& function, const Function& other)
{
    return function.values.size() == other.values.size() && std::ranges::equal(function.blocks, other.blocks, [](const Block& block, const Block& otherBlock) {
        return block.parameters == otherBlock.parameters && block.values == otherBlock.values && block.terminator.kind == otherBlock.terminator.kind && block.terminator.targets == otherBlock.terminator.targets;
    });
}

static ValueID findValueTypedWider(const TypePropagationInputs& inputs, const Function& typedFromNever, TypeArena& types)
{
    const Function& function = inputs.function;
    ValueID found;
    // Whichever typing cuts code numbers the values anew.
    if (!listsSameValuesInSameBlocks(function, typedFromNever))
        return found;
    forEachListedValue(function, inputs.analyses.cfg(), [&](ValueID id) {
        if (found)
            return;
        TypeID type = types.adopt(function.at(id).type);
        TypeID fromNever = typedFromNever.at(id).type;
        if (type == fromNever || types.at(type).isSubsetOf(types.at(fromNever), inputs.program))
            return;
        // How far the range of a variable of a loop is widened depends on the steps by which it grew, and they are other steps from never than from what changed.
        Type withRangeFromNever = types.at(type);
        withRangeFromNever.integers = types.at(fromNever).integers;
        if (!withRangeFromNever.isSubsetOf(types.at(fromNever), inputs.program))
            found = id;
    });
    return found;
}

TypePropagationOutcome typeChangedValues(const TypePropagationInputs& inputs)
{
    PendingTyping* pending = pendingTypingOf(inputs);
    RELEASE_ASSERT(pending);
    const ChangeLog& log = *inputs.function.changeLog();
    Statistics& statistics = inputs.build.statistics();
    if (pending->isEmpty(log)) {
        statistics.count(StatisticID::FunctionsTypedAsBefore);
        if (!pending->waitsToCutCode())
            inputs.function.setValuesAreTypedAsDefined();
        return { };
    }

    TypePropagationOutcome outcome;
    if (pending->isEverythingPending(log)) {
        statistics.count(StatisticID::FunctionsTypedFromNever);
        outcome = typeValuesFromNever(inputs);
        if (!outcome.replacedUpcasts)
            pending->didTypeEverything(log);
    } else {
        std::optional<Function> typedFromNever;
        std::optional<ScratchTypeArena> typesFromNever;
        // Where the rounds ran out, values stay above their rules until every value is typed again.
        if (inputs.build.options().verifiesAfterEveryPass && !pending->holdsTypesLeftWhenRoundsRanOut()) {
            typedFromNever = inputs.function;
            typesFromNever.emplace(inputs.program.types);
            adoptIDs(*typedFromNever, *typesFromNever);
            FunctionAnalyses analyses(inputs.analyses.context(), *typedFromNever);
            // Where the rounds run out or a range stays, the types depend on the types that typing began with.
            if (typeValuesFromNever({ inputs.program, inputs.id, *typedFromNever, analyses, *typesFromNever, inputs.build }).dependsOnTypesBefore())
                typedFromNever = std::nullopt;
        }
        outcome = typeUntilNothingIsCut(inputs, TypedFirst::PendingValues);
        statistics.count(outcome.didChange() ? StatisticID::FunctionsTypedFromChanges : StatisticID::FunctionsTypedAsBefore);
        // Where the log ran full while this typing wrote into it, every value is pending again.
        if (ValueID value = typedFromNever && !outcome.dependsOnTypesBefore() && !pending->isEverythingPending(log) ? findValueTypedWider(inputs, *typedFromNever, *typesFromNever) : ValueID())
            reportTypeOutOfDate(inputs.build, InternalError { .kind = InternalErrorKind::TypeOutOfDate, .function = inputs.analyses.subject().function, .value = value, .detail = "typed from what changed it is wider than typed from nothing"_s });
    }
    if (pending->isEmpty(log)) {
        inputs.function.changeLog()->didPropagateTypes();
        if (!pending->waitsToCutCode())
            inputs.function.setValuesAreTypedAsDefined();
    }
    return outcome;
}

bool becomeResolved(Function& function, ValueID id, const ConvertResolution& resolution)
{
    Value& convert = function.edit(id);
    ValueID operand = convert.valueOperand();
    TypeSiteID site = convert.declared().site;

    switch (resolution.cast) {
    case CastResolution::Nothing:
        if (!convert.expected().predicted() || convert.expected().predicted() == function.at(operand).expected().predicted())
            return false;
        becomeUpcast(function, id, operand);
        return true;
    case CastResolution::Upcast:
        convert.become(Opcode::Upcast);
        convert.setCastType(resolution.castType);
        break;
    case CastResolution::Check:
        convert.become(Opcode::Check);
        convert.setExpectedType(ExpectedType(resolution.castType));
        convert.setSite(site);
        break;
    case CastResolution::Assume:
        convert.becomeAssumeType(operand, Proven::byFrontEnd(resolution.castType));
        convert.setIsWordOfFrontEnd(true);
        return true;
    case CastResolution::Unresolved:
        RELEASE_ASSERT_NOT_REACHED();
    }
    convert.setValueOperand(operand);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
