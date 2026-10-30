/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVirtualObjectShapes.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTMemoryLocation.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/CommaPrinter.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

KeyOrderTable::KeyOrderTable()
{
    m_nodes.append(Node { });
}

KeyOrderID KeyOrderTable::append(KeyOrderID order, uint32_t field)
{
    uint64_t key = static_cast<uint64_t>(order.index()) << 32 | field;
    return m_children.ensure(key, [&] {
        KeyOrderID child(m_nodes.size());
        m_nodes.append(Node { order, field, lengthOf(order) + 1 });
        return child;
    }).iterator->value;
}

Vector<uint32_t, 8> KeyOrderTable::fieldsOf(KeyOrderID order) const
{
    Vector<uint32_t, 8> result(lengthOf(order));
    size_t position = result.size();
    while (order != emptyKeyOrder) {
        const Node& node = m_nodes[order.index()];
        result[--position] = node.field;
        order = node.parent;
    }
    return result;
}

VirtualFamily::VirtualFamily()
{
    m_shapes.append(VirtualShape { });
}

uint32_t VirtualFamily::addDescription(const VirtualObjectDescription& description)
{
    auto index = static_cast<uint32_t>(m_descriptions.size());
    m_descriptions.append(description);
    return index;
}

uint32_t VirtualFamily::ensureField(const VirtualField& field)
{
    if (std::optional<uint32_t> existing = findField(field))
        return *existing;
    auto number = static_cast<uint32_t>(m_fields.size());
    m_fields.append(field);
    m_hasUnprovenFields |= field.kind == VirtualFieldKind::UnprovenNamed;
    return number;
}

std::optional<uint32_t> VirtualFamily::findField(const VirtualField& field) const
{
    size_t number = m_fields.find(field);
    if (number == notFound)
        return std::nullopt;
    return static_cast<uint32_t>(number);
}

