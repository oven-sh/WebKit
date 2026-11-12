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

struct ArrayHeaderTest;

class ElementAccessEmitter final : public OperationEmitter {
public:
    explicit ElementAccessEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateGetArrayLength(const ValueContext&);
    void generateGetArrayStorage(const ValueContext&);
    void generateGetObjectStorage(const ValueContext&);
    void generateGetObjectStorageLength(const ValueContext&);
    void generateGetArrayCapacity(const ValueContext&);
    void generateExtendArrayLength(const ValueContext&);
    void generateShortenArray(const ValueContext&);
    void generateRestoreArrayStore(const ValueContext&);
    void generateGetElementWord(const ValueContext&);
    void generateRestoreElementWord(const ValueContext&);
    void generateSetArrayLength(const ValueContext&);
    void generateGetArrayElement(const ValueContext&);
    void generatePutArrayElement(const ValueContext&);
    void generateHasArrayElement(const ValueContext&);
    void generateAppendElements(const ValueContext&);
    void generateAppendMembers(const ValueContext&);
    void generateAppendValuesOfMap(const ValueContext&);
    void generateReserveArrayCapacity(const ValueContext&);
    void generateCopyElements(const ValueContext&);
    void generateCopyElementsDown(const ValueContext&);
    void generateCountCopyableElements(const ValueContext&);
    void generateFillElements(const ValueContext&);
    void generateConvertToDoubleStorage(const ValueContext&);
    void generateGetTypedArrayLength(const ValueContext&);
    void generateGetTypedArrayVector(const ValueContext&);
    void generateGetMirroredLength(const ValueContext&);
    void generateGetMirroredVector(const ValueContext&);
    void generateGetTypedArrayElement(const ValueContext&);
    void generatePutTypedArrayElement(const ValueContext&);
    void generateIsBelowLength(const ValueContext&);
    void generateGetDataViewLimit(const ValueContext&);
    void generateGetDataViewElement(const ValueContext&);
    void generatePutDataViewElement(const ValueContext&);
    void generateGetEnumeratorIndexedLength(const ValueContext&);
    void generateIsElementInStorage(const ValueContext&);
    void generateGetElementInStorageOrAbsent(const ValueContext&);
    bool asksTruthOfRawElement(ValueID read);
    bool testsArrayNumberTruth(ValueID read);
    void branchOnArrayNumberTruth(const ValueContext&);
    void generateGetStringLength(const ValueContext&);
    void generateGetStringCodeUnit(const ValueContext&);

    bool checksBounds();
    std::optional<ArrayStorage> kindOfListFoundInFrontOfTurns(const ValueContext& access, ValueID list, bool isStoredInto = false);
    std::optional<TypedArrayType> kindOfViewFoundInFrontOfTurns(const ValueContext& access, ValueID view);
    enum class StorageIsWritten : bool { No, Yes };
    void holdKindOfList(const ValueContext& access, StorageIsWritten = StorageIsWritten::No);
    void stopUnlessInside(const ValueContext& access, Condition isInside);
    void checkElementPresenceProof(const ValueContext& read, Condition isThere, StatisticID counted = StatisticID::ElementPresenceProofsChecked);
    void stopWhereHoleWouldBeStored(const OperationContext& store, const ArrayStorageForm&, Operand rawElement);
    void stopUnlessListIsAsMadeWhereNothingIsFound(const ValueContext& access, CellPointer list);

