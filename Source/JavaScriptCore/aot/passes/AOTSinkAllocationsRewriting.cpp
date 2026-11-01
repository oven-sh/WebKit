/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTDecidedBranches.h"
#include "AOTPredictedValues.h"
#include "AOTSort.h"
#include "AOTSpecifiedValues.h"
#include "AOTTypeArena.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

void ObjectValues::setField(uint32_t index, ValueID value)
{
    if (index >= fields.size())
        fields.grow(static_cast<size_t>(index) + 1);
    fields[index] = value;
}

const ObjectValues* ObjectValueMap::find(VirtualObjectID object) const
{
    const Entry* entry = findEntry(m_entries, &Entry::object, object);
    return entry ? &entry->values : nullptr;
}

RewriteCursor::RewriteCursor(AllocationSinking& sinking, IRBuilder& builder)
    : m_sinking(sinking)
    , m_builder(builder)
    , m_function(builder.function())
{
    m_builder.setHandler(std::nullopt);
}

void RewriteCursor::beginBlock(BlockID original, std::optional<Target>&& handler, ObjectInterpreter& interpreter)
{
    m_originalBlock = original;
    m_originalHandler = WTF::move(handler);
    m_frequency = m_function.at(original).frequency;
    m_handlerArgumentsAreStale = false;
    m_builder.moveToEndOf(original);
    if (!m_originalHandler)
        return;

    if (original == entryBlock) {
        cut(interpreter);
        return;
    }
    std::optional<Target> target = handlerTargetFor(interpreter);
    m_function.edit(original, { BlockEdit::Edges, BlockEdit::Arguments }).handler = WTF::move(target);
}

ValueID RewriteCursor::takeConditionOf(const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::Branch)
        return ValueID();

    ValueID condition = terminator.conditionOperand();
    BlockValues& values = m_function.edit(block(), BlockEdit::Values).values;
    if (!m_sinking.inputs().conditionsOnlyBranchedOn.get(condition.index()) || values.isEmpty() || values.last() != condition)
        return ValueID();
    values.removeLast();
    return condition;
}

ValueID RewriteCursor::emitSelection(ValueID condition, ValueID whenTrue, ValueID whenFalse, TypeID type)
{
    BlockID continuation = addUncoveredBlock();
    ValueID result = m_builder.addParameter(continuation, type);
    m_builder.emitBranch(condition, Target { continuation, { whenTrue }, { } }, Target { continuation, { whenFalse }, { } });
    moveTo(continuation);
    return result;
}

void RewriteCursor::willKeepThrowingValue(ObjectInterpreter& interpreter)
{
    if (m_originalHandler && m_handlerArgumentsAreStale)
        cut(interpreter);
}

void RewriteCursor::willBuild(ObjectInterpreter& interpreter)
{
    if (m_numberOfBuildsBegun++ || !m_originalHandler)
        return;
    if (m_handlerArgumentsAreStale) {
        std::optional<Target> target = handlerTargetFor(m_interpreterAtTerminator ? *m_interpreterAtTerminator : interpreter);
        if (m_function.at(block()).handler != target) {
            BlockID next = addUncoveredBlock();
            m_function.edit(next, { BlockEdit::Edges, BlockEdit::Arguments }).handler = WTF::move(target);
            m_builder.emitJump(next);
            moveTo(next);
        }
        m_handlerArgumentsAreStale = false;
    }
    m_handlerOfBuild = m_function.at(block()).handler;
}

void RewriteCursor::didBuild()
{
    if (!--m_numberOfBuildsBegun)
        m_handlerOfBuild = std::nullopt;
}

void RewriteCursor::cut(ObjectInterpreter& interpreter)
{
    BlockID next = addCoveredBlock(interpreter);
    m_builder.emitJump(next);
    moveTo(next);
}

BlockID RewriteCursor::addUncoveredBlock()
{
    BlockID block = m_builder.addBlock(m_frequency);
    m_function.edit(block, BlockEdit::Rest).shape = m_function.at(m_originalBlock).shape;
    if (m_handlerOfBuild)
        m_function.edit(block, { BlockEdit::Edges, BlockEdit::Arguments }).handler = m_handlerOfBuild;
    return block;
}

