/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkMergedObjects.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTPlan.h"
#include "AOTSlotPlan.h"
#include "AOTTypeArena.h"
#include "AOTWalkKnown.h"
#include "AOTWalkTextBuilders.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

bool Walker::isNamedAtHead(const WalkState& state, ValueID cell, BlockID inputBlock, const ScopedLambda<Reference(ValueID)>& nameOf)
{
    const BodyIndex& index = currentBodyIndex();
    if (!m_liveness.isComputedFor(index))
        m_liveness.compute(index);
    bool isNamed = false;
    auto look = [&](Reference name) {
        if (!name)
            return;
        if (name.isRecord())
            isNamed |= name.isValue() && underlyingCellOf(m_code, name.id()) == cell;
        else
            isNamed = true;
    };
    m_liveness.forEachLiveAtHead(inputBlock, [&](ValueID value) {
        if (isNamed)
            return;
        Reference name = nameOf(value);
        if (!name || !name.isVirtual() || standsForFrameOfGenerator(name))
            return look(name);
        indexedIteration(name).forEachValueHeld(look);
    });
    if (!isNamed)
        state.forEachValueHeldByAnother(look);
    return isNamed;
}

bool Walker::isPassedToCopiesAlone(ValueID cell) const
{
    static constexpr OpcodeSet allocations { Opcode::NewObject, Opcode::NewArray, Opcode::Data, Opcode::Construct };
    const Operation& made = m_code.at(cell);
    for (size_t place = m_frames.size() - 1; place && cell.index() < m_frames[place].numberOfValuesBuiltBefore; --place) {
        const Frame& copy = m_frames[place];
        if (!copy.callCopied || copy.inlinedHeadBlocks || copy.index->holdsAnyOf(opcodesThatMakeArguments))
            return false;
        const Frame& caller = m_frames[place - 1];
        const BodyIndex& index = *caller.index;
        ValueID passed;
        uint32_t timesPassed = 0;
        index.value(copy.callCopied).forEachOperand([&](ValueID operand, uint32_t) {
            Reference name = operand ? caller.references[operand.index()] : Reference();
            if (!name || !name.isValue() || underlyingCellOf(m_code, name.id()) != cell)
                return;
            passed = operand;
            ++timesPassed;
        });
        if (timesPassed != 1)
            return false;
        ValueID root = index.stripCasts(passed);
        for (; passed != root; passed = index.value(passed).operand(0)) {
            if (!index.hasOneUse(passed))
                return false;
        }
        bool isEntryParameter = place > 1 && index.isParameter(root) && index.blockOf(root) == entryBlock && cell.index() < caller.numberOfValuesBuiltBefore;
        bool isAllocation = !index.isParameter(root) && allocations.contains(index.opcodeOf(root)) && made.inlineFrame == caller.inlineFrame && made.position == index.value(root).origin().position;
        if (!index.hasOneUse(root) || (!isEntryParameter && !isAllocation) || index.loopOf(index.blockOf(root)) != index.loopOf(index.blockOf(copy.callCopied)))
            return false;
    }
    return true;
}

void Walker::keepCopiesThatTakeOver(ValueID cell)
{
    for (size_t place = m_frames.size() - 1; place && cell.index() < m_frames[place].numberOfValuesBuiltBefore; --place)
        m_frames[place].mayLeaveForCall = false;
}

bool Walker::canTakeOverObject(ValueID cell) const
{
    if (m_versionedLoops.isEmpty())
        return true;
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    const Frame& frame = m_frames[versionedLoop.frameIndex];
    BlockID head = frame.blocks.isEmpty() ? BlockID() : frame.blocks[versionedLoop.inputHead.index()].head;
    std::span<const ValueID> parameters = head ? m_code.at(head).parameters() : std::span<const ValueID>();
    return !parameters.empty() && cell.index() >= parameters.front().index();
}

