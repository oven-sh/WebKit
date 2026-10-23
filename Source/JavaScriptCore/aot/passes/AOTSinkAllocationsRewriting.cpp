/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTTypeArena.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

struct HeadParameterRange {
    uint32_t first { 0 };
    uint32_t count { 0 };
};

static HeadParameterRange headParametersOf(const RewriteTables& tables, BlockID block)
{
    if (!tables.blocksWithHeadParameters.get(block.index()))
        return { };
    return { tables.firstHeadParameter[block.index()], static_cast<uint32_t>(tables.headValues[block.index()].size()) };
}

static bool carriesField(const VirtualFamily& family, const ObjectState& state, uint32_t field)
{
    if (state.hasCell() && state.fieldsValidInCell.get(field))
        return true;
    return state.variants.containsIf([&](const ObjectVariant& variant) {
        return hasStored(family.at(variant.shape), field);
    });
}

// A read that may find the property missing selects undefined by the tag, so the carried value is only read where the field is stored.
static TypeID typeOfCarriedField(const AllocationSinking& sinking, VirtualObjectID object, const ObjectState& state, uint32_t fieldIndex, TypeArena& types)
{
    const VirtualObjectContext& context = sinking.objectContext();
    const Candidate& candidate = sinking.at(object);
    const VirtualFamily& family = sinking.familyOf(object);
    const VirtualField& field = family.fieldAt(fieldIndex);

    TypeID type = TypeTable::never;
    for (const ObjectVariant& variant : state.variants) {
        const VirtualShape& shape = family.at(variant.shape);
        if (hasStored(shape, fieldIndex))
            type = types.unionOf(type, typeOfField(context, family.descriptionOf(shape), field));
    }

    if (state.hasCell() && state.fieldsValidInCell.get(fieldIndex)) {
        std::span<const VirtualObjectDescription> descriptionsOfCells = family.descriptions();
        if (!candidate.isJoin)
            descriptionsOfCells = descriptionsOfCells.subspan(candidate.descriptionIndex, 1);
        for (const VirtualObjectDescription& description : descriptionsOfCells) {
            if (canLoadSpeculatively(context, description, field) == SpeculativeLoad::Safe)
                type = types.unionOf(type, typeOfField(context, description, field));
        }
    }
    return type;
}

static void becomeJump(Terminator& terminator, uint32_t targetIndex)
{
    Target target = WTF::move(terminator.targets[targetIndex]);
    RELEASE_ASSERT(target.proofs.isEmpty());
    terminator.become(TerminatorKind::Jump);
    terminator.targets.append(WTF::move(target));
}

static void becomeSwitchOnTag(const ShapeContext& context, const ObjectState& state, ValueID tag, Terminator& terminator)
{
    RELEASE_ASSERT(tag);
    Vector<int64_t, 4> cases;
    Vector<Target, 2> targets;
    for (const ObjectVariant& variant : state.variants) {
        std::optional<uint32_t> targetIndex = findShapeCase(context, context.family.at(variant.shape), terminator);
        RELEASE_ASSERT(targetIndex && variant.tagBit);
        cases.append(int64_t(1) << *variant.tagBit);
        targets.append(terminator.targets[*targetIndex]);
    }
    cases.removeLast();

    terminator.become(TerminatorKind::Switch);
    terminator.setSubjectOperand(tag);
    terminator.setCases(cases.span());
    terminator.targets = WTF::move(targets);
}

bool AllocationSinking::splitEdgesNeedingCode(IRBuilder& builder)
{
    if (m_edgesNeedingCode.isEmpty())
        return false;

    AOT::sort(m_edgesNeedingCode, { }, [](const Edge& edge) {
        return std::pair { edge.from, edge.targetIndex };
    });
    for (Edge edge : m_edgesNeedingCode)
        builder.splitEdge(edge);
    return true;
}