    Condition isPristineArray(CellPointer, OptionSet<PristineAspects>);
    Condition isPristineInstance(CellPointer, OptionSet<PristineAspects>);
    Condition hasArrayStorage(CellPointer array, ArrayStorage, bool isWritable = false);
    Condition passesHeaderTest(CellPointer, const ArrayHeaderTest&);
    Word32 cellTypeAndIndexing(CellPointer cell) { return Word32(m_out.load16ZeroExt32(m_out.address(cell.value(), m_heaps.JSCell_usefulBytes, 0))); }
    Word32 cellHeaderBytes(CellPointer cell, uint32_t mask) { return bitAnd(loadField<Word32>(cell, m_heaps.JSCell_usefulBytes), word32(mask)); }
    Condition listHasArrayStorage(ValueID list, ArrayStorage kind) { return hasArrayStorage(in<CellPointer>(list), kind); }
    Condition numericListHoldsDoubles(CellPointer);
    Condition numericListHoldsInt32s(CellPointer);
    Condition numericListHasArrayStorage(ValueID list, ArrayStorage kind) { return kind == ArrayStorage::Double ? numericListHoldsDoubles(in<CellPointer>(list)) : numericListHoldsInt32s(in<CellPointer>(list)); }
    Condition listTakesStoresInArrayStorage(ValueID list, ArrayStorage kind) { return kind == ArrayStorage::Double ? hasWritableShape(indexingMode(in<CellPointer>(list), IndexingShapeAndWritabilityMask), arrayStorageFormOf(ArrayStorage::Double)) : holdsWritableBoxedElements(in<CellPointer>(list)); }
    Condition viewIsOfKind(ValueID view, TypedArrayType);
    Condition isRangeInsideView(const ValueContext& access, Integer least, Integer greatest);
    Pointer storageOfList(CellPointer list) { return elementsOf(list).butterfly; }
    Condition areInt32Elements(LValue boxedWordsMet) { return isInt32(Boxed(boxedWordsMet)); }
    Word32 indexingMode(CellPointer array, IndexingType mask);
    Condition hasShape(Word32 maskedMode, const ArrayStorageForm& form) { return equal(maskedMode, word32(form.shape)); }
    Condition holdsBoxedElements(Word32 maskedMode);
    Condition holdsBoxedElements(CellPointer array);
    Condition objectHoldsBoxedElements(CellPointer object);
    Boxed loadArrayStorageElement(CellPointer object, Integer index, LBasicBlock notFound);
    Condition holdsWritableBoxedElements(CellPointer array);
    Condition takesNumbersByKindOfStorage(CellPointer array, bool admitsInt32Storage);
    Condition hasWritableNumericStorage(CellPointer array);
    Condition hasWritableShape(Word32 modeWithWritability, const ArrayStorageForm& form) { return equal(modeWithWritability, word32(form.indexingType)); }

    CellPointer askedAnew(CellPointer cell) { return CellPointer(m_out.opaque(cell.value())); }
    ArrayElements elementsOf(CellPointer array) { return { array, loadField<Pointer>(array, m_heaps.JSArray_butterfly) }; }
    ArrayElements elementsOf(const ValueContext& access, CellPointer array);
    Integer publicLength(const ArrayElements& elements) { return zeroExtended(loadField<Word32>(elements.butterfly, m_heaps.Butterfly_publicLength)); }
    Integer vectorLength(const ArrayElements& elements) { return zeroExtended(loadField<Word32>(elements.butterfly, m_heaps.Butterfly_vectorLength)); }
    void setPublicLength(const ArrayElements& elements, Integer length) { storeField(elements.butterfly, m_heaps.Butterfly_publicLength, low32(length)); }
    Word32 publicLengthAsWord(const ArrayElements& elements) { return loadField<Word32>(elements.butterfly, m_heaps.Butterfly_publicLength); }
    Word32 vectorLengthAsWord(const ArrayElements& elements) { return loadField<Word32>(elements.butterfly, m_heaps.Butterfly_vectorLength); }
    void setPublicLength(const ArrayElements& elements, Word32 length) { storeField(elements.butterfly, m_heaps.Butterfly_publicLength, length); }
    Integer arrayLength(CellPointer array) { return publicLength(elementsOf(array)); }
    ArrayElements elementsOfList(ValueID list) { return elementsOf(in<CellPointer>(list)); }
    Condition listTakesBox(ValueID list, ValueID stored);

