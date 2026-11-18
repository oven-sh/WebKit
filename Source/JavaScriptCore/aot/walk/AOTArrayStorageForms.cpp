/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayStorageForms.h"

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTIRProgram.h"
#include "AOTTypeArena.h"
#include "JSCJSValueInlines.h"
#include <algorithm>
#include <cmath>

namespace JSC { namespace AOT { namespace Typed {

static_assert(ArrayStorage::Other < ArrayStorage::Int32 && ArrayStorage::Int32 < ArrayStorage::Double && ArrayStorage::Double < ArrayStorage::Contiguous);

static constexpr TypeID typeReadFromInt32ArrayStorage = *tryMakeTypeOfPrimitives(Primitive::Integer | Primitive::Undefined, Range::int32());
static constexpr TypeID typeReadFromDoubleArrayStorage = typeOfPrimitives(Primitive::Number | Primitive::Undefined);

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
    return elements && !types.at(elements).isAnything() && isNumericStorage(arrayStorageHoldingValuesOf(types.at(elements)));
}

static bool isArrayOrHeir(TypeReader types, const Type& type)
{
    if (const ClassPart* part = singleBuiltinClassOf(types, type))
        return part->builtinClass == BuiltinClasses::Array;
    if (!type.hasOnlyLayouts() || type.hasOnlyClosures() || !holdsOneMember(type.layouts))
        return false;
    const Layout& layout = types.program()->at(LayoutID(positionOf(type.layouts)));
    return builtinClassOfInstances(layout) == BuiltinClasses::Array && !layout.mayBeOrdinaryObject;
}

bool isProvenToBeList(TypeReader types, TypeID array)
{
    if (!array || holdsOnlyPrimitives(array))
        return false;
    const Type& type = types.recordOf(array);
    return type.isObject() && isArrayOrHeir(types, type);
}

bool readTakesIndexOfProvenList(TypeReader types, TypeID array, TypeID position)
{
    return position && types.at(position).isInteger() && isProvenToBeList(types, array);
}

bool numberOfProvenListIsReadUnboxed(TypeReader types, TypeID array, TypeID position, Representation formOfPosition)
{
    if (readTakesIndexOfProvenList(types, array, position))
        return true;
    if (!position || !isProvenToBeList(types, array))
        return false;
    if (formOfPosition == Representation::OptionalDouble)
        return types.at(position).isNumberOrNullish();
    return formOfPosition == Representation::Double && types.at(position).isNumber();
}

bool keyOfProvenListMayBeAskedForInt32(TypeReader types, TypeID array, TypeID position)
{
    if (!position || !isProvenToBeList(types, array))
        return false;
    Type key = types.at(position);
    return (key.isNumber() || key.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined)) && !key.isInteger() && key.primitives & Primitive::Integer;
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
    ArrayStorage declared = array.contents ? arrayStorageDeclaredByElements(program.at(array.contents)) : ArrayStorage::Other;
    return std::max(narrowest, declared);
}

bool isShortStringList(const DataNode::Array& array)
{
    return !array.elements.isEmpty() && array.elements.size() <= mostWordsSwitched && std::ranges::all_of(array.elements, [](const DataNode& element) {
        return element.is<DataNode::String>();
    });
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
    if (kind == ArrayStorage::Double)
        return element && types.at(element).isNumber();
    return canStoreWithoutConversion(types, kind, element);
}

const ArrayStorageForm* findFormOfLiteral(const TypeArena& types, const Code& code, std::span<const ValueID> elements, ArrayStorage named, ArrayStorage declaredByList)
{
    bool isNeverMade = std::ranges::any_of(elements, [&](ValueID element) {
        return isNeverType(code.at(element).type);
    });
    if (isNeverMade)
        return &arrayStorageFormOf(ArrayStorage::Contiguous);

    const ArrayStorageForm* namedForm = findArrayStorageForm(named);
    const ArrayStorageForm& form = namedForm ? *namedForm : arrayStorageFormOf(hasArrayStorageForm(declaredByList) ? declaredByList : ArrayStorage::Contiguous);
    RELEASE_ASSERT(namedForm || form.kind != ArrayStorage::Double);
    bool canHoldElements = std::ranges::all_of(elements, [&](ValueID element) {
        return fitsStorageOfLiteral(types, form.kind, code.at(element).type);
    });
    if (canHoldElements)
        return &form;
    return namedForm ? nullptr : &arrayStorageFormOf(ArrayStorage::Contiguous);
}

ArrayStorage storageThatLiteralIsSurelyBornWith(const Program& program, const TypeArena& types, const Code& code, const Operation& made)
{
    std::span<const ValueID> elements = made.elementsOperands();
    if (made.storage() != AllocationStorage::Heap || !made.holes().empty() || elements.empty() || made.literalStorage() == ArrayLiteralStorage::ByElements)
        return ArrayStorage::Other;
    ArrayStorage named = made.arrayStorage();
    ArrayStorage declared = hasArrayStorageForm(named) ? ArrayStorage::Other : arrayStorageDeclaredBy(program, types, made.type).predicted();
    const ArrayStorageForm* form = findFormOfLiteral(types, code, elements, named, declared);
    if (!form || (form->kind == ArrayStorage::Contiguous && isNumericStorage(declared)))
        return ArrayStorage::Other;
    bool everyElementGoesIn = std::ranges::all_of(elements, [&](ValueID element) {
        return canStoreWithoutConversion(types, form->kind, code.at(element).type) && !(types.at(code.at(element).type).primitives & Primitive::Absent);
    });
    return everyElementGoesIn ? form->kind : ArrayStorage::Other;
}

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