BlockID RewriteCursor::addCoveredBlock(ObjectInterpreter& interpreter)
{
    BlockID block = addUncoveredBlock();
    std::optional<Target> target = handlerTargetFor(interpreter);
    m_function.edit(block, { BlockEdit::Edges, BlockEdit::Arguments }).handler = WTF::move(target);
    return block;
}

void RewriteCursor::moveTo(BlockID block)
{
    m_builder.moveToEndOf(block);
    m_handlerArgumentsAreStale = !m_function.at(block).handler;
}

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

static TypeID typeOfCarriedField(const AllocationSinking& sinking, VirtualObjectID object, const ObjectState& state, uint32_t fieldIndex, TypeArena& types)
{
    const VirtualObjectContext& context = sinking.objectContext();
    ShapeContext shapes = sinking.shapeContext(object);
    const Candidate& candidate = sinking.at(object);
    const VirtualFamily& family = sinking.familyOf(object);
    const VirtualField& field = family.fieldAt(fieldIndex);

    TypeID type = TypeTable::never;
    for (const ObjectVariant& variant : state.variants) {
        const VirtualShape& shape = family.at(variant.shape);
        if (hasStored(shape, fieldIndex))
            type = types.unionOf(type, typeOf(contentsOf(shapes, family.descriptionOf(shape), fieldIndex), types));
    }
    // A join passes its fields on to other joins of the family, whose variants may differ, so every join carries the union over the family.
    if (type != TypeTable::never && (field.kind == VirtualFieldKind::Element || (candidate.isJoin && field.isProperty()))) {
        for (const VirtualObjectDescription& description : family.descriptions())
            type = types.unionOf(type, typeOf(contentsOf(shapes, description, fieldIndex), types));
    }

    if (state.hasCell() && state.fieldsValidInCell.get(fieldIndex)) {
        std::span<const VirtualObjectDescription> descriptionsOfCells = family.descriptions();
        if (!candidate.isJoin)
            descriptionsOfCells = descriptionsOfCells.subspan(candidate.descriptionIndex, 1);
        for (const VirtualObjectDescription& description : descriptionsOfCells) {
            if (canLoadSpeculatively(context, description, field) == SpeculativeLoad::Safe)
                type = types.unionOf(type, typeOf(contentsOf(shapes, description, fieldIndex), types));
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
    // What is built on the exit of a block that a handler covers is built under that handler.
    for (Edge edge : m_edgesNeedingCode)
        builder.splitEdge(edge, m_function.at(edge.from).handler);
    return true;
}

// The class of a constructor function begins with its first object, and until then a set of guards takes what it says of that class to speak of no object. An object that is not made begins no class.
// So a test that names the class waits for it wherever its answer may be believed while such an object is not made: where that is so, and on every way there. The guard of the function's `prototype`
// holds from the definition of the class on. Where the test fails the object is wanted, and made.
void AllocationSinking::letTestsAwaitClassesOfObjectsThatAreNotMade(IRBuilder& builder)
{
    struct ClassNotBegun {
        LayoutID layout;
        BitVector blocks;
    };
    Vector<ClassNotBegun, 2> classes;
    Vector<Vector<uint32_t, 1>> classesOfFamilies;
    for (const Candidate& candidate : m_candidates) {
        if (candidate.isJoin || candidate.isAbandoned)
            continue;
        const Value& made = m_function.at(candidate.definition);
        if (made.opcode != Opcode::NewObject || !m_program.at(made.layout()).has(LayoutFlags::IsConstructorFunction))
            continue;
        size_t index = classes.findIf([&](const ClassNotBegun& entry) {
            return entry.layout == made.layout();
        });
        if (index == notFound) {
            index = classes.size();
            classes.append({ made.layout(), BitVector(m_function.blocks.size()) });
        }
        classes[index].blocks.set(candidate.block.index());
        if (classesOfFamilies.size() <= candidate.family.index())
            classesOfFamilies.grow(candidate.family.index() + 1);
        classesOfFamilies[candidate.family.index()].appendIfNotContains(static_cast<uint32_t>(index));
    }
    if (classes.isEmpty())
        return;
    for (uint32_t block = 0; block < m_function.blocks.size(); ++block) {
        const SinkingState* head = headOf(BlockID(block));
        if (!head)
            continue;
        for (const ObjectEntry& entry : head->objects) {
            size_t family = at(entry.object).family.index();
            if (entry.state.isCell() || family >= classesOfFamilies.size())
                continue;
            for (uint32_t index : classesOfFamilies[family])
                classes[index].blocks.set(block);
        }
    }
    for (ClassNotBegun& entry : classes) {
        Vector<BlockID, 16> worklist;
        for (size_t block : entry.blocks)
            worklist.append(BlockID(block));
        while (!worklist.isEmpty()) {
            for (Edge edge : m_analyses.cfg().incomingEdges(worklist.takeLast())) {
                if (!entry.blocks.set(edge.from.index()))
                    worklist.append(edge.from);
            }
        }
    }

    PropertyKeyID prototype = m_program.keyOf(BuiltinNames::prototype);
    bool didChangeTest = false;
    for (uint32_t block = 0; block < m_function.blocks.size(); ++block) {
        for (ValueID id : m_function.blocks.span()[block].values) {
            if (m_function.at(id).opcode != Opcode::AreMembersUnchanged)
                continue;
            Vector<MemberConditionRecord, 8> conditions;
            for (MemberConditionRecord condition : m_function.at(id).conditions())
                conditions.append(condition);
            size_t numberOfConditions = conditions.size();
            for (size_t index = 0; index < numberOfConditions; ++index) {
                MemberConditionRecord awaited { .kind = MemberConditionKind::MemberUnchanged, .layout = conditions[index].layout, .side = ClassSide::Static, .key = prototype };
                bool isAwaited = std::ranges::any_of(classes, [&](const ClassNotBegun& entry) {
                    return entry.layout == awaited.layout && entry.blocks.get(block);
                });
                if (isAwaited && !conditions.contains(awaited))
                    conditions.append(awaited);
            }
            if (conditions.size() == numberOfConditions)
                continue;
            AOT::sortInLine(conditions, { }, [](const MemberConditionRecord& condition) {
                return std::tuple { condition.kind, condition.layout, condition.side, condition.key };
            });
            builder.function().edit(id, ValueEdit::Definition).setConditions(conditions.span());
            didChangeTest = true;
        }
    }
    // A test that waits for a class no longer reads where the class begins.
    if (didChangeTest)
        m_analyses.didChangeValuesButNotUses();
}

void AllocationSinking::rewrite(IRBuilder& builder, const FreshResultShape* shape)
{
    letTestsAwaitClassesOfObjectsThatAreNotMade(builder);
    m_resultShape = shape;
    m_constantsBeforeRewriting = Vector<std::optional<VirtualConstant>>(m_function.values.size(), [&](size_t index) {
        return virtualConstantOf(m_objectContext, ValueID(index));
    });
    findWhetherObjectsAreBuiltBeforeBranches();
    RewriteTables& tables = m_tables.emplace();
    tables.headValues.grow(m_function.blocks.size());
    tables.firstHeadParameter.fill(0, m_function.blocks.size());
    tables.order.append(m_analyses.cfg().reversePostOrder());
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
    m_mustSolveAgain = false;
    m_decisions.takeDidChange();
    for (BlockID block : tables.order)
        rewriteBlock(block);
    RELEASE_ASSERT(!m_mustSolveAgain && !m_decisions.takeDidChange());
    builder.function().edit(entryBlock, BlockEdit::Values).values.insertVector(0, tables.entryConstants);
    builder.emptyUnreachableBlocks();
    removeBoundParameters(builder);
    ValueSubstitution substitution(builder.function());
    for (auto [read, replacement] : tables.replacedReads)
        replaceReadWithValue(m_program, builder.function(), substitution, read, replacement);
    substitution.apply(builder.function(), &m_analyses);
    nameFactsWhereUnspecifiedValuesAreUsed(builder, m_analyses);
    if (!tables.claimsAssumedOfContents.isEmpty()) {
        PredictedValues predicted(builder.function());
        settleProofsBehindAnsweredChecks(builder.function(), tables.claimsAssumedOfContents.span(), &predicted);
    }
}

ValueID AllocationSinking::sameValueThroughTests(ValueID id)
{
    if (m_tables)
        return id.index() < m_tables->identityOfValue.size() ? m_tables->identityOfValue[id.index()] : sameObjectThroughCasts(m_function, id).valueInFrontOfFacts();
    const ValueIdentities& identities = m_analyses.identities();
    return id.index() < identities.numberOfValues() ? identities.identityOf(id) : sameObjectThroughCasts(m_function, id).valueInFrontOfFacts();
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
    m_cursor->willEnterEdges(interpreter);
    for (const OutgoingEdge& outgoing : edges) {
        if (!interpreter.isDeadEdge(terminator, outgoing.edge.targetIndex))
            rewriteEdge(interpreter, terminator, outgoing);
    }
    m_cursor->didEnterEdges();
    if (foldTerminator(interpreter, terminator))
        m_tables->blocksWithDecidedBranches.append(m_cursor->block());
    for (const Target& target : terminator.targets)
        m_tables->enteredBlocks.set(target.block.index());
    interpreter.resolveOperands(terminator);
    if (condition)
        m_cursor->keep(condition);
    m_cursor->setOrigin(terminator.origin());
    m_cursor->builder().emitTerminator(WTF::move(terminator));
}

static uint64_t keyOf(Edge edge)
{
    return (static_cast<uint64_t>(edge.from.index()) << 32 | static_cast<uint64_t>(edge.targetIndex) << 1 | edge.isHandlerEdge()) + 1;
}

void AllocationSinking::setObjectsDeadOnEdge(Edge edge, Vector<VirtualObjectID, 4>&& objects)
{
    m_objectsDeadOnEdges.set(keyOf(edge), WTF::move(objects));
}

bool AllocationSinking::isDeadOnEdge(Edge edge, VirtualObjectID object) const
{
    auto found = m_objectsDeadOnEdges.find(keyOf(edge));
    return found != m_objectsDeadOnEdges.end() && found->value.contains(object);
}

static bool bringsOnlyShapesOfHead(const AllocationSinking& sinking, Edge taken, const SinkingState& edge, const SinkingState& head)
{
    return std::ranges::all_of(edge.objects, [&](const ObjectEntry& entry) {
        const ObjectState* atHead = head.find(entry.object);
        return !atHead || sinking.isDeadOnEdge(taken, entry.object) || std::ranges::all_of(entry.state.variants, [&](const ObjectVariant& variant) {
            return atHead->variants.containsIf([&](const ObjectVariant& other) {
                return other.shape == variant.shape;
            });
        });
    });
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
    RELEASE_ASSERT(!checksThatModesAgree() || !edgeState.isReachable || bringsOnlyShapesOfHead(*this, outgoing.edge, edgeState, *head));

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

void AllocationSinking::raiseFrequenciesBehindDecidedBranches()
{
    Function& function = m_cursor->builder().function();
    bool hasAnalysesOfRewrittenFunction = false;
    for (BlockID block : m_tables->blocksWithDecidedBranches) {
        if (!isRarerThan(function.at(function.at(block).terminator.next().block).frequency, function.at(block).frequency).predicted())
            continue;
        if (!std::exchange(hasAnalysesOfRewrittenFunction, true))
            m_analyses.didChangeControlFlow();
        raiseFrequenciesBehind(m_analyses, function, Edge { block, 0 });
    }
}

bool AllocationSinking::foldTerminator(ObjectInterpreter& interpreter, Terminator& terminator)
{
    if (!interpreter.hasInterpretedTest(terminator))
        return false;

    Vector<uint32_t, 2> liveTargets;
    for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
        if (!interpreter.isDeadEdge(terminator, targetIndex))
            liveTargets.append(targetIndex);
    }
    RELEASE_ASSERT(!liveTargets.isEmpty());
    if (liveTargets.size() == 1) {
        becomeJump(terminator, liveTargets[0]);
        return true;
    }
    if (terminator.kind != TerminatorKind::SwitchOnShape)
        return false;

    VirtualObjectID object = interpreter.objectOf(terminator.subjectOperand());
    becomeSwitchOnTag(shapeContext(object), *interpreter.state().find(object), interpreter.values().find(object)->tag, terminator);
    return false;
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
