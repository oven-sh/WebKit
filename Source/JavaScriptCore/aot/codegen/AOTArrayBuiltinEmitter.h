/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTInlineCall.h"
#include "AOTResultRules.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class ArrayBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit ArrayBuiltinEmitter(CodeGenerator& generator) : BuiltinFamilyEmitter(generator) { }

    void generateArrayAt(const InlineCall&);
    void generateArrayIndexOf(const InlineCall&);
    void generateArrayIncludes(const InlineCall&);
    void generateArrayJoin(const InlineCall&);
    void generateArrayConcat(const InlineCall&);
    void generateArrayShift(const InlineCall& call) { removeElementAtEnd(call, true); }
    void generateArrayPop(const InlineCall& call) { removeElementAtEnd(call, false); }
    void generateArraySlice(const InlineCall&);
    // `list.slice()` as a closed call whose answer is stored into by name: the copy of a list that is as made, born with room for its first named properties. Whatever else is the case leaves for `closed` with nothing done.
    Boxed copyWithPropertyRoomInFrontOfClosedCall(const OperationContext&, ValueID list, LBasicBlock closed);
    void generateArraySplice(const InlineCall&);
    void generateArrayUnshift(const InlineCall&);
    void generateArrayWith(const InlineCall&);
    void generateArrayToReversed(const InlineCall&);
    void generateArrayToSpliced(const InlineCall&);
    void generateArrayIterator(const InlineCall&);

private:
    static constexpr size_t maxConcatenatedArrays = 1 + maxInlineConcatOperands;
    // More elements than these are moved, or looked through, by a function that works on memory.
    static constexpr int64_t maxLengthForElementwiseMove = 32;
    static constexpr int64_t maxInlineSearchedElements = 16;

    enum class CopyDirection : bool { Forward, Backward };

    struct ElementRange {
        ArrayElements elements;
        Integer first;
    };

    struct InlineSplice {
        ArrayElements elements;
        Integer from;
        Integer tail;
        Integer newLength;
        int64_t removed;
    };

    using RawItems = Vector<Operand, maxInlineSpliceInsertions>;
    // The functor ends the block it is called in.
    using SpliceArm = ScopedLambda<void(const InlineSplice&, const ArrayStorageForm&)>;

    ElementAccessEmitter& elements() { return emitter<ElementAccessEmitter>(); }
    bool checkChosenForm(const InlineCall&, bool isHandled);
    CellPointer receiverOf(const InlineCall& call) { return in<CellPointer>(call.receiver()); }
    Operand resultFromRawElement(const InlineCall&, const ArrayStorageForm&, Operand rawElement);
    Operand undefinedResult(const InlineCall&);
    // The one kind that Facts names for every array that a value of the type may be, which is the kind that their creators give them. Other: there is none.
    ArrayStorage storageNamedByFactsFor(const Type& array);
    // That kind, where the value is proven to be one of those arrays and Facts has decided that they keep it.
    const ArrayStorageForm* provenFormOf(ValueID array);
    // By the stamp of the call, then by Facts.
    const ArrayStorageForm* provenFormOf(const InlineCall&);
    const ArrayStorageForm* provenFormOfArgument(const InlineCall&, ValueID list);
    TypeID elementTypeOf(const InlineCall&, TypeLevel);
    ArrayStorageFormList candidateFormsOf(const InlineCall&);
    ElementAccessEmitter::FirstForm firstCandidateFormOf(const InlineCall&);
    // The mask is arrayStorageTestMask for a read and IndexingShapeAndWritabilityMask for a store.
    void forEachFormOrMiss(const InlineCall&, CellPointer array, IndexingType mask, const ScopedLambda<void(const ArrayStorageForm&)>&);

    void removeElementAtEnd(const InlineCall&, bool removesFirst);
    Operand removeFirstElement(const InlineCall&, const ArrayStorageForm&, const ArrayElements&, LBasicBlock isAbsent);

    // Both say what to make ready for, and neither changes what the program sees: Facts knows of the arrays that the call makes what is done to them anywhere.
    bool resultGetsNamedProperties(const InlineCall&);
    bool resultIsExpectedToHoldDoubles(const InlineCall&);
    const ArrayStorageForm& formOfCopy(const InlineCall&, const ArrayStorageForm& formOfReceiver);
    // No: Facts has decided on another kind for the arrays that the call makes, which is then read without a test. The function behind the call makes that kind.
    bool mayMakeArrayOf(const InlineCall&, ArrayStorage);

    void copyElements(const ArrayStorageForm&, const ElementRange& source, const ElementRange& target, Integer count, CopyDirection);
    void copyElements(const ArrayStorageForm&, const ArrayStorageForm& formOfTarget, const ElementRange& source, const ElementRange& target, Integer count, CopyDirection, LBasicBlock ifHole = nullptr, bool reverses = false);
    using LengthOfCopy = ScopedLambda<Integer(Integer lengthOfReceiver)>;
    // It may miss: until it is done the new array has no length, and holes alone.
    using FillOfCopy = ScopedLambda<void(const ArrayStorageForm&, const ArrayStorageForm& made, ArrayElements source, ArrayElements target)>;
    void copyIntoNewArray(const InlineCall&, const LengthOfCopy&, const FillOfCopy&);
    Integer clampToLength(Integer relative, Integer length);
    void storeItem(const InlineCall&, const ArrayStorageForm&, ArrayElements target, Integer position, ValueID item);

    bool hasNumberSpliceRange(const InlineCall&);
    std::optional<int64_t> deleteCountOfInlineSplice(const InlineCall&);
    Integer wholeNumberOrElse(ValueID, LBasicBlock isNotWhole);
    void spliceInline(const InlineCall&, Integer start, int64_t removed, LBasicBlock outOfLine, const SpliceArm&);
    // nullopt: an item never fits the form, and the block has ended.
    std::optional<RawItems> rawItemsToInsert(const InlineCall&, const ArrayStorageForm&, LBasicBlock doesNotFit);
    void replaceElements(const InlineCall&, const InlineSplice&, const ArrayStorageForm&, std::span<const Operand> inserted);
    CellPointer copyRemovedElements(const InlineCall&, const InlineSplice&, const ArrayStorageForm&);
    // Leaves for `otherwise` where the range does not fit the operation, or the operation does not move the elements of such an array.
    Boxed spliceByOperation(const InlineCall&, LBasicBlock otherwise, const CallOptions&);

    Integer indexOfNumber(const InlineCall&, CellPointer array);
    // What the search takes a hole for: indexOf() passes it over, includes() reads undefined there.
    enum class HoleIs : bool { Nothing, Undefined };
    Integer indexOfValueComparedByIdentity(const InlineCall&, CellPointer array, HoleIs);
    Integer indexOfString(const InlineCall&, CellPointer array, CellPointer searched);
    bool handlesSearch(const InlineCall&);
    Integer indexOfSearched(const InlineCall&, HoleIs);
    // The answer for a short vector goes to `found`. The code behind it is reached by a longer one, with nothing compared.
    void scanShortVector(const ArrayStorageForm&, CellPointer array, Operand searchedRawElement, Join<Integer>& found);
};

static_assert(sizeof(ArrayBuiltinEmitter) == sizeof(OperationEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
