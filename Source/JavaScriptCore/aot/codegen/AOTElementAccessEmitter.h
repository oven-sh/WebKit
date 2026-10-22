/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTMemoryLocation.h"
#include "AOTOperationEmitter.h"
#include "TypedArrayType.h"
#include <optional>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

using ArrayStorageFormList = Vector<const ArrayStorageForm*, arrayStorageForms.size()>;

// The collector reaches the elements through the array alone, and B3 may load `butterfly` once in front of a loop and let the array die.
struct ArrayElements {
    CellPointer array;
    Pointer butterfly;
};

struct LengthTest {
    Condition isInside;
    Integer indexInside;
};

struct TypedArrayElements {
    CellPointer view;
    Integer length;
    Pointer vector;
};

class ElementAccessEmitter final : public OperationEmitter {
public:
    explicit ElementAccessEmitter(CodeGenerator&);

    void generateGetArrayLength(const ValueContext&);
    void generateGetArrayElement(const ValueContext&);
    void generatePutArrayElement(const ValueContext&);
    void generateHasArrayElement(const ValueContext&);
    void generateAppendElements(const ValueContext&);
    void generateHasSpareArrayCapacity(const ValueContext&);
    void generateReserveArrayCapacity(const ValueContext&);
    void generateConvertToDoubleStorage(const ValueContext&);
    void generateGetTypedArrayLength(const ValueContext&);
    void generateGetTypedArrayElement(const ValueContext&);
    void generatePutTypedArrayElement(const ValueContext&);
    void generateGetStringLength(const ValueContext&);
    void generateGetStringCodeUnit(const ValueContext&);

    Condition isPristineArray(CellPointer, OptionSet<PristineAspects>);
    Condition isPristineInstance(CellPointer, OptionSet<PristineAspects>);
    Condition hasArrayStorage(CellPointer array, ArrayStorage, bool isWritable = false);
    Word32 indexingMode(CellPointer array, IndexingType mask);
    Condition hasShape(Word32 maskedMode, const ArrayStorageForm&);
    // Storage of int32 and contiguous storage both hold boxed values, with 0 for a hole.
    Condition holdsBoxedElements(Word32 maskedMode);
    Condition hasWritableShape(Word32 modeWithWritability, const ArrayStorageForm&);

    ArrayElements elementsOf(CellPointer array);
    Integer publicLength(const ArrayElements&);
    Integer vectorLength(const ArrayElements&);
    void setPublicLength(const ArrayElements&, Integer length);
    Integer arrayLength(CellPointer array);

    TypedPointer elementAt(const ArrayStorageForm&, const ArrayElements&, Integer index);
    TypedPointer elementOfUnknownForm(const ArrayElements&, Integer index);
    Operand loadRawElement(const ArrayStorageForm&, const ArrayElements&, Integer index);
    bool isDense(ValueID array);
    Condition isHoleIn(ValueID array, const ArrayStorageForm&, Operand rawElement);
    Condition isHole(const ArrayStorageForm&, Operand rawElement);
    Operand rawHole(const ArrayStorageForm&);
    Boxed boxRawElement(const ArrayStorageForm&, Operand rawElement);
    Operand convertRawElement(const ArrayStorageForm&, Operand rawElement, Representation, const Type& result);
    Operand convertInt32Element(Word32, Representation);
    Operand absentElement(const OperationContext&, Representation);
    void storeRawElement(const ArrayStorageForm&, const ArrayElements&, Integer index, Operand rawElement);
    // nullopt: the value never fits and nothing was emitted, so the block is still open.
    std::optional<Operand> rawElementToStore(const ArrayStorageForm&, ValueID stored, LBasicBlock doesNotFit);
    // The forms rawElementToStore() has an answer for, the expected one first.
    ArrayStorageFormList candidateFormsFor(ValueID stored, ArrayStorage expected);
    // The functor ends the block it is called in. An array of none of the forms goes to hasNoForm.
    void forEachFormByWritableShape(Word32 modeWithWritability, const ArrayStorageFormList&, LBasicBlock hasNoForm, const ScopedLambda<void(const ArrayStorageForm&)>&);
    void forEachFormByReadableShape(Word32 maskedMode, const ArrayStorageFormList&, LBasicBlock hasNoForm, const ScopedLambda<void(const ArrayStorageForm&)>&);

