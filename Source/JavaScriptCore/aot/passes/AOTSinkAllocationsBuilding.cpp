/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTFlowApply.h"
#include "AOTVector.h"
#include <algorithm>
#include <ranges>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

struct MemberVisit {
    VirtualObjectID member;
    uint32_t numberOfVisitedHeldObjects { 0 };
};

enum class PropertyInitialization : uint8_t { Listed, ListedWithPlaceholder, ListedWithFiller, StoredLate };

struct PlannedProperty {
    uint32_t field { 0 };
    VirtualObjectID awaitedObject;
    PropertyInitialization initialization { PropertyInitialization::Listed };
};

static bool isStoredInAnyVariant(const VirtualFamily& family, const ObjectState& state, uint32_t field)
{
    return std::ranges::any_of(state.variants, [&](const ObjectVariant& variant) {
        return hasStored(family.at(variant.shape), field);
    });
}

static bool isStoredInEveryVariant(const VirtualFamily& family, const ObjectState& state, uint32_t field)
{
    return std::ranges::all_of(state.variants, [&](const ObjectVariant& variant) {
        return hasStored(family.at(variant.shape), field);
    });
}

static bool canBuildEveryVariant(const VirtualFamily& family, const ObjectState& state)
{
    return std::ranges::all_of(state.variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = family.at(variant.shape);
        return shape.kind != VirtualShapeKind::Fields || family.descriptionOf(shape).canBeBuilt;
    });
}

static bool hasVariantWithUnprovenField(const VirtualFamily& family, const ObjectState& state)
{
    return std::ranges::any_of(state.variants, [&](const ObjectVariant& variant) {
        return hasUnprovenField(family, family.at(variant.shape));
    });
}

static bool isHardEdge(const AllocationSinking& sinking, VirtualObjectID holder, const ObjectState& state, uint32_t field)
{
    if (state.needsTag() || sinking.at(holder).isJoin)
        return true;
    const VirtualFamily& family = sinking.familyOf(holder);
    const VirtualObjectDescription& description = family.descriptionOf(family.at(state.variants[0].shape));
    return canStoreLate(sinking.objectContext(), description, family.fieldAt(field)) == LateStore::Unknown;
}

// A property that holds an object which is built after its holder, because it refers to the holder, is stored late.
static Vector<PlannedProperty, 8> planProperties(const AllocationSinking& sinking, const SinkingState& state, VirtualObjectID object, const FreshResultVariant& variant)
{
    const VirtualFamily& family = sinking.familyOf(object);
    const ObjectState& objectState = *state.find(object);

    Vector<PlannedProperty, 8> properties;
    bool allocationHasEnded = false;
    for (const FreshResultField& property : variant.properties) {
        PlannedProperty planned;
        planned.field = *family.findField(property.field);
        if (property.field.kind == VirtualFieldKind::UnprovenNamed)
            planned.initialization = PropertyInitialization::ListedWithFiller;
        VirtualObjectID held = objectState.objectHeldIn(planned.field);
        if (held && !state.find(held)->isCell()) {
            planned.awaitedObject = held;
            switch (canStoreLate(sinking.objectContext(), variant.description, property.field)) {
            case LateStore::NeedsNoPlaceholder:
                planned.initialization = PropertyInitialization::StoredLate;
                break;
            case LateStore::NeedsUndefinedPlaceholder:
                planned.initialization = PropertyInitialization::ListedWithPlaceholder;
                break;
            case LateStore::CreatesProperty:
                allocationHasEnded = true;
                break;
            case LateStore::Unknown:
                break;
            }
        }
        if (allocationHasEnded)
            planned.initialization = PropertyInitialization::StoredLate;
        properties.append(planned);
    }
    return properties;
}