void AllocationSinking::rewrite(IRBuilder& builder, const FreshResultShape* shape)
{
    m_resultShape = shape;
    findWhetherObjectsAreBuiltBeforeBranches();
    RewriteTables& tables = m_tables.emplace();
    tables.headValues.grow(m_function.blocks.size());
    tables.firstHeadParameter.fill(0, m_function.blocks.size());
    tables.order.append(m_analyses.cfg().reversePostOrder());
    // No analysis can be made of a function that is half rewritten.
    tables.blocksLeftAsTheyAre.ensureSize(m_function.blocks.size());
    for (BlockID block : tables.order) {
        if (!isReached(block))
            continue;
        if (isLeftAsItIs(block) && !m_checksShortcuts)
            tables.blocksLeftAsTheyAre.quickSet(block.index());
        else
            prepare(block);
    }
    if (m_analyses.opcodesOfValues().contains(Opcode::GetArgument)) {
        const ValueIdentities& identities = m_analyses.identities();
        tables.identityOfValue = Vector<ValueID>(identities.numberOfValues(), [&](size_t index) {
            return identities.identityOf(ValueID(index));
        });
    }
    m_cursor.emplace(*this, builder);

    addHeadParameters(builder);
    tables.enteredBlocks.set(entryBlock.index());
    for (BlockID block : tables.order)
        rewriteBlock(block);
    insertEntryConstants();
    builder.emptyUnreachableBlocks();
    removeBoundParameters(builder);
    applyReplacements();
}

// An entry that checks an argument goes on with what the test hands to its passing side, which is the argument.
ValueID AllocationSinking::sameValueThroughTests(ValueID id)
{
    if (m_tables)
        return id.index() < m_tables->identityOfValue.size() ? m_tables->identityOfValue[id.index()] : sameObjectThroughCasts(m_function, id);
    const ValueIdentities& identities = m_analyses.identities();
    return id.index() < identities.numberOfValues() ? identities.identityOf(id) : sameObjectThroughCasts(m_function, id);
}

void AllocationSinking::forEachHeadValue(const SinkingState& head, NOESCAPE const ScopedLambda<void(const HeadValue&)>& functor) const
{
    for (const ObjectEntry& entry : head.objects) {
        const ObjectState& state = entry.state;
        if (state.needsTag())
            functor(HeadValue { entry.object, HeadValueKind::Tag, 0 });
        if (state.hasCell()) {
            functor(HeadValue { entry.object, HeadValueKind::Cell, 0 });
            if (at(entry.object).needsGuard)
                functor(HeadValue { entry.object, HeadValueKind::Guard, 0 });
        }
        if (state.isCell())
            continue;

        const VirtualFamily& family = familyOf(entry.object);
        for (uint32_t field = 0; field < family.numberOfFields(); ++field) {
            if (!state.objectHeldIn(field) && carriesField(family, state, field))
                functor(HeadValue { entry.object, HeadValueKind::Field, field });
        }
    }
}

