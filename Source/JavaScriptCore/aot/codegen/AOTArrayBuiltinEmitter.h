/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrayRules.h"
#include "AOTBuiltinFamilyEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTInlineCall.h"
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class ArrayBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit ArrayBuiltinEmitter(CodeGenerator&);

    void generateArrayAt(const InlineCall&);
    void generateArrayIndexOf(const InlineCall&);
    void generateArrayIncludes(const InlineCall&);
    void generateArrayJoin(const InlineCall&);
    void generateArrayConcat(const InlineCall&);
    void generateArrayShift(const InlineCall&);
    void generateArrayPop(const InlineCall&);
    bool resultGetsNamedProperties(const InlineCall&);
    void generateArraySlice(const InlineCall&);
    void generateArraySplice(const InlineCall&);
    void generateArraySplicePrefix(const InlineCall&);
    void generateArrayUnshift(const InlineCall&);
    void generateArrayIterator(const InlineCall&);

private:
    static constexpr size_t maxConcatenatedArrays = 1 + maxInlineConcatOperands;

    struct ConcatOperands {
        Vector<CellPointer, maxConcatenatedArrays> arrays;
        Vector<ArrayElements, maxConcatenatedArrays> elements;
        Vector<Integer, maxConcatenatedArrays> lengths;
        Integer length;
    };

    struct ConcatTarget {
        Pointer storage;
        Integer capacity;
        ArrayElements elements;
    };

    struct InlineSplice {
        ArrayElements elements;
        Integer from;
        Integer tail;
        Integer newLength;
        int64_t removed;
    };

    struct RemovedPrefix {
        Integer length;
        Vector<Boxed, 2> elements;
    };

    using RawItems = Vector<Operand, maxInlineSpliceInsertions>;
    // The functor ends the block it is called in.
    using SpliceArm = ScopedLambda<void(const InlineSplice&, const ArrayStorageForm&)>;

    bool checkChosenForm(const InlineCall&, bool isHandled);
    CellPointer receiverOf(const InlineCall&);
    Operand resultFromRawElement(const InlineCall&, const ArrayStorageForm&, Operand rawElement);
    Operand undefinedResult(const InlineCall&);
    ArrayStorageFormList candidateFormsOf(const InlineCall&);
    void forEachReadableFormOrMiss(const InlineCall&, CellPointer array, const ScopedLambda<void(const ArrayStorageForm&)>&);
    void forEachWritableFormOrMiss(const InlineCall&, CellPointer array, const ScopedLambda<void(const ArrayStorageForm&)>&);

    CellPointer joinOrMiss(const InlineCall&);
    ConcatOperands collectConcatOperands(const InlineCall&, const ArrayStorageForm&);
    void copyConcatenatedElements(const InlineCall&, const ConcatOperands&, const ConcatTarget&, const ArrayStorageForm&);
    Operand removeFirstElement(const InlineCall&, const ArrayStorageForm&, const ArrayElements&, LBasicBlock isAbsent);
    bool hasIntegerSpliceRange(const InlineCall&);
    std::optional<int64_t> deleteCountOfInlineSplice(const InlineCall&);
    Integer wholeNumberOrElse(ValueID, LBasicBlock isNotWhole);
    void spliceInline(const InlineCall&, Integer start, int64_t removed, LBasicBlock outOfLine, const SpliceArm&);
    void spliceElements(const InlineCall&, OperandJoin&, const InlineSplice&, const ArrayStorageForm&, LBasicBlock outOfLine);
    // nullopt: an item never fits the form, and the block has ended.
    std::optional<RawItems> rawItemsToInsert(const InlineCall&, const ArrayStorageForm&, LBasicBlock doesNotFit);
    void replaceElements(const InlineCall&, const InlineSplice&, const ArrayStorageForm&, std::span<const Operand> inserted);
    CellPointer copyRemovedElements(const InlineCall&, const InlineSplice&, const ArrayStorageForm&);
    RemovedPrefix loadRemovedPrefix(const InlineSplice&, const ArrayStorageForm&, unsigned count);
    // Leaves for `otherwise` where the range does not fit the operation, or the operation does not move the elements of such an array.
    Boxed spliceByOperation(const InlineCall&, LBasicBlock otherwise, const CallOptions&);
    Boxed spliceByFunction(const InlineCall&);
    RemovedPrefix readRemovedPrefix(const InlineCall&, Boxed removedElements, unsigned count);
    Integer indexOfNumber(const InlineCall&, CellPointer array);
    // What the search takes a hole for: indexOf() passes it over, includes() reads undefined there.
    enum class HoleIs : bool { Nothing, Undefined };
    Integer indexOfValueComparedByIdentity(const InlineCall&, CellPointer array, HoleIs);
    Integer indexOfSearched(const InlineCall&, HoleIs);
    // Ends the block it is called in. The answer for a short vector goes to `found`, a longer one goes to `isLong` with nothing compared.
    void scanShortVector(const ArrayStorageForm&, const ArrayElements&, Operand searchedRawElement, Join<Integer>& found, LBasicBlock isLong);
};

static_assert(sizeof(ArrayBuiltinEmitter) == sizeof(OperationEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
