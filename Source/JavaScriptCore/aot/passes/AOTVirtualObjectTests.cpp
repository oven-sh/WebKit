/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVirtualObjectShapes.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static KnownValue knownValueOf(VirtualConstant constant)
{
    switch (constant) {
    case VirtualConstant::Undefined:
        return KnownValue::undefined();
    case VirtualConstant::Null:
        return KnownValue::null();
    case VirtualConstant::False:
        return KnownValue::boolean(false);
    case VirtualConstant::True:
        return KnownValue::boolean(true);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static KnownValue knownValueOf(const VirtualObjectDescription& description)
{
    switch (description.kind) {
    case VirtualObjectKind::Closure:
        return KnownValue::function();
    case VirtualObjectKind::Object:
    case VirtualObjectKind::Array:
    case VirtualObjectKind::ArgumentList:
    case VirtualObjectKind::Arguments:
    case VirtualObjectKind::BuiltinFields:
    case VirtualObjectKind::BuiltinPrefix:
        return KnownValue::object();
    case VirtualObjectKind::Opaque:
    case VirtualObjectKind::Environment:
    case VirtualObjectKind::StringRange:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isLayoutObject(const VirtualObjectDescription& description)
{
    return description.kind == VirtualObjectKind::Object || description.kind == VirtualObjectKind::BuiltinFields;
}

static bool isArray(const VirtualObjectDescription& description)
{
    return description.kind == VirtualObjectKind::Array || description.kind == VirtualObjectKind::ArgumentList || description.kind == VirtualObjectKind::BuiltinPrefix;
}

static KnownBoolean evaluateTypeTestOfFields(const Program& program, const VirtualObjectDescription& description, const Type& tested)
{
    const Type& type = program.at(description.type);
    KnownBoolean result = description.absentResult ? evaluateTypeTest(program, type.withoutNullish(), tested) : evaluateTypeTest(program, type, tested);
    if (result != KnownBoolean::Unknown)
        return result;
    return evaluateTypeTest(program, knownValueOf(description), tested);
}

static KnownBoolean evaluateTypeTestOfShape(const ShapeContext& context, const VirtualShape& shape, const Type& tested)
{
    switch (shape.kind) {
    case VirtualShapeKind::Cell:
        return KnownBoolean::Unknown;
    case VirtualShapeKind::Constant:
        return evaluateTypeTest(context.objects.program, knownValueOf(shape.constant), tested);
    case VirtualShapeKind::Fields:
        return evaluateTypeTestOfFields(context.objects.program, context.family.descriptionOf(shape), tested);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static KnownBoolean hasLayoutIn(const Program& program, const VirtualObjectDescription& description, LayoutSetID layouts)
{
    if (description.layout)
        return knownBoolean(std::ranges::binary_search(program.at(layouts).members, description.layout));
    if (description.kind != VirtualObjectKind::Opaque)
        return KnownBoolean::False;
    const Type& type = program.at(description.type);
    return type.anyObject || type.layouts ? KnownBoolean::Unknown : KnownBoolean::False;
}

static KnownBoolean evaluateFieldTest(const ShapeContext& context, const VirtualShape& shape, const Value& test)
{
    VirtualFieldAccess access = lookUpField(context.objects, context.family.descriptionOf(shape), test);
    switch (access.lookup) {
    case FieldLookup::Unknown:
    case FieldLookup::DynamicElement:
        return KnownBoolean::Unknown;
    case FieldLookup::AbsentEverywhere:
        return KnownBoolean::False;
    case FieldLookup::Field:
        break;
    }
    std::optional<uint32_t> field = context.family.findField(access.field);
    bool isPastEnd = access.field.kind == VirtualFieldKind::Element && access.field.index >= shape.length;
    if (field && hasStored(shape, *field) && !isPastEnd)
        return access.whenPresent == PresentField::IsOwnData ? KnownBoolean::True : KnownBoolean::Unknown;
    return access.whenAbsent == AbsentField::FindsNothing ? KnownBoolean::False : KnownBoolean::Unknown;
}

static KnownBoolean evaluateKeyOrderTest(const ShapeContext& context, const VirtualShape& shape, const Value& test)
{
    const VirtualObjectDescription& description = context.family.descriptionOf(shape);
    if (!description.hasKeyOrder)
        return KnownBoolean::Unknown;
    if (description.layout != test.layout())
        return KnownBoolean::False;
    Vector<uint32_t, 8> order = context.orders.fieldsOf(shape.order);
    std::span<const uint32_t> testedOrder = test.fields();
    if (order.size() != testedOrder.size())
        return KnownBoolean::False;
    for (size_t position = 0; position < order.size(); ++position) {
        std::optional<uint32_t> layoutField = layoutFieldOf(context.objects.program, description, context.family.fieldAt(order[position]));
        if (!layoutField)
            return KnownBoolean::Unknown;
        if (*layoutField != testedOrder[position])
            return KnownBoolean::False;
    }
    return KnownBoolean::True;
}

static KnownBoolean evaluatePrototypeTest(const ShapeContext& context, const VirtualShape& shape)
{
    if (!isLayoutObject(context.family.descriptionOf(shape)))
        return KnownBoolean::Unknown;
    std::optional<uint32_t> prototype = context.family.findField(VirtualField::part(VirtualFieldKind::Prototype));
    return prototype && hasStored(shape, *prototype) ? KnownBoolean::Unknown : KnownBoolean::True;
}

static bool isSubjectOperand(Opcode opcode, uint32_t operandIndex)
{
    switch (opcode) {
    case Opcode::ToBoolean:
        return operandIndex == OperandIndex::ToBoolean::value;
    case Opcode::TypeTest:
        return operandIndex == OperandIndex::TypeTest::value;
    case Opcode::StrictEq:
        return operandIndex == OperandIndex::StrictEq::left || operandIndex == OperandIndex::StrictEq::right;
    case Opcode::ShapeTest:
        return operandIndex == OperandIndex::ShapeTest::object;
    case Opcode::IsSlotFilled:
        return operandIndex == OperandIndex::IsSlotFilled::object;
    case Opcode::PropHasOwn:
        return operandIndex == OperandIndex::PropHasOwn::object;
    case Opcode::PrivateHas:
        return operandIndex == OperandIndex::PrivateHas::object;
    case Opcode::PropHas:
        return operandIndex == OperandIndex::PropHas::object;
    case Opcode::ElemHas:
        return operandIndex == OperandIndex::ElemHas::object;
    case Opcode::HasArrayElement:
        return operandIndex == OperandIndex::HasArrayElement::array;
    case Opcode::HasArrayStorage:
        return operandIndex == OperandIndex::HasArrayStorage::array;
    case Opcode::IsInBounds:
        return operandIndex == OperandIndex::IsInBounds::array;
    case Opcode::HasNoExtraProperties:
        return operandIndex == OperandIndex::HasNoExtraProperties::object;
    case Opcode::IsNotFrozen:
        return operandIndex == OperandIndex::IsNotFrozen::object;
    case Opcode::HasLayoutPrototype:
        return operandIndex == OperandIndex::HasLayoutPrototype::object;
    case Opcode::BelongsToInstance:
        return operandIndex == OperandIndex::BelongsToInstance::object;
    case Opcode::IsPristine:
        return operandIndex == OperandIndex::IsPristine::value;
    case Opcode::HasKeyOrder:
        return operandIndex == OperandIndex::HasKeyOrder::object;
    case Opcode::IsShapeEpochCurrent:
        return operandIndex == OperandIndex::IsShapeEpochCurrent::guard;
    case Opcode::IsClosureOf:
        return operandIndex == OperandIndex::IsClosureOf::value;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static KnownBoolean evaluateTestOfConstant(const ShapeContext& context, VirtualConstant constant, const Value& test, uint32_t operandIndex)
{
    const Program& program = context.objects.program;
    KnownValue known = knownValueOf(constant);
    switch (test.opcode) {
    case Opcode::ToBoolean:
        return known.toBoolean(program);
    case Opcode::TypeTest:
        return evaluateTypeTest(program, known, program.at(test.testedType()));
    case Opcode::StrictEq: {
        // The sinker asks again while it rewrites the function, after it has given the casts it keeps new operands. Constants and
        // types are what it leaves alone, so they give one answer every time.
        const Function& function = context.objects.function;
        ValueID other = operandIndex == OperandIndex::StrictEq::left ? test.rightOperand() : test.leftOperand();
        if (std::optional<VirtualConstant> otherConstant = virtualConstantOf(program, function, other))
            return knownBoolean(constant == *otherConstant);
        return program.typeOf(function, other).overlaps(program.at(typeOf(constant)), program) ? KnownBoolean::Unknown : KnownBoolean::False;
    }
    case Opcode::ShapeTest:
    case Opcode::IsPristine:
    case Opcode::IsClosureOf:
        return KnownBoolean::False;
    case Opcode::IsSlotFilled:
    case Opcode::PropHasOwn:
    case Opcode::PrivateHas:
    case Opcode::PropHas:
    case Opcode::ElemHas:
    case Opcode::HasArrayElement:
    case Opcode::HasArrayStorage:
    case Opcode::IsInBounds:
    case Opcode::HasNoExtraProperties:
    case Opcode::IsNotFrozen:
    case Opcode::HasLayoutPrototype:
    case Opcode::BelongsToInstance:
    case Opcode::HasKeyOrder:
    case Opcode::IsShapeEpochCurrent:
        return KnownBoolean::Unknown;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static ArrayLiteralParts partsOfLiteral(const VirtualFamily& family, const VirtualShape& shape)
{
    auto isStored = [&](const VirtualField& field) {
        std::optional<uint32_t> index = family.findField(field);
        return index && hasStored(shape, *index);
    };

    ArrayLiteralParts parts;
    for (uint32_t position = 0; position < shape.length; ++position)
        (isStored(VirtualField::element(position)) ? parts.hasElements : parts.hasHoles) = true;
    parts.hasCapacity = isStored(VirtualField::part(VirtualFieldKind::Capacity));
    return parts;
}

static KnownBoolean evaluateArrayStorageTest(const ShapeContext& context, const VirtualShape& shape, const Value& test)
{
    const VirtualObjectDescription& description = context.family.descriptionOf(shape);
    if (!isArray(description))
        return KnownBoolean::Unknown;
    // An array that is never built has no storage for anybody to see, so the test goes the way it was expected to.
    if (!description.canBeBuilt)
        return KnownBoolean::True;
    if (description.kind != VirtualObjectKind::Array)
        return KnownBoolean::Unknown;

    // The array is built as the literal of a later shape: a store that leaves the proof of the test alone may have filled a hole or added an element by then.
    // A literal whose contents ask for Contiguous has that kind with any elements, and one that has an element keeps one.
    const Program& program = context.objects.program;
    const Type& array = program.at(description.type);
    if (arrayStorageAtAllocation(program, array) != ArrayStorage::Contiguous)
        return KnownBoolean::Unknown;
    ArrayStorage kind = arrayStorageOfLiteral(program, array, partsOfLiteral(context.family, shape));
    return kind == ArrayStorage::Other ? KnownBoolean::Unknown : knownBoolean(kind == test.arrayStorage());
}

static KnownBoolean evaluateBoundsTest(const ShapeContext& context, const VirtualShape& shape, const Value& test)
{
    if (!isArray(context.family.descriptionOf(shape)))
        return KnownBoolean::Unknown;
    KnownValue position = knownValueOf(context.objects.program, context.objects.function, test.positionOperand());
    if (position.kind() != KnownValue::Kind::Integer)
        return KnownBoolean::Unknown;
    return knownBoolean(position.asInteger() >= 0 && position.asInteger() < static_cast<int64_t>(shape.length));
}

static KnownBoolean evaluateTestOfFields(const ShapeContext& context, const VirtualShape& shape, const Value& test)
{
    const Program& program = context.objects.program;
    const VirtualObjectDescription& description = context.family.descriptionOf(shape);
    switch (test.opcode) {
    case Opcode::ToBoolean:
        return description.kind == VirtualObjectKind::StringRange ? KnownBoolean::Unknown : KnownBoolean::True;
    case Opcode::TypeTest:
        return evaluateTypeTestOfFields(program, description, program.at(test.testedType()));
    case Opcode::StrictEq:
        return description.hasIdentity ? KnownBoolean::False : KnownBoolean::Unknown;
    case Opcode::ShapeTest:
        return hasLayoutIn(program, description, test.layouts());
    case Opcode::IsSlotFilled:
    case Opcode::PropHasOwn:
    case Opcode::PrivateHas:
    case Opcode::PropHas:
    case Opcode::ElemHas:
    case Opcode::HasArrayElement:
        return evaluateFieldTest(context, shape, test);
    case Opcode::HasArrayStorage:
        return evaluateArrayStorageTest(context, shape, test);
    case Opcode::IsInBounds:
        return evaluateBoundsTest(context, shape, test);
    case Opcode::HasNoExtraProperties:
    case Opcode::IsNotFrozen:
    case Opcode::BelongsToInstance:
        return isLayoutObject(description) ? KnownBoolean::True : KnownBoolean::Unknown;
    case Opcode::HasLayoutPrototype:
        return evaluatePrototypeTest(context, shape);
    case Opcode::IsPristine:
        return isArray(description) && test.builtinClass() == BuiltinClasses::Array ? KnownBoolean::True : KnownBoolean::Unknown;
    case Opcode::HasKeyOrder:
        return evaluateKeyOrderTest(context, shape, test);
    case Opcode::IsShapeEpochCurrent:
        return KnownBoolean::True;
    case Opcode::IsClosureOf:
        return knownBoolean(description.kind == VirtualObjectKind::Closure && description.function == test.function());
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

KnownBoolean evaluateTest(const ShapeContext& context, const VirtualShape& shape, const Value& test, uint32_t operandIndex)
{
    if (testsLeftToRun.contains(test.opcode) || !isSubjectOperand(test.opcode, operandIndex))
        return KnownBoolean::Unknown;
    switch (shape.kind) {
    case VirtualShapeKind::Cell:
        return KnownBoolean::Unknown;
    case VirtualShapeKind::Constant:
        return evaluateTestOfConstant(context, shape.constant, test, operandIndex);
    case VirtualShapeKind::Fields:
        return evaluateTestOfFields(context, shape, test);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

KnownBoolean evaluateTestOfTwoShapes(const ShapeContext& context, const VirtualShape& left, const VirtualShape& right, bool areSameObject, const Value& test)
{
    if (test.opcode != Opcode::StrictEq)
        return KnownBoolean::Unknown;
    if (left.kind == VirtualShapeKind::Cell || right.kind == VirtualShapeKind::Cell)
        return KnownBoolean::Unknown;
    if (left.kind == VirtualShapeKind::Constant && right.kind == VirtualShapeKind::Constant)
        return knownBoolean(left.constant == right.constant);
    if (left.kind == VirtualShapeKind::Constant || right.kind == VirtualShapeKind::Constant)
        return KnownBoolean::False;
    if (!context.family.descriptionOf(left).hasIdentity)
        return KnownBoolean::Unknown;
    return knownBoolean(areSameObject);
}

static CastOutcome castOutcomeOf(KnownBoolean isWithinCastType)
{
    switch (isWithinCastType) {
    case KnownBoolean::Unknown:
        return CastOutcome::Unknown;
    case KnownBoolean::False:
        return CastOutcome::Throws;
    case KnownBoolean::True:
        return CastOutcome::Passes;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isNullish(const VirtualShape& shape)
{
    return shape.kind == VirtualShapeKind::Constant && (shape.constant == VirtualConstant::Undefined || shape.constant == VirtualConstant::Null);
}

CastOutcome evaluateCast(const ShapeContext& context, const VirtualShape& shape, const Value& cast)
{
    if (shape.kind == VirtualShapeKind::Cell)
        return CastOutcome::Unknown;
    switch (cast.opcode) {
    case Opcode::Upcast:
    case Opcode::AssumeType:
    case Opcode::Predict:
    case Opcode::FinishObject:
    case Opcode::CheckInitialized:
        return CastOutcome::Passes;
    case Opcode::Claim:
        return castOutcomeOf(evaluateTypeTestOfShape(context, shape, context.objects.program.at(cast.castType())));
    case Opcode::RequireObjectCoercible:
        return isNullish(shape) ? CastOutcome::Throws : CastOutcome::Passes;
    case Opcode::RequireObject:
        return shape.kind == VirtualShapeKind::Fields && context.family.descriptionOf(shape).hasIdentity ? CastOutcome::Passes : CastOutcome::Throws;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

std::optional<uint32_t> findShapeCase(const ShapeContext& context, const VirtualShape& shape, const Terminator& switchOnShape)
{
    std::span<const LayoutSetID> layoutSets = switchOnShape.layoutSets();
    auto otherwise = static_cast<uint32_t>(layoutSets.size());
    switch (shape.kind) {
    case VirtualShapeKind::Cell:
        return std::nullopt;
    case VirtualShapeKind::Constant:
        return otherwise;
    case VirtualShapeKind::Fields:
        break;
    }
    const VirtualObjectDescription& description = context.family.descriptionOf(shape);
    for (uint32_t index = 0; index < otherwise; ++index) {
        switch (hasLayoutIn(context.objects.program, description, layoutSets[index])) {
        case KnownBoolean::Unknown:
            return std::nullopt;
        case KnownBoolean::False:
            continue;
        case KnownBoolean::True:
            return index;
        }
    }
    return otherwise;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
