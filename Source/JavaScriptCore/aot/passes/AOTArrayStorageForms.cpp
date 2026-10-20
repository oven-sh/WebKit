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

namespace JSC { namespace AOT { namespace Typed {

// The answer is a prediction, and the creator and the readers of an array take the same one. A NaN, an undefined or a null that is stored at run time
// converts an array of numbers to Contiguous: canStoreWithoutConversion() says which stores have to test for that.
ArrayStorage arrayStorageHolding(const Type& elements)
{
    if (elements.hasObjects() || !elements.isJavaScriptValue())
        return ArrayStorage::Contiguous;

    uint16_t primitives = elements.primitives & ~(Primitive::UndefinedOrNull | Primitive::Absent);
    if (!primitives)
        return ArrayStorage::Other;
    if (primitives == Primitive::Integer && elements.integers.isSubsetOf(Range::int32()))
        return ArrayStorage::Int32;
    if (!(primitives & ~Primitive::Number))
        return ArrayStorage::Double;
    return ArrayStorage::Contiguous;
}

ArrayStorage arrayStorageAtAllocation(const Type& elements)
{
    ArrayStorage kind = arrayStorageHolding(elements);
    return kind == ArrayStorage::Other ? ArrayStorage::Contiguous : kind;
}

ArrayStorage arrayStorageAtAllocation(const Program& program, const Type& array)
{
    const ClassPart* part = array.singleBuiltinClass();
    TypeID contents = part ? part->contentsAt(program.builtins(), ContentsSlot::Element) : TypeID();
    return arrayStorageAtAllocation(program.at(contents ? contents : TypeTable::anything));
}

ArrayStorage arrayStorageOfLiteral(const Program& program, const Type& array, ArrayLiteralParts parts)
{
    // Selection may give the literal the kind that the arrays of its site have turned into.
    if (!parts.hasElements && !parts.hasHoles && !parts.hasCapacity)
        return ArrayStorage::Other;
    if (parts.hasHoles)
        return ArrayStorage::Contiguous;
    // A list of numbers that does not fit its kind is found when code is generated or when the literal runs.
    return arrayStorageAtAllocation(program, array) == ArrayStorage::Contiguous ? ArrayStorage::Contiguous : ArrayStorage::Other;
}

ArrayStorage arrayStorageOfLiteral(const Program& program, const Value& literal)
{
    RELEASE_ASSERT(literal.opcode == Opcode::NewArray);
    return arrayStorageOfLiteral(program, program.at(literal.type), { !literal.elementsOperands().empty(), !literal.holes().empty(), !!literal.capacityOperand() });
}

std::optional<ArrayStorage> arrayStorageOfNewHoles(const Value& allocation)
{
    switch (allocation.opcode) {
    case Opcode::NewDoubleArray:
        return ArrayStorage::Double;
    case Opcode::NewArrayLike:
    case Opcode::NewArrayWithCapacity:
        return allocation.arrayStorage() == ArrayStorage::Other ? ArrayStorage::Contiguous : allocation.arrayStorage();
    case Opcode::NewContiguousArray:
    case Opcode::ArraySpeciesCreate:
        return ArrayStorage::Contiguous;
    default:
        return std::nullopt;
    }
}

std::optional<ArrayStorage> arrayStorageOfNewHoles(const BuiltinOverload& overload)
{
    const BuiltinImplementation& implementation = overload.code.implementation;
    if (implementation.kind != BuiltinImplementationKind::Inline || implementation.inlineBuiltin != InlineBuiltin::ArrayWithLength)
        return std::nullopt;
    return arrayStorageAtAllocation(Type::never());
}

bool canStoreWithoutConversion(ArrayStorage kind, const Type& stored)
{
    switch (kind) {
    case ArrayStorage::Int32:
        return stored.isInt32();
    case ArrayStorage::Double:
        return stored.isNumber() && !stored.mayBeNaN();
    case ArrayStorage::Contiguous:
        return true;
    case ArrayStorage::Other:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ArrayStorage arrayStorageAfterStore(ArrayStorage kind, const Type& stored)
{
    if (canStoreWithoutConversion(kind, stored))
        return kind;
    if (kind == ArrayStorage::Int32 && canStoreWithoutConversion(ArrayStorage::Double, stored))
        return ArrayStorage::Double;
    return hasArrayStorageForm(kind) ? ArrayStorage::Contiguous : ArrayStorage::Other;
}

Type typeOfElementRead(ArrayStorage kind)
{
    switch (kind) {
    case ArrayStorage::Int32:
        return Type::integer(Range::int32()).withPrimitives(Primitive::Undefined);
    case ArrayStorage::Double:
        return Type::fromPrimitives(Primitive::Number | Primitive::Undefined);
    case ArrayStorage::Contiguous:
    case ArrayStorage::Other:
        return Type::anything();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<NarrowStorageChoice> narrowStorageHolding(const Type& elements)
{
    if (elements.isBoolean())
        return NarrowStorageChoice { NarrowElementKind::Boolean, TypeUint8 };
    if (!elements.isInteger())
        return std::nullopt;

    for (const NarrowStorageForm& form : narrowStorageForms) {
        if (elements.integers.isSubsetOf({ form.minimum, form.maximum }))
            return NarrowStorageChoice { NarrowElementKind::Integer, form.storage };
    }
    return std::nullopt;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