    TypedPointer elementAt(const ArrayStorageForm&, const ArrayElements&, Integer index);
    TypedPointer elementOfUnknownForm(const ArrayElements&, Integer index);
    bool boxIsMadeInFormOfType(ValueID) const;
    void storeWordOfOperand(const OperationContext&, ValueID stored, Boxed word, const ArrayElements&, Integer index);
    Operand loadRawElement(const ArrayStorageForm&, const ArrayElements&, Integer index);
    Boxed loadBoxedElement(const ArrayElements&, Integer index);
    Condition isHole(const ArrayStorageForm&, Operand rawElement);
    Operand rawHole(const ArrayStorageForm&);
    Boxed boxRawElement(const ArrayStorageForm&, Operand rawElement);
    Operand convertRawElement(const OperationContext&, const ArrayStorageForm&, Operand rawElement, Representation, TypeID result);
    void stopUnlessElementIsOf(const OperationContext&, Boxed element, TypeID proven);
    bool isTruncatedAlone(ValueID number) const;
    void stopUnlessDoubleIsOf(const OperationContext&, Operand element, TypeID proven);
    Operand convertElementThatHasBeenAsked(const OperationContext&, const ArrayStorageForm&, Operand rawElement, Representation, TypeID result);
    std::optional<Operand> tryUnboxNumberOrCrash(const OperationContext&, Boxed element, Representation, TypeID result);
    Operand convertInt32Element(Word32, Representation);
    Operand absentElement(const OperationContext&, Representation);
    Operand whatReadGivesWhereNothingIsFound(const ValueContext& read, Representation);
    void storeRawElement(const ArrayStorageForm&, const ArrayElements&, Integer index, Operand rawElement);
    std::optional<Operand> rawElementToStore(const ArrayStorageForm&, ValueID stored, LBasicBlock doesNotFit);
    ArrayStorageFormList candidateFormsFor(ValueID stored, ArrayStorage expected);
    ArrayStorageFormList formExpectedToHold(ValueID list, ValueID stored);
    bool goesByKindOfListOfNumbers(ValueID list, ValueID stored);
    const ArrayStorageForm& expectedFormOf(ValueID array);
    ArrayStorageFormList formsOfListOfNumbersFor(ValueID stored, bool hasArmForEveryKind);
    ArrayStorageFormList formsThatMayHold(ValueID list, ValueID stored);
    void storeInProvenList(const ValueContext&, const ArrayStorageFormList&);
    bool hasOnlyExpectedArmInLine(const ValueContext& store);
    using ShapeTest = Condition (ElementAccessEmitter::*)(Word32 mode, const ArrayStorageForm&);
    Condition hasBoxedShape(Word32 mode, ShapeTest);
    enum class FirstForm : bool { IsExpected, IsLikeTheOthers };
    void forEachFormByShape(Word32 mode, ShapeTest, const ArrayStorageFormList&, LBasicBlock hasNoForm, const ScopedLambda<void(const ArrayStorageForm&)>&, FirstForm = FirstForm::IsExpected, ValueID int32ThatArmOfValuesStoresInEitherKind = { });
    ValueID int32ThatArmOfValuesStoresInEitherKind(ValueID stored, const ArrayStorageFormList&);