bool Walker::mayNameObjectsInGenerators() const
{
    return !m_generatorsWalkedInConsumers.isEmpty() || !m_generatorsTakenByConsumers.isEmpty() || m_loopsOverGenerators.hasStepsWrittenByHand;
}

void Walker::findMergedHeaderObjects(BlockID inputHead, std::span<const Reference> entering, Vector<MergedHeaderObject, 1>& found)
{
    size_t frameIndex = m_frames.size() - 1;
    if (!has(Mechanism::EscapeAnalysis) || !has(Mechanism::LoopInvariants) || m_coldCopyDepth || isUnreachable() || !hasOpenTrialVisit(frameIndex, inputHead))
        return;
    if (mayNameObjectsInGenerators())
        return;
    if (specializesLoop(frameIndex, inputHead))
        return inheritMergedObjects(inputHead, entering, found);
    const LoopSummary& summary = m_loopSummaries[m_trialVisits.last().summaryIndex];
    if (summary.isPessimistic)
        return;
    BodyBlock head = currentBodyIndex().block(inputHead);
    auto cellOf = [&](Reference argument) {
        return argument && argument.isValue() ? underlyingCellOf(m_code, argument.id()) : ValueID();
    };
    for (uint32_t position = 0; position < entering.size(); ++position) {
        ValueID holder = head.parameter(position);
        ValueID cell = cellOf(entering[position]);
        if (!cell || summary.storesIntoScopes.unmergedHolders.contains(holder) || !m_state.isFresh(referenceTo(cell)) || !canTakeOverObject(cell))
            continue;
        if (!wasBuiltInCurrentFrame(cell) && !isPassedToCopiesAlone(cell))
            continue;
        std::optional<MergeableObject> object = findMergeableObject(*this, m_state, cell);
        if (!object || std::ranges::count_if(entering, [&](Reference other) { return cellOf(other) == cell; }) != 1)
            continue;
        if (isNamedAtHead(m_state, cell, inputHead, scopedLambda<Reference(ValueID)>([&](ValueID value) { return referenceOf(value); })))
            continue;
        keepCopiesThatTakeOver(cell);
        found.append(MergedHeaderObject { position, holder, cell, { }, WTF::move(*object) });
    }
}

void Walker::inheritMergedObjects(BlockID inputHead, std::span<const Reference> entering, Vector<MergedHeaderObject, 1>& found)
{
    const StoresIntoScopes& own = m_loopSummaries[m_trialVisits.last().summaryIndex].storesIntoScopes;
    bool hasInheritedSummary = m_inheritedSummaryHeader == inputHead && m_inheritedSummaryFrame == m_frames.size() - 1;
    if (m_loopSummaries[m_trialVisits.last().summaryIndex].isPessimistic || !hasInheritedSummary || !m_inheritedScopeStores.mergedObjects)
        return;
    for (const MergedHeaderObject& merged : *m_inheritedScopeStores.mergedObjects) {
        Reference argument = merged.position < entering.size() ? entering[merged.position] : Reference();
        bool isSameObject = argument && argument.isValue() && underlyingCellOf(m_code, argument.id()) == merged.entering;
        if (isSameObject && !own.unmergedHolders.contains(merged.holder) && !m_inheritedScopeStores.unmergedHolders.contains(merged.holder)) {
            keepCopiesThatTakeOver(merged.entering);
            found.append(merged);
        }
    }
}

TypeID Walker::typeOfMergedSlot(const FieldOfScope& slot, bool isWidened)
{
    FieldOfScope filled = loopCarriedObjectSlot(slot.scope, slot.typeOfHolders, *slot.positionOfHolder, slot.slotKey);
    TypeID type = filled.slotKey ? typeOfFieldInEveryHolder(filled) : plan().slot(slot.field.layout, slot.field.field).type;
    if (!type)
        type = TypeTable::anything;
    if (m_types.at(type).primitives & Primitive::Absent)
        type = m_types.withoutPrimitives(type, Primitive::Absent);
    TypeID own = ownTypeOf(slot.values[0]);
    if (!isWidened && own && m_types.isSubset(own, TypeTable::number) && !m_types.isSubset(type, TypeTable::number) && m_types.isSubset(TypeTable::number, type))
        type = TypeTable::number;
    return m_types.at(type).isJavaScriptValue() ? type : TypeID();
}

