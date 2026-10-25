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
#include "AOTResultRulesInternal.h"
#include "AOTSort.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <ranges>
#include <tuple>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t updatesBeforeJoiningBound = 8;
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
    return function.at(edge.from).frequency != BlockFrequency::Normal;
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
    m_isRemembered.ensureSize(m_inputs.function.values.size());
    m_statesBeforeRestart.grow(m_inputs.function.values.size());
}

void TypePropagation::willRestartValues()
{
    if (m_uses)
        return;
    willTypeValues();
    m_uses = &m_inputs.analyses.useLists();
    m_isQueued.ensureSize(m_inputs.function.values.size());
    m_isRestarted.ensureSize(m_inputs.function.values.size());
}

void TypePropagation::addTypeOutOfDate(ValueID id, ASCIILiteral detail)
{
    m_inputs.build.diagnostics().add(InternalError { .kind = InternalErrorKind::TypeOutOfDate, .function = m_inputs.analyses.subject().function, .value = id, .detail = detail });
}

void TypePropagation::reportRetypedValues(bool countsThem)
{
    RELEASE_ASSERT(!countsThem || !m_numberOfReportedValues);
    for (ValueID id : m_rememberedValues.span().subspan(m_numberOfReportedValues)) {
        const Value& value = m_inputs.function.at(id);
        const StateBeforeRestart& previous = m_statesBeforeRestart[id.index()];
        bool isRetyped = value.type != previous.type;
        bool isExpectedToBeOther = value.expected() != previous.expected;
        if (countsThem) {
            m_outcome.retypedValues += isRetyped;
            m_outcome.changedExpectedTypes += isExpectedToBeOther;
        }
        if (isRetyped || isExpectedToBeOther)
            m_inputs.function.didEdit(id, ValueEdit::Type, previous.type, previous.expected);
    }
    m_numberOfReportedValues = m_rememberedValues.size();
}

void TypePropagation::rememberState(ValueID id)
{
    if (m_isRemembered.quickSet(id.index()))
        return;
    const Value& value = m_inputs.function.at(id);
    m_statesBeforeRestart[id.index()] = { value.type, value.expected(), value.expectedOrTypeForTyping(), 0 };
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
    value.setExpected(expectedTypeToStore(id, value, computed));
    flags.add(SweepFlag::IsExpectedTyped);
    if (isChangedValue || value.expectedOrTypeForTyping() != before.expectedOrType)
        flags.add(SweepFlag::HasOtherExpectedType);
}

void TypePropagation::restart(ValueID id)
{
    rememberState(id);
    Value& value = m_inputs.function.editAndReportLater(id);
    value.type = TypeTable::never;
    value.setExpected({ });
    if (m_uses)
        m_isRestarted.quickSet(id.index());
}

void TypePropagation::restartAllValues()
{
    willRestartValues();
    for (const Block& block : m_inputs.function.blocks)
        forEachValueOf(block, [&](ValueID id) { restart(id); });
}

void TypePropagation::restartListedValues(std::span<const ValueID> values)
{
    willRestartValues();
    m_queuedValues = QueuedValues::OnlyListed;
    m_listedValues = &m_isRestarted;
    for (ValueID id : values)
        restart(id);
}

void TypePropagation::restartDependentsOf(std::span<const ValueID> values)
{
    willRestartValues();
    for (ValueID id : values)
        enqueue(id);
    for (size_t index = 0; index < m_worklist.size(); ++index)
        enqueueUsersOf(m_worklist[index]);

    for (ValueID id : m_worklist) {
        m_isQueued.quickClear(id.index());
        restart(id);
    }
    m_worklist.shrink(0);
}

void TypePropagation::listFromLastToFirst(std::span<const ValueID> values)
{
    Vector<std::pair<uint64_t, ValueID>, 64> positions;
    for (ValueID id : values) {
        ValueLocation location = m_uses->locationOf(id);
        if (location.placement == ValuePlacement::Detached)
            continue;
        uint64_t isInBlock = location.placement == ValuePlacement::InBlock;
        positions.append({ static_cast<uint64_t>(m_cfg.reversePostOrderIndex(location.block)) << 33 | isInBlock << 32 | location.index, id });
    }
    AOT::sort(positions, std::ranges::greater { }, &std::pair<uint64_t, ValueID>::first);
    for (auto [position, id] : positions)
        m_restartedValuesFromLastToFirst.append(id);
}