    enum class NaNIndexMode : bool { TakeSlowPath, ProduceOutOfBoundsIndex };
    Integer indexOrElse(ValueID position, bool keyIsIntegral, LBasicBlock isNotAnIndex, NaNIndexMode = NaNIndexMode::TakeSlowPath);
    std::optional<Boxed> wordOfPosition(ValueID position);
    Integer wordToIndexOrElse(Boxed, bool keyIsIntegral, LBasicBlock isNotAnIndex, NaNIndexMode);
    Integer indexOfAccessOrElse(const ValueContext& access, LBasicBlock isNotAnIndex, NaNIndexMode = NaNIndexMode::TakeSlowPath);
    enum class PositionUses : bool { One, OneOrSharedIndex };
    Integer indexInsideLength(ValueID position, PositionUses = PositionUses::One);
    Integer positionOfElementWord(const ValueContext&, const ArrayElements&, StorageIsWritten);
    enum class IndexBesideConstant : bool { IsNotNegative, IsSigned };
    std::optional<IndexBesideConstant> indexBesideConstant(ValueID position, LValue sum, PositionUses);
    std::optional<Integer> indexAboveSharedIndex(ValueID position);
    Integer indexFoundBelowLength(Integer index, TypedArrayType);
    Condition isBelowLengthOfView(Integer index, Integer length, TypedArrayType);
    std::optional<Integer> indexBesideWholeNumber(ValueID position, Number, LBasicBlock isNotAnIndex);
    LengthTest testLength(Integer index, const ArrayElements&);
    LengthTest testLength(Integer index, Operand length);
    Integer lengthOfObjectStorage(CellPointer object, bool countsDoubles);
    LengthTest testLength(const ValueContext&, Integer index, const ArrayElements&);
    Boxed boxedPosition(ValueID position);

    Operand readElement(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, LBasicBlock isAbsent, std::optional<Representation> = std::nullopt);
    bool readsElementBoxed(const ValueContext&, const ArrayStorageForm&);
    void defineElement(const ValueContext&, Operand, bool isBoxed);
    const PlacedSlot* extraPropertiesWordOfReadByPosition(const ValueContext& read);
    Boxed readElementOfExtraProperties(CellPointer object, const PlacedSlot& extraPropertiesWord, Integer index, LBasicBlock notFound, bool readsArrayStorage);
    void overwriteElementOfExtraProperties(const ValueContext& store, CellPointer object, const PlacedSlot& extraPropertiesWord, Integer index, Boxed stored, LBasicBlock notFound);
    void overwriteElementOfOrdinaryObject(const ValueContext& store, CellPointer holder, Integer index, Boxed stored, LBasicBlock notFound);
    Operand readElementInsideLength(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, LBasicBlock isHole, std::optional<Representation> = std::nullopt);
    void branchOnPresentElement(const ValueContext& test, const ArrayStorageForm&, BranchTargets);
    void branchOnPresentElementInAnyStorage(const ValueContext& test, BranchTargets);

    Condition hasDoubleOrInt32Storage(CellPointer array);
    void branchOnPresentElementInDoubleOrInt32Storage(const ValueContext& test, BranchTargets);

    void appendElement(const ValueContext& store, CellPointer array, ValueID element);
    Boxed appendOutOfLine(const OperationContext&, CellPointer array, ValueID element);
    Condition tryStoreElement(const ValueContext& store, CellPointer array, Integer index, ValueID stored, bool leavesStorageAsItIs = false, std::optional<Pointer> profileOfSite = std::nullopt);
    void widenStorageOfIntegers(CellPointer array, const ArrayStorageForm& wider, LBasicBlock isLeftToOthers);
    void turnIntegersIntoFractions(CellPointer array, const ArrayElements&, LBasicBlock isLeftToOthers);