bool Walker::addElementsOfMergedArray(const MergedHeaderObject& merged, TypeID type, std::span<const Reference> entering, bool isWidened, Vector<FieldOfScope, 4>& known)
{
    MergeableObject atHeader = merged.object;
    atHeader.shape = type;
    if (!hasKind(m_types, type, OperandKind::Array) || !m_types.isSubset(merged.object.shape, type) || !m_types.isSubset(typeOfMergeableObject(*this, atHeader), type))
        return false;
    Vector<FieldOfScope, 8> elements;
    for (uint32_t position = 0; position < merged.object.values.size(); ++position) {
        Draft place(Opcode::Integer);
        place.setInteger(position);
        FieldOfScope element;
        element.scope = entering[merged.position];
        element.element = constant(WTF::move(place));
        element.positionOfHolder = merged.position;
        element.typeOfHolders = type;
        element.isOfMergedObject = true;
        element.values.append(merged.object.values[position]);
        element.type = ownTypeOf(element.values[0]);
        for (TypeID general : { TypeID(TypeTable::number), TypeID(TypeTable::string), TypeID(TypeTable::boolean) }) {
            if (element.type && m_types.isSubset(element.type, general))
                element.type = general;
        }
        if (isWidened)
            element.type = TypeTable::anything;
        if (!element.type || !m_types.at(element.type).isJavaScriptValue())
            return false;
        elements.append(WTF::move(element));
    }
    if (isWidened)
        count(StatisticID::MergedArraysWidened);
    for (FieldOfScope& element : elements)
        known.append(WTF::move(element));
    return true;
}

void Walker::addSlotsOfMergedObjects(const LoopPreview& preview, std::span<const Reference> entering, Vector<MergedHeaderObject, 1>& objects, Vector<FieldOfScope, 4>& known)
{
    if (preview.effects.maySuspend) {
        objects.shrink(0);
        return;
    }
    const Vector<ValueID, 2>& widened = m_loopSummaries[m_trialVisits.last().summaryIndex].storesIntoScopes.widenedHolders;
    auto isWidened = [&](ValueID holder) {
        return widened.contains(holder) || m_inheritedScopeStores.widenedHolders.contains(holder);
    };
    objects.removeAllMatching([&](MergedHeaderObject& merged) {
        TypeID type = preview.typesOfParameters[merged.position];
        if (type && merged.object.length) {
            if (!addElementsOfMergedArray(merged, type, entering, isWidened(merged.holder), known))
                return true;
            merged.object.shape = type;
            return false;
        }
        if (!type || !m_types.at(type).hasOnlyLayouts() || m_types.at(type).primitives || singleLayoutOf(m_types, m_types.at(type)) != merged.object.layout)
            return true;
        Vector<FieldOfScope, 8> slots;
        for (size_t place = 0; place < merged.object.fields.size(); ++place) {
            FieldOfScope slot;
            slot.scope = entering[merged.position];
            slot.field = { merged.object.layout, merged.object.fields[place] };
            if (holdsBrandOfClass(*this, slot.field))
                continue;
            slot.slotKey = plan().slots().keyOf(program(), slot.field);
            slot.receivers = m_types.at(type).layouts;
            slot.positionOfHolder = merged.position;
            slot.typeOfHolders = type;
            slot.values.append(merged.object.values[place]);
            if (slot.slotKey)
                slot.type = typeOfMergedSlot(slot, isWidened(merged.holder));
            if (!slot.type)
                return true;
            slots.append(WTF::move(slot));
        }
        for (FieldOfScope& slot : slots) {
            slot.isOfMergedObject = true;
            known.removeAllMatching([&](const FieldOfScope& other) {
                return other.positionOfHolder == slot.positionOfHolder && other.slotKey == slot.slotKey;
            });
            known.append(WTF::move(slot));
        }
        return false;
    });
}

