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

namespace JSC { namespace AOT { namespace Typed {

using ArrayStorageFormList = Vector<const ArrayStorageForm*, arrayStorageForms.size()>;

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
    explicit ElementAccessEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateGetArrayLength(const ValueContext&);
    void generateGetArrayStorage(const ValueContext&);
    void generateGetArrayElement(const ValueContext&);
    void generatePutArrayElement(const ValueContext&);
    void generateHasArrayElement(const ValueContext&);
    void generateAppendElements(const ValueContext&);
    void generateAppendMembers(const ValueContext&);
    void generateGetTypedArrayLength(const ValueContext&);
    void generateGetTypedArrayVector(const ValueContext&);
    void generateGetTypedArrayElement(const ValueContext&);
    void generatePutTypedArrayElement(const ValueContext&);
    void generateGetEnumeratorIndexedLength(const ValueContext&);
    void generateIsElementInStorage(const ValueContext&);
    void generateGetStringLength(const ValueContext&);
    void generateGetStringCodeUnit(const ValueContext&);

    bool testsEveryPosition();
    void stopUnlessInside(const ValueContext& access, Condition isInside);
    void stopUnlessListIsAsMadeWhereNothingIsFound(const ValueContext& access, CellPointer list);

    Condition isPristineArray(CellPointer, OptionSet<PristineAspects>);
    Condition isPristineInstance(CellPointer, OptionSet<PristineAspects>);
    Condition hasArrayStorage(CellPointer array, ArrayStorage, bool isWritable = false);
    Word32 indexingMode(CellPointer array, IndexingType mask);
    Condition hasShape(Word32 maskedMode, const ArrayStorageForm& form) { return equal(maskedMode, word32(form.shape)); }
    Condition holdsBoxedElements(Word32 maskedMode);
    Condition holdsBoxedElements(CellPointer array);
    Condition hasWritableShape(Word32 modeWithWritability, const ArrayStorageForm& form) { return equal(modeWithWritability, word32(form.indexingType)); }

    ArrayElements elementsOf(CellPointer array) { return { array, loadField<Pointer>(array, m_heaps.JSArray_butterfly) }; }
    ArrayElements elementsOf(const ValueContext& access, CellPointer array);
    Integer publicLength(const ArrayElements& elements) { return zeroExtended(loadField<Word32>(elements.butterfly, m_heaps.Butterfly_publicLength)); }
    Integer vectorLength(const ArrayElements& elements) { return zeroExtended(loadField<Word32>(elements.butterfly, m_heaps.Butterfly_vectorLength)); }
    void setPublicLength(const ArrayElements& elements, Integer length) { storeField(elements.butterfly, m_heaps.Butterfly_publicLength, low32(length)); }
    Integer arrayLength(CellPointer array) { return publicLength(elementsOf(array)); }

    TypedPointer elementAt(const ArrayStorageForm&, const ArrayElements&, Integer index);
    TypedPointer elementOfUnknownForm(const ArrayElements&, Integer index);
    Operand loadRawElement(const ArrayStorageForm&, const ArrayElements&, Integer index);
    Boxed loadBoxedElement(const ArrayElements&, Integer index);
    Condition isHole(const ArrayStorageForm&, Operand rawElement);
    Operand rawHole(const ArrayStorageForm&);
    Boxed boxRawElement(const ArrayStorageForm&, Operand rawElement);
    Operand convertRawElement(const OperationContext&, const ArrayStorageForm&, Operand rawElement, Representation, TypeID result);
    void stopUnlessElementIsOf(const OperationContext&, Boxed element, TypeID proven);
    void stopUnlessDoubleIsOf(const OperationContext&, Operand element, TypeID proven);
    Operand convertElementThatHasBeenAsked(const OperationContext&, const ArrayStorageForm&, Operand rawElement, Representation, TypeID result);
    Operand convertInt32Element(Word32, Representation);
    Operand absentElement(const OperationContext&, Representation);
    void storeRawElement(const ArrayStorageForm&, const ArrayElements&, Integer index, Operand rawElement);
    std::optional<Operand> rawElementToStore(const ArrayStorageForm&, ValueID stored, LBasicBlock doesNotFit);
    ArrayStorageFormList candidateFormsFor(ValueID stored, ArrayStorage expected);
    ArrayStorageFormList formExpectedToHold(ValueID list, ValueID stored);
    bool goesByKindOfListOfNumbers(ValueID list, ValueID stored);
    ArrayStorageFormList formsOfListOfNumbersFor(ValueID stored, bool hasArmForEveryKind);
    ArrayStorageFormList formsThatMayHold(ValueID list, ValueID stored);
    void storeInProvenList(const ValueContext&, const ArrayStorageFormList&);
    bool hasOnlyExpectedArmInLine(const ValueContext& store);
    using ShapeTest = Condition (ElementAccessEmitter::*)(Word32 mode, const ArrayStorageForm&);
    enum class FirstForm : bool { IsExpected, IsLikeTheOthers };
    void forEachFormByShape(Word32 mode, ShapeTest, const ArrayStorageFormList&, LBasicBlock hasNoForm, const ScopedLambda<void(const ArrayStorageForm&)>&, FirstForm = FirstForm::IsExpected);

