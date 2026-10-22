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
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <ranges>
#include <tuple>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t updatesBeforeJoiningBound = 8;

static_assert(!TypeTable::never.index());

static void forEachListedValue(const Function& function, const CFG& cfg, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        for (ValueID parameter : block.parameters)
            functor(parameter);
        for (ValueID value : block.values)
            functor(value);
    }
}

static void setTypeAt(Value& value, TypeLevel level, TypeID type)
{
    switch (level) {
    case TypeLevel::Proven:
        value.type = type;
        return;
    case TypeLevel::Expected:
        value.setExpected(type);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID typeOfEntryParameter(const TypingContext& context, unsigned index)
{
    if (index >= context.function.numberOfEntryParameters())
        return TypeTable::anything;
    return declaredEntryTypeOf(context, context.function, index);
}

static TypeID widenAfterGivingUp(const Program& program, TypeArena& types, TypeID grown, TypeID bound)
{
    const Type& type = types.at(grown);
    if (!type.isJavaScriptValue())
        return grown;
    if (bound && type.isSubsetOf(types.at(bound), program))
        return bound;
    return types.withPrimitives(TypeTable::anything, type.primitives & Primitive::Absent);
}

TypePropagation::TypePropagation(const TypePropagationInputs& inputs)
    : m_inputs(inputs)
    , m_cfg(inputs.analyses.cfg())
{
    RELEASE_ASSERT(!inputs.memo || !inputs.knownPositions);
    if (const ChangeLog* log = inputs.function.changeLog())
        m_logAtStart = log->end();
}

void TypePropagation::willTypeValues()
{
    if (!m_statesBeforeRestart.isEmpty())
        return;
    size_t numberOfValues = m_inputs.function.values.size();
    m_isRemembered.ensureSize(numberOfValues);
    m_statesBeforeRestart.grow(numberOfValues);
}

void TypePropagation::willRestartValues()
{
    if (m_uses)
        return;
    willTypeValues();
    size_t numberOfValues = m_inputs.function.values.size();
    m_uses = &m_inputs.analyses.useLists();
    m_isQueued.ensureSize(numberOfValues);
    m_isRestarted.ensureSize(numberOfValues);
}

void TypePropagation::reportRetypedValues()
{
    Function& function = m_inputs.function;
    for (ValueID id : m_rememberedValues.span().subspan(m_numberOfReportedValues)) {
        const Value& value = function.at(id);
        const StateBeforeRestart& previous = m_statesBeforeRestart[id.index()];
        if (value.type != previous.type || value.expected() != previous.expected)
            function.didEdit(id, ValueEdit::Type, previous.type, previous.expected);
    }
    m_numberOfReportedValues = m_rememberedValues.size();
}

TypePropagation::~TypePropagation()
{
    reportRetypedValues();
}

void TypePropagation::rememberState(ValueID id)
{
    if (m_isRemembered.quickSet(id.index()))
        return;
    const Value& value = m_inputs.function.at(id);
    m_statesBeforeRestart[id.index()] = { value.type, value.expected(), value.expectedOrTypeForTyping(), 0 };
    m_rememberedValues.append(id);
}

std::span<const Edge> TypePropagation::incomingEdgesOf(BlockID block) const
{
    return m_cfg.incomingEdges(block);
}

TypeID TypePropagation::provenTypeByRule(ValueID id)
{
    return computeValue(typingContext(TypeLevel::Proven), id);
}

TypeID TypePropagation::provenTypeOfParameterByRule(ValueID id, BlockID block, uint32_t index)
{
    return computeParameter(typingContext(TypeLevel::Proven), id, block, index);
}

bool TypePropagation::hasRetreatingEdge() const
{
    return m_cfg.hasCycle();
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
    if (hasRetreatingEdge() || !isEveryValueTypedBehindItsSources()) {
        restartAllValues();
        solveTypes();
        solveExpectedTypes();
        return;
    }

    willTypeValues();
    Function& function = m_inputs.function;
    m_sweepFlags.fill(SweepFlag::IsChangedValue, function.values.size());
    auto startOver = [&](ValueID id) {
        rememberState(id);
        Value& value = function.editAndReportLater(id);
        value.type = TypeTable::never;
        value.setExpected({ });
    };
    for (const Block& block : function.blocks) {
        std::ranges::for_each(block.parameters, startOver);
        std::ranges::for_each(block.values, startOver);
    }
    sweep(TypeLevel::Proven);
    sweep(TypeLevel::Expected);
}

void TypePropagation::typeDependentsOf(std::span<const ValueID> changedValues, ChangedValues kind)
{
    m_everyOtherValueIsAsTyped = kind == ChangedValues::EveryOtherValueIsAsTyped;
    if (hasRetreatingEdge()) {
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
    // Nothing in front of the first changed value is typed from a changed value.
    bool hasReachedChangedValue = false;
    auto standsInFrontOfChangedValues = [&](ValueID id) {
        if (hasReachedChangedValue)
            return false;
        hasReachedChangedValue = m_sweepFlags[id.index()].contains(SweepFlag::IsChangedValue);
        return !hasReachedChangedValue;
    };
    for (BlockID blockID : m_cfg.reversePostOrder()) {
        const Block& block = function.at(blockID);
        for (uint32_t index = 0; index < block.parameters.size(); ++index) {
            ValueID id = block.parameters[index];
            if (!standsInFrontOfChangedValues(id) && isParameterToSweep(level, blockID, index, id))
                storeSweptType(level, id, computeParameter(context, id, blockID, index));
        }
        for (ValueID id : block.values) {
            if (!standsInFrontOfChangedValues(id) && isValueToSweep(level, id))
                storeSweptType(level, id, computeValue(context, id));
        }
    }
}

bool TypePropagation::hasOperandTypedOtherwise(TypeLevel level, const Instruction& user) const
{
    const Function& function = m_inputs.function;
    SweepFlag hasOtherType = level == TypeLevel::Proven ? SweepFlag::HasOtherType : SweepFlag::HasOtherExpectedType;
    SweepFlag isTyped = level == TypeLevel::Proven ? SweepFlag::IsTyped : SweepFlag::IsExpectedTyped;
    bool found = false;
    user.forEachOperand([&](ValueID operand, uint32_t) {
        OptionSet<SweepFlag> flags = m_sweepFlags[operand.index()];
        found |= flags.contains(hasOtherType) || (flags.contains(isTyped) && opcodesWithIndirectUsers.contains(function.at(operand).opcode));
    });
    return found;
}

bool TypePropagation::isValueToSweep(TypeLevel level, ValueID id) const
{
    OptionSet<SweepFlag> flags = m_sweepFlags[id.index()];
    if (flags.contains(SweepFlag::IsChangedValue) || (level == TypeLevel::Expected && flags.contains(SweepFlag::IsTyped)))
        return true;
    return hasOperandTypedOtherwise(level, m_inputs.function.at(id));
}

bool TypePropagation::isParameterToSweep(TypeLevel level, BlockID block, uint32_t index, ValueID id) const
{
    OptionSet<SweepFlag> flags = m_sweepFlags[id.index()];
    if (flags.contains(SweepFlag::IsChangedValue) || (level == TypeLevel::Expected && flags.contains(SweepFlag::IsTyped)))
        return true;

    const Function& function = m_inputs.function;
    SweepFlag hasOtherType = level == TypeLevel::Proven ? SweepFlag::HasOtherType : SweepFlag::HasOtherExpectedType;
    for (Edge edge : m_cfg.incomingEdges(block)) {
        const Target& target = targetOf(function, edge);
        if (index < target.arguments.size()) {
            if (m_sweepFlags[target.arguments[index].index()].contains(hasOtherType))
                return true;
            continue;
        }
        if (!edge.isHandlerEdge() && hasOperandTypedOtherwise(level, function.at(edge.from).terminator))
            return true;
    }
    return false;
}

// Null where nothing narrower than the type is expected. What is proven may leave nothing of what was expected: then the expectation was wrong and says nothing,
// which is another thing than to expect no value.
TypeID TypePropagation::partOfExpectedTypeInside(TypeID type, TypeID expected)
{
    expected = m_inputs.types.intersectionOf(type, expected);
    return expected == TypeTable::never ? TypeID() : narrowerTypeOrNull(expected, type);
}

void TypePropagation::storeSweptType(TypeLevel level, ValueID id, TypeID computed)
{
    rememberState(id);
    Value& value = m_inputs.function.editAndReportLater(id);
    const StateBeforeRestart& before = m_statesBeforeRestart[id.index()];
    OptionSet<SweepFlag>& flags = m_sweepFlags[id.index()];
    bool isChangedValue = !m_everyOtherValueIsAsTyped || flags.contains(SweepFlag::IsChangedValue);
    switch (level) {
    case TypeLevel::Proven:
        value.type = computed;
        flags.add(SweepFlag::IsTyped);
        if (isChangedValue || computed != before.type)
            flags.add(SweepFlag::HasOtherType);
        return;
    case TypeLevel::Expected: {
        // Flow finds classes below the one that allocates. Nothing but the rule says what such an allocation makes.
        TypeID expected = mayAllocateOtherLayout(m_inputs.program, value) ? computed : meetWithPreviousExpectation(id, computed);
        if (expected != TypeTable::never)
            expected = partOfExpectedTypeInside(value.type, expected);
        value.setExpected(expected);
        flags.add(SweepFlag::IsExpectedTyped);
        if (isChangedValue || value.expectedOrTypeForTyping() != before.expectedOrType)
            flags.add(SweepFlag::HasOtherExpectedType);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void TypePropagation::restart(ValueID id)
{
    rememberState(id);
    Value& value = m_inputs.function.editAndReportLater(id);
    value.type = TypeTable::never;
    value.setExpected({ });
    m_isRestarted.quickSet(id.index());
}

void TypePropagation::restartAllValues()
{
    willRestartValues();
    auto restartValue = [&](ValueID id) {
        restart(id);
    };
    // The verifier reads the type ids of unreachable blocks too.
    for (const Block& block : m_inputs.function.blocks) {
        std::ranges::for_each(block.parameters, restartValue);
        std::ranges::for_each(block.values, restartValue);
    }
}

void TypePropagation::restartListedValues(std::span<const ValueID> values)
{
    willRestartValues();
    m_typesRestartedValuesAlone = true;
    for (ValueID id : values)
        restart(id);
}

// From its current type a value that depends on itself only confirms that type, so everything a changed value reaches starts over.
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

void TypePropagation::listRestartedValuesInOrder()
{
    if (std::exchange(m_didListRestartedValues, true))
        return;

    const Function& function = m_inputs.function;
    constexpr size_t valuesWalkedForOneSorted = 8;
    if (m_rememberedValues.size() * valuesWalkedForOneSorted < function.values.size()) {
        Vector<std::pair<uint64_t, ValueID>, 64> positions;
        for (ValueID id : m_rememberedValues) {
            ValueLocation location = m_uses->locationOf(id);
            if (location.placement == ValuePlacement::Detached)
                continue;
            uint64_t isInBlock = location.placement == ValuePlacement::InBlock;
            positions.append({ static_cast<uint64_t>(m_cfg.reversePostOrderIndex(location.block)) << 33 | isInBlock << 32 | location.index, id });
        }
        std::ranges::sort(positions, std::ranges::greater { }, &std::pair<uint64_t, ValueID>::first);
        for (auto [position, id] : positions)
            m_restartedValuesFromLastToFirst.append(id);
        return;
    }

    auto appendIfRestarted = [&](ValueID id) {
        if (m_isRestarted.quickGet(id.index()))
            m_restartedValuesFromLastToFirst.append(id);
    };
    std::span<const BlockID> order = m_cfg.reversePostOrder();
    for (size_t blockIndex = order.size(); blockIndex--;) {
        const Block& block = function.at(order[blockIndex]);
        for (size_t index = block.values.size(); index--;)
            appendIfRestarted(block.values[index]);
        for (size_t index = block.parameters.size(); index--;)
            appendIfRestarted(block.parameters[index]);
    }
}

bool TypePropagation::isJoin(ValueID id) const
{
    ValueLocation location = m_uses->locationOf(id);
    return location.placement == ValuePlacement::Parameter && m_cfg.incomingEdges(location.block).size() > 1;
}

void TypePropagation::enqueue(ValueID id)
{
    RELEASE_ASSERT(id.index() < m_statesBeforeRestart.size());
    if (m_queuesOnlySomeValues && !m_mayBeQueued.quickGet(id.index()))
        return;
    if (m_queuesAllButSomeValues && m_mayBeQueued.quickGet(id.index()))
        return;
    if (m_typesRestartedValuesAlone && !m_isRestarted.quickGet(id.index()))
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

// A ResultAt and an operand narrowed on an edge are typed from the operands of the value they name, whose own type may stay the same.
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

void TypePropagation::enqueueRestartedValues()
{
    listRestartedValuesInOrder();
    for (ValueID id : m_restartedValuesFromLastToFirst)
        enqueue(id);
}

TypingContext TypePropagation::typingContext(TypeLevel level)
{
    return { m_inputs.program, m_inputs.function, m_inputs.types, level, m_inputs.memo ? m_inputs.memo : &m_ownMemo, m_inputs.knownPositions, true };
}

void TypePropagation::propagate(TypeLevel level, Direction direction)
{
    TypingContext context = typingContext(level);
    while (!m_worklist.isEmpty()) {
        ValueID id = m_worklist.takeLast();
        m_isQueued.quickClear(id.index());
        bool isTakenAgain = m_wasTakenFromWorklist.quickGet(id.index());

        ValueLocation location = m_uses->locationOf(id);
        bool isParameter = location.placement == ValuePlacement::Parameter;
        // A parameter may be passed for itself.
        if (!isTakenAgain)
            markSourcesThatWereNotTaken(id, isParameter, location.block, location.index);
        m_wasTakenFromWorklist.quickSet(id.index());
        TypeID computed = isParameter ? computeParameter(context, id, location.block, location.index) : computeValue(context, id);
        if (level == TypeLevel::Expected)
            computed = meetWithPreviousExpectation(id, computed);

        // Whatever is typed from a restarted value is restarted too, and all that is restarted waits in the list at first. So what is typed from a value that is taken for the first time
        // still waits there, unless it was taken before that value.
        if (!update(level, direction, id, computed))
            continue;
        if (isTakenAgain || m_hasUserThatWasTakenFirst.quickGet(id.index())) {
            enqueueUsersOf(id);
            continue;
        }
        if (m_inputs.build.options().verifiesAfterEveryPass) [[unlikely]] {
            size_t numberOfWaitingValues = m_worklist.size();
            enqueueUsersOf(id);
            if (m_worklist.size() != numberOfWaitingValues)
                m_inputs.build.diagnostics().add(InternalError { .kind = InternalErrorKind::TypeOutOfDate, .function = m_inputs.analyses.subject().function, .value = id });
        }
    }
}

// What stands in a block that nothing reaches waits in no list: it is typed when one of its operands is.
void TypePropagation::willTakeValuesFromWorklist()
{
    const Function& function = m_inputs.function;
    size_t numberOfValues = function.values.size();
    m_wasTakenFromWorklist.clearAll();
    m_wasTakenFromWorklist.ensureSize(numberOfValues);
    if (std::exchange(m_didMarkSourcesOfUnreachableValues, true))
        return;

    m_hasUserThatWasTakenFirst.ensureSize(numberOfValues);
    if (m_cfg.reversePostOrder().size() == function.blocks.size())
        return;
    auto mark = [&](ValueID id, uint32_t = 0) {
        if (id)
            m_hasUserThatWasTakenFirst.quickSet(id.index());
    };
    auto markWithOperands = [&](ValueID id, uint32_t = 0) {
        if (!id)
            return;
        mark(id);
        function.at(id).forEachOperand(mark);
    };
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID blockID(blockIndex);
        if (m_cfg.isReachable(blockID))
            continue;
        const Block& block = function.at(blockID);
        for (ValueID id : block.values)
            function.at(id).forEachOperand(markWithOperands);
        for (const Target& target : block.terminator.targets)
            std::ranges::for_each(target.arguments, mark);
        if (block.handler)
            std::ranges::for_each(block.handler->arguments, mark);
        block.terminator.forEachOperand(markWithOperands);
    }
}

// The other end of enqueueUsersOf(): whatever the value is typed from and has not had its turn yet will have to look for its users when it gets one.
void TypePropagation::markSourcesThatWereNotTaken(ValueID id, bool isParameter, BlockID block, uint32_t index)
{
    const Function& function = m_inputs.function;
    auto mark = [&](ValueID source, uint32_t = 0) {
        if (source && !m_wasTakenFromWorklist.quickGet(source.index()))
            m_hasUserThatWasTakenFirst.quickSet(source.index());
    };
    // A ResultAt and what a terminator says about an edge are typed from the operands of the value they name. Where that value has had its turn, it has marked them itself.
    auto markWithOperands = [&](ValueID source, uint32_t = 0) {
        if (!source || m_wasTakenFromWorklist.quickGet(source.index()))
            return;
        m_hasUserThatWasTakenFirst.quickSet(source.index());
        if (opcodesWithIndirectUsers.contains(function.at(source).opcode))
            function.at(source).forEachOperand(mark);
    };

    if (!isParameter) {
        function.at(id).forEachOperand(markWithOperands);
        return;
    }
    for (Edge edge : m_cfg.incomingEdges(block)) {
        const Target& target = targetOf(function, edge);
        if (index < target.arguments.size())
            mark(target.arguments[index]);
        else if (!edge.isHandlerEdge())
            function.at(edge.from).terminator.forEachOperand(markWithOperands);
    }
}

void TypePropagation::solve(TypeLevel level)
{
    willRestartValues();
    willTakeValuesFromWorklist();
    enqueueRestartedValues();
    propagate(level, Direction::Ascending);

    // Where the ascent stopped every type holds, so whatever the rules give from here on holds too: it takes back what widening added without need,
    // and what a rule answered for a narrower operand and no longer answers.
    for (ValueID value : m_valuesAboveTheirRules)
        enqueue(value);
    m_valuesAboveTheirRules.shrink(0);
    propagate(level, Direction::Descending);
    for (ValueID id : m_restartedValuesFromLastToFirst)
        m_statesBeforeRestart[id.index()].numberOfUpdates = 0;
}

void TypePropagation::solveTypes()
{
    solve(TypeLevel::Proven);
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

TypeID TypePropagation::computeValue(const TypingContext& context, ValueID id)
{
    const Value& value = context.function.at(id);
    TypeID type = computeResultTypeID(context, value);
    if (m_inputs.build.options().verifiesAfterEveryPass && type != context.types.intern(context.types.adopt(computeResultType(context, value)))) [[unlikely]]
        m_inputs.build.diagnostics().add(InternalError { .kind = InternalErrorKind::TypeOutOfDate, .function = m_inputs.analyses.subject().function, .value = id });
    // A proof that leaves nothing of what its operand was expected to be: as in partOfExpectedTypeInside().
    if (type == TypeTable::never && context.level == TypeLevel::Expected && value.opcode == Opcode::AssumeType && context.typeIDOf(value.valueOperand()) != TypeTable::never)
        return value.type;
    return type;
}

TypeID TypePropagation::computeParameter(const TypingContext& context, ValueID id, BlockID block, uint32_t index)
{
    if (block == entryBlock)
        return typeOfEntryParameter(context, index);

    const Function& function = context.function;
    auto isSeldomTaken = [&](Edge edge) {
        return function.at(edge.from).frequency != BlockFrequency::Normal;
    };
    // What a path that is seldom taken brings says nothing about what the parameter usually holds.
    auto incomingEdges = m_cfg.incomingEdges(block);
    bool skipsSeldomTakenEdges = context.level == TypeLevel::Expected && !std::ranges::all_of(incomingEdges, isSeldomTaken);

    TypeID joined = TypeTable::never;
    for (Edge edge : incomingEdges) {
        if (skipsSeldomTakenEdges && isSeldomTaken(edge))
            continue;
        // Typing runs before the verifier has checked the edges.
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
    if (result.kind == EdgeResultKind::Guard)
        return TypeTable::guard;
    if (result.kind != EdgeResultKind::NarrowedOperand || !result.test)
        return context.types.intern(context.types.adopt(Typed::computeTypeOnEdge(context, position)));

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
    if (known && !m_inputs.build.options().verifiesAfterEveryPass) [[likely]]
        return known;
    // A test narrows by its own words and the types of its operands.
    TypeID type = sharedOrComputedType(context, SharedAnswers::AboutTypes, hasher.hash(), addWords, [&] {
        return context.types.intern(context.types.adopt(Typed::computeTypeOnEdge(context, position)));
    });
    if (!known)
        context.memo->results.add(hasher.hash(), type);
    else if (known != type)
        m_inputs.build.diagnostics().add(InternalError { .kind = InternalErrorKind::TypeOutOfDate, .function = m_inputs.analyses.subject().function, .value = result.test });
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
        // Each join that is widened is reported for its own parameter.
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
    // A range that has not left the one the parameter had before is widened no further than that one.
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
        // Only a join can feed itself. The parameter of a block with one incoming edge follows the values that dominate it, so the range an edge cuts stays cut.
        if (next != old && isJoin(id))
            next = widenGrownParameter(id, old, next);
        // Not every rule answers more for a wider operand: a cast takes the contents of an array from its operand while they are proven, and from its type afterwards.
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

    // The update that passes the limit still counts: a pass that asks for one value to be retyped relies on that type.
    setTypeAt(editTypeOf(id), level, next);
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
        switch (direction) {
        case Direction::Ascending:
            std::ranges::for_each(m_restartedValuesFromLastToFirst | std::views::reverse, expectAsBefore);
            return;
        case Direction::Descending:
            std::ranges::for_each(pendingValues, expectAsBefore);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    forEachListedValue(function, m_cfg, [&](ValueID id) {
        rememberState(id);
    });

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
        return grown != old && retype(parameter, widenAfterGivingUp(m_inputs.program, types, grown, m_statesBeforeRestart[parameter.index()].type));
    };

    // The last sweep changes nothing: every join is inside its parameter and every other type is what its rule gives.
    // A sweep that only retypes a value is not the last one: the value may go around a loop into a parameter the sweep has passed.
    for (bool didChange = true; didChange;) {
        didChange = false;
        for (BlockID id : m_cfg.reversePostOrder()) {
            const Block& block = function.at(id);
            for (uint32_t index = 0; index < block.parameters.size(); ++index) {
                ValueID parameter = block.parameters[index];
                if (id == entryBlock)
                    didChange |= retype(parameter, computeParameter(context, parameter, id, index));
                else
                    didChange |= widenParameter(parameter, id, index);
            }
            for (ValueID value : block.values)
                didChange |= retype(value, computeValue(context, value));
        }
    }
}

// An intersection may keep a part of one side that the subset test does not find in the other: the class Function met with every function.
TypeID TypePropagation::narrowerTypeOrNull(TypeID narrower, TypeID type)
{
    if (narrower == TypeTable::never || narrower == type)
        return { };

    TypeMemo& memo = (m_inputs.memo ? m_inputs.memo : &m_ownMemo)->narrowerTypes;
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
    Function& function = m_inputs.function;
    auto isRemembered = [&](ValueID id) {
        return !m_rememberedValues.isEmpty() && m_isRemembered.quickGet(id.index());
    };
    auto dropIfItSaysNothing = [&](ValueID id) {
        const Value& value = function.at(id);
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
        forEachListedValue(function, m_cfg, dropIfItSaysNothing);
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

TypePropagationOutcome TypePropagation::finishWithoutCompacting(Vector<ValueID>& parametersBehindRemovedEdges)
{
    // A join is widened where its list has grown past a limit, and a list grows where one side of the join gets narrower: a family joined with one of its members is the family,
    // the rest of the family joined with that member is longer than either. What an upcast of the join said still holds; the types no longer show it, so it is assumed.
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

    dropExpectedTypesThatSayNothing();
    countChanges();
    if (mayHaveCodeToCut())
        cutAfterNeverValues();

    if (m_outcome.cutBlocks)
        didCutBlocks(parametersBehindRemovedEdges);
    else if (m_outcome.didChange())
        m_inputs.analyses.didRetypeValues();
    return m_outcome;
}

void TypePropagation::didCutBlocks(Vector<ValueID>& parametersBehindRemovedEdges)
{
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
}

void TypePropagation::countChanges()
{
    RELEASE_ASSERT(!m_numberOfReportedValues);
    Function& function = m_inputs.function;
    for (ValueID id : m_rememberedValues) {
        const Value& value = function.at(id);
        const StateBeforeRestart& previous = m_statesBeforeRestart[id.index()];
        bool isRetyped = value.type != previous.type;
        bool isExpectedToBeOther = value.expected() != previous.expected;
        m_outcome.retypedValues += isRetyped;
        m_outcome.changedExpectedTypes += isExpectedToBeOther;
        if (isRetyped || isExpectedToBeOther)
            function.didEdit(id, ValueEdit::Type, previous.type, previous.expected);
    }
    m_numberOfReportedValues = m_rememberedValues.size();
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
    if (waitsForGenericVersion(m_inputs.program, function))
        return;
    auto isNever = [&](ValueID id) {
        return function.at(id).type == TypeTable::never;
    };
    // The type of such a narrowing is the holder's, which an access behind it that was resolved for what the narrowing used to leave does not fit.
    TypingContext typing { m_inputs.program, function, m_inputs.types };
    auto isNeverReached = [&](ValueID id) {
        const Value& value = function.at(id);
        return value.opcode == Opcode::NarrowByField && narrowsHolderToNothing(typing, typing.typeOf(value.valueOperand()), value.key(), typing.typeOf(value.readOperand()));
    };

    for (BlockID id : m_cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        bool endsInUnreachable = block.terminator.kind == TerminatorKind::Unreachable;
        if (id != entryBlock && std::ranges::any_of(block.parameters, isNever)) {
            if (!endsInUnreachable || !block.values.isEmpty())
                cutBlock(id, 0, block.terminator.origin());
            continue;
        }

        size_t index = block.values.findIf([&](ValueID value) {
            return (isNever(value) && resultRuleOf(function.at(value).opcode) != ResultRule::None) || isNeverReached(value);
        });
        if (index == notFound)
            continue;
        if (isNeverReached(block.values[index])) {
            cutBlock(id, static_cast<uint32_t>(index), function.at(block.values[index]).origin());
            continue;
        }
        if (!endsInUnreachable || index + 1 != block.values.size())
            cutBlock(id, static_cast<uint32_t>(index + 1), function.at(block.values[index]).origin());
    }
}

void TypePropagation::cutBlock(BlockID id, uint32_t firstErasedIndex, Origin origin)
{
    Block& block = m_inputs.function.edit(id);
    block.values.shrink(firstErasedIndex);
    block.terminator.become(TerminatorKind::Unreachable);
    block.terminator.setOrigin(origin);
    ++m_outcome.cutBlocks;
}

void TypePropagation::typeAgain(ValueID id, bool isParameter, BlockID block, uint32_t index)
{
    willTypeValues();
    rememberState(id);
    Value& value = m_inputs.function.editAndReportLater(id);
    TypingContext proving = typingContext(TypeLevel::Proven);
    value.type = isParameter ? computeParameter(proving, id, block, index) : computeValue(proving, id);

    TypingContext expecting = typingContext(TypeLevel::Expected);
    TypeID computed = isParameter ? computeParameter(expecting, id, block, index) : computeValue(expecting, id);
    TypeID expected = mayAllocateOtherLayout(m_inputs.program, value) ? computed : meetWithPreviousExpectation(id, computed);
    if (expected != TypeTable::never)
        expected = partOfExpectedTypeInside(value.type, expected);
    value.setExpected(expected == value.type ? TypeID() : expected);
}

// A join of arguments that all have the types of the parameter gives those types again.
bool TypePropagation::hasTypesOfEveryArgument(BlockID block, uint32_t index, const Value& parameter) const
{
    const Function& function = m_inputs.function;
    std::span<const Edge> edges = m_cfg.incomingEdges(block);
    auto isSeldomTaken = [&](Edge edge) {
        return function.at(edge.from).frequency != BlockFrequency::Normal;
    };
    bool countsSeldomTakenEdges = std::ranges::all_of(edges, isSeldomTaken);
    TypeID expected = parameter.expectedOrTypeForTyping();
    for (Edge edge : edges) {
        const Target& target = targetOf(function, edge);
        if (index >= target.arguments.size())
            return false;
        const Value& argument = function.at(target.arguments[index]);
        if (argument.type != parameter.type)
            return false;
        if ((countsSeldomTakenEdges || !isSeldomTaken(edge)) && argument.expectedOrTypeForTyping() != expected)
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
        // Nothing may have reached it only since the blocks that it leads to were last typed: they have lost an edge then, and no entry says so.
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
    // In a cycle a parameter may be typed from itself: there it matters which values are passed, whatever their types.
    bool mayBeTypedFromItself = m_cycles && m_cycles->isInCycle(blockID);
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        ValueID id = parameters[index];
        if (!mayBeTypedFromItself && blockID != entryBlock && hasTypesOfEveryArgument(blockID, index, function.at(id)))
            continue;
        m_placesOfParameters[id.index()] = { blockID.index(), index };
        enqueue(id);
    }
}

// What the log of changes has named since types were last propagated. A value that was edited is computed again. Where what is typed from it was typed from other types than it has now,
// that follows. The parameters of a block are joins over its incoming edges.
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

    // Where a value may be typed from itself, it matters which values it is typed from, whatever their types.
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
        // Only the lists of uses say where a parameter stands.
        if (value.opcode == Opcode::Parameter)
            willRestartValues();
        enqueue(change.value);
        if (change.edits.contains(ValueEdit::Definition) && opcodesWithIndirectUsers.contains(value.opcode)) {
            willRestartValues();
            enqueueIndirectUsersOf(change.value);
        }
        // The first entry holds the types that whatever is typed from the value has last seen.
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
        if (change.edits.contains(BlockEdit::OperandsOfSameTypes)) {
            if (m_cycles) {
                function.at(change.block).forEachTarget([&](const Target& target) {
                    if (target.block && m_cfg.isReachable(target.block) && m_cycles->isInCycle(target.block))
                        std::ranges::for_each(function.at(target.block).parameters, enqueueIfTypedFromItself);
                });
            }
            continue;
        }
        if (!change.edits.containsAny(editsOfWhatIsPassedOn) || didLookAtTargetsOf.set(change.block.index()))
            continue;
        function.at(change.block).forEachTarget([&](const Target& target) {
            if (target.block)
                enqueueParametersOf(target.block);
        });
    }
}

void TypePropagation::typePendingValues(PendingTyping& pending)
{
    m_everyOtherValueIsAsTyped = true;
    m_dropsExpectedTypesAtOnce = true;
    enqueuePendingValues(pending);
    if (!m_worklist.isEmpty())
        typeQueuedValues();
}

// Every value in the list is computed from the types that its sources have now, and what is typed from it follows where its types come out different. In which order does not matter
// where no value is typed from itself: the last time a value is computed, its sources have the types they keep.
// From its current type a value that depends on itself only confirms that type. So the values that are typed from one another start over together.
void TypePropagation::typeQueuedValues()
{
    const Function& function = m_inputs.function;
    if (m_cycles)
        willRestartValues();
    // The first in the list have their turn first.
    std::ranges::reverse(m_worklist);

    while (true) {
        while (!m_worklist.isEmpty()) {
            ValueID id = m_worklist.takeLast();
            m_isQueued.quickClear(id.index());
            const Value& value = function.at(id);
            bool isParameter = value.opcode == Opcode::Parameter;
            BlockID block;
            uint32_t index = 0;
            if (m_uses) {
                ValueLocation location = m_uses->locationOf(id);
                if (location.placement == ValuePlacement::Detached || !m_cfg.isReachable(location.block))
                    continue;
                block = location.block;
                index = location.index;
            } else if (isParameter) {
                block = BlockID(m_placesOfParameters[id.index()].block);
                index = m_placesOfParameters[id.index()].index;
            }

            if (m_cycles) {
                if (std::optional<unsigned> cycle = m_cycles->outermostCycleOf(block)) {
                    uint32_t component = componentOf(id, *cycle);
                    if (component != noComponent) {
                        if (!m_isWaitingComponent.set(component))
                            m_waitingComponents.append(component);
                        continue;
                    }
                }
            }

            TypeID type = value.type;
            TypeID expected = value.expectedOrTypeForTyping();
            typeAgain(id, isParameter, block, index);
            if (value.type != type || value.expectedOrTypeForTyping() != expected) {
                willRestartValues();
                enqueueUsersOf(id);
            }
        }
        if (m_waitingComponents.isEmpty())
            return;

        // A component has a smaller number than those of its cycle that are typed from it.
        size_t position = std::ranges::min_element(m_waitingComponents) - m_waitingComponents.begin();
        uint32_t component = m_waitingComponents[position];
        m_waitingComponents[position] = m_waitingComponents.last();
        m_waitingComponents.removeLast();
        m_isWaitingComponent.quickClear(component);
        typeComponent(component);
    }
}

// The values of a cycle of blocks that are typed from themselves. Such a value is typed, by way of others, from a parameter of a block that an edge from further down leads to, and that
// parameter from it. So a walk from those parameters to what they are typed from, and no further than the cycle, meets them all.
uint32_t TypePropagation::componentOf(ValueID id, unsigned cycle)
{
    if (m_componentOfValue.isEmpty())
        m_componentOfValue.fill(notVisited, m_inputs.function.values.size());
    if (!m_isSearchedCycle.set(cycle))
        findComponentsOf(cycle);
    uint32_t component = m_componentOfValue[id.index()];
    return component == notVisited ? noComponent : component;
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
    // While a value is on the stack, its entry holds its number, with this bit.
    constexpr uint32_t isOnStack = 1u << 31;
    uint32_t nextNumber = 0;

    auto push = [&](ValueID id) {
        uint32_t number = nextNumber++;
        m_componentOfValue[id.index()] = number | isOnStack;
        stack.append(id);
        uint32_t firstSource = sources.size();
        auto addSource = [&](ValueID source) {
            if (!source)
                return;
            ValueLocation location = m_uses->locationOf(source);
            if (location.placement != ValuePlacement::Detached && m_cfg.isReachable(location.block) && m_cycles->outermostCycleOf(location.block) == cycle)
                sources.append(source);
        };
        ValueLocation location = m_uses->locationOf(id);
        if (location.placement != ValuePlacement::Parameter) {
            for (ValueID operand : function.at(id).operands())
                addSource(operand);
        } else {
            for (Edge edge : m_cfg.incomingEdges(location.block)) {
                const Target& target = targetOf(function, edge);
                if (location.index < target.arguments.size())
                    addSource(target.arguments[location.index]);
                else if (!edge.isHandlerEdge()) {
                    for (ValueID operand : function.at(edge.from).terminator.operands())
                        addSource(operand);
                }
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

            Frame finished = frame;
            frames.removeLast();
            sources.shrink(frames.isEmpty() ? 0 : frames.last().endOfSources);
            if (!frames.isEmpty())
                frames.last().lowest = std::min(frames.last().lowest, finished.lowest);
            if (finished.lowest != finished.number)
                continue;
            if (stack.last() == finished.value && !finished.isTypedFromItselfAlone) {
                stack.removeLast();
                m_componentOfValue[finished.value.index()] = noComponent;
                continue;
            }
            uint32_t component = m_firstMemberOfComponent.size();
            m_firstMemberOfComponent.append(m_membersOfComponents.size());
            while (true) {
                ValueID member = stack.takeLast();
                m_componentOfValue[member.index()] = component;
                m_membersOfComponents.append(member);
                if (member == finished.value)
                    break;
            }
        }
    };

    for (BlockID blockID : m_cfg.reversePostOrder()) {
        if (m_cycles->outermostCycleOf(blockID) != cycle)
            continue;
        const Block& block = function.at(blockID);
        if (block.parameters.isEmpty())
            continue;
        uint32_t order = m_cfg.reversePostOrderIndex(blockID);
        bool isHeader = std::ranges::any_of(m_cfg.incomingEdges(blockID), [&](Edge edge) {
            return m_cfg.reversePostOrderIndex(edge.from) >= order;
        });
        if (!isHeader)
            continue;
        for (ValueID parameter : block.parameters)
            search(parameter);
    }
}

void TypePropagation::typeComponent(uint32_t component)
{
    const Function& function = m_inputs.function;
    size_t first = m_firstMemberOfComponent[component];
    size_t end = component + 1 < m_firstMemberOfComponent.size() ? m_firstMemberOfComponent[component + 1] : m_membersOfComponents.size();
    std::span<const ValueID> members = m_membersOfComponents.span().subspan(first, end - first);

    Vector<std::pair<uint64_t, ValueID>, 32> positions;
    Vector<std::pair<TypeID, TypeID>, 32> typesBefore;
    for (ValueID id : members) {
        ValueLocation location = m_uses->locationOf(id);
        uint64_t isInBlock = location.placement == ValuePlacement::InBlock;
        positions.append({ static_cast<uint64_t>(m_cfg.reversePostOrderIndex(location.block)) << 33 | isInBlock << 32 | location.index, id });
    }
    std::ranges::sort(positions, std::ranges::greater { }, &std::pair<uint64_t, ValueID>::first);

    m_restartedValuesFromLastToFirst.shrink(0);
    m_mayBeQueued.ensureSize(function.values.size());
    for (auto [position, id] : positions) {
        const Value& value = function.at(id);
        typesBefore.append({ value.type, value.expectedOrTypeForTyping() });
        restart(id);
        m_mayBeQueued.quickSet(id.index());
        m_restartedValuesFromLastToFirst.append(id);
    }
    m_didListRestartedValues = true;
    m_queuesOnlySomeValues = true;
    solveTypes();
    solveExpectedTypes();
    m_queuesOnlySomeValues = false;

    for (auto [position, id] : positions) {
        const Value& value = function.at(id);
        if (TypeID expected = value.expected())
            editTypeOf(id).setExpected(narrowerTypeOrNull(expected, value.type));
        else if (value.type == TypeTable::never)
            editTypeOf(id).setExpected({ });
    }
    // The members have come to rest, so only what stands outside follows.
    m_queuesAllButSomeValues = true;
    for (size_t index = 0; index < positions.size(); ++index) {
        const Value& value = function.at(positions[index].second);
        if (value.type != typesBefore[index].first || value.expectedOrTypeForTyping() != typesBefore[index].second)
            enqueueUsersOf(positions[index].second);
    }
    m_queuesAllButSomeValues = false;
    for (auto [position, id] : positions)
        m_mayBeQueued.quickClear(id.index());
}

// Whatever this has typed was computed by its rule behind every edit that the log held when this began, and so was all that is typed from it.
void TypePropagation::tellWhatIsTyped()
{
    PendingTyping* pending = m_inputs.memo ? m_inputs.memo->pendingTyping : nullptr;
    const ChangeLog* log = m_inputs.function.changeLog();
    if (!pending || !log)
        return;
    reportRetypedValues();
    if (log->hasForgottenSince(m_logAtStart))
        return;
    if (m_didTypeAllValues || m_didGiveUp) {
        // What it has written into the log itself: which types have changed, an upcast that it has replaced, a block that it has cut.
        pending->didTypeEverythingBefore(m_logAtStart);
        pending->takeEntries(*log);
        pending->forgetChangesOfTypesAlone();
        return;
    }
    if (pending->isEverythingPending(*log))
        return;

    // Where it stops behind a value that keeps its types, what is typed from a value that somebody has given other types by hand may not have had its turn.
    if (m_everyOtherValueIsAsTyped)
        return;

    const Function& function = m_inputs.function;
    auto isTyped = [&](ValueID id) {
        return !m_rememberedValues.isEmpty() && id.index() < m_isRemembered.size() && m_isRemembered.quickGet(id.index());
    };
    auto areParametersTyped = [&](BlockID block) {
        return std::ranges::all_of(function.at(block).parameters, isTyped);
    };
    auto isStillPending = [&](const BlockChange& change) {
        if (!areParametersTyped(change.block))
            return true;
        bool isPending = false;
        function.at(change.block).forEachTarget([&](const Target& target) {
            isPending |= target.block && !areParametersTyped(target.block);
        });
        return isPending;
    };

    size_t numberOfOwnValueEntries = log->valuesSince(m_logAtStart).size();
    size_t numberOfOwnBlockEntries = log->blocksSince(m_logAtStart).size();
    pending->takeEntries(*log);
    size_t firstOwnValueEntry = pending->m_values.size() - numberOfOwnValueEntries;
    size_t firstOwnBlockEntry = pending->m_blocks.size() - numberOfOwnBlockEntries;
    size_t kept = 0;
    for (size_t index = 0; index < pending->m_values.size(); ++index) {
        const ValueChange& change = pending->m_values[index];
        // Its own entries say which types have changed, but for an upcast that it has replaced.
        bool isPending = index < firstOwnValueEntry ? !isTyped(change.value) : change.edits.containsAny({ ValueEdit::Added, ValueEdit::Definition });
        if (isPending)
            pending->m_values[kept++] = change;
    }
    pending->m_values.shrink(kept);
    kept = 0;
    for (size_t index = 0; index < pending->m_blocks.size(); ++index) {
        const BlockChange& change = pending->m_blocks[index];
        if (index >= firstOwnBlockEntry || isStillPending(change))
            pending->m_blocks[kept++] = change;
    }
    pending->m_blocks.shrink(kept);
}

namespace {

enum class TypedFirst : uint8_t { Nothing, DependentsOfChangedValues, AllValues, PendingValues };
using ChangedValues = TypePropagation::ChangedValues;

} // anonymous namespace

static PendingTyping* pendingTypingOf(const TypePropagationInputs& inputs)
{
    return inputs.memo && inputs.function.changeLog() ? inputs.memo->pendingTyping : nullptr;
}

static TypePropagationOutcome typeUntilNothingIsCut(const TypePropagationInputs& inputs, TypedFirst typedFirst, std::span<const ValueID> changedValues = { }, ChangedValues kind = ChangedValues::MayLeaveOthersOutOfDate)
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
            propagation.typeDependentsOf(changedValues, kind);
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
            // What it has written into the log itself: which types have changed, an upcast that it has replaced, a block that it has cut.
            pending->didTypePendingValues();
            pending->takeEntries(*inputs.function.changeLog());
            pending->forgetChangesOfTypesAlone();
        } else if (typedFirst != TypedFirst::Nothing)
            propagation.tellWhatIsTyped();
        total += outcome;
        if (!outcome.cutBlocks || parametersBehindRemovedEdges.isEmpty())
            break;
        // The log names the blocks that have lost an edge.
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
    return total;
}

TypePropagationOutcome typeValuesFromNever(const TypePropagationInputs& inputs)
{
    return typeUntilNothingIsCut(inputs, TypedFirst::AllValues);
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

TypePropagationOutcome finishTypedValues(const TypePropagationInputs& inputs)
{
    return typeUntilNothingIsCut(inputs, TypedFirst::Nothing);
}

TypePropagationOutcome narrowValueTypes(const TypePropagationInputs& inputs, std::span<const ValueID> changedValues)
{
    return typeUntilNothingIsCut(inputs, TypedFirst::DependentsOfChangedValues, changedValues);
}

TypePropagationOutcome narrowValueTypesBehindChanges(const TypePropagationInputs& inputs, std::span<const ValueID> changedValues)
{
    return typeUntilNothingIsCut(inputs, TypedFirst::DependentsOfChangedValues, changedValues, ChangedValues::EveryOtherValueIsAsTyped);
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

void PendingTyping::didTypeEverything(const ChangeLog& log)
{
    didTypePendingValues();
    m_isEverythingPending = false;
    m_seen = log.end();
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

// Renumbering makes the log forget. What is pending is pending afterwards, under its new number.
bool PendingTyping::compact(Function& function, Renumbering* renumberingForCaller)
{
    const ChangeLog& log = *function.changeLog();
    takeEntries(log);
    bool wasEverythingPending = isEverythingPending(log);
    Renumbering renumbering;
    bool didMove = compactFunction(function, &renumbering);
    if (didMove && !wasEverythingPending) {
        size_t kept = 0;
        for (ValueChange change : m_values) {
            change.value = renumbering.newIDOf(change.value);
            if (change.value)
                m_values[kept++] = change;
        }
        m_values.shrink(kept);
        kept = 0;
        for (BlockChange change : m_blocks) {
            change.block = renumbering.newIDOf(change.block);
            if (change.block)
                m_blocks[kept++] = change;
        }
        m_blocks.shrink(kept);
        m_seen = log.end();
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

// That the types hold is for the verifier to say. Here they are held to say no less than typing every value says.
static ValueID findValueTypedWider(const TypePropagationInputs& inputs, const Function& typedFromNever)
{
    const Function& function = inputs.function;
    // A narrower type may have cut a block that the wider one leaves, and a cut renumbers the values.
    if (function.values.size() != typedFromNever.values.size())
        return { };
    auto isTypedWider = [&](ValueID id) {
        TypeID type = function.at(id).type;
        TypeID fromNever = typedFromNever.at(id).type;
        return type != fromNever && !inputs.types.at(type).isSubsetOf(inputs.types.at(fromNever), inputs.program);
    };
    // Typing from never visits no block that nothing reaches, so a value there stays never, whatever type the pass that made it gave it.
    for (BlockID block : inputs.analyses.cfg().reversePostOrder()) {
        for (ValueID parameter : function.at(block).parameters) {
            if (isTypedWider(parameter))
                return parameter;
        }
        for (ValueID value : function.at(block).values) {
            if (isTypedWider(value))
                return value;
        }
    }
    return { };
}

TypePropagationOutcome typeChangedValues(const TypePropagationInputs& inputs)
{
    PendingTyping* pending = pendingTypingOf(inputs);
    RELEASE_ASSERT(pending);
    const ChangeLog& log = *inputs.function.changeLog();
    Statistics& statistics = inputs.build.statistics();
    if (pending->isEmpty(log)) {
        statistics.count(StatisticID::FunctionsTypedAsBefore);
        inputs.function.setValuesAreTypedAsDefined();
        return { };
    }

    TypePropagationOutcome outcome;
    if (pending->isEverythingPending(log)) {
        statistics.count(StatisticID::FunctionsTypedFromNever);
        outcome = typeValuesFromNever(inputs);
        // Whatever stood behind a removed edge was typed again.
        if (!outcome.replacedUpcasts)
            pending->didTypeEverything(log);
    } else {
        std::optional<Function> typedFromNever;
        if (inputs.build.options().verifiesAfterEveryPass) {
            typedFromNever = inputs.function;
            FunctionAnalyses analyses(inputs.analyses.context(), *typedFromNever);
            typeValuesFromNever({ inputs.program, inputs.id, *typedFromNever, analyses, inputs.types, inputs.build });
        }
        outcome = typeUntilNothingIsCut(inputs, TypedFirst::PendingValues, { }, ChangedValues::EveryOtherValueIsAsTyped);
        statistics.count(outcome.didChange() ? StatisticID::FunctionsTypedFromChanges : StatisticID::FunctionsTypedAsBefore);
        if (typedFromNever) {
            if (ValueID value = findValueTypedWider(inputs, *typedFromNever))
                inputs.build.diagnostics().add(InternalError { .kind = InternalErrorKind::TypeOutOfDate, .function = inputs.analyses.subject().function, .value = value });
        }
    }
    // An upcast that was replaced has still to be typed.
    if (pending->isEmpty(log))
        inputs.function.setValuesAreTypedAsDefined();
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
        // What is expected of the value from here on stands on a cast that changes nothing else.
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