TypeID AllocationSinking::typeOfHeadValue(const SinkingState& head, const HeadValue& value, TypeArena& types) const
{
    switch (value.kind) {
    case HeadValueKind::Tag:
        return TypeTable::int32;
    case HeadValueKind::Guard:
        return TypeTable::guard;
    case HeadValueKind::Cell:
        if (at(value.object).isJoin)
            return m_function.at(at(value.object).definition).type;
        return descriptionOf(value.object).type;
    case HeadValueKind::Field:
        return typeOfCarriedField(*this, value.object, *head.find(value.object), value.field, types);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool AllocationSinking::isLeftAsItIs(BlockID block) const
{
    auto mayHaveObjectsAtHead = [&](BlockID other) {
        return !isReached(other) || hasObjectsAtHead(other);
    };
    const Block& contents = m_function.at(block);
    if (!holdsNothingAtHead(block) || m_inputs.blocksWithCandidates.get(block.index()))
        return false;
    // The entry is cut in front of what its handler covers.
    if (contents.handler && (block == entryBlock || mayHaveObjectsAtHead(contents.handler->block)))
        return false;
    if (mentionsName(contents.terminator, block) || m_names.names.get(contents.subject ? contents.subject.index() : m_function.values.size()))
        return false;
    return std::ranges::none_of(contents.terminator.targets, [&](const Target& target) {
        return mayHaveObjectsAtHead(target.block);
    });
}

bool AllocationSinking::needsHeadParameters(BlockID block) const
{
    if (!hasObjectsAtHead(block))
        return false;
    return m_inputs.mergeBlocks.get(block.index()) || m_inputs.blocksInCycles.get(block.index()) || m_function.at(block).isHandlerBlock();
}

void AllocationSinking::addHeadParameters(IRBuilder& builder)
{
    RewriteTables& tables = *m_tables;
    for (BlockID block : tables.order) {
        if (!needsHeadParameters(block))
            continue;

        const SinkingState& head = *headOf(block);
        Vector<ValueID, 4>& headValues = tables.headValues[block.index()];
        builder.setOrigin(m_function.at(block).terminator.origin());
        forEachHeadValue(head, [&](const HeadValue& value) {
            headValues.append(builder.addParameter(block, typeOfHeadValue(head, value, builder.types())));
        });
        if (!headValues.isEmpty())
            tables.firstHeadParameter[block.index()] = static_cast<uint32_t>(m_function.at(block).parameters.find(headValues.first()));
        tables.blocksWithHeadParameters.set(block.index());
    }
}

void AllocationSinking::loadHeadValues(BlockID block, const SinkingState& head, ObjectValueMap& values)
{
    for (const ObjectEntry& entry : head.objects)
        values.ensure(entry.object);

    const Vector<ValueID, 4>& headValues = m_tables->headValues[block.index()];
    size_t numberOfLoadedValues = 0;
    forEachHeadValue(head, [&](const HeadValue& value) {
        ValueID id = headValues[numberOfLoadedValues++];
        ObjectValues& carried = values.ensure(value.object);
        switch (value.kind) {
        case HeadValueKind::Tag:
            carried.tag = id;
            return;
        case HeadValueKind::Cell:
            carried.cell = id;
            return;
        case HeadValueKind::Guard:
            carried.guard = id;
            return;
        case HeadValueKind::Field:
            carried.setField(value.field, id);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    RELEASE_ASSERT(numberOfLoadedValues == headValues.size());
}

void AllocationSinking::rewriteBlock(BlockID block)
{
    if (m_tables->blocksLeftAsTheyAre.quickGet(block.index())) {
        forEachOutgoingEdge(m_function, block, [&](Edge, const Target& target) {
            m_tables->enteredBlocks.set(target.block.index());
        });
        return;
    }
    const SinkingState* head = headOf(block);
    if (!head)
        return;
    // The states at the heads only grow while they are computed: a block keeps the one it got over an edge that the final states decide the other way.
    bool isFilledByOneEdge = !head->objects.isEmpty() && !m_tables->blocksWithHeadParameters.get(block.index());
    if (isFilledByOneEdge && !m_tables->enteredBlocks.get(block.index()))
        return;

    SinkingState state = *head;
    ObjectValueMap values;
    loadHeadValues(block, *head, values);

    Function& function = m_cursor->builder().function();
    Vector<OutgoingEdge, 2> edges;
    for (uint32_t targetIndex = 0; targetIndex < function.at(block).terminator.targets.size(); ++targetIndex)
        edges.append(outgoingEdgeOf(function, Edge { block, targetIndex }));

    BlockValues oldValues = std::exchange(function.edit(block, BlockEdit::Values).values, { });
    Terminator terminator = std::exchange(function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator, Terminator());
    std::optional<Target> handler = std::exchange(function.edit(block, { BlockEdit::Edges, BlockEdit::Arguments }).handler, std::nullopt);
    if (handler)
        m_tables->enteredBlocks.set(handler->block.index());

    ObjectInterpreter interpreter(*this, SinkingMode::Rewrite, state, &values);
    if (VirtualObjectID subject = interpreter.objectOf(function.at(block).subject)) {
        RELEASE_ASSERT(state.find(subject)->isCell());
        function.edit(block, BlockEdit::Rest).subject = values.find(subject)->cell;
    }
    m_cursor->beginBlock(block, WTF::move(handler), interpreter);
    if (state.objects.isEmpty() && !m_inputs.blocksWithCandidates.get(block.index()))
        function.edit(m_cursor->block(), BlockEdit::Values).values = WTF::move(oldValues);
    else {
        for (ValueID id : oldValues)
            interpreter.execute(id);
    }

    m_cursor->setOrigin(terminator.origin());
    ValueID condition = m_cursor->takeConditionOf(terminator);
    interpreter.executeTerminator(terminator);
    if (m_policy.returnedObjects == ReturnedObjects::StayVirtual && terminator.kind == TerminatorKind::Return && terminator.valueOperand()) {
        interpreter.returnParts(terminator);
        return;
    }

    if (m_inputs.throwingTerminators.get(block.index()))
        m_cursor->willKeepThrowingValue(interpreter);
    rewriteEdges(interpreter, terminator, edges.span());
    foldTerminator(interpreter, terminator);
    for (const Target& target : terminator.targets)
        m_tables->enteredBlocks.set(target.block.index());
    interpreter.resolveOperands(terminator);
    if (condition)
        m_cursor->keep(condition);
    m_cursor->finishBlock(WTF::move(terminator));
}

void AllocationSinking::rewriteEdges(ObjectInterpreter& interpreter, Terminator& terminator, std::span<const OutgoingEdge> edges)
{
    for (const OutgoingEdge& outgoing : edges) {
        if (!interpreter.isDeadEdge(terminator, outgoing.edge.targetIndex))
            rewriteEdge(interpreter, terminator, outgoing);
    }
}

void AllocationSinking::rewriteEdge(ObjectInterpreter& interpreter, Terminator& terminator, const OutgoingEdge& outgoing)
{
    const SinkingState* head = headOf(outgoing.target);
    RELEASE_ASSERT(head);
    bool testIsInterpreted = interpreter.hasInterpretedTest(terminator);
    if (!testIsInterpreted && interpreter.state().objects.isEmpty() && head->objects.isEmpty())
        return;

    SinkingState edgeState = interpreter.state();
    ObjectValueMap edgeValues = interpreter.values();
    ObjectInterpreter edge(*this, SinkingMode::Rewrite, edgeState, &edgeValues);
    edge.enterEdge(outgoing, &terminator, head);

    Target& target = terminator.targets[outgoing.edge.targetIndex];
    if (testIsInterpreted)
        convertProofsToArguments(edge, outgoing, target);
    fillTarget(edge, outgoing, target);
}

void AllocationSinking::convertProofsToArguments(ObjectInterpreter& edge, const OutgoingEdge& outgoing, Target& target)
{
    for (size_t numberOfProofs = target.proofs.size(); numberOfProofs--;) {
        const IncomingBinding& binding = outgoing.bindings[target.arguments.size()];
        bool provesNothing = binding.kind == IncomingBinding::Kind::Guard && !edge.guardedObjectOf(binding.value);
        target.arguments.append(provesNothing ? m_cursor->emit(Value(Opcode::AllGuards)) : binding.value);
    }
    target.proofs.clear();
}

bool AllocationSinking::isBoundParameter(BlockID block, ValueID parameter) const
{
    const SinkingState* head = headOf(block);
    if (!head)
        return false;
    if (head->findAlias(parameter))
        return true;
    VirtualObjectID join = candidateDefinedBy(parameter);
    return join && head->find(join);
}

void AllocationSinking::fillTarget(ObjectInterpreter& edge, const OutgoingEdge& outgoing, Target& target)
{
    HeadParameterRange headParameters = headParametersOf(*m_tables, target.block);
    for (uint32_t index = 0; index < target.arguments.size(); ++index) {
        ValueID parameter = m_function.at(target.block).parameters[index < headParameters.first ? index : index + headParameters.count];
        if (isBoundParameter(target.block, parameter))
            continue;
        ValueID argument = target.arguments[index];
        target.arguments[index] = m_function.at(parameter).type == TypeTable::guard ? edge.resolveGuard(argument) : edge.resolve(argument);
    }

    Vector<ValueID, 8> values = collectHeadValues(edge, target.block, outgoing.edge.isHandlerEdge());
    for (ValueID value : values)
        RELEASE_ASSERT(value);

    if (!m_tables->blocksWithHeadParameters.get(target.block.index())) {
        Vector<ValueID, 4>& headValues = m_tables->headValues[target.block.index()];
        headValues.clear();
        headValues.appendVector(values);
        return;
    }

    RELEASE_ASSERT(values.size() == headParameters.count);
    for (uint32_t index = 0; index < headParameters.count; ++index)
        target.arguments.insert(headParameters.first + index, values[index]);
}

Vector<ValueID, 8> AllocationSinking::collectHeadValues(ObjectInterpreter& edge, BlockID target, bool isHandlerEdge)
{
    const SinkingState& head = *headOf(target);
    TypeArena& types = m_cursor->builder().types();

    auto constant = [&](Value&& value) {
        return isHandlerEdge ? entryConstant(WTF::move(value)) : m_cursor->emit(WTF::move(value));
    };
    auto integer = [&](int64_t number) {
        Value value(Opcode::Integer);
        value.setInteger(number);
        return constant(WTF::move(value));
    };
    auto unspecified = [&](const HeadValue& headValue) {
        Value value(Opcode::Unspecified);
        value.setValueType(typeOfHeadValue(head, headValue, types));
        return constant(WTF::move(value));
    };

    Vector<ValueID, 8> result;
    forEachHeadValue(head, [&](const HeadValue& headValue) {
        VirtualObjectID object = headValue.object;
        const VirtualFamily& family = familyOf(object);
        const ObjectState* state = edge.state().find(object);
        if (state && state->isCell() && !head.find(object)->hasCell())
            state = nullptr;
        const ObjectValues* carried = edge.values().find(object);
        switch (headValue.kind) {
        case HeadValueKind::Tag:
            if (!state)
                result.append(integer(0));
            else if (state->needsTag())
                result.append(carried->tag);
            else {
                VirtualShapeID shape = state->variants[0].shape;
                std::optional<uint8_t> tagBit = family.findTagBit(target, shape);
                for (const ObjectVariant& variant : head.find(object)->variants) {
                    if (!tagBit && variant.shape == shape)
                        tagBit = variant.tagBit;
                }
                result.append(integer(tagBit ? int64_t(1) << *tagBit : 0));
            }
            return;
        case HeadValueKind::Cell:
            if (state && state->isCell())
                result.append(edge.cellOf(object));
            else
                result.append(state && state->hasCell() ? carried->cell : unspecified(headValue));
            return;
        case HeadValueKind::Guard:
            result.append(carried && carried->guard ? carried->guard : constant(Value(Opcode::AllGuards)));
            return;
        case HeadValueKind::Field:
            if (state && carriesField(family, *state, headValue.field))
                result.append(edge.valueOfField(object, headValue.field));
            else
                result.append(unspecified(headValue));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    return result;
}

std::optional<Target> RewriteCursor::handlerTargetFor(ObjectInterpreter& interpreter)
{
    if (!m_originalHandler)
        return std::nullopt;

    Target target = *m_originalHandler;
    const SinkingState* head = m_sinking.headOf(target.block);
    if (!head || (interpreter.state().objects.isEmpty() && head->objects.isEmpty()))
        return target;

    OutgoingEdge outgoing { Edge::toHandlerOf(m_originalBlock), target.block, { } };
    for (ValueID argument : target.arguments)
        outgoing.bindings.append(IncomingBinding { IncomingBinding::Kind::Argument, argument });

    SinkingState edgeState = interpreter.state();
    ObjectValueMap edgeValues = interpreter.values();
    ObjectInterpreter edge(m_sinking, SinkingMode::Rewrite, edgeState, &edgeValues);
    edge.enterEdge(outgoing, nullptr, head);
    m_sinking.fillTarget(edge, outgoing, target);
    return target;
}

void AllocationSinking::foldTerminator(ObjectInterpreter& interpreter, Terminator& terminator)
{
    if (!interpreter.hasInterpretedTest(terminator))
        return;

    Vector<uint32_t, 2> liveTargets;
    for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
        if (!interpreter.isDeadEdge(terminator, targetIndex))
            liveTargets.append(targetIndex);
    }
    RELEASE_ASSERT(!liveTargets.isEmpty());
    if (liveTargets.size() == 1) {
        becomeJump(terminator, liveTargets[0]);
        return;
    }
    if (terminator.kind != TerminatorKind::SwitchOnShape)
        return;

    VirtualObjectID object = interpreter.objectOf(terminator.subjectOperand());
    becomeSwitchOnTag(shapeContext(object), *interpreter.state().find(object), interpreter.values().find(object)->tag, terminator);
}

ValueID AllocationSinking::entryConstant(Value&& value)
{
    for (ValueID existing : m_tables->entryConstants) {
        const Value& other = m_function.at(existing);
        if (other.opcode == value.opcode && other.hasSamePayloadAs(value))
            return existing;
    }

    ValueID id = m_cursor->builder().addDetached(WTF::move(value));
    m_tables->entryConstants.append(id);
    return id;
}

void AllocationSinking::insertEntryConstants()
{
    m_cursor->builder().function().edit(entryBlock, BlockEdit::Values).values.insertVector(0, m_tables->entryConstants);
}

void AllocationSinking::removeBoundParameters(IRBuilder& builder)
{
    for (BlockID block : m_tables->order) {
        if (!isReached(block) || holdsNothingAtHead(block))
            continue;

        const CompactList<ValueID>& parameters = m_function.at(block).parameters;
        BitVector boundIndexes;
        for (uint32_t index = 0; index < parameters.size(); ++index) {
            if (isBoundParameter(block, parameters[index]))
                boundIndexes.set(index);
        }
        builder.removeParameters(block, boundIndexes);
    }
}

void AllocationSinking::applyReplacements()
{
    Function& function = m_cursor->builder().function();
    ValueSubstitution substitution(function);
    for (auto [read, replacement] : m_tables->replacedReads)
        replaceReadWithValue(m_program, function, substitution, read, replacement);
    substitution.apply(function, &m_analyses);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