void Walker::dropIncompleteMergedObjects(Vector<MergedHeaderObject, 1>& objects, Vector<FieldOfScope, 4>& known)
{
    objects.removeAllMatching([&](const MergedHeaderObject& merged) {
        auto isSlot = [&](const FieldOfScope& field) {
            return field.isOfMergedObject && field.positionOfHolder == merged.position;
        };
        size_t numberOfBrands = std::ranges::count_if(merged.object.fields, [&](FieldIndex field) {
            return holdsBrandOfClass(*this, { merged.object.layout, field });
        });
        if (std::ranges::count_if(known, isSlot) + numberOfBrands == merged.object.values.size())
            return false;
        known.removeAllMatching(isSlot);
        return true;
    });
}

void Walker::noteMergedHeaderObjects(BlockID head, Vector<MergedHeaderObject, 1>&& objects)
{
    for (MergedHeaderObject& merged : objects) {
        merged.parameter = m_code.at(head).parameters()[merged.position];
        noteMergedObject(*this, Reference::value(merged.parameter), merged.object);
    }
    m_mergedHeaderObjects.set(head.index(), std::make_shared<const Vector<MergedHeaderObject, 1>>(WTF::move(objects)));
}

void Walker::bindMergedHeaderObjects(BlockID head)
{
    auto found = m_mergedHeaderObjects.find(head.index());
    if (found == m_mergedHeaderObjects.end() || isUnderHandler())
        return;
    Vector<MergedHeaderObject, 1> objects = *found->value;
    for (MergedHeaderObject& merged : objects) {
        Reference allocated = allocateMergedObject(*this, merged.parameter, m_origin);
        if (m_hasFailed || isUnreachable())
            return;
        if (!allocated)
            continue;
        merged.allocated = allocated.id();
        recordMergedFreshObject(merged.allocated);
        m_state.markEscaped(Reference::value(merged.parameter));
        bind(merged.holder, allocated);
        count(StatisticID::MergedHeaderObjectsAllocated);
    }
    m_mergedHeaderObjects.set(head.index(), std::make_shared<const Vector<MergedHeaderObject, 1>>(WTF::move(objects)));
}

bool Walker::usesBoundHeaderParameters() const
{
    BitVector bound;
    for (const auto& merged : m_mergedHeaderObjects.values()) {
        for (const MergedHeaderObject& object : *merged) {
            if (object.allocated)
                bound.set(object.parameter.index());
        }
    }
    auto isBound = [&](ValueID value) {
        return value && !m_code.isConstant(value) && bound.get(value.index());
    };
    for (uint32_t index = 0; index < m_code.numberOfValues(); ++index) {
        if (m_code.at(ValueID(index)).blockIndex != Operation::noBlock && std::ranges::any_of(m_code.at(ValueID(index)).operands(), isBound))
            return true;
    }
    for (uint32_t index = 0; index < m_code.numberOfEdges(); ++index) {
        if (std::ranges::any_of(m_code.at(EdgeID(index)).arguments(), isBound))
            return true;
    }
    return false;
}

ValueID Walker::parameterOfHeaderAllocation(BlockID head, ValueID name) const
{
    auto found = m_mergedHeaderObjects.find(head.index());
    if (found == m_mergedHeaderObjects.end())
        return name;
    for (const MergedHeaderObject& merged : *found->value) {
        if (merged.allocated && merged.allocated == name)
            return merged.parameter;
    }
    return name;
}