// A late store is the store that was folded. One that follows a placeholder has taken the place of a store that creates the field.
static unsigned countRepeatedStores(const AllocationSinking& sinking, const SinkingState& state, VirtualObjectID object)
{
    const ObjectState& objectState = *state.find(object);
    const VirtualFamily& family = sinking.familyOf(object);
    const VirtualShape& shape = family.at(objectState.variants[0].shape);
    if (objectState.needsTag() || shape.kind != VirtualShapeKind::Fields)
        return 0;

    FreshResultVariant variant = variantOfShape(sinking, object, shape);
    BitVector fieldsListedByAllocation;
    forEachInitialField(sinking.objectContext(), variant.description, [&](const VirtualField& initialField, ValueID) {
        fieldsListedByAllocation.set(*family.findField(initialField));
    });

    unsigned count = 0;
    for (const PlannedProperty& property : planProperties(sinking, state, object, variant)) {
        switch (property.initialization) {
        case PropertyInitialization::Listed:
            break;
        case PropertyInitialization::ListedWithPlaceholder:
            count += fieldsListedByAllocation.get(property.field);
            break;
        case PropertyInitialization::ListedWithFiller:
        case PropertyInitialization::StoredLate:
            ++count;
            break;
        }
    }
    return count;
}

static bool isBorrowingCall(const BuildCause& cause)
{
    return cause.opcode && effectRuleOf(*cause.opcode) == EffectRule::Call && cause.reasons.isEmpty() && cause.limit == LimitID::None;
}

template<typename InstructionType>
static void resolveOperandsOf(ObjectInterpreter& interpreter, InstructionType& instruction)
{
    instruction.forEachOperand([&](ValueID& operand, uint32_t operandIndex) {
        bool isGuard = instruction.operandKind(operandIndex) == OperandKind::Guard;
        operand = isGuard ? interpreter.resolveGuard(operand) : interpreter.resolve(operand);
    });
}

void ObjectInterpreter::requireCell(ValueID value, const BuildCause& cause)
{
    if (VirtualObjectID object = objectOf(value))
        build(object, cause);
}

void ObjectInterpreter::build(VirtualObjectID root, const BuildCause& cause)
{
    if (stateOf(root)->isCell())
        return;

    Vector<VirtualObjectID, 4> members = collectVirtualMembers(root);
    for (VirtualObjectID member : members) {
        const ObjectState& state = *stateOf(member);
        if (!canBuildEveryVariant(m_sinking.familyOf(member), state))
            m_sinking.abandon(member);
        if (!cause.admitsStoresByName && hasVariantWithUnprovenField(m_sinking.familyOf(member), state))
            m_sinking.refuseUnprovenValues(member);
        if (m_sinking.at(member).isJoin && !fitsType(member, function().at(m_sinking.at(member).definition).type))
            m_sinking.abandon(member);
        if (m_mode == SinkingMode::Count && state.hasCell())
            m_sinking.didBuildAfterMerge(member, state.cellJoinedAt);
    }

    if (!isRewriting()) {
        buildMembers(members.span(), cause);
        return;
    }

    Origin userOrigin = cursor().builder().origin();
    buildMembers(members.span(), cause);
    cursor().setOrigin(userOrigin);
}

Vector<VirtualObjectID, 4> ObjectInterpreter::collectVirtualMembers(VirtualObjectID root) const
{
    Vector<VirtualObjectID, 4> members;
    Vector<VirtualObjectID, 4> worklist { root };
    BitVector visited;
    while (!worklist.isEmpty()) {
        VirtualObjectID member = worklist.takeLast();
        const ObjectState* state = stateOf(member);
        RELEASE_ASSERT(state);
        if (state->isCell() || !visited.add(member.index()))
            continue;

        members.append(member);
        const VirtualFamily& family = m_sinking.familyOf(member);
        for (const HeldObject& held : state->heldObjects | std::views::reverse) {
            if (isStoredInAnyVariant(family, *state, held.field))
                worklist.append(held.object);
        }
    }
    return members;
}

// In front of the switch on the tag of the holder the fields of such an object are unspecified on some way. An arm that stores the object is entered only where it is there.
bool ObjectInterpreter::isBuiltInArmsOf(VirtualObjectID holder, const HeldObject& held) const
{
    const ObjectState& state = *stateOf(holder);
    return stateOf(held.object)->isMissingOnSomeWay && state.needsTag() && !hasBothAllocationOutcomes(holder)
        && !isStoredInEveryVariant(m_sinking.familyOf(holder), state, held.field);
}

