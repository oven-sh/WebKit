/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayStorageForms.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinTable.h"
#include "AOTIR.h"
#include "AOTType.h"
#include "JSCJSValueInlines.h"
#include <algorithm>
#include <cmath>

namespace JSC { namespace AOT { namespace Typed {

// The answer is a prediction, and the creator and the readers of an array take the same one.
ArrayStorage arrayStorageHolding(const Type& elements)
{
    if (elements.hasObjects() || !elements.isJavaScriptValue())
        return ArrayStorage::Contiguous;
    uint16_t primitives = elements.primitives & ~(Primitive::UndefinedOrNull | Primitive::Absent);
    if (!primitives)
        return ArrayStorage::Other;
    if (primitives == Primitive::Integer && elements.integers.isSubsetOf(Range::int32()))
        return ArrayStorage::Int32;
    return primitives & ~Primitive::Number ? ArrayStorage::Contiguous : ArrayStorage::Double;
}

ArrayStorage arrayStorageDeclaredBy(const Program& program, const ClassPart& array)
{
    if (array.arrayStorage != ArrayStorage::Other)
        return array.arrayStorage;
    const Type& elements = program.at(array.contentsAt(program.builtins(), ContentsSlot::Element));
    return elements.isAnything() ? ArrayStorage::Other : arrayStorageHolding(elements);
}

ArrayStorage arrayStorageDeclaredBy(const Program& program, const Type& array)
{
    const ClassPart* part = array.singleBuiltinClass();
    return part ? arrayStorageDeclaredBy(program, *part) : ArrayStorage::Other;
}

static ArrayStorage kindAtAllocation(ArrayStorage kind)
{
    return kind == ArrayStorage::Other ? ArrayStorage::Contiguous : kind;
}

ArrayStorage arrayStorageAtAllocation(const Type& elements)
{
    return kindAtAllocation(arrayStorageHolding(elements));
}

ArrayStorage arrayStorageAtAllocation(const Program& program, const Type& array)
{
    return kindAtAllocation(arrayStorageDeclaredBy(program, array));
}

ArrayStorage arrayStorageOfLiteral(const Program& program, const Type& array, ArrayLiteralParts parts)
{
    // A literal without an element has the declared kind, or a more general one that the arrays of its site have turned into, and none is more general than Contiguous.
    if (!parts.hasElements && !parts.hasHoles)
        return arrayStorageDeclaredBy(program, array) == ArrayStorage::Contiguous ? ArrayStorage::Contiguous : ArrayStorage::Other;
    // A list of numbers that does not fit its kind is found when code is generated or when the literal runs.
    return arrayStorageAtAllocation(program, array) == ArrayStorage::Contiguous ? ArrayStorage::Contiguous : ArrayStorage::Other;
}

ArrayStorage arrayStorageOfLiteral(const Program& program, const Value& literal)
{
    RELEASE_ASSERT(literal.opcode == Opcode::NewArray);
    return arrayStorageOfLiteral(program, program.at(literal.type), { !literal.elementsOperands().empty(), !literal.holes().empty(), !!literal.capacityOperand() });
}

ArrayStorage arrayStorageOfData(const Program& program, const DataNode::Array& array)
{
    bool areInt32 = true;
    bool areDoublesOrHoles = true;
    for (const DataNode& element : array.elements) {
        const auto* number = element.tryAs<DataNode::Number>();
        areInt32 &= number && jsNumber(purifyNaN(number->value)).isInt32();
        areDoublesOrHoles &= number ? !std::isnan(number->value) : element.is<DataNode::Hole>();
    }
    // The readers of an array that is declared to hold numbers test for Double storage, also where every constant is a whole number.
    if (areInt32 && arrayStorageHolding(program.at(array.contents)) != ArrayStorage::Double)
        return ArrayStorage::Int32;
    return areDoublesOrHoles ? ArrayStorage::Double : ArrayStorage::Contiguous;
}

bool isStorageOfArray(const DataNode::Array& array)
{
    return !array.elements.isEmpty() && std::ranges::all_of(array.elements, [](const DataNode& element) {
        switch (element.kind()) {
        case DataNode::Kind::Undefined:
        case DataNode::Kind::Null:
        case DataNode::Kind::Hole:
        case DataNode::Kind::Boolean:
        case DataNode::Kind::Number:
            return true;
        case DataNode::Kind::String:
        case DataNode::Kind::BigInt:
        case DataNode::Kind::Array:
        case DataNode::Kind::Object:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

std::optional<ArrayStorage> arrayStorageOfNewHoles(const Value& allocation)
{
    if (allocation.opcode == Opcode::NewDoubleArray)
        return ArrayStorage::Double;
    if (allocation.opcode == Opcode::NewArrayLike || allocation.opcode == Opcode::NewArrayWithCapacity)
        return kindAtAllocation(allocation.arrayStorage());
    if (allocation.opcode == Opcode::NewContiguousArray || allocation.opcode == Opcode::ArraySpeciesCreate)
        return ArrayStorage::Contiguous;
    return std::nullopt;
}

bool canStoreWithoutConversion(ArrayStorage kind, const Type& stored)
{
    if (kind == ArrayStorage::Double)
        return stored.isNumber() && !stored.mayBeNaN();
    return kind == ArrayStorage::Contiguous || (kind == ArrayStorage::Int32 && stored.isInt32());
}

Type typeOfElementRead(ArrayStorage kind)
{
    if (kind == ArrayStorage::Int32)
        return Type::integer(Range::int32()).withPrimitives(Primitive::Undefined);
    return kind == ArrayStorage::Double ? Type::fromPrimitives(Primitive::Number | Primitive::Undefined) : Type::anything();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
