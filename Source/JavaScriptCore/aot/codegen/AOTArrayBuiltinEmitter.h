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
    bool resultGetsNamedProperties(const InlineCall&);
    void generateArraySlice(const InlineCall&);
    void generateArraySplice(const InlineCall&);
    void generateArraySplicePrefix(const InlineCall&);
    void generateArrayUnshift(const InlineCall&);
    void generateArrayIterator(const InlineCall&);

private:
    static constexpr size_t maxConcatenatedArrays = 1 + maxInlineConcatOperands;

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

    struct RemovedPrefix {
        Integer length;
        Vector<Boxed, 2> elements;
    };

    using RawItems = Vector<Operand, maxInlineSpliceInsertions>;
    // The functor ends the block it is called in.
    using SpliceArm = ScopedLambda<void(const InlineSplice&, const ArrayStorageForm&)>;

    ElementAccessEmitter& elements() { return emitter<ElementAccessEmitter>(); }
    bool checkChosenForm(const InlineCall&, bool isHandled);
    CellPointer receiverOf(const InlineCall& call) { return in<CellPointer>(call.receiver()); }
    Operand resultFromRawElement(const InlineCall&, const ArrayStorageForm&, Operand rawElement);
    Operand undefinedResult(const InlineCall&);
    ArrayStorageFormList candidateFormsOf(const InlineCall&);
    // The mask is arrayStorageTestMask for a read and IndexingShapeAndWritabilityMask for a store.
    void forEachFormOrMiss(const InlineCall&, CellPointer array, IndexingType mask, const ScopedLambda<void(const ArrayStorageForm&)>&);

    void removeElementAtEnd(const InlineCall&, bool removesFirst);
    void copyElements(const ArrayStorageForm&, const ElementRange& source, const ElementRange& target, Integer count, CopyDirection);
    Operand removeFirstElement(const InlineCall&, const ArrayStorageForm&, const ArrayElements&, LBasicBlock isAbsent);
    bool hasIntegerSpliceRange(const InlineCall&);
    std::optional<int64_t> deleteCountOfInlineSplice(const InlineCall&);
    Integer wholeNumberOrElse(ValueID, LBasicBlock isNotWhole);
    void spliceInline(const InlineCall&, Integer start, int64_t removed, LBasicBlock outOfLine, const SpliceArm&);
    // nullopt: an item never fits the form, and the block has ended.
    std::optional<RawItems> rawItemsToInsert(const InlineCall&, const ArrayStorageForm&, LBasicBlock doesNotFit);
    void replaceElements(const InlineCall&, const InlineSplice&, const ArrayStorageForm&, std::span<const Operand> inserted);
    CellPointer copyRemovedElements(const InlineCall&, const InlineSplice&, const ArrayStorageForm&);
    RemovedPrefix loadRemovedPrefix(const InlineSplice&, const ArrayStorageForm&, unsigned count);
    // Leaves for `otherwise` where the range does not fit the operation, or the operation does not move the elements of such an array.
    Boxed spliceByOperation(const InlineCall&, LBasicBlock otherwise, const CallOptions&);
    RemovedPrefix readRemovedPrefix(const InlineCall&, Boxed removedElements, unsigned count);
    Integer indexOfNumber(const InlineCall&, CellPointer array);
    // What the search takes a hole for: indexOf() passes it over, includes() reads undefined there.
    enum class HoleIs : bool { Nothing, Undefined };
    Integer indexOfValueComparedByIdentity(const InlineCall&, CellPointer array, HoleIs);
    Integer indexOfSearched(const InlineCall&, HoleIs);
    // The answer for a short vector goes to `found`. The code behind it is reached by a longer one, with nothing compared. Compact code scans nothing.
    void scanShortVector(const InlineCall&, const ArrayStorageForm&, CellPointer array, Operand searchedRawElement, Join<Integer>& found);
};

static_assert(sizeof(ArrayBuiltinEmitter) == sizeof(OperationEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
