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

class Code;
class Program;
struct Operation;

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

constexpr bool accessToElementsHasCode(Opcode opcode, ArrayStorage kind, bool admitsInt32Storage, bool isWritable)
{
    if (opcode != Opcode::HasArrayStorage)
        return !isWritable && (!admitsInt32Storage || kind == ArrayStorage::Double || kind == ArrayStorage::Contiguous);
    if (!hasArrayStorageForm(kind))
        return isWritable;
    return !admitsInt32Storage || kind == ArrayStorage::Double || kind == ArrayStorage::Contiguous || (kind != ArrayStorage::Int32 && !isWritable);
}

constexpr bool isNumericStorage(ArrayStorage kind)
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

inline constexpr size_t maxInlineConcatOperands = 3;
inline constexpr size_t maxInlineSpliceInsertions = 2;
inline constexpr int64_t maxRemovedByInlineSplice = 2;

ArrayStorage arrayStorageHolding(const TypeArena&, TypeID elements);
ArrayStorage arrayStorageHolding(TypeReader, TypeID elements);
Expected<ArrayStorage> arrayStorageDeclaredBy(const Program&, TypeReader, TypeID array);
bool isDeclaredToHoldNumbersAlone(const Program&, TypeReader, TypeID array);
bool isProvenToBeList(TypeReader, TypeID array);
bool readTakesIndexOfProvenList(TypeReader, TypeID array, TypeID position);
bool numberOfProvenListIsReadUnboxed(TypeReader, TypeID array, TypeID position, Representation formOfPosition, bool testsForItself);
bool keyOfProvenListMayBeAskedForInt32(TypeReader, TypeID array, TypeID position);
inline bool storeTakesIndexOfProvenList(TypeReader types, TypeID array, TypeID position) { return readTakesIndexOfProvenList(types, array, position); }

JS_EXPORT_PRIVATE ArrayStorage arrayStorageOfData(const Program&, const DataNode::Array&);
JS_EXPORT_PRIVATE bool isStorageOfArray(const DataNode::Array&);
inline constexpr uint32_t maxNodesOfExpandedData = 16;
bool canStoreWithoutConversion(const TypeArena&, ArrayStorage, TypeID stored);
bool canStoreWithoutConversion(ArrayStorage, double stored);
bool fitsStorageOfLiteral(const TypeArena&, ArrayStorage, TypeID element);
const ArrayStorageForm* findFormOfLiteral(const TypeArena&, const Code&, std::span<const ValueID> elements, ArrayStorage named, ArrayStorage declaredByList);
ArrayStorage storageThatLiteralIsSurelyBornWith(const Program&, const TypeArena&, const Code&, const Operation& made);
TypeID typeOfElementRead(ArrayStorage);

static_assert(arrayStorageOf(ArrayWithInt32) == ArrayStorage::Int32 && arrayStorageOf(CopyOnWriteArrayWithDouble) == ArrayStorage::Double);
static_assert(arrayStorageOf(ArrayWithContiguous) == ArrayStorage::Contiguous && arrayStorageOf(ArrayWithArrayStorage) == ArrayStorage::Other);
static_assert(arrayStorageOf(ArrayWithUndecided) == ArrayStorage::Other && !hasArrayStorageForm(ArrayStorage::Other));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
