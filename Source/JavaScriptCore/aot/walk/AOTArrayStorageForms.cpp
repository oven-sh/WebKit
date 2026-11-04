/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayStorageForms.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTTypeArena.h"
#include "JSCJSValueInlines.h"
#include <algorithm>
#include <cmath>

namespace JSC { namespace AOT { namespace Typed {

// Each kind holds whatever the kinds in front of it hold.
static_assert(ArrayStorage::Other < ArrayStorage::Int32 && ArrayStorage::Int32 < ArrayStorage::Double && ArrayStorage::Double < ArrayStorage::Contiguous);

static constexpr TypeID typeReadFromInt32ArrayStorage = *tryMakeTypeOfPrimitives(Primitive::Integer | Primitive::Undefined, Range::int32());
static constexpr TypeID typeReadFromDoubleArrayStorage = typeOfPrimitives(Primitive::Number | Primitive::Undefined);

static constexpr ArrayStorage arrayStorageWithFormFor(ArrayStorage kind)
{
    return hasArrayStorageForm(kind) ? kind : ArrayStorage::Contiguous;
}

// The answer is a prediction, and the creator and the readers of an array take the same one.
static ArrayStorage arrayStorageHoldingValuesOf(const Type& elements)
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

// `number[]` says nothing of fractions, and most such lists hold integers. Out of storage for doubles each of them is asked at every read whether it is an int32 before it is boxed.
static ArrayStorage arrayStorageDeclaredByElements(const Type& elements)
{
    if (elements.isAnything())
        return ArrayStorage::Other;
    ArrayStorage storage = arrayStorageHoldingValuesOf(elements);
    return storage == ArrayStorage::Double ? ArrayStorage::Other : storage;
}

ArrayStorage arrayStorageHolding(const TypeArena& types, TypeID elements)
{
    return elements ? arrayStorageHoldingValuesOf(types.at(elements)) : ArrayStorage::Other;
}

ArrayStorage arrayStorageHolding(TypeReader types, TypeID elements)
{
    return elements ? arrayStorageHoldingValuesOf(types.at(elements)) : ArrayStorage::Other;
}

// Only what is written of the elements counts. A named object says nothing of its contents, so a type that names arrays alone declares nothing.
Expected<ArrayStorage> arrayStorageDeclaredBy(const Program& program, TypeReader types, TypeID array)
{
    if (!array || holdsOnlyPrimitives(array))
        return ArrayStorage::Other;
    Type type = types.at(array);
    const ClassPart* part = singleBuiltinClassOf(types, type);
    if (!part || part->builtinClass != BuiltinClasses::Array)
        return ArrayStorage::Other;
    TypeID elements = part->expectedContentsAt(program.builtins(), ContentsSlot::Element).predicted();
    return arrayStorageDeclaredByElements(types.at(elements));
}

bool isDeclaredToHoldNumbersAlone(const Program& program, TypeReader types, TypeID array)
{
    if (!array || holdsOnlyPrimitives(array))
        return false;
    Type type = types.at(array);
    const ClassPart* part = singleBuiltinClassOf(types, type);
    if (!part || part->builtinClass != BuiltinClasses::Array)
        return false;
    TypeID elements = part->expectedContentsAt(program.builtins(), ContentsSlot::Element).predicted();
    return elements && !types.at(elements).isAnything() && holdsOnlyNumbers(arrayStorageHoldingValuesOf(types.at(elements)));
}

bool isProvenToBeList(TypeReader types, TypeID array)
{
    if (!array || !types.at(array).isObject())
        return false;
    const ClassPart* part = singleBuiltinClassOf(types, types.at(array));
    return part && part->builtinClass == BuiltinClasses::Array;
}

bool readTakesIndexOfProvenList(TypeReader types, TypeID array, TypeID position)
{
    if (!array || !position || !types.at(array).isObject() || !types.at(position).isInteger())
        return false;
    const ClassPart* part = singleBuiltinClassOf(types, types.at(array));
    return part && part->builtinClass == BuiltinClasses::Array;
}

ArrayStorage arrayStorageAtAllocation(const TypeArena& types, TypeID elements)
{
    return arrayStorageWithFormFor(arrayStorageHolding(types, elements));
}

ArrayStorage arrayStorageOfLiteral(const Program& program, const TypeArena& types, TypeID array, ArrayLiteralParts parts)
{
    // The run time gives a long run of holes storage without a form, and the parts do not say how long the run is.
    if (parts.hasHoles)
        return ArrayStorage::Other;
    ArrayStorage declared = arrayStorageDeclaredBy(program, types, array).predicted();
    // A literal without an element has the declared kind, or a more general one that the arrays of its site have turned into, and none is more general than Contiguous.
    if (!parts.hasElements)
        return declared == ArrayStorage::Contiguous ? ArrayStorage::Contiguous : ArrayStorage::Other;
    // A list of numbers that does not fit its kind is found when code is generated or when the literal runs.
    return arrayStorageWithFormFor(declared) == ArrayStorage::Contiguous ? ArrayStorage::Contiguous : ArrayStorage::Other;
}

ArrayStorage arrayStorageOfData(const Program& program, const DataNode::Array& array)
{
    bool areInt32 = true;
    bool areDoublesOrHoles = true;
    for (const DataNode& element : array.elements) {
        const auto* number = element.tryAs<DataNode::Number>();
        areInt32 &= number && canStoreWithoutConversion(ArrayStorage::Int32, number->value);
        areDoublesOrHoles &= number ? canStoreWithoutConversion(ArrayStorage::Double, number->value) : element.is<DataNode::Hole>();
    }
    ArrayStorage narrowest = areInt32 ? ArrayStorage::Int32 : areDoublesOrHoles ? ArrayStorage::Double : ArrayStorage::Contiguous;
    // The readers of an array test for the kind that is declared for it, also where every constant fits a narrower one.
    ArrayStorage declared = array.contents ? arrayStorageDeclaredByElements(program.at(array.contents)) : ArrayStorage::Other;
    return std::max(narrowest, declared);
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

// Every node counts, leaves and holes too.
bool isDataWrittenOutForItsSizeAlone(const DataNode& root)
{
    uint32_t numberOfNodes = 0;
    Vector<const DataNode*, maxNodesOfExpandedData> pending { &root };
    while (!pending.isEmpty()) {
        const DataNode& node = *pending.takeLast();
        if (node.is<DataNode::Object>() || ++numberOfNodes > maxNodesOfExpandedData)
            return false;
        const auto* array = node.tryAs<DataNode::Array>();
        if (!array)
            continue;
        if (array->elements.size() > maxNodesOfExpandedData)
            return false;
        for (const DataNode& element : array->elements)
            pending.append(&element);
    }
    return true;
}

// Only NewContiguousArray and NewDoubleArray always make their kind. At a length where the engine makes sparse arrays, NewArrayLike and NewArrayWithCapacity make what the engine makes.
std::optional<ArrayStorage> arrayStorageOfNewHoles(Opcode allocation, ArrayStorage named)
{
    if (allocation == Opcode::NewContiguousArray)
        return ArrayStorage::Contiguous;
    if (allocation == Opcode::NewDoubleArray)
        return ArrayStorage::Double;
    if (allocation == Opcode::NewArrayLike || allocation == Opcode::NewArrayWithCapacity)
        return arrayStorageWithFormFor(named);
    return std::nullopt;
}

bool canStoreWithoutConversion(const TypeArena& types, ArrayStorage kind, TypeID stored)
{
    switch (kind) {
    case ArrayStorage::Other:
        return false;
    case ArrayStorage::Int32:
        return stored && types.at(stored).isInt32();
    case ArrayStorage::Double: {
        if (!stored)
            return false;
        Type type = types.at(stored);
        return type.isNumber() && !type.mayBeNaN();
    }
    case ArrayStorage::Contiguous:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool canStoreWithoutConversion(ArrayStorage kind, double stored)
{
    switch (kind) {
    case ArrayStorage::Other:
        return false;
    case ArrayStorage::Int32:
        return jsNumber(purifyNaN(stored)).isInt32();
    case ArrayStorage::Double:
        return !std::isnan(stored);
    case ArrayStorage::Contiguous:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool fitsStorageOfLiteral(const TypeArena& types, ArrayStorage kind, TypeID element)
{
    if (isNeverType(element))
        return true;
    // A NaN among the numbers is found when the literal runs, and the array is then born with the storage that holds everything.
    if (kind == ArrayStorage::Double)
        return element && types.at(element).isNumber();
    return canStoreWithoutConversion(types, kind, element);
}

// With undefined for a hole. Whoever reads a hole as something else, or looks above the array for it, adds that.
TypeID typeOfElementRead(ArrayStorage kind)
{
    switch (kind) {
    case ArrayStorage::Int32:
        return typeReadFromInt32ArrayStorage;
    case ArrayStorage::Double:
        return typeReadFromDoubleArrayStorage;
    case ArrayStorage::Other:
    case ArrayStorage::Contiguous:
        return TypeTable::anything;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
