/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include <algorithm>
#include <wtf/EnumSet.h>

namespace JSC { namespace AOT { namespace Typed {

static bool isWithin(const Program& program, TypeID type, TypeID bound)
{
    return type == bound || program.at(type).isSubsetOf(program.at(bound), program);
}

static bool mayBeUndefined(const Type& type)
{
    return type.primitives & Primitive::Undefined;
}

static std::optional<VirtualFieldAccess> lookUpOnAllVariants(const AllocationSinking& sinking, VirtualObjectID object, const ObjectState& state, const Value& access)
{
    const VirtualFamily& family = sinking.familyOf(object);
    std::optional<VirtualFieldAccess> result;
    BitVector visitedDescriptions;
    for (const ObjectVariant& variant : state.variants) {
        const VirtualShape& shape = family.at(variant.shape);
        if (shape.kind == VirtualShapeKind::Constant)
            return std::nullopt;
        if (shape.kind == VirtualShapeKind::Cell || !visitedDescriptions.add(shape.descriptionIndex))
            continue;

        VirtualFieldAccess found = lookUpField(sinking.objectContext(), family.descriptionOf(shape), access);
        if (found.lookup == FieldLookup::Unknown)
            return std::nullopt;
        if (!result) {
            result = found;
            continue;
        }
        if (found.lookup != result->lookup || found.field != result->field || found.dynamicIndex != result->dynamicIndex)
            return std::nullopt;
        if (found.whenAbsent != result->whenAbsent)
            result->whenAbsent = AbsentField::Unknown;
        if (found.whenPresent != result->whenPresent)
            result->whenPresent = PresentField::Unknown;
    }
    return result;
}

static EnumSet<VirtualObjectKind> virtualKindsOf(const VirtualFamily& family, const ObjectState& state)
{
    EnumSet<VirtualObjectKind> kinds;
    for (const ObjectVariant& variant : state.variants) {
        const VirtualShape& shape = family.at(variant.shape);
        if (shape.kind == VirtualShapeKind::Fields)
            kinds.add(family.descriptionOf(shape).kind);
    }
    return kinds;
}

static std::optional<uint32_t> commonArrayLengthOf(const VirtualFamily& family, const ObjectState& state)
{
    std::optional<uint32_t> length;
    for (const ObjectVariant& variant : state.variants) {
        const VirtualShape& shape = family.at(variant.shape);
        if (shape.kind != VirtualShapeKind::Fields || family.descriptionOf(shape).kind != VirtualObjectKind::Array)
            return std::nullopt;
        if (length && *length != shape.length)
            return std::nullopt;
        length = shape.length;
    }
    return length;
}

static bool isStoredIn(const VirtualFamily& family, const VirtualShape& shape, uint32_t field)
{
    if (!hasStored(shape, field))
        return false;
    const VirtualField& virtualField = family.fieldAt(field);
    return virtualField.kind != VirtualFieldKind::Element || virtualField.index < shape.length;
}

static bool isCarriedIn(const VirtualFamily& family, const ObjectState& state, const VirtualShape& shape, uint32_t field)
{
    if (shape.kind == VirtualShapeKind::Cell)
        return state.fieldsValidInCell.get(field);
    return isStoredIn(family, shape, field);
}

static std::optional<uint32_t> tagBitsWhenCarried(const VirtualFamily& family, const ObjectState& state, uint32_t field)
{
    uint32_t carriedBits = 0;
    uint32_t absentBits = 0;
    for (const ObjectVariant& variant : state.variants) {
        if (!variant.tagBit)
            return std::nullopt;
        uint32_t tagMask = 1u << *variant.tagBit;
        if (isCarriedIn(family, state, family.at(variant.shape), field))
            carriedBits |= tagMask;
        else
            absentBits |= tagMask;
    }
    if (carriedBits & absentBits)
        return std::nullopt;
    return carriedBits;
}

enum class UnprovenVariants : uint8_t { None, Some, All };

static UnprovenVariants unprovenVariantsOf(const VirtualFamily& family, const ObjectState& state, uint32_t namedField)
{
    std::optional<uint32_t> unprovenField = family.findCounterpart(namedField);
    if (!unprovenField)
        return UnprovenVariants::None;
    size_t count = std::ranges::count_if(state.variants, [&](const ObjectVariant& variant) {
        return hasStored(family.at(variant.shape), *unprovenField);
    });
    if (!count)
        return UnprovenVariants::None;
    return count == state.variants.size() ? UnprovenVariants::All : UnprovenVariants::Some;
}

static TypeID typeOfCell(const AllocationSinking& sinking, VirtualObjectID object)
{
    const Candidate& candidate = sinking.at(object);
    return candidate.isJoin ? sinking.function().at(candidate.definition).type : sinking.descriptionOf(object).type;
}

static bool fieldValueFits(const AllocationSinking& sinking, VirtualObjectID object, const ObjectState& state, uint32_t field, TypeID type)
{
    const VirtualFamily& family = sinking.familyOf(object);
    auto fitsIn = [&](const VirtualObjectDescription& description) {
        return isWithin(sinking.program(), typeOfField(sinking.objectContext(), description, family.fieldAt(field)), type);
    };
    return std::ranges::all_of(state.variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = family.at(variant.shape);
        switch (shape.kind) {
        case VirtualShapeKind::Cell:
            return !sinking.at(object).isJoin && fitsIn(sinking.descriptionOf(object));
        case VirtualShapeKind::Constant:
            return false;
        case VirtualShapeKind::Fields:
            return !hasStored(shape, field) || fitsIn(family.descriptionOf(shape));
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static bool isAssumedWithin(const Program& program, const ObjectState& state, TypeID type)
{
    return state.assumedTypes.containsIf([&](TypeID assumedType) {
        return isWithin(program, assumedType, type);
    });
}

static bool fitsWithoutCasts(const AllocationSinking& sinking, VirtualObjectID object, const ObjectState& state, TypeID type)
{
    const Program& program = sinking.program();
    const VirtualFamily& family = sinking.familyOf(object);
    return std::ranges::all_of(state.variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = family.at(variant.shape);
        switch (shape.kind) {
        case VirtualShapeKind::Cell:
            return isWithin(program, typeOfCell(sinking, object), type);
        case VirtualShapeKind::Constant:
            return isWithin(program, typeOf(shape.constant), type);
        case VirtualShapeKind::Fields: {
            const VirtualObjectDescription& description = family.descriptionOf(shape);
            if (!description.absentResult)
                return isWithin(program, description.type, type);
            return program.at(description.type).withoutNullish().isSubsetOf(program.at(type), program);
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static bool fitsElementTypes(const AllocationSinking& sinking, VirtualObjectID array, const ObjectState& state, TypeID type)
{
    const VirtualFamily& family = sinking.familyOf(array);
    return std::ranges::all_of(state.variants, [&](const ObjectVariant& variant) {
        return isWithin(sinking.program(), type, family.descriptionOf(family.at(variant.shape)).elementType);
    });
}

static bool exceedsLimit(AllocationSinking& sinking, SinkingMode mode, LimitID limit, uint64_t amount, VirtualObjectID object)
{
    if (mode == SinkingMode::Count)
        return sinking.exceeds(limit, amount, sinking.at(object).definition);
    return amount > sinking.limit(limit);
}

static bool isLateEdge(const VirtualObjectContext& context, const VirtualFamily& family, const VirtualField& field)
{
    if (field.kind != VirtualFieldKind::Named && field.kind != VirtualFieldKind::LayoutField)
        return false;
    return std::ranges::all_of(family.descriptions(), [&](const VirtualObjectDescription& description) {
        return !layoutFieldOf(context.program, description, field) || canStoreLate(context, description, field) != LateStore::Unknown;
    });
}

static void canonicalizeVariants(ObjectState& state)
{
    std::ranges::sort(state.variants);
    removeRepeatedElements(state.variants);
    state.normalize();
}

static ValueID emitRangeLength(RewriteCursor& cursor, const Program& program, ValueID start, ValueID end)
{
    constexpr int64_t lengthMask = Range::length().maximum;
    static_assert(!(lengthMask & (lengthMask + 1)), "Masking keeps every length and gives the difference the type of a string length, which the result rule of Sub cannot prove.");

    ValueID difference = cursor.emit(IRBuilder::make(Opcode::Sub, { end, start }));
    if (!program.typeOf(cursor.builder().function(), difference).isInt32())
        difference = cursor.emit(IRBuilder::make(Opcode::ToInt32, { difference }));
    ValueID mask = cursor.builder().appendInteger(lengthMask);
    return cursor.emit(IRBuilder::make(Opcode::BitAnd, { difference, mask }));
}

static ValueID emitValueOrUndefined(AllocationSinking& sinking, ObjectInterpreter& interpreter, VirtualObjectID object, uint32_t field, TypeID type)
{
    std::optional<uint32_t> carriedBits = tagBitsWhenCarried(sinking.familyOf(object), *interpreter.state().find(object), field);
    RELEASE_ASSERT(carriedBits);

    RewriteCursor& cursor = sinking.cursor();
    IRBuilder& builder = cursor.builder();
    ValueID carried = interpreter.valueOfField(object, field);
    ValueID none = builder.appendUndefined();
    ValueID isCarried = interpreter.emitTagTest(object, *carriedBits);
    BlockID continuation = cursor.addUncoveredBlock();
    ValueID result = builder.addParameter(continuation, type);
    builder.emitBranch(isCarried, Target { continuation, { carried }, { } }, Target { continuation, { none }, { } });
    cursor.moveTo(continuation);
    return result;
}

static ValueID emitValueOfEitherField(AllocationSinking& sinking, ObjectInterpreter& interpreter, VirtualObjectID object, uint32_t namedField, uint32_t unprovenField, TypeID type)
{
    std::optional<uint32_t> unprovenBits = tagBitsWhenCarried(sinking.familyOf(object), *interpreter.state().find(object), unprovenField);
    RELEASE_ASSERT(unprovenBits);

    RewriteCursor& cursor = sinking.cursor();
    IRBuilder& builder = cursor.builder();
    ValueID proven = interpreter.valueOfField(object, namedField);
    ValueID unproven = interpreter.valueOfField(object, unprovenField);
    ValueID isUnproven = interpreter.emitTagTest(object, *unprovenBits);
    BlockID continuation = cursor.addUncoveredBlock();
    ValueID result = builder.addParameter(continuation, type);
    builder.emitBranch(isUnproven, Target { continuation, { unproven }, { } }, Target { continuation, { proven }, { } });
    cursor.moveTo(continuation);
    return result;
}

Execution ObjectInterpreter::executeRead(ValueID id)
{
    ValueID receiver = function().at(id).operand(0);
    VirtualObjectID object = objectOf(receiver);
    if (!object)
        return executeOther(id);

    auto executeOnCell = [&] {
        if (hasPositionFromAnsweredTest(id, object))
            m_sinking.abandon(object);
        requireCell(receiver, causeOf(id, 0));
        return executeOther(id);
    };

    const ObjectState& state = *stateOf(object);
    std::optional<VirtualFieldAccess> access = lookUpOnAllVariants(m_sinking, object, state, function().at(id));
    if (!access)
        return executeOnCell();

    const VirtualFamily& family = m_sinking.familyOf(object);
    EnumSet<VirtualObjectKind> kinds = virtualKindsOf(family, state);
    bool hasOneVirtualKind = state.isVirtual() && kinds.hasExactlyOneBitSet();
    if (kinds.containsAny({ VirtualObjectKind::ArgumentList, VirtualObjectKind::Arguments })) {
        if (!hasOneVirtualKind)
            return executeOnCell();
        return kinds.contains(VirtualObjectKind::ArgumentList) ? readArgumentList(id, object, *access) : readArguments(id, object, *access);
    }
    if (access->lookup == FieldLookup::DynamicElement)
        return executeDynamicElementRead(id, object, access->dynamicIndex);

    const Type& type = program().typeOf(function(), id);
    bool readsLength = access->lookup == FieldLookup::Field && access->field.kind == VirtualFieldKind::Length;
    std::optional<uint32_t> arrayLength = readsLength ? commonArrayLengthOf(family, state) : std::nullopt;
    if (arrayLength) {
        if (!Type::integer(Range::one(*arrayLength)).isSubsetOf(type, program()))
            return executeOnCell();
        if (isRewriting())
            replaceRead(id, cursor().builder().appendInteger(*arrayLength));
        return Execution::Interpreted;
    }
    if (readsLength && kinds.contains(VirtualObjectKind::StringRange)) {
        if (!hasOneVirtualKind || !Type::integer(Range::length()).isSubsetOf(type, program()))
            return executeOnCell();
        if (isRewriting()) {
            ValueID start = valueOfField(object, *family.findField(VirtualField::part(VirtualFieldKind::RangeStart)));
            ValueID end = valueOfField(object, *family.findField(VirtualField::part(VirtualFieldKind::RangeEnd)));
            replaceRead(id, emitRangeLength(cursor(), program(), start, end));
        }
        return Execution::Interpreted;
    }

    std::optional<uint32_t> field = access->lookup == FieldLookup::Field ? family.findField(access->field) : std::nullopt;
    switch (field ? unprovenVariantsOf(family, state, *field) : UnprovenVariants::None) {
    case UnprovenVariants::None:
        break;
    case UnprovenVariants::Some:
        if (!canReadEitherField(id, object, *access, *field))
            return executeOnCell();
        if (isRewriting())
            replaceRead(id, emitValueOfEitherField(m_sinking, *this, object, *field, *family.findCounterpart(*field), function().at(id).type));
        return Execution::Interpreted;
    case UnprovenVariants::All:
        if (function().at(id).opcode != Opcode::PropGet)
            return executeOnCell();
        field = family.findCounterpart(*field);
        break;
    }
    FieldRead read = classifyRead(object, *access, field);
    if (!fitsRead(id, object, field, read))
        return executeOnCell();

    switch (read) {
    case FieldRead::Unknown:
        break;
    case FieldRead::HeldObject:
        bind(id, state.objectHeldIn(*field));
        return Execution::Interpreted;
    case FieldRead::Value:
        if (isRewriting())
            replaceRead(id, valueOfField(object, *field));
        return Execution::Interpreted;
    case FieldRead::ValueOrUndefined:
        if (isRewriting())
            replaceRead(id, emitValueOrUndefined(m_sinking, *this, object, *field, function().at(id).type));
        return Execution::Interpreted;
    case FieldRead::Undefined:
        if (isRewriting())
            replaceRead(id, emitUndefined());
        return Execution::Interpreted;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Some variants hold the value in the slot of the field and the others hold one that nothing proves to fit. A read by name takes either.
bool ObjectInterpreter::canReadEitherField(ValueID read, VirtualObjectID object, const VirtualFieldAccess& access, uint32_t namedField) const
{
    const ObjectState& state = *stateOf(object);
    const VirtualFamily& family = m_sinking.familyOf(object);
    uint32_t unprovenField = *family.findCounterpart(namedField);
    if (function().at(read).opcode != Opcode::PropGet || access.whenPresent != PresentField::IsOwnData || function().at(read).type != TypeTable::anything)
        return false;
    if (state.objectHeldIn(namedField) || state.objectHeldIn(unprovenField) || !tagBitsWhenCarried(family, state, unprovenField))
        return false;
    return std::ranges::all_of(state.variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = family.at(variant.shape);
        return hasStored(shape, namedField) || hasStored(shape, unprovenField);
    });
}

FieldRead ObjectInterpreter::classifyRead(VirtualObjectID object, const VirtualFieldAccess& access, std::optional<uint32_t> field) const
{
    const ObjectState& state = *stateOf(object);
    switch (access.lookup) {
    case FieldLookup::Unknown:
    case FieldLookup::DynamicElement:
        return FieldRead::Unknown;
    case FieldLookup::AbsentEverywhere:
        return state.hasCell() ? FieldRead::Unknown : FieldRead::Undefined;
    case FieldLookup::Field:
        break;
    }

    const VirtualFamily& family = m_sinking.familyOf(object);
    bool readsOwnData = access.whenPresent == PresentField::IsOwnData;
    bool findsNothing = access.whenAbsent == AbsentField::FindsNothing;
    bool hasUnknown = false;
    bool hasValue = false;
    bool hasUndefined = false;
    for (const ObjectVariant& variant : state.variants) {
        const VirtualShape& shape = family.at(variant.shape);
        switch (shape.kind) {
        case VirtualShapeKind::Constant:
            hasUnknown = true;
            break;
        case VirtualShapeKind::Cell:
            if (readsOwnData && field && state.fieldsValidInCell.get(*field))
                hasValue = true;
            else
                hasUnknown = true;
            break;
        case VirtualShapeKind::Fields:
            if (field && isStoredIn(family, shape, *field)) {
                hasValue |= readsOwnData;
                hasUnknown |= !readsOwnData;
            } else {
                hasUndefined |= findsNothing;
                hasUnknown |= !findsNothing;
            }
            break;
        }
    }

    if (hasUnknown)
        return FieldRead::Unknown;
    if (!hasValue)
        return FieldRead::Undefined;
    bool isHeld = !!state.objectHeldIn(*field);
    if (!hasUndefined)
        return isHeld ? FieldRead::HeldObject : FieldRead::Value;
    if (isHeld || !tagBitsWhenCarried(family, state, *field))
        return FieldRead::Unknown;
    return FieldRead::ValueOrUndefined;
}

bool ObjectInterpreter::fitsRead(ValueID read, VirtualObjectID object, std::optional<uint32_t> field, FieldRead kind) const
{
    TypeID type = function().at(read).type;
    const ObjectState& state = *stateOf(object);
    switch (kind) {
    case FieldRead::Unknown:
        return false;
    case FieldRead::Undefined:
        return mayBeUndefined(program().at(type));
    case FieldRead::ValueOrUndefined:
        return mayBeUndefined(program().at(type)) && fieldValueFits(m_sinking, object, state, *field, type);
    case FieldRead::Value:
        return fieldValueFits(m_sinking, object, state, *field, type);
    case FieldRead::HeldObject:
        return fitsType(state.objectHeldIn(*field), type);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool ObjectInterpreter::fitsType(VirtualObjectID object, TypeID type) const
{
    const ObjectState* state = stateOf(object);
    RELEASE_ASSERT(state);
    return isAssumedWithin(program(), *state, type) || fitsWithoutCasts(m_sinking, object, *state, type);
}

bool ObjectInterpreter::fitsTypeWithoutCasts(VirtualObjectID object, TypeID type) const
{
    const ObjectState* state = stateOf(object);
    RELEASE_ASSERT(state);
    return fitsWithoutCasts(m_sinking, object, *state, type);
}

// A block parameter that carries the cell of a join has the type of the join.
void ObjectInterpreter::assumeTypeOfAlias(VirtualObjectID object, TypeID type)
{
    if (!isWithin(program(), typeOfCell(m_sinking, object), type))
        stateOf(object)->assume(type);
}

Execution ObjectInterpreter::executeDynamicElementRead(ValueID read, VirtualObjectID object, ValueID index)
{
    ValueID receiver = function().at(read).operand(0);
    TypeID type = function().at(read).type;
    auto takesEveryElement = [&] {
        if (!hasListedElements(object) || !mayBeUndefined(program().at(type)))
            return false;
        const VirtualShape& shape = shapeOf(object, stateOf(object)->variants[0]);
        return isWithin(program(), m_sinking.familyOf(object).descriptionOf(shape).elementType, type);
    };
    if (!takesEveryElement()) {
        requireCell(receiver, causeOf(read, 0));
        return executeOther(read);
    }

    requireCellsOfListedElements(object, causeOf(read, 0));
    if (!isRewriting())
        return Execution::Interpreted;

    ValueID none = emitUndefined();
    Vector<ValueID, 8> elements = listedElementsOf(object);
    if (elements.isEmpty()) {
        replaceRead(read, none);
        return Execution::Interpreted;
    }

    IRBuilder& builder = cursor().builder();
    BlockID continuation = cursor().addUncoveredBlock();
    ValueID result = builder.addParameter(continuation, type);
    Vector<std::pair<int64_t, Target>> cases;
    cases.reserveInitialCapacity(elements.size());
    for (size_t position = 0; position < elements.size(); ++position)
        cases.append({ static_cast<int64_t>(position), Target { continuation, { elements[position] }, { } } });
    builder.emitSwitch(resolve(index), WTF::move(cases), Target { continuation, { none }, { } });
    cursor().moveTo(continuation);
    replaceRead(read, result);
    return Execution::Interpreted;
}

Execution ObjectInterpreter::executeStore(ValueID id)
{
    Opcode opcode = function().at(id).opcode;
    ValueID receiver = function().at(id).operand(0);
    VirtualObjectID object = objectOf(receiver);
    if (!object)
        return executeOther(id);

    auto executeOnCell = [&] {
        if (hasPositionFromAnsweredTest(id, object))
            m_sinking.abandon(object);
        requireCell(receiver, causeOf(id, 0));
        return executeOther(id);
    };
    auto finishStore = [&] {
        if (m_mode == SinkingMode::Count)
            ++m_sinking.at(object).numberOfFoldedStores;
        stateDidChange(object);
        return Execution::Interpreted;
    };

    if (!stateOf(object)->isVirtual())
        return executeOnCell();

    ValueID stored = opcode == Opcode::AppendHole ? ValueID() : function().at(id).valueOperand();
    if (opcode == Opcode::AppendElement || opcode == Opcode::AppendHole) {
        std::optional<uint32_t> length = commonArrayLengthOf(m_sinking.familyOf(object), *stateOf(object));
        if (!length || exceedsLimit(m_sinking, m_mode, LimitID::MaxVirtualArrayLength, static_cast<uint64_t>(*length) + 1, object))
            return executeOnCell();
        if (stored && !fitsElementTypes(m_sinking, object, *stateOf(object), function().at(stored).type))
            return executeOnCell();
        if (!fieldFor(object, VirtualField::element(*length)))
            return executeOnCell();
        if (m_state.isDestructured(object)) {
            m_sinking.abandon(object);
            return executeOnCell();
        }
        appendElement(object, stored);
        return finishStore();
    }

    std::optional<VirtualFieldAccess> access = lookUpOnAllVariants(m_sinking, object, *stateOf(object), function().at(id));
    if (!access || access->lookup != FieldLookup::Field)
        return executeOnCell();
    EnumSet<VirtualObjectKind> kinds = virtualKindsOf(m_sinking.familyOf(object), *stateOf(object));
    if (kinds.contains(VirtualObjectKind::Arguments))
        return kinds.hasExactlyOneBitSet() ? storeArgument(id, object, *access) : executeOnCell();

    std::optional<uint32_t> field = fieldFor(object, access->field);
    if (!field)
        return executeOnCell();
    if (!canStore(object, *access, *field, stored)) {
        if (opcode != Opcode::PropSet || !canStoreUnprovenValue(object, *access, *field))
            return executeOnCell();
        field = fieldFor(object, VirtualField::unprovenNamed(access->field.key));
        if (!field)
            return executeOnCell();
    }
    if (m_state.isDestructured(object)) {
        m_sinking.abandon(object);
        return executeOnCell();
    }

    if (VirtualObjectID held = objectOf(stored); held && !canHold(object, *field, held)) {
        requireCell(stored, BuildCause { .reasons = EscapeReason::StoredInEscapingObject });
        if (!stateOf(object)->isVirtual())
            return executeOnCell();
    }
    storeField(object, *field, stored);
    return finishStore();
}

std::optional<uint32_t> ObjectInterpreter::fieldFor(VirtualObjectID object, const VirtualField& field)
{
    VirtualFamily& family = m_sinking.familyOf(object);
    if (std::optional<uint32_t> existing = family.findField(field))
        return existing;
    if (exceedsLimit(m_sinking, m_mode, LimitID::MaxVirtualObjectFields, static_cast<uint64_t>(family.numberOfFields()) + 1, object))
        return std::nullopt;
    return family.ensureField(field);
}

bool ObjectInterpreter::canStore(VirtualObjectID object, const VirtualFieldAccess& access, uint32_t field, ValueID stored) const
{
    constexpr EnumSet<VirtualObjectKind> writableKinds { VirtualObjectKind::Object, VirtualObjectKind::Environment, VirtualObjectKind::Array, VirtualObjectKind::Arguments };
    const VirtualFamily& family = m_sinking.familyOf(object);
    const VirtualField& virtualField = family.fieldAt(field);
    TypeID storedType = function().at(stored).type;
    return std::ranges::all_of(stateOf(object)->variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = family.at(variant.shape);
        if (shape.kind != VirtualShapeKind::Fields)
            return false;
        const VirtualObjectDescription& description = family.descriptionOf(shape);
        if (!writableKinds.contains(description.kind))
            return false;
        if (description.kind == VirtualObjectKind::Array && virtualField.kind == VirtualFieldKind::Element && virtualField.index >= shape.length)
            return false;
        bool isPresent = hasStored(shape, field) || hasStoredCounterpart(family, shape, field);
        bool runsNoCode = isPresent ? access.whenPresent == PresentField::IsOwnData : access.whenAbsent == AbsentField::FindsNothing;
        return runsNoCode && isWithin(program(), storedType, typeOfField(m_sinking.objectContext(), description, virtualField));
    });
}

// The store by name of a value that the slot may not admit. At run time the field leaves its slot if it has to, and the object stays what it was for everybody who looks by name.
bool ObjectInterpreter::canStoreUnprovenValue(VirtualObjectID object, const VirtualFieldAccess& access, uint32_t namedField) const
{
    const VirtualFamily& family = m_sinking.familyOf(object);
    if (access.whenPresent != PresentField::IsOwnData || family.refusesUnprovenValues() || m_sinking.storesByNameWaitIn(m_state.block))
        return false;
    return std::ranges::all_of(stateOf(object)->variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = family.at(variant.shape);
        if (shape.kind != VirtualShapeKind::Fields || !canHoldUnprovenValue(program(), family.descriptionOf(shape), access.field))
            return false;
        return hasStored(shape, namedField) || hasStoredCounterpart(family, shape, namedField);
    });
}

bool ObjectInterpreter::canHold(VirtualObjectID holder, uint32_t field, VirtualObjectID held) const
{
    if (m_sinking.decisions().holdsCells(holder, field) || m_sinking.familyOf(holder).fieldAt(field).kind == VirtualFieldKind::UnprovenNamed)
        return false;

    const VirtualFamily& heldFamily = m_sinking.familyOf(held);
    bool canBuildHeldObject = std::ranges::all_of(stateOf(held)->variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = heldFamily.at(variant.shape);
        return shape.kind != VirtualShapeKind::Fields || heldFamily.descriptionOf(shape).canBeBuilt;
    });
    if (!canBuildHeldObject)
        return false;

    const VirtualFamily& family = m_sinking.familyOf(holder);
    if (isLateEdge(m_sinking.objectContext(), family, family.fieldAt(field)))
        return true;
    return !m_state.canReach(held, holder);
}

void ObjectInterpreter::storeField(VirtualObjectID object, uint32_t field, ValueID stored)
{
    VirtualObjectID held = objectOf(stored);
    if (held && !canHold(object, field, held)) {
        requireCell(stored, BuildCause { .reasons = EscapeReason::StoredInEscapingObject });
        held = { };
        if (!stateOf(object)->isVirtual()) {
            m_sinking.abandon(object);
            return;
        }
    }

    ObjectState& state = *stateOf(object);
    VirtualFamily& family = m_sinking.familyOf(object);
    if (held)
        state.hold(field, held);
    else
        state.release(field);
    if (std::optional<uint32_t> counterpart = family.findCounterpart(field))
        state.release(*counterpart);
    if (isRewriting())
        values().ensure(object).setField(field, held ? ValueID() : resolve(stored));

    for (ObjectVariant& variant : state.variants) {
        RELEASE_ASSERT(family.at(variant.shape).kind == VirtualShapeKind::Fields);
        VirtualShape next = shapeAfterStore(family, m_sinking.orders(), family.at(variant.shape), field);
        variant.shape = family.intern(WTF::move(next));
    }
    canonicalizeVariants(state);
}

void ObjectInterpreter::appendElement(VirtualObjectID object, ValueID stored)
{
    VirtualFamily& family = m_sinking.familyOf(object);
    ObjectState& state = *stateOf(object);
    std::optional<uint32_t> length = commonArrayLengthOf(family, state);
    if (!length) {
        m_sinking.abandon(object);
        return;
    }

    uint32_t appendedField = 0;
    for (ObjectVariant& variant : state.variants) {
        VirtualShape shape = family.at(variant.shape);
        variant.shape = family.intern(shapeAfterAppend(family, shape, !stored, appendedField));
    }
    canonicalizeVariants(state);

    if (isRewriting()) {
        ValueID newLength = cursor().builder().appendInteger(static_cast<int64_t>(*length) + 1);
        values().ensure(object).setField(family.ensureField(VirtualField::part(VirtualFieldKind::Length)), newLength);
    }
    if (stored)
        storeField(object, appendedField, stored);
}

void ObjectInterpreter::replaceRead(ValueID read, ValueID replacement)
{
    if (!isRewriting())
        return;
    m_sinking.tables().replacedReads.append({ read, replacement });
    cursor().keep(read);
}

ValueID ObjectInterpreter::valueOfField(VirtualObjectID object, uint32_t field)
{
    RELEASE_ASSERT(isRewriting());
    const ObjectValues* objectValues = values().find(object);
    RELEASE_ASSERT(objectValues);
    if (ValueID value = objectValues->field(field))
        return value;

    VirtualObjectID held = stateOf(object)->objectHeldIn(field);
    RELEASE_ASSERT(held);
    return cellOf(held);
}

ValueID ObjectInterpreter::emitUndefined()
{
    return cursor().emit(Value(Opcode::Undefined));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
