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

// The collector reaches the elements through the array alone.
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

    // TestingSwitch::TestsEveryPosition, --aot-harden=ignore-told-ranges. What the walk has found out about a position is held against the length that the list, the view or the text has where the access stands.
    // The process stops where it is not so: operationAOTStopAtFalseProof().
    bool testsEveryPosition();
    void stopUnlessInside(const ValueContext& access, Condition isInside);

    Condition isPristineArray(CellPointer, OptionSet<PristineAspects>);
    Condition isPristineInstance(CellPointer, OptionSet<PristineAspects>);
    Condition hasArrayStorage(CellPointer array, ArrayStorage, bool isWritable = false);
    Word32 indexingMode(CellPointer array, IndexingType mask);
    Condition hasShape(Word32 maskedMode, const ArrayStorageForm& form) { return equal(maskedMode, word32(form.shape)); }
    // Storage of int32 and contiguous storage both hold boxed values, with 0 for a hole.
    Condition holdsBoxedElements(Word32 maskedMode);
    Condition holdsBoxedElements(CellPointer array);
    Condition hasWritableShape(Word32 modeWithWritability, const ArrayStorageForm& form) { return equal(modeWithWritability, word32(form.indexingType)); }

    ArrayElements elementsOf(CellPointer array) { return { array, loadField<Pointer>(array, m_heaps.JSArray_butterfly) }; }
    // Of an access that may name its storage (`storage` in the schema). Where it does, nothing is read.
    ArrayElements elementsOf(const ValueContext& access, CellPointer array);
    Integer publicLength(const ArrayElements& elements) { return zeroExtended(loadField<Word32>(elements.butterfly, m_heaps.Butterfly_publicLength)); }
    Integer vectorLength(const ArrayElements& elements) { return zeroExtended(loadField<Word32>(elements.butterfly, m_heaps.Butterfly_vectorLength)); }
    void setPublicLength(const ArrayElements& elements, Integer length) { storeField(elements.butterfly, m_heaps.Butterfly_publicLength, low32(length)); }
    Integer arrayLength(CellPointer array) { return publicLength(elementsOf(array)); }

    TypedPointer elementAt(const ArrayStorageForm&, const ArrayElements&, Integer index);
    TypedPointer elementOfUnknownForm(const ArrayElements&, Integer index);
    Operand loadRawElement(const ArrayStorageForm&, const ArrayElements&, Integer index);
    // Of storage that holdsBoxedElements().
    Boxed loadBoxedElement(const ArrayElements&, Integer index);
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
    // Of a store that names no kind of storage: the one kind that a value in the representation of the stored one is expected to go into.
    ArrayStorageFormList formExpectedToHold(ValueID list, ValueID stored);
    // The value is a proven number, and numbers alone are written of the elements of the list.
    bool goesByKindOfListOfNumbers(ValueID list, ValueID stored);
    // Where goesByKindOfListOfNumbers(): storage for int32 and for doubles, and behind them, where it is asked for, storage for any value.
    ArrayStorageFormList formsOfListOfNumbersFor(ValueID stored, bool hasArmForEveryKind);
    // Of a store that names no kind of storage and comes again and again: every kind that may take the value.
    ArrayStorageFormList formsThatMayHold(ValueID list, ValueID stored);
    // A PutArrayElement that tests its own guess where storeTakesIndexOfProvenList().
    void storeInProvenList(const ValueContext&, const ArrayStorageFormList&);
    // A PutArrayElement or an AppendElement outside the loops of the unit: the way that is expected has code in line, and a stub takes every other way.
    bool hasOnlyExpectedArmInLine(const ValueContext& store);
    // hasShape or hasWritableShape, whichever the mode was read for.
    using ShapeTest = Condition (ElementAccessEmitter::*)(Word32 mode, const ArrayStorageForm&);
    // The functor ends the block it is called in. An array of none of the forms goes to hasNoForm.
    enum class FirstForm : bool { IsExpected, IsLikeTheOthers };
    void forEachFormByShape(Word32 mode, ShapeTest, const ArrayStorageFormList&, LBasicBlock hasNoForm, const ScopedLambda<void(const ArrayStorageForm&)>&, FirstForm = FirstForm::IsExpected);

    // The result may be negative or huge: compare it unsigned with a length before any use.
    enum class NaNIndexMode : bool { TakeSlowPath, ProduceOutOfBoundsIndex };
    Integer indexOrElse(ValueID position, bool keyIsIntegral, LBasicBlock isNotAnIndex, NaNIndexMode = NaNIndexMode::TakeSlowPath);
    Integer indexInsideLength(ValueID position);
    // Of storage that has a form. The index of the result is the position only where the test has passed.
    LengthTest testLength(Integer index, const ArrayElements&);
    LengthTest testLength(const ValueContext&, Integer index, const ArrayElements&);
    Boxed boxedPosition(ValueID position);

    Operand readElement(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, LBasicBlock isAbsent);
    // Of an ElemGet by position of an object of one layout, such as a name of an enum by its value. The elements of such an object lie in its extra properties, and the layout says where those are.
    const PlacedSlot* extraPropertiesWordOfReadByPosition(const ValueContext& read);
    // What is not found there goes to notFound: it may be up the chain, and it may be of any other kind of storage.
    Boxed readElementOfExtraProperties(CellPointer object, const PlacedSlot& extraPropertiesWord, Integer index, LBasicBlock notFound);
    Operand readElementInsideLength(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, LBasicBlock isHole);
    // Taken if the array has an element of its own at the position. A position that is no index has none.
    void branchOnPresentElement(const ValueContext& test, const ArrayStorageForm&, BranchTargets);
    void branchOnPresentElementInAnyStorage(const ValueContext& test, BranchTargets);

    // A list of numbers that has Double or Int32 storage: what HasArrayStorage, IsElementPresent and GetArrayElement do with admitsInt32Storage.
    Condition hasDoubleOrInt32Storage(CellPointer array);
    void branchOnPresentElementInDoubleOrInt32Storage(const ValueContext& test, BranchTargets);

    void appendElement(const ValueContext& store, CellPointer array, ValueID element);
    // The result is the new length.
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
    bool replacesBoxedInt32(const ValueContext& store, const ArrayStorageForm&);
    // ByStub: a call in place of a test and two arms, for a block that seldom runs.
    enum class NumberBoxing : bool { InLine, ByStub };
    Boxed boxForStub(const OperationContext&, Operand, const Type&, NumberBoxing);
    void storeOutOfLine(const ValueContext&, CellPointer array, NumberBoxing);
    Boxed appendOutOfLine(const OperationContext&, CellPointer array, ValueID element, NumberBoxing);
    void storeInVector(const ValueContext&, const ArrayStorageForm&, const ArrayElements&, Integer index, Operand rawElement, LBasicBlock isBeyondVector);
    // AppendElement::lengthOperands().
    std::optional<Integer> integerNamedByAppend(const ValueContext& store, size_t index);
    Integer lengthNamedOrRead(const ValueContext& store, const ArrayElements&);
    void branchUnlessHasRoomBehindLength(const ValueContext& store, const ArrayElements&, Integer length, LBasicBlock isFull);

    // Of an access to a list that is marked as needing no test of its position. It counts the access, or builds the test.
    void holdPositionInsideList(const ValueContext& access);
    void holdPositionInsideView(const ValueContext& access, Integer index, const TypedArrayElements&);
    // Of IsElementPresent. With isInBounds nothing is tested: the position is an index.
    Integer indexOfPresenceTest(const ValueContext& test, LBasicBlock isNotAnIndex);
    // No form: storage that holdsBoxedElements().
    void branchOnElementPresence(const ValueContext&, const ArrayStorageForm*, const ArrayElements&, Integer index, BranchTargets);
    TypedArrayElements typedArrayElementsInsideView(const ValueContext&, CellPointer view);
    // A number with whatever NaN the bytes spell.
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