    TypedArrayElements typedArrayElementsOf(CellPointer view, LBasicBlock isNotFixedLength);
    TypedArrayElements typedArrayElementsOf(ValueID view, LBasicBlock isNotFixedLength);
    Operand loadTypedArrayElement(const TypedArrayElements&, Integer index, TypedArrayType);
    void storeTypedArrayElement(const TypedArrayElements&, Integer index, TypedArrayType, Operand stored);
    void forEachKindOfView(const ValueContext&, CellPointer view, NOESCAPE const ScopedLambda<void(TypedArrayType)>& inArm);

private:
    Pointer bytesInsideDataView(const ValueContext&);
    void readWithKnownStorage(const ValueContext&, const ArrayStorageForm&);
    void readInsideLength(const ValueContext&, const ArrayStorageForm&);
    void readWithAnyStorage(const ValueContext&);
    bool hasOneArmInLine(const ValueContext& read);
    void readWithOneArmInLine(const ValueContext&, const ArrayStorageForm* knownForm);
    void readFromDoubleOrInt32Storage(const ValueContext&);
    bool isReckonedWithFraction(ValueID read) const;
    void readTruncatedNumber(const ValueContext&, const ArrayElements&, std::optional<Integer> index);
    Operand numberOfInt32Element(const ValueContext&, Boxed element);
    ValueID findFusedRangeTest(const ValueContext& test, ValueID& read);
    bool tryFusePresenceTest(const ValueContext& test, const ArrayStorageForm&);
    Operand readNumberInsideLength(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, LBasicBlock isHole, std::optional<Representation> = std::nullopt);
    Boxed readOutOfLine(const ValueContext&, CellPointer array);
    bool findsElement(const ValueContext& read);
    bool leavesIterationAtHole(const ValueContext& read, std::optional<Representation>);
    Boxed readBoxedElement(const ValueContext&, const ArrayElements&, Integer index, LBasicBlock isAbsent);

    void storeInsideLength(const ValueContext&, const ArrayStorageForm&);
    void replaceElement(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, Operand rawElement);
    enum class NumberBoxing : bool { InLine, ByStub };
    Boxed boxForStub(const OperationContext&, Operand, const Type&, NumberBoxing);
    void storeOutOfLine(const ValueContext&, CellPointer array, NumberBoxing);
    Boxed appendOutOfLine(const OperationContext&, CellPointer array, ValueID element, NumberBoxing);
    void storeInVector(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, Operand rawElement, LBasicBlock isBeyondVector);
    std::optional<int64_t> lengthFilledByLoop(const ValueContext& store);
    void requireRoomInVector(const ValueContext& store, const ArrayElements&, Integer index, LBasicBlock isBeyondVector);
    void appendInVector(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, Operand rawElement, LBasicBlock isBeyondVector);

    void holdPositionInsideList(const ValueContext& access);
    void holdPositionInsideView(const ValueContext& access, Integer index, const TypedArrayElements&);
public:
    TypedPointer doubleElement(LValue storage, LValue index) { return elementOfStorage(ArrayStorage::Double, storage, index); }
    TypedPointer elementOfStorage(ArrayStorage, LValue storage, LValue index);
private:
    Integer indexOfPresenceTest(const ValueContext& test, LBasicBlock isNotAnIndex);
    struct TestedElement {
        Integer index;
        std::optional<Operand> raw;
    };
    TestedElement branchOnElementPresence(const ValueContext&, const ArrayStorageForm*, const ArrayElements&, Integer index, BranchTargets);
    ValueID findReadAfterPresenceTest(const ValueContext& test);
    TypedArrayElements typedArrayElementsInsideView(const ValueContext&, CellPointer view);
    TypedPointer wordOfElementPair(const ValueContext& access, bool isUpper);
    std::optional<Operand> readElementOfPair(const ValueContext&);
    bool storeElementOfPair(const ValueContext&);
public:
    TypedPointer elementInsideView(const ValueContext& access);
    TypedPointer elementBelowNamedLength(const ValueContext& access);
private:
    std::optional<TypedArrayElements> typedArrayElementsNamedBy(const ValueContext&, CellPointer view);
    std::optional<TypedArrayElements> findElementsReadBefore(const ValueContext& access, CellPointer view);
    void askViewWherePositionIsNotBelowNamedLength(LBasicBlock isNotBelow, const ScopeFork&, const ValueContext& access, CellPointer view, Integer index, LBasicBlock outside, LBasicBlock otherwise);
    CellPointer viewOnSlowPath(const ValueContext& access, CellPointer view);
    bool becomesIndexWithoutTest(const ValueContext& access);
    Operand loadBytesOfTypedArrayElement(const TypedArrayElements&, Integer index, TypedArrayType);
    Word32 clampedToUint8(Integer);
    Word32 clampedToUint8(Word32);
    Word32 clampedToUint8(Number);
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
