/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include "AOTSchemaRecords.h"
#include "IndexingType.h"
#include "PureNaN.h"
#include <array>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class Program;
struct ClassPart;
struct Type;
struct Value;

struct ArrayStorageForm {
    ArrayStorage kind;
    IndexingType shape;
    IndexingType indexingType;
    Representation element;
    uint64_t hole;
    bool holdsCells;
};

inline constexpr std::array<ArrayStorageForm, 3> arrayStorageForms { {
    { ArrayStorage::Int32, Int32Shape, ArrayWithInt32, Representation::Int32, 0, false },
    { ArrayStorage::Double, DoubleShape, ArrayWithDouble, Representation::Double, PNaNAsBits, false },
    { ArrayStorage::Contiguous, ContiguousShape, ArrayWithContiguous, Representation::JSValue, 0, true },
} };

constexpr const ArrayStorageForm* findArrayStorageForm(ArrayStorage kind)
{
    for (const ArrayStorageForm& form : arrayStorageForms) {
        if (form.kind == kind)
            return &form;
    }
    return nullptr;
}

constexpr const ArrayStorageForm& arrayStorageFormOf(ArrayStorage kind)
{
    const ArrayStorageForm* form = findArrayStorageForm(kind);
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(form);
    return *form;
}

constexpr ArrayStorage arrayStorageOf(IndexingType indexingType)
{
    for (const ArrayStorageForm& form : arrayStorageForms) {
        if (form.shape == (indexingType & IndexingShapeMask))
            return form.kind;
    }
    return ArrayStorage::Other;
}

constexpr bool hasArrayStorageForm(ArrayStorage kind) { return findArrayStorageForm(kind); }

constexpr bool holdsOnlyNumbers(ArrayStorage kind)
{
    const ArrayStorageForm* form = findArrayStorageForm(kind);
    return form && !form->holdsCells;
}

constexpr Representation elementRepresentationOf(ArrayStorage kind)
{
    const ArrayStorageForm* form = findArrayStorageForm(kind);
    return form ? form->element : Representation::JSValue;
}

inline constexpr IndexingType arrayStorageTestMask = IndexingShapeMask;

ArrayStorage arrayStorageHolding(const Type& elements);
// The kind that the declared element type gives an array, for its creators and its readers alike. Other: nothing is declared.
Expected<ArrayStorage> arrayStorageDeclaredBy(const Program&, const ClassPart& array);
Expected<ArrayStorage> arrayStorageDeclaredBy(const Program&, const Type& array);
ArrayStorage arrayStorageAtAllocation(const Type& elements);
ArrayStorage arrayStorageAtAllocation(const Program&, const Type& array);

struct ArrayLiteralParts {
    bool hasElements { false };
    bool hasHoles { false };
    bool hasCapacity { false };
};

// The kind that a literal has for certain once it is allocated.
ArrayStorage arrayStorageOfLiteral(const Program&, const Type& array, ArrayLiteralParts);
ArrayStorage arrayStorageOfLiteral(const Program&, const Value& literal);
// The kind that an array of constant data has once it is made, in the table of the image too.
JS_EXPORT_PRIVATE ArrayStorage arrayStorageOfData(const Program&, const DataNode::Array&);
// No element is a cell, so what the table of the image holds is what the storage of the array holds, and the array points at it until it is written to.
JS_EXPORT_PRIVATE bool isStorageOfArray(const DataNode::Array&);
// The kind of an array that is created with a length and nothing in it.
std::optional<ArrayStorage> arrayStorageOfNewHoles(const Value& allocation);
// NaN marks a hole in Double storage, so an array that is given one gets the storage that holds everything.
bool canStoreWithoutConversion(ArrayStorage, const Type& stored);
bool canStoreWithoutConversion(ArrayStorage, double stored);
// A literal that names no storage is born with the kind that is declared for it where all its elements fit that kind, and with Contiguous otherwise.
// One that names storage for numbers alone is born with it: requiredTypeOfOperand() holds its elements to its proven contents, which are stored there without conversion.
bool fitsStorageOfLiteral(ArrayStorage, const Type& element);

Type typeOfElementRead(ArrayStorage);

static_assert(arrayStorageOf(ArrayWithInt32) == ArrayStorage::Int32 && arrayStorageOf(CopyOnWriteArrayWithDouble) == ArrayStorage::Double);
static_assert(arrayStorageOf(ArrayWithContiguous) == ArrayStorage::Contiguous && arrayStorageOf(ArrayWithArrayStorage) == ArrayStorage::Other);
static_assert(arrayStorageOf(ArrayWithUndecided) == ArrayStorage::Other && !hasArrayStorageForm(ArrayStorage::Other));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
