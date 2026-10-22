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
#include "TypedArrayType.h"
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

constexpr Representation elementRepresentationOf(ArrayStorage kind)
{
    const ArrayStorageForm* form = findArrayStorageForm(kind);
    return form ? form->element : Representation::JSValue;
}

inline constexpr IndexingType arrayStorageTestMask = IndexingShapeMask;

ArrayStorage arrayStorageHolding(const Type& elements);
// The kind that the declared element type gives an array, for its creators and its readers alike. Other: nothing is declared.
ArrayStorage arrayStorageDeclaredBy(const Program&, const ClassPart& array);
ArrayStorage arrayStorageDeclaredBy(const Program&, const Type& array);
ArrayStorage arrayStorageAtAllocation(const Type& elements);
ArrayStorage arrayStorageAtAllocation(const Program&, const Type& array);

struct ArrayLiteralParts {
    bool hasElements { false };
    bool hasHoles { false };
    bool hasCapacity { false };
};

// The kind that a literal has for certain once it is allocated. Other where the representations of its elements or what its site has seen decide.
// The prototypes of arrays are locked and take no index, so the realm of compiled code gives a literal no storage of another sort.
ArrayStorage arrayStorageOfLiteral(const Program&, const Type& array, ArrayLiteralParts);
ArrayStorage arrayStorageOfLiteral(const Program&, const Value& literal);
// The kind that an array of constant data has once it is made, in the table of the image too.
JS_EXPORT_PRIVATE ArrayStorage arrayStorageOfData(const Program&, const DataNode::Array&);
// No element is a cell, so what the table of the image holds is what the storage of the array holds, and the array points at it until it is written to.
JS_EXPORT_PRIVATE bool isStorageOfArray(const DataNode::Array&);
// The kind of an array that is created with a length and nothing in it. Its creator has no place for the elements that flow found in the arrays of its site,
// so the readers expect this kind and not the one that would hold the elements. Nothing for any other creator.
std::optional<ArrayStorage> arrayStorageOfNewHoles(const Value& allocation);
bool canStoreWithoutConversion(ArrayStorage, const Type& stored);

Type typeOfElementRead(ArrayStorage);

struct NarrowStorageForm {
    TypedArrayType storage;
    int64_t minimum;
    int64_t maximum;
};

inline constexpr std::array<NarrowStorageForm, 5> narrowStorageForms { {
    { TypeUint8, 0, 255 },
    { TypeInt8, -128, 127 },
    { TypeUint16, 0, 65535 },
    { TypeInt16, -32768, 32767 },
    { TypeInt32, -2147483648ll, 2147483647ll },
} };

enum class NarrowElementKind : uint8_t { Boolean, Integer };

struct NarrowStorageChoice {
    NarrowElementKind elements { NarrowElementKind::Integer };
    TypedArrayType storage { NotTypedArray };
};

std::optional<NarrowStorageChoice> narrowStorageHolding(const Type& elements);

static_assert(arrayStorageOf(ArrayWithInt32) == ArrayStorage::Int32 && arrayStorageOf(CopyOnWriteArrayWithDouble) == ArrayStorage::Double);
static_assert(arrayStorageOf(ArrayWithContiguous) == ArrayStorage::Contiguous && arrayStorageOf(ArrayWithArrayStorage) == ArrayStorage::Other);
static_assert(arrayStorageOf(ArrayWithUndecided) == ArrayStorage::Other && !hasArrayStorageForm(ArrayStorage::Other));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