void Walker::checkMergedHeaderObjects(BlockID head, std::span<const Reference> arguments)
{
    auto found = m_mergedHeaderObjects.find(head.index());
    if (found == m_mergedHeaderObjects.end())
        return;
    auto cellOf = [&](Reference argument) {
        return argument && argument.isValue() ? underlyingCellOf(m_code, argument.id()) : ValueID();
    };
    for (const MergedHeaderObject& merged : *found->value) {
        ValueID cell = cellOf(arguments[merged.position]);
        bool isMerged = cell && !m_code.isConstant(cell) && cell.index() >= merged.parameter.index() && arrivesAsMergedObject(*this, cell, merged.object)
            && std::ranges::count_if(arguments, [&](Reference other) { return cellOf(other) == cell; }) == 1;
        if (isMerged) {
            m_state.forEachValueHeldByAnother([&](Reference value) {
                isMerged &= cellOf(value) != cell;
            });
        }
        if (isMerged)
            continue;
        TrialVisit* visit = findTryOfBlockAheadOfEdges(head);
        if (!visit) {
            fail("an object that something else may name comes round to a merged header object"_s);
            return;
        }
        Vector<ValueID, 2>& unmerged = m_loopSummaries[visit->summaryIndex].storesIntoScopes.unmergedHolders;
        if (!unmerged.contains(merged.holder))
            unmerged.append(merged.holder);
        visit->misfits = true;
    }
}

bool Walker::readsMergedElements(BlockID head, uint32_t position) const
{
    auto carried = m_fieldsOfScopesOfHeads.find(head.index());
    auto merged = m_mergedHeaderObjects.find(head.index());
    if (carried == m_fieldsOfScopesOfHeads.end() || merged == m_mergedHeaderObjects.end())
        return true;
    ValueID allocated;
    for (const MergedHeaderObject& object : *merged->value) {
        if (object.position == position)
            allocated = object.allocated;
    }
    Vector<ValueID, 8> elements;
    for (const FieldOfScope& field : carried->value) {
        if (field.isOfMergedObject && field.positionOfHolder == position && field.parameter.isValue())
            elements.append(field.parameter.id());
    }
    if (elements.isEmpty())
        return true;
    for (uint32_t index = elements.first().index(); index < m_code.numberOfValues(); ++index) {
        const Operation& operation = m_code.at(ValueID(index));
        if (ValueID(index) == allocated || m_code.isConstant(ValueID(index)) || conversionsOfForm.contains(operation.opcode))
            continue;
        for (ValueID operand : operation.operands()) {
            if (operand && !m_code.isConstant(operand) && elements.contains(m_code.valueBehindNames<conversionsOfForm>(operand)))
                return true;
        }
    }
    return false;
}

void Walker::widenMergedObject(TrialVisit& visit, uint32_t position)
{
    StoresIntoScopes& stores = m_loopSummaries[visit.summaryIndex].storesIntoScopes;
    ValueID holder = m_frames[visit.frameIndex].index->block(visit.inputHead).parameter(position);
    BlockID head = m_frames[visit.frameIndex].blocks[visit.inputHead.index()].head;
    auto merged = m_mergedHeaderObjects.find(head.index());
    bool isArray = merged != m_mergedHeaderObjects.end() && std::ranges::any_of(*merged->value, [&](const MergedHeaderObject& object) { return object.position == position && object.object.length; });
    if (visit.widenedHolders.contains(holder))
        return;
    visit.widenedHolders.append(holder);
    bool staysMerged = !stores.widenedHolders.contains(holder) && (!isArray || !readsMergedElements(head, position));
    Vector<ValueID, 2>& holders = staysMerged ? stores.widenedHolders : stores.unmergedHolders;
    if (!holders.contains(holder))
        holders.append(holder);
    visit.misfits = true;
}