// Behind the arms nobody can ask for what only some of them have built.
bool ObjectInterpreter::isReachedOnlyThrough(VirtualObjectID holder, VirtualObjectID held) const
{
    Vector<VirtualObjectID, 4> members = collectVirtualMembers(held);
    if (members.contains(holder))
        return false;
    for (VirtualObjectID member : members) {
        if (!stateOf(member)->isMissingOnSomeWay)
            return false;
    }
    for (const ObjectEntry& entry : m_state.objects) {
        if (members.contains(entry.object))
            continue;
        for (const HeldObject& other : entry.state.heldObjects) {
            if (!members.contains(other.object))
                continue;
            if (entry.object != holder || other.object != held || !isBuiltInArmsOf(holder, other))
                return false;
        }
    }
    return !m_state.aliases.containsIf([&](const AliasEntry& alias) {
        return members.contains(alias.object);
    }) && !m_state.tests.containsIf([&](const TestEntry& test) {
        return members.contains(test.object);
    });
}

void ObjectInterpreter::buildMembers(std::span<const VirtualObjectID> members, const BuildCause& cause)
{
    Vector<LateFieldStore, 2> late;
    Vector<MemberVisit, 4> visits;
    BitVector membersBeingVisited;
    BitVector membersBuiltInArms;
    for (VirtualObjectID member : members) {
        const ObjectState& state = *stateOf(member);
        for (const HeldObject& held : state.heldObjects) {
            if (stateOf(held.object)->isCell() || !isBuiltInArmsOf(member, held))
                continue;
            if (isStoredInAnyVariant(m_sinking.familyOf(member), state, held.field) && !isReachedOnlyThrough(member, held.object)) {
                RELEASE_ASSERT(!isRewriting());
                m_sinking.abandon(held.object);
            }
            if (!isRewriting())
                continue;
            for (VirtualObjectID memberOfArms : collectVirtualMembers(held.object))
                membersBuiltInArms.set(memberOfArms.index());
        }
    }

    auto beginVisit = [&](VirtualObjectID member) {
        if (stateOf(member)->isCell() || membersBuiltInArms.get(member.index()))
            return;
        membersBeingVisited.set(member.index());
        visits.append(MemberVisit { member, 0 });
    };

    auto emitCell = [&](VirtualObjectID member) {
        bool isAsAllocated = hasBothAllocationOutcomes(member);
        if (stateOf(member)->needsTag() && !isAsAllocated) {
            buildByTag(member, late, cause);
            return;
        }
        ValueID cell = isAsAllocated
            ? emitAllocation(member, VirtualShape(shapeOf(member, stateOf(member)->variants[1])), late, AbsentResult::IsPossible)
            : emitValueOfVariant(member, stateOf(member)->variants[0], late);
        ObjectValues& carried = values().ensure(member);
        carried.cell = cell;
        if (m_sinking.at(member).needsGuard && !carried.guard)
            carried.guard = cursor().emit(Value(Opcode::AllGuards));
    };

    auto finishVisit = [&](VirtualObjectID member) {
        visits.removeLast();
        membersBeingVisited.clear(member.index());
        if (m_mode == SinkingMode::Count)
            m_sinking.at(member).numberOfRepeatedStores += countRepeatedStores(m_sinking, m_state, member);
        if (isRewriting())
            emitCell(member);
        didBuild(member, cause);
        if (isRewriting() && m_sinking.at(member).isJoin)
            values().ensure(member).cell = cellOf(member);
    };

    for (VirtualObjectID first : members | std::views::reverse) {
        beginVisit(first);
        while (!visits.isEmpty()) {
            VirtualObjectID member = visits.last().member;
            const ObjectState& state = *stateOf(member);
            uint32_t position = visits.last().numberOfVisitedHeldObjects;
            if (position == state.heldObjects.size()) {
                finishVisit(member);
                continue;
            }

            ++visits.last().numberOfVisitedHeldObjects;
            HeldObject held = state.heldObjects[position];
            if (stateOf(held.object)->isCell() || !isStoredInAnyVariant(m_sinking.familyOf(member), state, held.field))
                continue;
            if (isBuiltInArmsOf(member, held) && !isReachedOnlyThrough(member, held.object)) {
                RELEASE_ASSERT(!isRewriting());
                m_sinking.abandon(held.object);
            }
            if (!isHardEdge(m_sinking, member, state, held.field) && m_state.canReach(held.object, member)) {
                if (m_mode == SinkingMode::Count)
                    m_sinking.at(held.object).isStoredLate = true;
                continue;
            }
            if (membersBeingVisited.get(held.object.index())) {
                RELEASE_ASSERT(!isRewriting());
                m_sinking.abandon(member);
                continue;
            }
            beginVisit(held.object);
        }
    }

    emitLateStores(late.span());
}