void TypePropagation::listRestartedValuesInOrder()
{
    if (std::exchange(m_didListRestartedValues, true))
        return;

    const Function& function = m_inputs.function;
    constexpr size_t valuesWalkedForOneSorted = 8;
    if (m_rememberedValues.size() * valuesWalkedForOneSorted < function.values.size()) {
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
    if (!m_isQueued.quickSet(id.index()))
        m_worklist.append(id);
}

void TypePropagation::enqueueEdgeResultParameters(BlockID from)
{
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

    for (const Use& use : m_uses->usesOf(id)) {
        if (hasMultipleResults && use.kind == Use::Kind::Operand)
            enqueue(use.user);
        if (isTest && use.kind == Use::Kind::TerminatorOperand && function.at(use.block).terminator.isConditionalBranch())
            enqueueEdgeResultParameters(use.block);
    }
}

void TypePropagation::enqueueUsersOf(ValueID id)
{
    const Function& function = m_inputs.function;
    for (const Use& use : m_uses->usesOf(id)) {
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

void TypePropagation::propagate(TypeLevel level, Direction direction)
{
    TypingContext context = typingContext(level);
    auto markIfNotTaken = [&](ValueID source) {
        if (source && !m_wasTakenFromWorklist.quickGet(source.index()))
            m_hasUserThatWasTakenFirst.quickSet(source.index());
    };
    while (!m_worklist.isEmpty()) {
        ValueID id = m_worklist.takeLast();
        m_isQueued.quickClear(id.index());
        ValueLocation location = m_uses->locationOf(id);
        bool isTakenAgain = m_wasTakenFromWorklist.quickGet(id.index());
        if (!isTakenAgain && location.placement == ValuePlacement::Parameter)
            forEachSourceOfParameter(location.block, location.index, markIfNotTaken);
        else if (!isTakenAgain)
            forEachSourceOf(id, markIfNotTaken);
        m_wasTakenFromWorklist.quickSet(id.index());
        TypeID computed = compute(context, id, location);
        if (level == TypeLevel::Expected)
            computed = meetWithPreviousExpectation(id, computed);

        if (!update(level, direction, id, computed))
            continue;
        if (isTakenAgain || m_hasUserThatWasTakenFirst.quickGet(id.index())) {
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
    m_wasTakenFromWorklist.clearAll();
    m_wasTakenFromWorklist.ensureSize(function.values.size());
    if (std::exchange(m_didMarkSourcesOfUnreachableValues, true))
        return;

    m_hasUserThatWasTakenFirst.ensureSize(function.values.size());
    if (m_cfg.reversePostOrder().size() == function.blocks.size())
        return;
    auto mark = [&](ValueID id, uint32_t = 0) {
        if (id)
            m_hasUserThatWasTakenFirst.quickSet(id.index());
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
        editTypeOf(id).setExpected(TypeTable::never);
    solve(TypeLevel::Expected);
}

TypeID TypePropagation::meetWithPreviousExpectation(ValueID id, TypeID computed)
{
    TypeID previous = m_statesBeforeRestart[id.index()].expected;
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

TypeID TypePropagation::computeParameter(const TypingContext& context, ValueID id, BlockID block, uint32_t index)
{
    const Function& function = context.function;
    if (block == entryBlock)
        return index < function.numberOfEntryParameters() ? declaredEntryTypeOf(context, function, index) : TypeTable::anything;

    auto incomingEdges = m_cfg.incomingEdges(block);
    bool skipsSeldomTakenEdges = context.level == TypeLevel::Expected && !isEveryEdgeSeldomTaken(function, incomingEdges);
    TypeID joined = TypeTable::never;
    for (Edge edge : incomingEdges) {
        if (skipsSeldomTakenEdges && isSeldomTaken(function, edge))
            continue;
        const Target& target = targetOf(function, edge);
        TypeID incoming = TypeTable::anything;
        if (index < target.arguments.size())
            incoming = context.typeIDOf(target.arguments[index]);
        else if (!edge.isHandlerEdge()) {
            EdgeResultPosition position { edge.from, edge.targetIndex, index - static_cast<uint32_t>(target.arguments.size()) };
            if (std::optional<EdgeResult> result = tryGetEdgeResult(function, position))
                incoming = computeTypeOnEdge(context, position, *result);
        }
        joined = joinIntoParameter(context, id, joined, incoming);
    }
    return joined;
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
    bool didWiden = false;
    TypeID joined = types.sharedAnswer(SharedAnswers::AboutProgram, TypeArena::sharedKeyOf(SharedTypeMemo::Relation::Join, current, incoming), [&] {
        WideningContext widening { context.program, types, m_inputs.build, m_inputs.analyses.subject(parameter), &didWiden };
        TypeID computed = types.intern(types.at(current).unionWideningWith(types.at(incoming), widening));
        if (didWiden)
            types.markCurrentAnswersUnshareable();
        return computed;
    });
    if (!didWiden)
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

bool TypePropagation::update(TypeLevel level, Direction direction, ValueID id, TypeID computed)
{
    TypeArena& types = m_inputs.types;
    const Value& value = m_inputs.function.at(id);
    TypeID old = level == TypeLevel::Proven ? value.type : value.expectedOrTypeForTyping();

    TypeID next = computed;
    if (direction == Direction::Ascending) {
        next = types.unionOf(old, computed);
        auto isJoin = [&] {
            ValueLocation location = m_uses->locationOf(id);
            return location.placement == ValuePlacement::Parameter && m_cfg.incomingEdges(location.block).size() > 1;
        };
        if (next != old && isJoin())
            next = widenGrownParameter(id, old, next);
        if (next != computed)
            m_valuesAboveTheirRules.append(id);
    }
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
        editTypeOf(id).setExpected(next);
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
    m_didGiveUp = true;

    Vector<ValueID, 64> pendingValues = std::exchange(m_worklist, { });
    m_isQueued.clearAll();
    m_valuesAboveTheirRules.clear();

    if (level == TypeLevel::Expected) {
        auto expectAsBefore = [&](ValueID id) {
            Value& value = editTypeOf(id);
            TypeID previous = m_statesBeforeRestart[id.index()].expected;
            value.setExpected(previous ? types.intersectionOf(value.type, previous) : TypeID());
        };
        if (direction == Direction::Ascending)
            std::ranges::for_each(m_restartedValuesFromLastToFirst | std::views::reverse, expectAsBefore);
        else
            std::ranges::for_each(pendingValues, expectAsBefore);
        return;
    }

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
        TypeID expected = value.expected();
        if (!expected)
            return;
        TypeID narrower = narrowerTypeOrNull(expected, value.type);
        if (narrower != expected)
            editTypeOf(id).setExpected(narrower);
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
        compactFunction(m_inputs.function);
        m_inputs.analyses.didChangeControlFlow();
    }
    return m_outcome;
}

void TypePropagation::replaceUpcastsOfWidenedValues()
{
    Function& function = m_inputs.function;
    const TypeArena& types = m_inputs.types;
    for (ValueID id : m_rememberedValues) {
        TypeID before = m_statesBeforeRestart[id.index()].type;
        TypeID now = function.at(id).type;
        if (!before || before == now)
            continue;
        const Type& typeBefore = types.at(before);
        const Type& type = types.at(now);
        bool mayBeWider = (type.anyObject && !typeBefore.anyObject) || (type.anyFunction && !typeBefore.anyFunction) || (type.primitives & ~typeBefore.primitives);
        if (!mayBeWider)
            continue;
        for (const Use& use : m_inputs.analyses.useLists().usesOf(id)) {
            if (use.kind != Use::Kind::Operand || function.at(use.user).opcode != Opcode::Upcast)
                continue;
            TypeID castType = function.at(use.user).castType();
            if (type.isSubsetOf(types.at(castType), m_inputs.program))
                continue;
            Value& upcast = function.edit(use.user);
            upcast.become(Opcode::AssumeType);
            upcast.setValueOperand(id);
            upcast.setCastType(castType);
            ++m_outcome.replacedUpcasts;
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
    if (mayHaveCodeToCut())
        cutAfterNeverValues();

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

void TypePropagation::cutAfterNeverValues()
{
    const Function& function = m_inputs.function;
    bool waits = waitsForGenericVersion(m_inputs.program, function);
    m_outcome.didLookForCodeToCut = true;
    auto isNever = [&](ValueID id) {
        return function.at(id).type == TypeTable::never;
    };
    TypingContext typing { m_inputs.program, function, m_inputs.types };
    auto isNeverReached = [&](ValueID id) {
        const Value& value = function.at(id);
        return value.opcode == Opcode::NarrowByField && narrowsHolderToNothing(typing, typing.typeOf(value.valueOperand()), value.key(), typing.typeOf(value.readOperand()));
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
        if (endsInUnreachable && (isNeverEntered ? block.values.isEmpty() : keepsValue && index + 1 == block.values.size()))
            continue;
        if (waits) {
            m_outcome.waitsToCutCode = true;
            return;
        }
        Origin origin = isNeverEntered ? block.terminator.origin() : function.at(block.values[index]).origin();
        Block& cut = m_inputs.function.edit(id);
        cut.values.shrink(index + keepsValue);
        cut.terminator.become(TerminatorKind::Unreachable);
        cut.terminator.setOrigin(origin);
        ++m_outcome.cutBlocks;
    }
}

bool TypePropagation::hasTypesOfEveryArgument(BlockID block, uint32_t index, const Value& parameter) const
{
    const Function& function = m_inputs.function;
    std::span<const Edge> edges = m_cfg.incomingEdges(block);
    bool countsSeldomTakenEdges = isEveryEdgeSeldomTaken(function, edges);
    TypeID expected = parameter.expectedOrTypeForTyping();
    for (Edge edge : edges) {
        const Target& target = targetOf(function, edge);
        if (index >= target.arguments.size())
            return false;
        const Value& argument = function.at(target.arguments[index]);
        if (argument.type != parameter.type)
            return false;
        if ((countsSeldomTakenEdges || !isSeldomTaken(function, edge)) && argument.expectedOrTypeForTyping() != expected)
            return false;
    }
    return !edges.empty();
}

void TypePropagation::enqueueParametersOf(BlockID blockID)
{
    if (m_didEnqueueParametersOf.set(blockID.index()))
        return;
    const Function& function = m_inputs.function;
    if (!m_cfg.isReachable(blockID)) {
        Vector<BlockID, 8> unreachableBlocks { blockID };
        while (!unreachableBlocks.isEmpty()) {
            function.at(unreachableBlocks.takeLast()).forEachTarget([&](const Target& target) {
                if (!target.block)
                    return;
                if (m_cfg.isReachable(target.block))
                    enqueueParametersOf(target.block);
                else if (!m_didEnqueueParametersOf.set(target.block.index()))
                    unreachableBlocks.append(target.block);
            });
        }
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
    m_isQueued.ensureSize(numberOfValues);
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
        if (change.typeBefore != value.type || change.expectedBefore != value.expectedForTyping()) {
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

    while (true) {
        while (!m_worklist.isEmpty()) {
            ValueID id = m_worklist.takeLast();
            m_isQueued.quickClear(id.index());
            ValueLocation location;
            if (m_uses) {
                location = m_uses->locationOf(id);
                if (location.placement == ValuePlacement::Detached || !m_cfg.isReachable(location.block))
                    continue;
            } else if (function.at(id).opcode == Opcode::Parameter)
                location = { ValuePlacement::Parameter, BlockID(m_placesOfParameters[id.index()].block), m_placesOfParameters[id.index()].index };

            std::optional<unsigned> cycle = m_cycles ? m_cycles->outermostCycleOf(location.block) : std::nullopt;
            if (uint32_t component = cycle ? componentOf(id, *cycle) : noComponent; component != noComponent) {
                if (!m_isWaitingComponent.set(component))
                    m_waitingComponents.append(component);
                continue;
            }

            rememberState(id);
            Value& value = m_inputs.function.editAndReportLater(id);
            TypeID type = value.type;
            TypeID expected = value.expectedOrTypeForTyping();
            value.type = compute(typingContext(TypeLevel::Proven), id, location);
            TypeID expectedNow = expectedTypeToStore(id, value, compute(typingContext(TypeLevel::Expected), id, location));
            value.setExpected(expectedNow == value.type ? TypeID() : expectedNow);
            if (value.type != type || value.expectedOrTypeForTyping() != expected) {
                willRestartValues();
                enqueueUsersOf(id);
            }
        }
        if (m_waitingComponents.isEmpty())
            return;

        size_t position = std::ranges::min_element(m_waitingComponents) - m_waitingComponents.begin();
        uint32_t component = m_waitingComponents[position];
        m_waitingComponents[position] = m_waitingComponents.last();
        m_waitingComponents.removeLast();
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

    for (BlockID blockID : m_cfg.reversePostOrder()) {
        if (m_cycles->outermostCycleOf(blockID) != cycle)
            continue;
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
        typesBefore.append({ function.at(id).type, function.at(id).expectedOrTypeForTyping() });
        restart(id);
        m_isMemberOfTypedComponent.quickSet(id.index());
    }
    m_queuedValues = QueuedValues::OnlyListed;
    solveTypes();
    solveExpectedTypes();

    for (ValueID id : members) {
        const Value& value = function.at(id);
        if (TypeID expected = value.expected())
            editTypeOf(id).setExpected(narrowerTypeOrNull(expected, value.type));
        else if (value.type == TypeTable::never)
            editTypeOf(id).setExpected({ });
    }
    m_queuedValues = QueuedValues::AllButListed;
    for (size_t index = 0; index < members.size(); ++index) {
        const Value& value = function.at(members[index]);
        if (value.type != typesBefore[index].first || value.expectedOrTypeForTyping() != typesBefore[index].second)
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
    if (m_didTypeAllValues || m_didGiveUp) {
        pending->didTypeEverythingBefore(m_logAtStart);
        pending->takeEntries(*log);
        pending->forgetChangesOfTypesAlone();
        return;
    }
    if (pending->isEverythingPending(*log) || m_everyOtherValueIsAsTyped)
        return;

    const Function& function = m_inputs.function;
    auto isTyped = [&](ValueID id) {
        return id.index() < m_isRemembered.size() && isRemembered(id);
    };
    auto areParametersTyped = [&](BlockID block) {
        return std::ranges::all_of(function.at(block).parameters, isTyped);
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
    index = 0;
    pending->m_blocks.removeAllMatching([&](const BlockChange& change) {
        return index++ < firstOwnBlockEntry && isTypedWithItsTargets(change.block);
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
        if (!outcome.cutBlocks || parametersBehindRemovedEdges.isEmpty())
            break;
        if (typedFirst == TypedFirst::PendingValues)
            continue;
        typedFirst = TypedFirst::DependentsOfChangedValues;
        changedValues = parametersBehindRemovedEdges.span();
    }

    if (total.cutBlocks) {
        if (pending)
            pending->compact(inputs.function);
        else
            compactFunction(inputs.function);
        inputs.analyses.didChangeControlFlow();
    }
    if (pending && total.didLookForCodeToCut)
        pending->setWaitsToCutCode(total.waitsToCutCode);
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

bool PendingTyping::compact(Function& function, Renumbering* renumberingForCaller)
{
    const ChangeLog& log = *function.changeLog();
    takeEntries(log);
    bool wasEverythingPending = isEverythingPending(log);
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

static ValueID findValueTypedWider(const TypePropagationInputs& inputs, const Function& typedFromNever)
{
    const Function& function = inputs.function;
    ValueID found;
    if (function.values.size() != typedFromNever.values.size())
        return found;
    forEachListedValue(function, inputs.analyses.cfg(), [&](ValueID id) {
        TypeID type = function.at(id).type;
        TypeID fromNever = typedFromNever.at(id).type;
        if (!found && type != fromNever && !inputs.types.at(type).isSubsetOf(inputs.types.at(fromNever), inputs.program))
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
        if (inputs.build.options().verifiesAfterEveryPass) {
            typedFromNever = inputs.function;
            FunctionAnalyses analyses(inputs.analyses.context(), *typedFromNever);
            typeValuesFromNever({ inputs.program, inputs.id, *typedFromNever, analyses, inputs.types, inputs.build });
        }
        outcome = typeUntilNothingIsCut(inputs, TypedFirst::PendingValues);
        statistics.count(outcome.didChange() ? StatisticID::FunctionsTypedFromChanges : StatisticID::FunctionsTypedAsBefore);
        if (ValueID value = typedFromNever ? findValueTypedWider(inputs, *typedFromNever) : ValueID())
            inputs.build.diagnostics().add(InternalError { .kind = InternalErrorKind::TypeOutOfDate, .function = inputs.analyses.subject().function, .value = value, .detail = "typed from what changed it is wider than typed from nothing"_s });
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
        if (!convert.expected() || convert.expected() == function.at(operand).expected())
            return false;
        becomeUpcast(function, id, operand);
        return true;
    case CastResolution::Upcast:
        convert.become(Opcode::Upcast);
        break;
    case CastResolution::Check:
        convert.become(Opcode::Check);
        convert.setSite(site);
        break;
    case CastResolution::Unresolved:
    case CastResolution::Assume:
        RELEASE_ASSERT_NOT_REACHED();
    }
    convert.setValueOperand(operand);
    convert.setCastType(resolution.castType);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