void Walker::updateMergedObjects(LoopSummary& summary, std::span<const Reference> entering)
{
    if (!summary.storesIntoScopes.mergedObjects)
        return;
    Vector<MergedHeaderObject, 1> objects;
    for (const MergedHeaderObject& merged : *summary.storesIntoScopes.mergedObjects) {
        Reference argument = merged.position < entering.size() ? entering[merged.position] : Reference();
        ValueID cell = argument && argument.isValue() ? underlyingCellOf(m_code, argument.id()) : ValueID();
        std::optional<MergeableObject> object = cell ? findMergeableObject(*this, m_state, cell) : std::nullopt;
        if (object && object->layout == merged.object.layout && object->fields == merged.object.fields && !object->length == !merged.object.length && object->values.size() == merged.object.values.size())
            objects.append(MergedHeaderObject { merged.position, merged.holder, cell, { }, WTF::move(*object) });
    }
    summary.storesIntoScopes.mergedObjects = objects.isEmpty() ? nullptr : std::make_shared<const Vector<MergedHeaderObject, 1>>(WTF::move(objects));
}

void Walker::learnSlotsOfMergedObjects(LoopSummary& summary, BlockID inputHead, BlockID head)
{
    auto found = head ? m_mergedHeaderObjects.find(head.index()) : m_mergedHeaderObjects.end();
    if (found == m_mergedHeaderObjects.end())
        return;
    Vector<SlotOfLoop, 2>& slots = summary.storesIntoScopes.slots;
    summary.storesIntoScopes.mergedObjects = found->value;
    for (const MergedHeaderObject& merged : *found->value) {
        for (FieldIndex field : merged.object.fields) {
            if (holdsBrandOfClass(*this, { merged.object.layout, field }))
                continue;
            SlotOfLoop slot { merged.holder, plan().slots().keyOf(program(), { merged.object.layout, field }) };
            auto place = std::ranges::lower_bound(slots, slot.order(), { }, &SlotOfLoop::order);
            if (place == slots.end() || place->order() != slot.order())
                slots.insert(place - slots.begin(), slot);
        }
    }
    UNUSED_PARAM(inputHead);
}

Vector<Reference, 8> Walker::allocateMergedObjectsAtRestart(std::span<const Reference> valuesAtHeader)
{
    Vector<Reference, 8> values(valuesAtHeader);
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    BlockID head = inputBlock(versionedLoop.frameIndex, versionedLoop.inputHead).head;
    auto found = m_mergedHeaderObjects.find(head.index());
    if (found == m_mergedHeaderObjects.end())
        return values;
    auto carried = m_fieldsOfScopesOfHeads.find(head.index());
    RELEASE_ASSERT(carried != m_fieldsOfScopesOfHeads.end());
    for (const MergedHeaderObject& merged : *found->value) {
        MergeableObject atHeader = merged.object;
        for (size_t place = 0; place < atHeader.values.size(); ++place) {
            if (!atHeader.length && holdsBrandOfClass(*this, { atHeader.layout, atHeader.fields[place] }))
                continue;
            size_t slot = carried->value.findIf([&](const FieldOfScope& field) {
                if (!field.isOfMergedObject || field.positionOfHolder != merged.position)
                    return false;
                return field.element ? m_code.at(field.element.id()).integer() == static_cast<int64_t>(place) : field.field.field == atHeader.fields[place];
            });
            RELEASE_ASSERT(slot != notFound);
            atHeader.values[place] = carried->value[slot].parameter;
        }
        Reference allocated = allocateMergeableObject(*this, atHeader, m_origin);
        if (!allocated) {
            if (!isUnreachable())
                fail("a merged header object is not allocated where its iteration restarts"_s);
            return values;
        }
        values[merged.position] = allocated;
    }
    return values;
}