    enum class NaNIndexMode : bool { TakeSlowPath, ProduceOutOfBoundsIndex };
    Integer indexOrElse(ValueID position, bool keyIsIntegral, LBasicBlock isNotAnIndex, NaNIndexMode = NaNIndexMode::TakeSlowPath);
    Integer indexInsideLength(ValueID position);
    std::optional<Integer> indexBesideWholeNumber(ValueID position, Number, LBasicBlock isNotAnIndex);
    LengthTest testLength(Integer index, const ArrayElements&);
    LengthTest testLength(const ValueContext&, Integer index, const ArrayElements&);
    Boxed boxedPosition(ValueID position);

    Operand readElement(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, LBasicBlock isAbsent);
    const PlacedSlot* extraPropertiesWordOfReadByPosition(const ValueContext& read);
    Boxed readElementOfExtraProperties(CellPointer object, const PlacedSlot& extraPropertiesWord, Integer index, LBasicBlock notFound);
    Operand readElementInsideLength(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, LBasicBlock isHole);
    void branchOnPresentElement(const ValueContext& test, const ArrayStorageForm&, BranchTargets);
    void branchOnPresentElementInAnyStorage(const ValueContext& test, BranchTargets);

    Condition hasDoubleOrInt32Storage(CellPointer array);
    void branchOnPresentElementInDoubleOrInt32Storage(const ValueContext& test, BranchTargets);

    void appendElement(const ValueContext& store, CellPointer array, ValueID element);
    Boxed appendOutOfLine(const OperationContext&, CellPointer array, ValueID element);
    Condition tryStoreElement(const ValueContext& store, CellPointer array, Integer index, ValueID stored);

    TypedArrayElements typedArrayElementsOf(CellPointer view, LBasicBlock isNotFixedLength);
    TypedArrayElements typedArrayElementsOf(ValueID view, LBasicBlock isNotFixedLength);
    Operand loadTypedArrayElement(const TypedArrayElements&, Integer index, TypedArrayType);
    void storeTypedArrayElement(const TypedArrayElements&, Integer index, TypedArrayType, Operand stored);

private:
    void readWithKnownStorage(const ValueContext&, const ArrayStorageForm&);
    void readInsideLength(const ValueContext&, const ArrayStorageForm&);
    void readWithAnyStorage(const ValueContext&);
    bool hasOneArmInLine(const ValueContext& read);
    void readWithOneArmInLine(const ValueContext&, const ArrayStorageForm* knownForm);
    void readFromDoubleOrInt32Storage(const ValueContext&);
    Operand readNumberInsideLength(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, LBasicBlock isHole);
    Boxed readOutOfLine(const ValueContext&, CellPointer array);
    bool findsElement(const ValueContext& read);
    Boxed readBoxedElement(const ValueContext&, const ArrayElements&, Integer index, LBasicBlock isAbsent);

    void storeInsideLength(const ValueContext&, const ArrayStorageForm&);
    void replaceElement(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, Operand rawElement);
    enum class NumberBoxing : bool { InLine, ByStub };
    Boxed boxForStub(const OperationContext&, Operand, const Type&, NumberBoxing);
    void storeOutOfLine(const ValueContext&, CellPointer array, NumberBoxing);
    Boxed appendOutOfLine(const OperationContext&, CellPointer array, ValueID element, NumberBoxing);
    void storeInVector(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, Operand rawElement, LBasicBlock isBeyondVector);
    std::optional<Integer> integerNamedByAppend(const ValueContext& store, size_t index);
    Integer lengthNamedOrRead(const ValueContext& store, const ArrayElements&);
    void branchUnlessHasRoomBehindLength(const ValueContext& store, const ArrayElements&, Integer length, LBasicBlock isFull);

    void holdPositionInsideList(const ValueContext& access);
    void holdPositionInsideView(const ValueContext& access, Integer index, const TypedArrayElements&);
    Integer indexOfPresenceTest(const ValueContext& test, LBasicBlock isNotAnIndex);
    void branchOnElementPresence(const ValueContext&, const ArrayStorageForm*, const ArrayElements&, Integer index, BranchTargets);
    TypedArrayElements typedArrayElementsInsideView(const ValueContext&, CellPointer view);
    std::optional<TypedArrayElements> typedArrayElementsNamedBy(const ValueContext&, CellPointer view);
    void askViewWherePositionIsNotBelowNamedLength(LBasicBlock isNotBelow, const ScopeFork&, CellPointer view, LBasicBlock outside, LBasicBlock otherwise);
    bool becomesIndexWithoutTest(const ValueContext& access);
    Operand loadBytesOfTypedArrayElement(const TypedArrayElements&, Integer index, TypedArrayType);
    Word32 clampedToUint8(Integer);
    Word32 clampedToUint8(Word32);
};

static_assert(sizeof(ElementAccessEmitter) == sizeof(OperationEmitter));

inline TypedPointer ElementAccessEmitter::elementOfUnknownForm(const ArrayElements& elements, Integer index)
{
    static_assert(sizeof(EncodedJSValue) == sizeof(double));
    const Machine::AbstractHeap& elementsOfEveryForm = m_heaps.at(m_program, MemoryLocation::elements(ElementBase::Array));
    return m_out.baseIndex(elementsOfEveryForm, elements.butterfly.value(), index.value(), ScaleEight);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