std::optional<uint32_t> VirtualFamily::findCounterpart(uint32_t field) const
{
    if (!m_hasUnprovenFields)
        return std::nullopt;
    const VirtualField& known = m_fields[field];
    switch (known.kind) {
    case VirtualFieldKind::Named:
        return findField(VirtualField::unprovenNamed(known.key));
    case VirtualFieldKind::UnprovenNamed:
        return findField(VirtualField::named(known.key));
    case VirtualFieldKind::LayoutField:
    case VirtualFieldKind::Element:
    case VirtualFieldKind::Capture:
    case VirtualFieldKind::Length:
    case VirtualFieldKind::Capacity:
    case VirtualFieldKind::Prototype:
    case VirtualFieldKind::Callee:
    case VirtualFieldKind::ArgumentCount:
    case VirtualFieldKind::RestList:
    case VirtualFieldKind::StringBase:
    case VirtualFieldKind::RangeStart:
    case VirtualFieldKind::RangeEnd:
    case VirtualFieldKind::Operand:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

VirtualShapeID VirtualFamily::intern(VirtualShape&& shape)
{
    size_t existing = m_shapes.find(shape);
    if (existing != notFound)
        return VirtualShapeID(existing);
    VirtualShapeID id(m_shapes.size());
    m_shapes.append(WTF::move(shape));
    return id;
}

static uint64_t tagBitKey(BlockID block, VirtualShapeID shape)
{
    return static_cast<uint64_t>(block.index()) << 32 | shape.index();
}

std::optional<uint8_t> VirtualFamily::tagBitFor(BlockID block, VirtualShapeID shape, uint64_t maxTagBits)
{
    if (std::optional<uint8_t> existing = findTagBit(block, shape))
        return existing;
    if (m_numberOfTagBits >= std::min<uint64_t>(maxTagBits, maxTagBitsInInt32))
        return std::nullopt;
    auto tagBit = static_cast<uint8_t>(m_numberOfTagBits++);
    m_tagBits.add(tagBitKey(block, shape), tagBit);
    return tagBit;
}

std::optional<uint8_t> VirtualFamily::findTagBit(BlockID block, VirtualShapeID shape) const
{
    return m_tagBits.getOptional(tagBitKey(block, shape));
}

void VirtualFamily::shareTagBit(BlockID block, VirtualShapeID shape, uint8_t existingTagBit)
{
    RELEASE_ASSERT(existingTagBit < m_numberOfTagBits);
    bool isNewEntry = m_tagBits.add(tagBitKey(block, shape), existingTagBit).isNewEntry;
    RELEASE_ASSERT(isNewEntry);
}

std::optional<uint8_t> VirtualFamily::reserveConsecutiveTagBits(unsigned count, uint64_t maxTagBits)
{
    RELEASE_ASSERT(m_numberOfTagBits == m_numberOfReservedTagBits);
    if (static_cast<uint64_t>(m_numberOfTagBits) + count > std::min<uint64_t>(maxTagBits, maxTagBitsInInt32))
        return std::nullopt;
    auto first = static_cast<uint8_t>(m_numberOfTagBits);
    m_numberOfTagBits += count;
    m_numberOfReservedTagBits += count;
    return first;
}

void VirtualFamily::forgetTagBits()
{
    m_tagBits.clear();
    m_numberOfTagBits = m_numberOfReservedTagBits;
}

TypeID typeOfShape(const ShapeContext& context, const VirtualShape& shape)
{
    switch (shape.kind) {
    case VirtualShapeKind::Cell:
        return { };
    case VirtualShapeKind::Constant:
        return typeOf(shape.constant);
    case VirtualShapeKind::Fields:
        return context.family.descriptionOf(shape).type;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool hasStored(const VirtualShape& shape, uint32_t field)
{
    return shape.kind == VirtualShapeKind::Fields && shape.stored.get(field);
}

bool hasStoredCounterpart(const VirtualFamily& family, const VirtualShape& shape, uint32_t field)
{
    std::optional<uint32_t> counterpart = family.findCounterpart(field);
    return counterpart && hasStored(shape, *counterpart);
}

bool hasUnprovenField(const VirtualFamily& family, const VirtualShape& shape)
{
    if (shape.kind != VirtualShapeKind::Fields || !family.hasUnprovenFields())
        return false;
    for (size_t field : shape.stored) {
        if (family.fieldAt(static_cast<uint32_t>(field)).kind == VirtualFieldKind::UnprovenNamed)
            return true;
    }
    return false;
}

// The order of the keys lists the Named field of a key, whichever of the two is stored.
static void addStoredField(const VirtualFamily& family, KeyOrderTable& orders, VirtualShape& shape, uint32_t field)
{
    bool wasStored = shape.stored.set(field);
    if (wasStored)
        return;
    std::optional<uint32_t> counterpart = family.findCounterpart(field);
    if (counterpart && shape.stored.get(*counterpart)) {
        shape.stored.clear(*counterpart);
        return;
    }
    if (!family.descriptionOf(shape).hasKeyOrder || !family.fieldAt(field).isProperty())
        return;
    bool isUnproven = family.fieldAt(field).kind == VirtualFieldKind::UnprovenNamed;
    RELEASE_ASSERT(!isUnproven || counterpart);
    shape.order = orders.append(shape.order, isUnproven ? *counterpart : field);
}

VirtualShape shapeAtAllocation(const VirtualObjectContext& context, VirtualFamily& family, KeyOrderTable& orders, uint32_t descriptionIndex)
{
    const VirtualObjectDescription& description = family.descriptionAt(descriptionIndex);
    VirtualShape shape;
    shape.kind = VirtualShapeKind::Fields;
    shape.descriptionIndex = descriptionIndex;
    shape.length = initialLengthOf(context.function, description);
    forEachInitialField(context, description, [&](const VirtualField& initialField, ValueID) {
        addStoredField(family, orders, shape, family.ensureField(initialField));
    });
    if (description.kind == VirtualObjectKind::Array)
        addStoredField(family, orders, shape, family.ensureField(VirtualField::part(VirtualFieldKind::Length)));
    return shape;
}

VirtualShape shapeAfterStore(const VirtualFamily& family, KeyOrderTable& orders, const VirtualShape& shape, uint32_t field)
{
    RELEASE_ASSERT(shape.kind == VirtualShapeKind::Fields);
    ASSERT(family.descriptionOf(shape).kind != VirtualObjectKind::Array || family.fieldAt(field).kind != VirtualFieldKind::Element || family.fieldAt(field).index < shape.length);
    VirtualShape result = shape;
    addStoredField(family, orders, result, field);
    return result;
}

VirtualShape shapeAfterDelete(const VirtualFamily& family, KeyOrderTable& orders, const VirtualShape& shape, uint32_t namedField)
{
    RELEASE_ASSERT(shape.kind == VirtualShapeKind::Fields && family.fieldAt(namedField).kind == VirtualFieldKind::Named);
    VirtualShape result = shape;
    bool wasStored = result.stored.clear(namedField);
    if (std::optional<uint32_t> counterpart = family.findCounterpart(namedField))
        wasStored |= result.stored.clear(*counterpart);
    if (!wasStored || !family.descriptionOf(shape).hasKeyOrder)
        return result;
    result.order = emptyKeyOrder;
    for (uint32_t field : orders.fieldsOf(shape.order)) {
        if (field != namedField)
            result.order = orders.append(result.order, field);
    }
    return result;
}

VirtualShape shapeAfterAppend(VirtualFamily& family, const VirtualShape& shape, bool isHole, uint32_t& appendedField)
{
    RELEASE_ASSERT(shape.kind == VirtualShapeKind::Fields);
    RELEASE_ASSERT(family.descriptionOf(shape).kind == VirtualObjectKind::Array);
    appendedField = family.ensureField(VirtualField::element(shape.length));
    VirtualShape result = shape;
    ++result.length;
    if (!isHole)
        result.stored.set(appendedField);
    return result;
}

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
    case VirtualObjectKind::ViewRange:
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
    // What the built-in would make is an ordinary object of the engine: of no layout of the program, of no class, no function.
    if (description.kind == VirtualObjectKind::BuiltinFields && !tested.anyObject)
        return KnownBoolean::False;
    return evaluateTypeTest(program, knownValueOf(description), tested);
}

KnownBoolean evaluateTypeTestOfShape(const ShapeContext& context, const VirtualShape& shape, const Type& tested)
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
        return knownBoolean(std::ranges::binary_search(program.at(layouts).completeMembers(), description.layout));
    if (description.kind != VirtualObjectKind::Opaque)
        return KnownBoolean::False;
    const Type& type = program.at(description.type);
    return type.anyObject || type.layouts ? KnownBoolean::Unknown : KnownBoolean::False;
}

FieldContents contentsOf(const ShapeContext& context, const VirtualObjectDescription& description, uint32_t field)
{
    const VirtualField& virtualField = context.family.fieldAt(field);
    return { typeOfField(context.objects, description, virtualField), context.family.mayHoldUndefinedOrNull(field) && holdsUndefinedAndNullToo(context.objects, description, virtualField) };
}

bool isWithin(const Program& program, FieldContents contents, TypeID bound)
{
    if (contents.mayAlsoBeUndefinedOrNull && !program.at(TypeTable::nullish).isSubsetOf(program.at(bound), program))
        return false;
    return contents.type == bound || program.at(contents.type).isSubsetOf(program.at(bound), program);
}

bool mayBeUndefinedOrNull(const Program& program, FieldContents contents)
{
    return contents.mayAlsoBeUndefinedOrNull || program.at(contents.type).mayBeUndefinedOrNull();
}

TypeID typeOf(FieldContents contents, TypeArena& types)
{
    return contents.mayAlsoBeUndefinedOrNull && contents.type != TypeTable::never ? types.unionOf(contents.type, TypeTable::nullish) : contents.type;
}

static bool rejectsNullishValues(const Value& test)
{
    return test.opcode == Opcode::IsSlotFilled && test.holdsDeclaredType();
}

static bool staysInSlot(const Program& program, const VirtualObjectDescription& description, const VirtualField& field)
{
    std::optional<uint32_t> layoutField = layoutFieldOf(program, description, field);
    return layoutField && staysInSlot(program, { description.layout, *layoutField });
}

static bool hasUnprovenFieldThatMayLeaveSlot(const ShapeContext& context, const VirtualShape& shape)
{
    if (!hasUnprovenField(context.family, shape))
        return false;
    const VirtualObjectDescription& description = context.family.descriptionOf(shape);
    for (size_t index : shape.stored) {
        const VirtualField& field = context.family.fieldAt(static_cast<uint32_t>(index));
        if (field.kind == VirtualFieldKind::UnprovenNamed && !staysInSlot(context.objects.program, description, field))
            return true;
    }
    return false;
}

static KnownBoolean evaluateFieldTest(const ShapeContext& context, const VirtualShape& shape, const Value& test)
{
    const VirtualObjectDescription& description = context.family.descriptionOf(shape);
    VirtualFieldAccess access = lookUpField(context.objects, description, test);
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
    if (field && hasStored(shape, *field) && !isPastEnd) {
        if (access.whenPresent != PresentField::IsOwnData)
            return KnownBoolean::Unknown;
        if (rejectsNullishValues(test) && mayBeUndefinedOrNull(context.objects.program, contentsOf(context, description, *field)))
            return KnownBoolean::Unknown;
        return KnownBoolean::True;
    }
    if (field && hasStoredCounterpart(context.family, shape, *field)) {
        if (access.whenPresent != PresentField::IsOwnData)
            return KnownBoolean::Unknown;
        if (test.opcode != Opcode::IsSlotFilled)
            return KnownBoolean::True;
        // A failed test of the declared type is followed by a lookup by name. A failed test of a field that may leave its slot says nothing without HasNoFieldOutsideSlot.
        if (rejectsNullishValues(test))
            return KnownBoolean::False;
        return knownBoolean(staysInSlot(context.objects.program, description, access.field));
    }
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

static bool takesPrototypeOfConstructorFunction(const Program& program, LayoutID declared)
{
    const Layout& layout = program.at(declared);
    return layout.has(LayoutFlags::IsConstructorFunction) && layout.classFlow.prototypeProperty != MemberMutability::Fixed;
}

bool creationMayBeOrderedWithGuards(const VirtualObjectContext& objects, const VirtualObjectDescription& description)
{
    const Value& allocation = objects.function.at(description.allocation);
    if (allocation.opcode != Opcode::NewObject)
        return false;
    return allocation.prototypeOperand() || takesPrototypeOfConstructorFunction(objects.program, declaredLayoutOf(objects.program, allocation.layout()));
}

GuardsOrderedWithCreation guardsOrderedWithCreation(const ShapeContext& context, const VirtualShape& shape)
{
    if (shape.kind != VirtualShapeKind::Fields)
        return { };
    const Program& program = context.objects.program;
    const VirtualObjectDescription& description = context.family.descriptionOf(shape);
    if (!isLayoutObject(description)) {
        // The prototype decides which class the object belongs to.
        bool isGivenPrototype = description.kind == VirtualObjectKind::Opaque && creationMayBeOrderedWithGuards(context.objects, description);
        return isGivenPrototype ? GuardsOrderedWithCreation { LocationMasks::entireHeap, true } : GuardsOrderedWithCreation { };
    }
    LayoutID declared = declaredLayoutOf(program, description.layout);
    if (evaluatePrototypeTest(context, shape) != KnownBoolean::True)
        return { maskOf(program, MemoryLocation::memberGuards(declared)), true };
    if (takesPrototypeOfConstructorFunction(program, declared))
        return { maskOf(program, MemoryLocation::memberGuards(declared)), false };
    return { };
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
    case Opcode::IsElementPresent:
        return operandIndex == OperandIndex::IsElementPresent::array;
    case Opcode::HasArrayStorage:
        return operandIndex == OperandIndex::HasArrayStorage::array;
    case Opcode::IsInBounds:
        return operandIndex == OperandIndex::IsInBounds::array;
    case Opcode::HasNoExtraProperties:
        return operandIndex == OperandIndex::HasNoExtraProperties::object;
    case Opcode::HasNoFieldOutsideSlot:
        return operandIndex == OperandIndex::HasNoFieldOutsideSlot::object;
    case Opcode::HasBirthFieldsFilled:
        return operandIndex == OperandIndex::HasBirthFieldsFilled::object;
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
        // The sinker asks again while it rewrites the function, after it has given the casts it keeps new operands.
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
    case Opcode::IsElementPresent:
    case Opcode::HasArrayStorage:
    case Opcode::IsInBounds:
    case Opcode::HasNoExtraProperties:
    case Opcode::HasNoFieldOutsideSlot:
    case Opcode::HasBirthFieldsFilled:
    case Opcode::IsNotFrozen:
    case Opcode::HasLayoutPrototype:
    case Opcode::BelongsToInstance:
    case Opcode::HasKeyOrder:
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

Type typeOfNumbersIn(ArrayStorage kind)
{
    return typeOfElementRead(kind).withoutPrimitives(Primitive::Undefined);
}

ListOfNumbers classifyListOfNumbers(const Program& program, const VirtualObjectDescription& description)
{
    if (description.kind != VirtualObjectKind::Array || !description.canBeBuilt)
        return ListOfNumbers::No;
    ArrayStorage expected = arrayStorageAtAllocation(program, program.at(description.type));
    if (expected == ArrayStorage::Contiguous)
        return ListOfNumbers::No;
    return fitsStorageOfLiteral(expected, program.at(description.elementType)) ? ListOfNumbers::FitsByType : ListOfNumbers::MayNotFit;
}

// Nobody sees the storage of an array that is not there, and what is read from it is what was stored, so the test goes the way it was expected to.
// The answer says nothing of the array once it is built: a NaN among the numbers is found when the literal runs. ObjectInterpreter::reliesOnAnswerThatCellDoesNotKeep() sees to that.
static KnownBoolean evaluateStorageTestOfNumbers(const Program& program, const VirtualObjectDescription& description, ArrayLiteralParts parts, const Value& test)
{
    if (!parts.hasElements || parts.hasHoles || parts.hasCapacity)
        return KnownBoolean::Unknown;
    // Its readers may expect storage for every value, where an element may be no number.
    if (test.arrayStorage() == ArrayStorage::Contiguous)
        return KnownBoolean::True;
    if (test.arrayStorage() != arrayStorageAtAllocation(program, program.at(description.type)))
        return KnownBoolean::False;
    return classifyListOfNumbers(program, description) == ListOfNumbers::FitsByType ? KnownBoolean::True : KnownBoolean::Unknown;
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
    const Program& program = context.objects.program;
    const Type& array = program.at(description.type);
    if (arrayStorageAtAllocation(program, array) != ArrayStorage::Contiguous)
        return evaluateStorageTestOfNumbers(program, description, partsOfLiteral(context.family, shape), test);
    ArrayStorage kind = arrayStorageOfLiteral(program, array, partsOfLiteral(context.family, shape));
    return kind == ArrayStorage::Other ? KnownBoolean::Unknown : knownBoolean(kind == test.arrayStorage());
}

static KnownBoolean evaluateBoundsTest(const ShapeContext& context, const VirtualShape& shape, const Value& test)
{
    if (!isArray(context.family.descriptionOf(shape)))
        return KnownBoolean::Unknown;
    KnownValue position = knownValueOf(context.objects.program, context.objects.function, test.positionOperand());
    if (position.kind() == KnownValue::Kind::Integer)
        return knownBoolean(position.asInteger() >= 0 && position.asInteger() < static_cast<int64_t>(shape.length));
    const Type& type = context.objects.program.typeOf(context.objects.function, test.positionOperand());
    if (!type.isInteger())
        return KnownBoolean::Unknown;
    Range positions { 0, static_cast<int64_t>(shape.length) - 1 };
    if (type.integers.isSubsetOf(positions))
        return KnownBoolean::True;
    return type.integers.intersectWith(positions).isEmpty() ? KnownBoolean::False : KnownBoolean::Unknown;
}

static KnownBoolean evaluateBirthTest(const ShapeContext& context, const VirtualShape& shape)
{
    const Program& program = context.objects.program;
    const VirtualObjectDescription& description = context.family.descriptionOf(shape);
    if (!isLayoutObject(description))
        return KnownBoolean::Unknown;
    const Layout& layout = program.at(description.layout);
    for (uint32_t index = 0; index < layout.fields.size(); ++index) {
        if (!isFilledAtBirth(program, { description.layout, index }))
            continue;
        PropertyKeyID key = layout.fields[index].key;
        std::optional<uint32_t> field = key ? context.family.findField(VirtualField::named(key)) : std::nullopt;
        if (field && !hasStored(shape, *field) && hasStoredCounterpart(context.family, shape, *field))
            field = context.family.findCounterpart(*field);
        if (!field || !hasStored(shape, *field))
            return KnownBoolean::Unknown;
        if (holdsRawValueFromBirth(program, { description.layout, index }) && mayBeUndefinedOrNull(program, contentsOf(context, description, *field)))
            return KnownBoolean::Unknown;
    }
    return KnownBoolean::True;
}

bool hasEveryFieldOfBirthStored(const ShapeContext& context, const VirtualShape& shape)
{
    return shape.kind == VirtualShapeKind::Fields && !hasUnprovenField(context.family, shape) && evaluateBirthTest(context, shape) == KnownBoolean::True;
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
    case Opcode::IsElementPresent:
        return evaluateFieldTest(context, shape, test);
    case Opcode::HasArrayStorage:
        return evaluateArrayStorageTest(context, shape, test);
    case Opcode::IsInBounds:
        return evaluateBoundsTest(context, shape, test);
    case Opcode::HasNoExtraProperties:
    case Opcode::HasNoFieldOutsideSlot:
        if (hasUnprovenFieldThatMayLeaveSlot(context, shape))
            return KnownBoolean::False;
        [[fallthrough]];
    case Opcode::IsNotFrozen:
    case Opcode::BelongsToInstance:
        return isLayoutObject(description) ? KnownBoolean::True : KnownBoolean::Unknown;
    case Opcode::HasBirthFieldsFilled:
        if (hasUnprovenFieldThatMayLeaveSlot(context, shape))
            return KnownBoolean::False;
        return evaluateBirthTest(context, shape);
    case Opcode::HasLayoutPrototype:
        return evaluatePrototypeTest(context, shape);
    case Opcode::IsPristine:
        return isArray(description) && test.builtinClass() == BuiltinClasses::Array ? KnownBoolean::True : KnownBoolean::Unknown;
    case Opcode::HasKeyOrder:
        if (hasUnprovenFieldThatMayLeaveSlot(context, shape))
            return KnownBoolean::Unknown;
        return evaluateKeyOrderTest(context, shape, test);
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
    case Opcode::Expect:
    case Opcode::NarrowByField:
    case Opcode::AssumeType:
    case Opcode::FinishObject:
    case Opcode::CheckInitialized:
        return CastOutcome::Passes;
    case Opcode::Check:
        return evaluateTypeTestOfShape(context, shape, context.objects.program.at(cast.expectedType().predicted())) == KnownBoolean::True ? CastOutcome::Passes : CastOutcome::Unknown;
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