ValueID ObjectInterpreter::emitValueOfVariant(VirtualObjectID object, const ObjectVariant& variant, Vector<LateFieldStore, 2>& late)
{
    const VirtualShape& shape = shapeOf(object, variant);
    switch (shape.kind) {
    case VirtualShapeKind::Cell: {
        ValueID cell = values().find(object)->cell;
        RELEASE_ASSERT(cell);
        return cell;
    }
    case VirtualShapeKind::Constant:
        return cursor().emit(makeConstant(shape.constant));
    case VirtualShapeKind::Fields:
        return emitAllocation(object, VirtualShape(shape), late);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The operands of such an allocation decide between its two outcomes, so it is emitted as it was while every way here carries them.
bool ObjectInterpreter::hasBothAllocationOutcomes(VirtualObjectID object) const
{
    std::optional<uint8_t> tagBitWhenAbsent = m_sinking.at(object).tagBitWhenAbsent;
    if (!tagBitWhenAbsent || m_sinking.descriptionOf(object).staysInPlace || stateOf(object)->lacksFieldsOnSomeWay)
        return false;
    const auto& variants = stateOf(object)->variants;
    return variants.size() == 2 && variants[0].tagBit == *tagBitWhenAbsent && variants[1].tagBit == *tagBitWhenAbsent + 1;
}

ValueID ObjectInterpreter::emitAllocation(VirtualObjectID object, const VirtualShape& shape, Vector<LateFieldStore, 2>& late, AbsentResult absentResult)
{
    const VirtualFamily& family = m_sinking.familyOf(object);
    FreshResultVariant variant = variantOfShape(m_sinking, object, shape);
    cursor().setOrigin(variant.prototype.origin());

    ValueID placeholder;
    Vector<FieldValue, 8> properties;
    Vector<FieldValue, 2> storedByName;
    for (const PlannedProperty& property : planProperties(m_sinking, m_state, object, variant)) {
        const VirtualField& field = family.fieldAt(property.field);
        if (property.initialization == PropertyInitialization::Listed) {
            properties.append(FieldValue { field, valueOfField(object, property.field) });
            continue;
        }
        if (property.initialization == PropertyInitialization::ListedWithFiller) {
            properties.append(FieldValue { field, cursor().emit(makeFiller(program(), variant.description, field)) });
            storedByName.append(FieldValue { field, valueOfField(object, property.field) });
            continue;
        }

        ValueID knownValue = property.awaitedObject ? ValueID() : valueOfField(object, property.field);
        late.append(LateFieldStore { object, property.field, property.awaitedObject, knownValue });
        if (property.initialization == PropertyInitialization::StoredLate)
            continue;
        if (!placeholder)
            placeholder = emitUndefined();
        properties.append(FieldValue { field, placeholder });
    }
    Vector<FieldValue, 4> parts;
    for (const FreshResultField& part : variant.parts)
        parts.append(FieldValue { part.field, valueOfField(object, *family.findField(part.field)) });

    ValueID cell = cursor().emit(makeAllocation(program(), AllocationRequest { variant.description, variant.prototype, properties.span(), parts.span(), variant.length, absentResult == AbsentResult::IsPossible }));
    for (const FieldValue& stored : storedByName)
        cursor().emit(makeStoreByName(stored, cell));
    if (m_sinking.at(object).needsGuard) {
        Value guard(Opcode::AllGuards);
        guard.setGuardsOperands(singleElementSpan(cell));
        ValueID cellGuard = cursor().emit(WTF::move(guard));
        values().ensure(object).guard = cellGuard;
    }
    return cell;
}

void ObjectInterpreter::buildByTag(VirtualObjectID object, Vector<LateFieldStore, 2>& late, const BuildCause& cause)
{
    struct MemberOfArms {
        VirtualObjectID member;
        ObjectState state;
        ObjectValues values;
    };

    IRBuilder& builder = cursor().builder();
    const ObjectState& state = *stateOf(object);
    Vector<HeldObject, 2> heldObjectsOfArms;
    Vector<MemberOfArms, 2> membersOfArms;
    for (const HeldObject& held : state.heldObjects) {
        if (stateOf(held.object)->isCell() || !isBuiltInArmsOf(object, held))
            continue;
        heldObjectsOfArms.append(held);
        for (VirtualObjectID member : collectVirtualMembers(held.object)) {
            if (membersOfArms.containsIf([&](const MemberOfArms& known) { return known.member == member; }))
                continue;
            const ObjectValues* carried = values().find(member);
            membersOfArms.append(MemberOfArms { member, *stateOf(member), carried ? *carried : ObjectValues { } });
        }
    }
    bool needsGuard = m_sinking.at(object).needsGuard;
    size_t numberOfLateStores = late.size();
    ValueID carriedTag = values().find(object)->tag;
    ValueID carriedCell = values().find(object)->cell;
    ValueID carriedGuard = values().find(object)->guard;
    RELEASE_ASSERT(carriedTag && (carriedCell || !state.hasCell()));

    ShapeContext context = m_sinking.shapeContext(object);
    TypeID type = TypeTable::never;
    for (const ObjectVariant& variant : state.variants) {
        const VirtualShape& shape = shapeOf(object, variant);
        type = builder.types().unionOf(type, shape.kind == VirtualShapeKind::Cell ? function().at(carriedCell).type : typeOfShape(context, shape));
    }

    BlockID continuation = cursor().addUncoveredBlock();
    ValueID cellParameter = builder.addParameter(continuation, type);
    ValueID guardParameter = needsGuard ? builder.addParameter(continuation, TypeTable::guard) : ValueID();

    Vector<BlockID, 4> arms;
    Vector<std::pair<int64_t, Target>> cases;
    for (const ObjectVariant& variant : state.variants) {
        RELEASE_ASSERT(variant.tagBit);
        arms.append(cursor().addUncoveredBlock());
        cases.append({ int64_t(1) << *variant.tagBit, Target { arms.last(), { }, { } } });
    }
    Target otherwise = cases.takeLast().second;
    builder.emitSwitch(carriedTag, WTF::move(cases), WTF::move(otherwise));

    for (size_t index = 0; index < arms.size(); ++index) {
        const ObjectVariant& variant = state.variants[index];
        cursor().moveTo(arms[index]);
        for (const MemberOfArms& member : membersOfArms) {
            *stateOf(member.member) = member.state;
            values().ensure(member.member) = member.values;
        }
        for (const HeldObject& held : heldObjectsOfArms) {
            if (hasStored(shapeOf(object, variant), held.field) && !stateOf(held.object)->isCell())
                buildMembers(collectVirtualMembers(held.object).span(), cause);
        }
        Arguments arguments { emitValueOfVariant(object, variant, late) };
        if (needsGuard) {
            ValueID guard;
            switch (shapeOf(object, variant).kind) {
            case VirtualShapeKind::Cell:
                guard = carriedGuard;
                break;
            case VirtualShapeKind::Constant:
                break;
            case VirtualShapeKind::Fields:
                guard = values().find(object)->guard;
                break;
            }
            arguments.append(guard ? guard : cursor().emit(Value(Opcode::AllGuards)));
        }
        builder.emitJump(continuation, WTF::move(arguments));
    }
    RELEASE_ASSERT(late.size() == numberOfLateStores);
    for (const MemberOfArms& member : membersOfArms) {
        m_state.remove(member.member);
        values().remove(member.member);
    }

    cursor().moveTo(continuation);
    ObjectValues& carried = values().ensure(object);
    carried.cell = cellParameter;
    carried.guard = guardParameter;
}

void ObjectInterpreter::emitLateStores(std::span<const LateFieldStore> stores)
{
    for (const LateFieldStore& store : stores) {
        const VirtualObjectDescription& description = m_sinking.descriptionOf(store.holder);
        ValueID holder = values().find(store.holder)->cell;
        ValueID stored = store.held ? cellOf(store.held) : store.value;
        RELEASE_ASSERT(holder && stored);

        const VirtualField& field = m_sinking.familyOf(store.holder).fieldAt(store.field);
        cursor().setOrigin(function().at(description.allocation).origin());
        if (field.kind == VirtualFieldKind::UnprovenNamed) {
            cursor().emit(makeStoreByName(FieldValue { field, stored }, holder));
            continue;
        }
        Value value = makeFieldStore(program(), description, FieldValue { field, stored }, holder, holder);
        if (Value::opcodesWithReceivers.contains(value.opcode))
            setExactReceivers(value, description.layout, cursor().builder().types());
        cursor().setOrigin(function().at(description.allocation).origin());
        cursor().emit(WTF::move(value));
    }
}

BlockFrequency ObjectInterpreter::frequencyOfBuild() const
{
    const BlockFrequencies& frequencies = m_sinking.analyses().blockFrequencies();
    if (!m_enteredEdge.edge)
        return frequencies.classOf(m_state.block);
    Edge edge = m_enteredEdge.edge->edge;
    return rarerOf(frequencies.classOf(edge.from), frequencies.classOf(edge));
}

void ObjectInterpreter::didBuild(VirtualObjectID object, const BuildCause& cause)
{
    ObjectState& state = *stateOf(object);
    if (m_mode == SinkingMode::Count) {
        BlockFrequency frequency = frequencyOfBuild();
        bool isBorrowed = isBorrowingCall(cause);
        auto countBuilds = [&](VirtualObjectID built, unsigned numberOfBuilds) {
            Candidate& candidate = m_sinking.at(built);
            candidate.numberOfBuilds += numberOfBuilds;
            if (isRarerThan(frequency, m_sinking.analyses().blockFrequencies().classOf(candidate.block)))
                candidate.numberOfRarerBuilds += numberOfBuilds;
            else if (isBorrowed)
                candidate.numberOfBorrowingBuilds += numberOfBuilds;
            if (!candidate.firstCause)
                candidate.firstCause = cause;
        };
        unsigned numberOfAllocations = 0;
        for (const ObjectVariant& variant : state.variants) {
            const VirtualShape& shape = shapeOf(object, variant);
            if (shape.kind != VirtualShapeKind::Fields)
                continue;
            ++numberOfAllocations;
            VirtualObjectID allocation = m_sinking.candidateDefinedBy(m_sinking.familyOf(object).descriptionOf(shape).allocation);
            if (allocation && allocation != object)
                countBuilds(allocation, 1);
        }
        countBuilds(object, numberOfAllocations);
    }

    state.becomeCell();
    stateDidChange(object);
    if (!isRewriting())
        return;

    ObjectValues& carried = values().ensure(object);
    carried.tag = { };
    carried.fields.clear();
}

ValueID ObjectInterpreter::resolve(ValueID value)
{
    VirtualObjectID object = objectOf(value);
    return object ? cellOf(object) : value;
}

ValueID ObjectInterpreter::cellOf(VirtualObjectID object)
{
    const ObjectState& state = *stateOf(object);
    RELEASE_ASSERT(state.isCell());
    ValueID cell = values().find(object)->cell;
    RELEASE_ASSERT(cell);

    // A handler edge carries no value of the block it leaves, so it has no place for a cast.
    if (m_enteredEdge.edge && m_enteredEdge.edge->edge.isHandlerEdge())
        return cell;

    // The casts went while the object was virtual.
    for (TypeID assumedType : state.assumedTypes) {
        if (program().typeOf(function(), cell).isSubsetOf(program().at(assumedType), program()))
            continue;
        if (m_sinking.context().build.validates())
            cursor().emit(makeValidateType(cell, assumedType));
        cell = cursor().emit(makeAssumeType(cell, assumedType));
    }
    return cell;
}

ValueID ObjectInterpreter::resolveGuard(ValueID guard)
{
    VirtualObjectID object = guardedObjectOf(guard);
    if (!object)
        return guard;

    RELEASE_ASSERT(stateOf(object)->isCell());
    ValueID cellGuard = values().find(object)->guard;
    RELEASE_ASSERT(cellGuard);
    return cellGuard;
}

void ObjectInterpreter::resolveOperands(Value& value)
{
    resolveOperandsOf(*this, value);
}

void ObjectInterpreter::resolveOperands(Terminator& terminator)
{
    resolveOperandsOf(*this, terminator);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
