/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTExpected.h"
#include "AOTSchemaEnums.h"
#include "AOTSchemaRecords.h"
#include "AOTType.h"
#include "IndexingType.h"
#include "PureNaN.h"
#include <array>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class Program;

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

// The most operands for which concat() has code in line, and the most elements that splice() and toSpliced() insert in line. The walk chooses the form by them, and the emitter writes one copy or one store for each.
inline constexpr size_t maxInlineConcatOperands = 3;
inline constexpr size_t maxInlineSpliceInsertions = 2;
// The most elements that splice() takes out in line. As many as the stub for a literal takes in registers, at most. A call that takes out more is made by a function that lists them.
inline constexpr int64_t maxRemovedByInlineSplice = 2;

// AOTArrayStorageForms.cpp: pure functions of types.
ArrayStorage arrayStorageHolding(const TypeArena&, TypeID elements);
ArrayStorage arrayStorageHolding(TypeReader, TypeID elements);
// The kind that the declared element type gives an array, for its creators and its readers alike. Other: nothing is declared.
Expected<ArrayStorage> arrayStorageDeclaredBy(const Program&, TypeReader, TypeID array);
// What is written of the elements is numbers and nothing else. No kind is declared by that: the list has storage for int32 or for doubles, whichever the numbers so far have given it.
bool isDeclaredToHoldNumbersAlone(const Program&, TypeReader, TypeID array);
// Of a GetArrayElement that testsItsOwnGuess(): the list is proven one and the position an integer, so neither is tested, and the position is taken as the integer that it is.
bool isProvenToBeList(TypeReader, TypeID array);
bool readTakesIndexOfProvenList(TypeReader, TypeID array, TypeID position);
// Of a PutArrayElement that testsItsOwnGuess(): the same, and a number is taken as it is held where storage for numbers takes it so.
inline bool storeTakesIndexOfProvenList(TypeReader types, TypeID array, TypeID position) { return readTakesIndexOfProvenList(types, array, position); }
ArrayStorage arrayStorageAtAllocation(const TypeArena&, TypeID elements);

struct ArrayLiteralParts {
    bool hasElements { false };
    bool hasHoles { false };
    bool hasCapacity { false };
};

// The kind that a literal has for certain once it is allocated.
ArrayStorage arrayStorageOfLiteral(const Program&, const TypeArena&, TypeID array, ArrayLiteralParts);
// The kind that an array of constant data has once it is made, in the table of the image too.
JS_EXPORT_PRIVATE ArrayStorage arrayStorageOfData(const Program&, const DataNode::Array&);
// No element is a cell, so what the table of the image holds is what the storage of the array holds, and the array points at it until it is written to.
JS_EXPORT_PRIVATE bool isStorageOfArray(const DataNode::Array&);
// A tree of constant data of at most this many nodes is written out as allocations.
inline constexpr uint32_t maxNodesOfExpandedData = 16;
// Arrays alone, and no more nodes than that: the walk writes the tree out whatever else holds. So each of its arrays is a literal, which is born with the kind that Facts decides for it.
JS_EXPORT_PRIVATE bool isDataWrittenOutForItsSizeAlone(const DataNode& root);
// The kind of an array that is created with a length and nothing in it: NewContiguousArray, NewDoubleArray, or what NewArrayLike and NewArrayWithCapacity name.
std::optional<ArrayStorage> arrayStorageOfNewHoles(Opcode allocation, ArrayStorage named);
// NaN marks a hole in Double storage, so an array that is given one gets the storage that holds everything.
bool canStoreWithoutConversion(const TypeArena&, ArrayStorage, TypeID stored);
bool canStoreWithoutConversion(ArrayStorage, double stored);
// A literal that names no storage is born with the kind that is declared for it where all its elements fit that kind, and with Contiguous otherwise.
bool fitsStorageOfLiteral(const TypeArena&, ArrayStorage, TypeID element);
TypeID typeOfElementRead(ArrayStorage);

static_assert(arrayStorageOf(ArrayWithInt32) == ArrayStorage::Int32 && arrayStorageOf(CopyOnWriteArrayWithDouble) == ArrayStorage::Double);
static_assert(arrayStorageOf(ArrayWithContiguous) == ArrayStorage::Contiguous && arrayStorageOf(ArrayWithArrayStorage) == ArrayStorage::Other);
static_assert(arrayStorageOf(ArrayWithUndecided) == ArrayStorage::Other && !hasArrayStorageForm(ArrayStorage::Other));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