void Walker::allocateMergedObjectsOnWay(EdgeID& edge, BlockID to, BlockID inputBlock, const OpenEdge& open, std::span<const uint32_t> joined, std::span<const uint32_t> joinedNames, std::span<const ObjectsMetAtJoin> objectsMet, Vector<Reference, 8>& arguments)
{
    if (isUnderHandler() || m_coldCopyDepth || mayNameObjectsInGenerators())
        return;
    const Frame& frame = m_frames.last();
    auto cellOf = [&](Reference name) {
        return name && name.isValue() ? underlyingCellOf(m_code, name.id()) : ValueID();
    };
    for (size_t place = 0; place < arguments.size(); ++place) {
        ValueID cell = cellOf(arguments[place]);
        if (!cell || m_code.isConstant(cell) || !isMergedFreshObject(cell) || !m_state.isFresh(Reference::value(cell)) || !wasBuiltInCurrentFrame(cell))
            continue;
        if (std::ranges::any_of(objectsMet, [&](const ObjectsMetAtJoin& met) { return met.placeAmongParameters == place; }))
            continue;
        auto namesCell = [&](Reference other) { return cellOf(other) == cell; };
        auto isNameOnWay = [&](uint32_t slot) { return namesCell(open.names.nameIn(slot)); };
        if (std::ranges::count_if(open.arguments, namesCell) + std::ranges::count_if(joinedNames, isNameOnWay) != 1)
            continue;
        ValueID passed = place < joined.size() ? currentBodyIndex().block(inputBlock).parameter(joined[place]) : frame.duplicatedValues->slots[joinedNames[place - joined.size()]].value;
        bool isNamed = isNamedAtHead(m_state, cell, inputBlock, scopedLambda<Reference(ValueID)>([&](ValueID value) -> Reference {
            if (value == passed)
                return { };
            if (frame.duplicatedValues) {
                if (uint32_t slot = frame.duplicatedValues->slotOf(value); slot != Frame::DuplicatedValues::noSlot)
                    return open.names.nameIn(slot);
            }
            return referenceBoundIn(frame, value);
        }));
        std::optional<MergeableObject> object = isNamed ? std::nullopt : findMergeableObject(*this, m_state, cell);
        if (!object)
            continue;
        if (edge)
            layBlockOnEdge(edge, to);
        Reference allocated = allocateMergeableObject(*this, *object, m_origin);
        if (m_hasFailed || isUnreachable())
            return;
        if (!allocated)
            continue;
        arguments[place] = allocated;
        count(StatisticID::MergedObjectsAllocatedOnEdges);
    }
}

void Walker::bindMergedJoinObjects(BlockID inputBlock, std::span<const uint32_t> joined, std::span<const ValueID> parameters, std::span<const ObjectsMetAtJoin> objectsMet)
{
    if (isUnreachable() || isUnderHandler() || mayNameObjectsInGenerators() || !m_code.at(m_block).isInLoop || m_code.at(m_block).frequency != BlockFrequency::Normal)
        return;
    for (const ObjectsMetAtJoin& met : objectsMet) {
        Reference allocated = allocateMergedObject(*this, parameters[met.placeAmongParameters], m_origin);
        if (m_hasFailed || isUnreachable())
            return;
        if (!allocated)
            continue;
        recordMergedFreshObject(allocated.id());
        m_state.markEscaped(Reference::value(parameters[met.placeAmongParameters]));
        bind(currentBodyIndex().block(inputBlock).parameter(joined[met.placeAmongParameters]), allocated);
        count(StatisticID::MergedJoinObjectsAllocated);
    }
}

Vector<Reference, 8> Walker::allocateMergedObjectsIn(std::span<const Reference> written)
{
    Vector<Reference, 8> values(written);
    for (Reference& value : values) {
        ValueID cell = value && value.isValue() ? underlyingCellOf(m_code, value.id()) : ValueID();
        if (!cell || m_code.isConstant(cell) || !isMergedFreshObject(cell) || !m_state.isFresh(Reference::value(cell)))
            continue;
        if (Reference allocated = allocateMergedObject(*this, cell, m_origin))
            value = allocated;
        if (isUnreachable())
            break;
    }
    return values;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
