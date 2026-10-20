/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypePropagation.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include <algorithm>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet requestOpcodes { Opcode::ClaimDeclared, Opcode::ClaimRead };
static constexpr uint32_t updatesBeforeJoiningBound = 8;

enum class WrittenTypes : uint8_t { MayExceedTypes, AreInsideTypes };

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
    case TypeLevel::Written:
        value.setWritten(type);
        return;
    case TypeLevel::Predicted:
        value.setPrediction(type);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// An intersection may keep a part of one side that the subset test does not find in the other: the class Function met with every function.
static TypeID narrowerTypeOrNull(const Program& program, TypeID narrower, TypeID type)
{
    if (narrower == TypeTable::never || narrower == type || !program.at(narrower).isSubsetOf(program.at(type), program))
        return { };
    return narrower;
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
{
    size_t numberOfValues = m_inputs.function.values.size();
    m_isQueued.ensureSize(numberOfValues);
    m_isRestarted.ensureSize(numberOfValues);
    m_parameterPositions.grow(numberOfValues);
    m_numberOfUpdates.fill(0, numberOfValues);
    collectPreviousTypes();
    collectParameters();
}

void TypePropagation::collectPreviousTypes()
{
    const Function& function = m_inputs.function;
    m_previousTypes.reserveInitialCapacity(function.values.size());
    for (const Value& value : function.values)
        m_previousTypes.append(StoredTypes { value.type, value.written(), value.prediction() });
}

void TypePropagation::collectParameters()
{
    const Function& function = m_inputs.function;
    for (BlockID id : m_inputs.analyses.cfg().reversePostOrder()) {
        const CompactList<ValueID>& parameters = function.at(id).parameters;
        for (uint32_t index = 0; index < parameters.size(); ++index)
            m_parameterPositions[parameters[index].index()] = ParameterPosition { id, index };
    }
}

void TypePropagation::restart(ValueID id)
{
    Value& value = m_inputs.function.at(id);
    value.type = TypeTable::never;
    value.setWritten({ });
    value.setPrediction({ });
    m_isRestarted.quickSet(id.index());
}

void TypePropagation::restartAllValues()
{
    auto restartValue = [&](ValueID id) {
        restart(id);
    };
    // The verifier reads the type ids of unreachable blocks too.
    for (const Block& block : m_inputs.function.blocks) {
        std::ranges::for_each(block.parameters, restartValue);
        std::ranges::for_each(block.values, restartValue);
    }
}

// From its current type a value that depends on itself only confirms that type, so everything a changed value reaches starts over.
void TypePropagation::restartDependentsOf(std::span<const ValueID> values)
{
    for (ValueID id : values)
        enqueue(id);
    for (size_t index = 0; index < m_worklist.size(); ++index)
        enqueueUsersOf(m_worklist[index]);

    for (ValueID id : std::exchange(m_worklist, { })) {
        m_isQueued.quickClear(id.index());
        restart(id);
    }
}

bool TypePropagation::isEntryParameter(ValueID id) const
{
    const std::optional<ParameterPosition>& position = m_parameterPositions[id.index()];
    return position && position->block == entryBlock;
}

bool TypePropagation::isJoin(ValueID id) const
{
    const std::optional<ParameterPosition>& position = m_parameterPositions[id.index()];
    return position && m_inputs.analyses.cfg().incomingEdges(position->block).size() > 1;
}

void TypePropagation::enqueue(ValueID id)
{
    RELEASE_ASSERT(id.index() < m_numberOfUpdates.size());
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

// A ResultAt, an operand narrowed on an edge and the ClaimRead of a read by key are typed from the operands of the value they name,
// whose own type may stay the same.
void TypePropagation::enqueueIndirectUsersOf(ValueID id)
{
    const Function& function = m_inputs.function;
    const Value& value = function.at(id);
    bool isTest = provesOf(value.opcode) != Proof::None;
    bool hasMultipleResults = numberOfResultsOf(m_inputs.program, function, value).has_value();
    bool isReadByKey = value.opcode == Opcode::ElemGet;
    if (!isTest && !hasMultipleResults && !isReadByKey)
        return;

    for (const Use& use : m_inputs.analyses.useLists().usesOf(id)) {
        if (hasMultipleResults && use.kind == Use::Kind::Operand)
            enqueue(use.user);
        if (isReadByKey && use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ClaimRead)
            enqueue(use.user);
        if (isTest && use.kind == Use::Kind::TerminatorOperand && function.at(use.block).terminator.isConditionalBranch())
            enqueueEdgeResultParameters(use.block);
    }
}

void TypePropagation::enqueueUsersOf(ValueID id)
{
    const Function& function = m_inputs.function;
    for (const Use& use : m_inputs.analyses.useLists().usesOf(id)) {
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
    const Function& function = m_inputs.function;
    auto enqueueIfRestarted = [&](ValueID id) {
        if (m_isRestarted.quickGet(id.index()))
            enqueue(id);
    };
    std::span<const BlockID> order = m_inputs.analyses.cfg().reversePostOrder();
    for (size_t blockIndex = order.size(); blockIndex--;) {
        const Block& block = function.at(order[blockIndex]);
        for (size_t index = block.values.size(); index--;)
            enqueueIfRestarted(block.values[index]);
        for (size_t index = block.parameters.size(); index--;)
            enqueueIfRestarted(block.parameters[index]);
    }
}

void TypePropagation::propagate(TypeLevel level, Direction direction)
{
    const Function& function = m_inputs.function;
    TypingContext context { m_inputs.program, function, m_inputs.types, level, m_inputs.fieldReadTypes ? m_inputs.fieldReadTypes : &m_fieldReadTypes };
    while (!m_worklist.isEmpty()) {
        ValueID id = m_worklist.takeLast();
        m_isQueued.quickClear(id.index());

        TypeID computed;
        if (m_parameterPositions[id.index()])
            computed = computeParameter(context, id);
        else if (requestOpcodes.contains(function.at(id).opcode))
            computed = computeRequest(context, function.at(id));
        else
            computed = computeValue(context, id);

        if (update(level, direction, id, computed))
            enqueueUsersOf(id);
    }
}

void TypePropagation::solve(TypeLevel level)
{
    enqueueRestartedValues();
    propagate(level, Direction::Ascending);

    // Where the ascent stopped every type holds, so whatever the rules give from here on holds too: it takes back what widening added without need,
    // and what a rule answered for a narrower operand and no longer answers.
    for (ValueID value : std::exchange(m_valuesAboveTheirRules, { }))
        enqueue(value);
    propagate(level, Direction::Descending);
    m_numberOfUpdates.fill(0);
}

static bool hasUnresolvedTailCast(const Function& function, const CFG& cfg)
{
    for (BlockID id : cfg.reversePostOrder()) {
        const Terminator& terminator = function.at(id).terminator;
        if (terminator.kind != TerminatorKind::ReturnCall)
            continue;
        for (TailCast cast : terminator.casts()) {
            if (cast.resolution == CastResolution::Unresolved)
                return true;
        }
    }
    return false;
}

static bool asksForWrittenType(const Value& value, bool reportsMismatches)
{
    return value.opcode == Opcode::ClaimRead || (reportsMismatches && value.opcode == Opcode::ClaimDeclared);
}

void TypePropagation::solveTypes()
{
    Function& function = m_inputs.function;
    TypeArena& types = m_inputs.types;
    const CFG& cfg = m_inputs.analyses.cfg();

    // A value is reported where it enters the written types, so a request that only reports asks for the written type of its operand.
    bool reportsMismatches = m_inputs.build.options().recorded.policy() == MismatchPolicy::Report;
    bool readsWrittenTypes = reportsMismatches && hasUnresolvedTailCast(function, cfg);
    bool hasWrittenTypes = false;
    forEachListedValue(function, cfg, [&](ValueID id) {
        const Value& value = function.at(id);
        readsWrittenTypes |= asksForWrittenType(value, reportsMismatches);
        hasWrittenTypes |= !!value.written();
    });

    if (readsWrittenTypes) {
        // The type of a ClaimRead depends on the written type of its receiver.
        forEachListedValue(function, cfg, [&](ValueID id) {
            if (m_isRestarted.quickGet(id.index()))
                function.at(id).setWritten(TypeTable::never);
        });
        solve(TypeLevel::Written);
    } else if (hasWrittenTypes) {
        // Only a request asks what was written. Without one a written type is a prediction and nothing more.
        forEachListedValue(function, cfg, [&](ValueID id) {
            Value& value = function.at(id);
            if (!value.written())
                return;
            if (!value.prediction())
                value.setPrediction(value.written());
            value.setWritten({ });
        });
    }
    solve(TypeLevel::Proven);

    if (!readsWrittenTypes)
        return;
    forEachListedValue(function, cfg, [&](ValueID id) {
        Value& value = function.at(id);
        if (TypeID written = value.written())
            value.setWritten(narrowerTypeOrNull(m_inputs.program, types.intersectionOf(value.type, written), value.type));
    });
}

void TypePropagation::solvePredictions()
{
    Function& function = m_inputs.function;
    forEachListedValue(function, m_inputs.analyses.cfg(), [&](ValueID id) {
        if (m_isRestarted.quickGet(id.index()))
            function.at(id).setPrediction(TypeTable::never);
    });
    solve(TypeLevel::Predicted);
}

TypeID TypePropagation::computeValue(const TypingContext& context, ValueID id)
{
    return context.types.intern(computeResultType(context, context.function.at(id)));
}

TypeID TypePropagation::computeParameter(const TypingContext& context, ValueID id)
{
    const ParameterPosition& position = *m_parameterPositions[id.index()];
    if (isEntryParameter(id))
        return typeOfEntryParameter(context, position.index);

    const Function& function = context.function;
    auto isSeldomTaken = [&](Edge edge) {
        return function.at(edge.from).frequency != BlockFrequency::Normal;
    };
    // What a path that is seldom taken brings says nothing about what the parameter usually holds.
    auto incomingEdges = m_inputs.analyses.cfg().incomingEdges(position.block);
    bool skipsSeldomTakenEdges = context.predicts() && !std::ranges::all_of(incomingEdges, isSeldomTaken);

    TypeID joined = TypeTable::never;
    for (Edge edge : incomingEdges) {
        if (skipsSeldomTakenEdges && isSeldomTaken(edge))
            continue;
        // Typing runs before the verifier has checked the edges.
        const Target& target = targetOf(function, edge);
        TypeID incoming = TypeTable::anything;
        if (position.index < target.arguments.size())
            incoming = context.typeIDOf(target.arguments[position.index]);
        else if (!edge.isHandlerEdge()) {
            EdgeResultPosition result { edge.from, edge.targetIndex, position.index - static_cast<uint32_t>(target.arguments.size()) };
            if (tryGetEdgeResult(function, result))
                incoming = context.types.intern(computeTypeOnEdge(context, result));
        }
        joined = joinIntoParameter(context, id, joined, incoming);
    }
    return joined;
}

TypeID TypePropagation::joinIntoParameter(const TypingContext& context, ValueID parameter, TypeID current, TypeID incoming)
{
    if (current == incoming || incoming == TypeTable::never)
        return current;
    if (current == TypeTable::never)
        return incoming;

    TypeArena& types = context.types;
    WideningContext widening { context.program, types, m_inputs.build, m_inputs.analyses.subject(parameter) };
    return types.intern(types.at(current).unionWideningWith(types.at(incoming), widening));
}

TypeID TypePropagation::widenGrownParameter(ValueID parameter, TypeID old, TypeID grown)
{
    TypeArena& types = m_inputs.types;
    TypeID bound = m_previousTypes[parameter.index()].type;
    const Type& type = types.at(grown);

    Range widened = type.integers.widenedFrom(types.at(old).integers);
    // A range that has not left the one the parameter had before is widened no further than that one.
    if (bound && !widened.isEmpty() && type.integers.isSubsetOf(types.at(bound).integers))
        widened = widened.intersectWith(types.at(bound).integers);
    if (widened != type.integers) {
        Type withWidenedRange = type;
        withWidenedRange.integers = widened;
        grown = types.intern(withWidenedRange);
    }
    bool joinsBound = bound && m_numberOfUpdates[parameter.index()] >= updatesBeforeJoiningBound;
    return joinsBound ? types.unionOf(grown, bound) : grown;
}

bool TypePropagation::update(TypeLevel level, Direction direction, ValueID id, TypeID computed)
{
    TypeArena& types = m_inputs.types;
    Value& value = m_inputs.function.at(id);
    TypeID old = TypingContext { m_inputs.program, m_inputs.function, types, level }.typeIDOf(id);

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
    if (level == TypeLevel::Predicted && next != old) {
        TypeID known = value.writtenOrType();
        next = types.intersectionOf(known, next);
        if (next == TypeTable::never && direction == Direction::Descending)
            next = known;
    }
    if (next == old)
        return false;

    // The update that passes the limit still counts: a pass that asks for one value to be retyped relies on that type.
    setTypeAt(value, level, next);
    if (++m_numberOfUpdates[id.index()] > m_inputs.build.limit(LimitID::MaxTypeValuesRounds)) {
        giveUpOnRounds(level, direction);
        return false;
    }
    return true;
}

void TypePropagation::giveUpOnRounds(TypeLevel level, Direction direction)
{
    Function& function = m_inputs.function;
    TypeArena& types = m_inputs.types;
    const CFG& cfg = m_inputs.analyses.cfg();
    m_inputs.build.gaveUp(LimitID::MaxTypeValuesRounds, m_inputs.analyses.subject());

    Vector<ValueID> pendingValues = std::exchange(m_worklist, { });
    m_isQueued.clearAll();
    m_valuesAboveTheirRules.clear();

    if (level != TypeLevel::Proven) {
        auto forget = [&](ValueID id) {
            setTypeAt(function.at(id), level, { });
        };
        switch (direction) {
        case Direction::Ascending:
            forEachListedValue(function, cfg, [&](ValueID id) {
                if (m_isRestarted.quickGet(id.index()))
                    forget(id);
            });
            return;
        case Direction::Descending:
            std::ranges::for_each(pendingValues, forget);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    TypingContext context { m_inputs.program, function, types, TypeLevel::Proven };
    auto retype = [&](ValueID id, TypeID type) {
        Value& value = function.at(id);
        if (value.type == type)
            return false;
        value.type = type;
        return true;
    };
    auto widenParameter = [&](ValueID parameter) {
        TypeID old = function.at(parameter).type;
        TypeID grown = types.unionOf(old, computeParameter(context, parameter));
        return grown != old && retype(parameter, widenAfterGivingUp(m_inputs.program, types, grown, m_previousTypes[parameter.index()].type));
    };

    // The last sweep changes nothing: every join is inside its parameter and every other type is what its rule gives.
    // A sweep that only retypes a value is not the last one: the value may go around a loop into a parameter the sweep has passed.
    for (bool didChange = true; didChange;) {
        didChange = false;
        for (BlockID id : cfg.reversePostOrder()) {
            const Block& block = function.at(id);
            for (ValueID parameter : block.parameters) {
                if (id == entryBlock)
                    didChange |= retype(parameter, computeParameter(context, parameter));
                else
                    didChange |= widenParameter(parameter);
            }
            for (ValueID valueID : block.values) {
                const Value& value = function.at(valueID);
                didChange |= retype(valueID, requestOpcodes.contains(value.opcode) ? computeRequest(context, value) : computeValue(context, valueID));
            }
        }
    }
}

static Type declaredTypeOfRead(const TypingContext& positions, const Type& receiver, const Value& claimRead)
{
    ClaimSource source = claimRead.source();
    switch (source.kind()) {
    case ClaimSource::Kind::None:
        break;
    case ClaimSource::Kind::Field:
        if (!hasWrittenField(positions.program, receiver, source.as<ClaimSource::Field>().key))
            return Type::anything();
        return typeOfFieldRead(positions, receiver, source.as<ClaimSource::Field>().key);
    case ClaimSource::Kind::Contents: {
        if (receiver.classes.isEmpty())
            return Type::anything();
        // Only a promise hands over what it holds. A map that is awaited is the result itself, whatever its values are.
        bool isAwaited = positions.function.at(claimRead.valueOperand()).opcode == Opcode::Await;
        Type contents = isAwaited ? typeOfAwaited(positions, receiver) : typeOfContentsRead(positions, receiver, source.as<ClaimSource::Contents>().slot);
        // An array pattern with more elements than a typed array has reads undefined, and undefined and null pass every test.
        return positions.predicts() ? contents : contents.withPrimitives(Primitive::UndefinedOrNull);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID boundOfReadFromNullish(ClaimSource::Kind kind, TypeID receiverType)
{
    // A named read of undefined or null throws. Awaiting one gives it back.
    switch (kind) {
    case ClaimSource::Kind::None:
        break;
    case ClaimSource::Kind::Field:
        return TypeTable::never;
    case ClaimSource::Kind::Contents:
        return receiverType;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID findBoundIn(const TypingContext& positions, TypeID receiverType, const Value& claimRead)
{
    const Type& receiver = positions.types.at(receiverType);
    if (receiver.hasOnlyPrimitives(Primitive::UndefinedOrNull))
        return boundOfReadFromNullish(claimRead.source().kind(), receiverType);

    Type bound = declaredTypeOfRead(positions, receiver, claimRead);
    if (bound.isAnything())
        return { };
    return positions.types.intern(bound);
}

// `a[k]` reads an element only under a number. Under any other key it reads a property, which the contents say nothing about,
// and that the checker believes k is a number licenses no TypeError.
static bool mayReadProperty(const TypingContext& context, const Value& claimRead)
{
    ClaimSource source = claimRead.source();
    if (source.kind() != ClaimSource::Kind::Contents || source.as<ClaimSource::Contents>().slot != ContentsSlot::Element)
        return false;
    const Value& read = context.function.at(claimRead.valueOperand());
    if (read.opcode != Opcode::ElemGet)
        return false;
    const Type& key = context.typeOf(read.propertyOperand());
    return !key.isNever() && !key.isNumber();
}

static RequestBound::Source sourceAt(TypeLevel level)
{
    switch (level) {
    case TypeLevel::Proven:
        return RequestBound::Source::Type;
    case TypeLevel::Written:
        return RequestBound::Source::Written;
    case TypeLevel::Predicted:
        return RequestBound::Source::Prediction;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static RequestBound findBound(const TypingContext& context, const Value& claimRead, WrittenTypes writtenTypes)
{
    TypeArena& types = context.types;
    ValueID receiver = claimRead.receiverOperand();

    TypeID consulted;
    for (TypeLevel level : { TypeLevel::Proven, TypeLevel::Written, TypeLevel::Predicted }) {
        TypeID receiverType = context.atLevel(level).typeIDOf(receiver);
        if (level == TypeLevel::Written && writtenTypes == WrittenTypes::MayExceedTypes)
            receiverType = types.intersectionOf(context.function.at(receiver).type, receiverType);
        // The positions of a receiver say more at the predicted level: what an int32 promise holds is only there.
        if (receiverType == consulted && level != TypeLevel::Predicted)
            continue;
        consulted = receiverType;

        // Only the level being solved may conclude that the read never completes.
        if (level != context.level && types.at(receiverType).hasOnlyPrimitives(Primitive::UndefinedOrNull))
            continue;

        TypingContext positions = context.atLevel(level == TypeLevel::Predicted ? TypeLevel::Predicted : TypeLevel::Written);
        TypeID bound = findBoundIn(positions, receiverType, claimRead);
        if (!bound)
            continue;
        if (mayReadProperty(context.atLevel(TypeLevel::Proven), claimRead))
            return { RequestBound::Source::Prediction, bound };
        return { sourceAt(level), bound };
    }
    return { };
}

RequestBound boundOfClaimRead(const TypingContext& context, const Value& claimRead)
{
    return findBound(context, claimRead, WrittenTypes::AreInsideTypes);
}

TypeID TypePropagation::computeRequest(const TypingContext& context, const Value& request)
{
    TypeArena& types = context.types;
    TypeID sourceID = context.typeIDOf(request.valueOperand());
    const Type& source = types.at(sourceID);
    if (request.opcode == Opcode::ClaimDeclared) {
        // `e as int32` proves nothing about e: what the result is stored into is judged by e.
        DeclaredType declared = request.declared();
        bool isPromise = declared.flags.contains(PositionFlags::IsInt32Promise);
        if (isPromise && declared.site && context.program.at(declared.site).kind == TypeSiteKind::Assertion)
            return sourceID;
        // What was written where the value came from proves no shape, so it does not narrow what is written here: after `base as Derived`
        // a read is held to the fields of Derived.
        Type entering = context.level == TypeLevel::Written ? source.withoutProofs() : source;
        return types.intern(entering.castTo(context.typeOf(declared), types));
    }

    RELEASE_ASSERT(request.opcode == Opcode::ClaimRead);
    switch (context.level) {
    case TypeLevel::Proven: {
        if (m_inputs.build.options().recorded.policy() == MismatchPolicy::Report)
            return sourceID;
        RequestBound bound = findBound(context, request, WrittenTypes::MayExceedTypes);
        if (!bound.allowsClaim())
            return sourceID;
        return types.intern(source.castTo(types.at(bound.type).withoutProofs(), types));
    }
    case TypeLevel::Written: {
        TypeID bound = findBoundIn(context, context.typeIDOf(request.receiverOperand()), request);
        if (!bound || mayReadProperty(context, request))
            return sourceID;
        return types.intern(source.castTo(types.at(bound), types));
    }
    case TypeLevel::Predicted: {
        RequestBound bound = findBound(context, request, WrittenTypes::AreInsideTypes);
        if (bound.source == RequestBound::Source::NotFound)
            return sourceID;
        return types.intern(source.castTo(types.at(bound.type), types));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypePropagationOutcome TypePropagation::finish()
{
    Function& function = m_inputs.function;

    forEachListedValue(function, m_inputs.analyses.cfg(), [&](ValueID id) {
        Value& value = function.at(id);
        if (TypeID prediction = value.prediction())
            value.setPrediction(narrowerTypeOrNull(m_inputs.program, prediction, value.type));
    });

    countChanges();
    cutAfterNeverValues();

    if (m_outcome.cutBlocks) {
        compactFunction(function);
        m_inputs.analyses.didChangeControlFlow();
    } else if (m_outcome.didChange())
        m_inputs.analyses.didChangeValues();
    return m_outcome;
}

void TypePropagation::countChanges()
{
    const Function& function = m_inputs.function;
    for (size_t index = 0; index < m_previousTypes.size(); ++index) {
        const Value& value = function.values[index];
        const StoredTypes& previous = m_previousTypes[index];
        m_outcome.retypedValues += value.type != previous.type;
        m_outcome.changedWrittenTypes += value.written() != previous.written;
        m_outcome.changedPredictions += value.prediction() != previous.prediction;
    }
}

void TypePropagation::cutAfterNeverValues()
{
    const Function& function = m_inputs.function;
    auto isNever = [&](ValueID id) {
        return function.at(id).type == TypeTable::never;
    };

    for (BlockID id : m_inputs.analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(id);
        bool endsInUnreachable = block.terminator.kind == TerminatorKind::Unreachable;
        if (id != entryBlock && std::ranges::any_of(block.parameters, isNever)) {
            if (!endsInUnreachable || !block.values.isEmpty())
                cutBlock(id, 0, block.terminator.origin());
            continue;
        }

        size_t index = block.values.findIf([&](ValueID value) {
            return isNever(value) && resultRuleOf(function.at(value).opcode) != ResultRule::None;
        });
        if (index == notFound)
            continue;
        if (!endsInUnreachable || index + 1 != block.values.size())
            cutBlock(id, static_cast<uint32_t>(index + 1), function.at(block.values[index]).origin());
    }
}

void TypePropagation::cutBlock(BlockID id, uint32_t firstErasedIndex, Origin origin)
{
    Block& block = m_inputs.function.at(id);
    block.values.shrink(firstErasedIndex);
    block.terminator.become(TerminatorKind::Unreachable);
    block.terminator.setOrigin(origin);
    ++m_outcome.cutBlocks;
}

TypePropagationOutcome typeValuesFromNever(const TypePropagationInputs& inputs)
{
    TypePropagationOutcome total;
    while (true) {
        TypePropagation propagation(inputs);
        propagation.restartAllValues();
        propagation.solveTypes();
        propagation.solvePredictions();
        TypePropagationOutcome outcome = propagation.finish();
        total += outcome;
        // A cut removes edges, and the parameters they led to were joined over them.
        if (!outcome.cutBlocks)
            return total;
    }
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
    propagation.restartAllValues();
    propagation.solveTypes();
    propagation.solvePredictions();
}

TypePropagationOutcome finishTypedValues(const TypePropagationInputs& inputs)
{
    TypePropagationOutcome outcome = TypePropagation(inputs).finish();
    if (outcome.cutBlocks)
        outcome += typeValuesFromNever(inputs);
    return outcome;
}

TypePropagationOutcome narrowValueTypes(const TypePropagationInputs& inputs, std::span<const ValueID> changedValues)
{
    TypePropagation propagation(inputs);
    propagation.restartDependentsOf(changedValues);
    propagation.solveTypes();
    propagation.solvePredictions();
    TypePropagationOutcome outcome = propagation.finish();
    // A cut renumbers the values, so the parameters behind the removed edges are no longer known by id.
    if (outcome.cutBlocks)
        outcome += typeValuesFromNever(inputs);
    return outcome;
}

namespace {

class DefinitionHasher {
public:
    void add(uint32_t word)
    {
        m_hash = (m_hash ^ word) * 0x9e3779b97f4a7c15ull;
        m_hash ^= m_hash >> 29;
    }

    void add(ValueID id) { add(id.index()); }
    void add(BlockID id) { add(id.index()); }

    void add(const Instruction& instruction)
    {
        add(instruction.flags);
        for (uint32_t word : instruction.inlinePayload())
            add(word);
        add(static_cast<uint32_t>(instruction.outOfLinePayload().size()));
        for (uint32_t word : instruction.outOfLinePayload())
            add(word);
        uint32_t numberOfOperands = instruction.numberOfOperands();
        add(numberOfOperands);
        for (uint32_t index = 0; index < numberOfOperands; ++index)
            add(instruction.operand(index));
    }

    void add(const Target& target)
    {
        add(target.block);
        add(static_cast<uint32_t>(target.arguments.size()));
        for (ValueID argument : target.arguments)
            add(argument);
        add(static_cast<uint32_t>(target.proofs.size()));
        for (uint32_t proof : target.proofs)
            add(proof);
    }

    // Zero stands for a value that no reachable block lists.
    uint64_t hash() const { return m_hash ? m_hash : 1; }

private:
    uint64_t m_hash { 0x243f6a8885a308d3ull };
};

} // anonymous namespace

static uint64_t hashOfValue(const Value& value)
{
    DefinitionHasher hasher;
    hasher.add(static_cast<uint32_t>(value.opcode));
    hasher.add(static_cast<const Instruction&>(value));
    return hasher.hash();
}

// A parameter is typed from what comes in over each edge: an argument, or what the terminator at the other end says about the edge.
static uint64_t hashOfParameter(const Function& function, const CFG& cfg, BlockID block, uint32_t index)
{
    DefinitionHasher hasher;
    hasher.add(block);
    hasher.add(index);
    for (Edge edge : cfg.incomingEdges(block)) {
        const Block& from = function.at(edge.from);
        const Target& target = targetOf(function, edge);
        hasher.add(edge.from);
        hasher.add(edge.targetIndex);
        hasher.add(static_cast<uint32_t>(edge.kind));
        hasher.add(static_cast<uint32_t>(from.frequency));
        if (index < target.arguments.size()) {
            hasher.add(target.arguments[index]);
            continue;
        }
        hasher.add(static_cast<uint32_t>(from.terminator.kind));
        hasher.add(static_cast<const Instruction&>(from.terminator));
        hasher.add(static_cast<uint32_t>(from.terminator.targets.size()));
        hasher.add(target);
    }
    return hasher.hash();
}

std::optional<Vector<ValueID>> TypedDefinitions::recordAndFindChanges(const TypePropagationInputs& inputs)
{
    const Function& function = inputs.function;
    const CFG& cfg = inputs.analyses.cfg();
    bool reportsMismatches = inputs.build.options().recorded.policy() == MismatchPolicy::Report;

    Vector<Definition> previous = std::exchange(m_definitions, Vector<Definition>(function.values.size()));
    Vector<ValueID> changedValues;
    auto record = [&](ValueID id, uint64_t hash) {
        const Value& value = function.at(id);
        Definition definition { hash, value.type, value.written(), value.prediction() };
        if (id.index() >= previous.size() || previous[id.index()] != definition)
            changedValues.append(id);
        m_definitions[id.index()] = definition;
    };

    bool asksForWrittenTypes = reportsMismatches && hasUnresolvedTailCast(function, cfg);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID id(blockIndex);
        const Block& block = function.at(id);
        // What no path reaches has the type never, and nothing is typed from it.
        bool isReachable = cfg.isReachable(id);
        for (uint32_t index = 0; index < block.parameters.size(); ++index)
            record(block.parameters[index], isReachable ? hashOfParameter(function, cfg, id, index) : 0);
        for (ValueID value : block.values) {
            record(value, isReachable ? hashOfValue(function.at(value)) : 0);
            asksForWrittenTypes |= isReachable && asksForWrittenType(function.at(value), reportsMismatches);
        }
    }

    Signature signature { function.kind, function.flags, function.captures, function.thisType, function.parameters, function.results, asksForWrittenTypes };
    bool isComparable = std::exchange(m_isRecorded, true) && signature == m_signature;
    m_signature = WTF::move(signature);
    if (!isComparable)
        return std::nullopt;
    return changedValues;
}

static bool hasSameTypes(const Function& first, const Function& second)
{
    return std::ranges::equal(first.values, second.values, [](const Value& a, const Value& b) {
        return a.type == b.type && a.written() == b.written() && a.prediction() == b.prediction();
    });
}

TypePropagationOutcome retypeChangedValues(const TypePropagationInputs& inputs, TypedDefinitions& definitions)
{
    Statistics& statistics = inputs.build.statistics();
    std::optional<Vector<ValueID>> changedValues = definitions.recordAndFindChanges(inputs);
    if (!changedValues) {
        statistics.count(StatisticID::FunctionsTypedFromNever);
        TypePropagationOutcome outcome = typeValuesFromNever(inputs);
        if (outcome.cutBlocks)
            definitions.forget();
        return outcome;
    }

    std::optional<Function> typedFromNever;
    if (inputs.build.options().verifiesAfterEveryPass) {
        typedFromNever = inputs.function;
        FunctionAnalyses analyses(inputs.analyses.context(), *typedFromNever);
        typeValuesFromNever({ inputs.program, inputs.id, *typedFromNever, analyses, inputs.types, inputs.build });
    }

    TypePropagationOutcome outcome;
    statistics.count(changedValues->isEmpty() ? StatisticID::FunctionsTypedAsBefore : StatisticID::FunctionsTypedFromChanges);
    if (!changedValues->isEmpty())
        outcome = narrowValueTypes(inputs, changedValues->span());
    if (outcome.cutBlocks)
        definitions.forget();
    RELEASE_ASSERT(!typedFromNever || hasSameTypes(inputs.function, *typedFromNever));
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