    // The result may be negative or huge: compare it unsigned with a length before any use.
    enum class NaNIndexMode : bool { TakeSlowPath, ProduceOutOfBoundsIndex };
    Integer indexOrElse(ValueID position, bool keyIsIntegral, LBasicBlock isNotAnIndex, NaNIndexMode = NaNIndexMode::TakeSlowPath);
    Integer indexInsideLength(ValueID position);
    // Of storage that has a form. The index of the result is the position only where the test has passed.
    LengthTest testLength(Integer index, const ArrayElements&);
    Boxed boxedPosition(ValueID position);

    Operand readElement(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, LBasicBlock isAbsent);
    Operand readElementInsideLength(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, LBasicBlock isHole);
    // Taken if the array has an element of its own at the position. A position that is no index has none.
    void branchOnPresentElement(const ValueContext& test, const ArrayStorageForm&, BranchTargets);
    void appendElement(const ValueContext& store, CellPointer array, ValueID element);
    Condition tryStoreElement(const ValueContext& store, CellPointer array, Integer index, ValueID stored);

    TypedArrayElements typedArrayElementsOf(CellPointer view, LBasicBlock isNotFixedLength);
    Operand loadTypedArrayElement(const TypedArrayElements&, Integer index, TypedArrayType);
    void storeTypedArrayElement(const TypedArrayElements&, Integer index, TypedArrayType, Operand stored);

private:
    void readWithKnownStorage(const ValueContext&, const ArrayStorageForm&);
    void readInsideLength(const ValueContext&, const ArrayStorageForm&);
    void readWithAnyStorage(const ValueContext&);
    Boxed readOutOfLine(const ValueContext&, CellPointer array);
    bool leavesAbsentElementsToStub(const ValueContext& read);
    bool findsElement(const ValueContext& read);
    Boxed loadBoxedElement(const ArrayElements&, Integer index);
    Boxed readBoxedElement(const ValueContext&, const ArrayElements&, Integer index, LBasicBlock isAbsent);

    void storeWithKnownStorage(const ValueContext&, const ArrayStorageForm&);
    void storeInsideLength(const ValueContext&, const ArrayStorageForm&);
    void replaceElement(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, Operand rawElement);
    bool replacesBoxedInt32(const ValueContext& store, const ArrayStorageForm&);
    Operand rawElementThatFits(const ArrayStorageForm&, ValueID stored);
    void storeWithAnyStorage(const ValueContext&);
    void storeOutOfLine(const ValueContext&, CellPointer array);
    void storeInVector(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, Operand rawElement, LBasicBlock isBeyondVector);

    void appendWithMode(const ValueContext& store, const ArrayStorageForm&, const ArrayElements&, ValueID element, LBasicBlock slowPath);
    void appendOutOfLine(const ValueContext& store, CellPointer array, ValueID element);

    void branchOnElementPresence(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, BranchTargets);
    void branchOnElementPresence(const ValueContext&, const ArrayElements&, Integer index, BranchTargets, const ScopedLambda<Condition(Integer)>& loadsHole);
    TypedArrayElements typedArrayElementsInsideView(const ValueContext&, CellPointer view, Integer index);
    Word32 clampedToUint8(Integer);
};

static_assert(sizeof(ElementAccessEmitter) == sizeof(OperationEmitter));

inline TypedPointer ElementAccessEmitter::elementOfUnknownForm(const ArrayElements& elements, Integer index)
{
    static_assert(sizeof(EncodedJSValue) == sizeof(double));
    const B3::AbstractHeap& elementsOfEveryForm = m_heaps.at(m_program, MemoryLocation::elements(ElementBase::Array));
    return m_out.baseIndex(elementsOfEveryForm, elements.butterfly.value(), index.value(), FTL::ScaleEight);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
