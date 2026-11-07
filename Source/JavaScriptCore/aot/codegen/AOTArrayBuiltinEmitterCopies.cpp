/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTBarrierEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

void ArrayBuiltinEmitter::copyElements(const ArrayStorageForm& form, const ElementRange& source, const ElementRange& target, Integer count, CopyDirection direction)
{
    copyElements(form, form, source, target, count, direction);
}

void ArrayBuiltinEmitter::copyElements(const ArrayStorageForm& form, const ArrayStorageForm& formOfTarget, const ElementRange& source, const ElementRange& target, Integer count, CopyDirection direction, LBasicBlock ifHole, bool reverses)
{
    bool isBackward = direction == CopyDirection::Backward;
    LBasicBlock loop = newBlock();
    LBasicBlock element = newBlock();
    LBasicBlock copied = newBlock();
    ValueFromBlock start = m_out.anchor((isBackward ? count : integer(0)).value());
    m_out.jump(loop);

    m_out.appendTo(loop);
    Integer position(m_out.phi(Machine::Int64, start));
    branch(isBackward ? isNotZero(position) : below(position, count), { normalEdge(element), normalEdge(copied) });

    m_out.appendTo(element);
    Integer index = isBackward ? sub(position, integer(1)) : position;
    Operand rawElement = elements().loadRawElement(form, source.elements, add(source.first, index));
    if (ifHole)
        branchUnless(logicalNot(elements().isHole(form, rawElement)), ifHole);
    if (formOfTarget.kind == ArrayStorage::Double && form.kind != formOfTarget.kind) {
        RELEASE_ASSERT(form.kind == ArrayStorage::Int32);
        Number number = toNumber(unboxInt32Element(rawElement.as<Boxed>()));
        rawElement = ifHole ? number : select(elements().isHole(form, rawElement), elements().rawHole(formOfTarget).as<Number>(), number);
    } else if (form.kind == ArrayStorage::Double && form.kind != formOfTarget.kind) {
        RELEASE_ASSERT(formOfTarget.kind == ArrayStorage::Contiguous);
        Boxed boxed = elements().boxRawElement(form, rawElement);
        rawElement = ifHole ? boxed : select(elements().isHole(form, rawElement), elements().rawHole(formOfTarget).as<Boxed>(), boxed);
    } else
        RELEASE_ASSERT(form.kind == formOfTarget.kind || (form.kind == ArrayStorage::Int32 && formOfTarget.kind == ArrayStorage::Contiguous));
    elements().storeRawElement(formOfTarget, target.elements, add(target.first, reverses ? sub(sub(count, integer(1)), index) : index), rawElement);
    m_out.addIncomingToPhi(position.value(), m_out.anchor((isBackward ? index : add(position, integer(1))).value()));
    m_out.jump(loop);

    m_out.appendTo(copied);
}

static std::span<const ValueID> itemsInsertedBySplice(const InlineCall& call)
{
    return call.numberOfArguments() > 2 ? call.arguments().subspan(2) : std::span<const ValueID> { };
}

void ArrayBuiltinEmitter::generateArrayConcat(const InlineCall& call)
{
    bool areElementsOrArrays = std::ranges::all_of(call.arguments(), [&](ValueID argument) {
        return !typeOf(argument).hasObjects() || types().isSubset(typeIDOf(argument), types().builtinClass(BuiltinClasses::Array));
    });
    if (!checkChosenForm(call, call.canMiss() && areElementsOrArrays && call.numberOfArguments() <= maxInlineConcatOperands))
        return;

    ArrayAllocationEmitter allocation(generator());
    LBasicBlock miss = missBlockOf(call);
    Join<CellPointer> join(m_out);
    bool isExpectedToHoldDoubles = resultIsExpectedToHoldDoubles(call);
    auto knownFormOf = [&](ValueID list) -> const ArrayStorageForm* {
        ValueID written = definitionOf(list);
        const Operation& literal = operationAt(written);
        if (literal.opcode != Opcode::NewArray || literal.elementsOperands().empty() || !literal.holes().empty())
            return nullptr;
        if (const ArrayStorageForm* named = findArrayStorageForm(literal.arrayStorage()))
            return named;
        return &allocation.formOfLiteral(call.context(), literal.elementsOperands(), typeOf(written));
    };
    auto concatenate = [&](const ArrayStorageForm& form) {
        Vector<CellPointer, maxConcatenatedArrays> arrays { receiverOf(call) };
        Vector<const ArrayStorageForm*, maxConcatenatedArrays> forms { &form };
        const ArrayStorageForm* widest = &form;
        for (ValueID argument : call.arguments()) {
            if (!typeOf(argument).hasObjects()) {
                arrays.append(CellPointer());
                forms.append(nullptr);
                continue;
            }
            CellPointer array = cellOperand(argument);
            const ArrayStorageForm* known = knownFormOf(argument);
            forms.append(known ? known : &form);
            missUnless(call, elements().isPristineArray(array, { PristineAspects::Storage, PristineAspects::Names }));
            missUnless(call, elements().hasShape(elements().indexingMode(array, arrayStorageTestMask), *forms.last()));
            arrays.append(array);
            if (forms.last()->shape > widest->shape)
                widest = forms.last();
        }
        const ArrayStorageForm& made = isExpectedToHoldDoubles && widest->kind == ArrayStorage::Int32 ? arrayStorageFormOf(ArrayStorage::Double) : *widest;

        Vector<ArrayElements, maxConcatenatedArrays> sources;
        Vector<Integer, maxConcatenatedArrays> lengths;
        Integer length = integer(0);
        for (CellPointer array : arrays) {
            sources.append(array ? elements().elementsOf(array) : ArrayElements { });
            lengths.append(array ? elements().publicLength(sources.last()) : integer(1));
            length = add(length, lengths.last());
        }
        missUnless(call, belowOrEqual(length, integer(maxLengthForElementwiseCopy)));

        Vector<std::optional<Operand>, maxConcatenatedArrays> rawItems;
        for (unsigned i = 0; i < arrays.size(); ++i) {
            rawItems.append(arrays[i] ? std::nullopt : elements().rawElementToStore(made, call.argument(i - 1), miss));
            if (!arrays[i] && !rawItems.last()) {
                m_out.jump(miss);
                m_out.appendTo(newBlock());
            }
        }
        CellPointer concatenated = allocation.allocateArrayOfHoles(call.context(), length, made, miss, ArrayAllocationEmitter::ElementsBelowLength::NotCleared);
        ArrayElements target = elements().elementsOf(concatenated);
        {
            CodeGenerator::InitializationScope scope(generator());
            Integer position = integer(0);
            for (unsigned i = 0; i < arrays.size(); ++i) {
                if (arrays[i])
                    copyElements(*forms[i], made, { sources[i], integer(0) }, { target, position }, lengths[i], CopyDirection::Forward);
                else if (rawItems[i])
                    elements().storeRawElement(made, target, position, *rawItems[i]);
                position = add(position, lengths[i]);
            }
        }
        emitter<BarrierEmitter>().fenceAfterInitialization();
        join.add(concatenated);
    };
    forEachFormOrMiss(call, receiverOf(call), arrayStorageTestMask, concatenate);
    finish(call, join.finish());
}

const ArrayStorageForm& ArrayBuiltinEmitter::formOfCopy(const InlineCall& call, const ArrayStorageForm& formOfReceiver)
{
    if (formOfReceiver.kind != ArrayStorage::Int32)
        return formOfReceiver;
    return resultIsExpectedToHoldDoubles(call) ? arrayStorageFormOf(ArrayStorage::Double) : formOfReceiver;
}

void ArrayBuiltinEmitter::copyIntoNewArray(const InlineCall& call, const LengthOfCopy& lengthOfCopy, const FillOfCopy& fill)
{
    ArrayAllocationEmitter allocation(generator());
    Join<CellPointer> join(m_out);
    forEachFormOrMiss(call, receiverOf(call), arrayStorageTestMask, [&](const ArrayStorageForm& form) {
        const ArrayStorageForm& made = formOfCopy(call, form);
        ArrayElements source = elements().elementsOf(receiverOf(call));
        Integer length = lengthOfCopy(elements().publicLength(source));
        missUnless(call, belowOrEqual(length, integer(maxLengthForElementwiseCopy)));
        CellPointer copy = allocation.allocateArrayOfHoles(call.context(), length, made, missBlockOf(call), ArrayAllocationEmitter::ElementsBelowLength::Cleared, ArrayAllocationEmitter::LengthIsSetBy::Caller);
        ArrayElements target = elements().elementsOf(copy);
        {
            CodeGenerator::InitializationScope scope(generator());
            fill(form, made, source, target);
            elements().setPublicLength(target, length);
        }
        emitter<BarrierEmitter>().fenceAfterInitialization();
        join.add(copy);
    });
    finish(call, join.finish());
}

Integer ArrayBuiltinEmitter::clampToLength(Integer relative, Integer length)
{
    Integer fromEnd = add(relative, length);
    return select(lessThan(relative, 0), select(lessThan(fromEnd, 0), 0, fromEnd), select(lessThan(relative, length), relative, length));
}

void ArrayBuiltinEmitter::storeItem(const InlineCall& call, const ArrayStorageForm& form, ArrayElements target, Integer position, ValueID item)
{
    if (std::optional<Operand> rawElement = elements().rawElementToStore(form, item, missBlockOf(call))) {
        elements().storeRawElement(form, target, position, *rawElement);
        return;
    }
    m_out.jump(missBlockOf(call));
    m_out.appendTo(newBlock());
}

void ArrayBuiltinEmitter::generateArrayWith(const InlineCall& call)
{
    if (!checkChosenForm(call, call.canMiss() && call.numberOfArguments() == 2 && typeOf(call.argument(0)).isNumber()))
        return;
    LBasicBlock miss = missBlockOf(call);
    Integer relative = wholeNumberOrElse(call.argument(0), miss);
    Integer length;
    Integer position;
    copyIntoNewArray(call, [&](Integer lengthOfReceiver) {
        length = lengthOfReceiver;
        position = select(lessThan(relative, 0), add(relative, length), relative);
        missUnless(call, below(position, length));
        return length;
    }, [&](const ArrayStorageForm& form, const ArrayStorageForm& made, ArrayElements source, ArrayElements target) {
        copyElements(form, made, { source, integer(0) }, { target, integer(0) }, length, CopyDirection::Forward, miss);
        storeItem(call, made, target, position, call.argument(1));
    });
}

void ArrayBuiltinEmitter::generateArrayToReversed(const InlineCall& call)
{
    if (!checkChosenForm(call, call.canMiss() && !call.numberOfArguments()))
        return;
    Integer length;
    copyIntoNewArray(call, [&](Integer lengthOfReceiver) {
        return length = lengthOfReceiver;
    }, [&](const ArrayStorageForm& form, const ArrayStorageForm& made, ArrayElements source, ArrayElements target) {
        copyElements(form, made, { source, integer(0) }, { target, integer(0) }, length, CopyDirection::Forward, missBlockOf(call), true);
    });
}

void ArrayBuiltinEmitter::generateArrayToSpliced(const InlineCall& call)
{
    size_t numberOfRangeArguments = std::min<size_t>(call.numberOfArguments(), 2);
    bool hasNumberRange = std::ranges::all_of(call.arguments().first(numberOfRangeArguments), [&](ValueID bound) {
        return typeOf(bound).isNumber();
    });
    std::span<const ValueID> items = itemsInsertedBySplice(call);
    if (!checkChosenForm(call, call.canMiss() && hasNumberRange && items.size() <= maxInlineSpliceInsertions))
        return;
    LBasicBlock miss = missBlockOf(call);
    Integer start = numberOfRangeArguments ? wholeNumberOrElse(call.argument(0), miss) : integer(0);
    Integer skipCount = numberOfRangeArguments > 1 ? wholeNumberOrElse(call.argument(1), miss) : Integer();
    Integer from;
    Integer behind;
    Integer tail;
    copyIntoNewArray(call, [&](Integer length) {
        from = clampToLength(start, length);
        Integer left = sub(length, from);
        Integer skipped = !numberOfRangeArguments ? integer(0) : !skipCount ? left : select(lessThan(skipCount, 0), 0, select(lessThan(skipCount, left), skipCount, left));
        behind = add(from, skipped);
        tail = sub(length, behind);
        return add(add(from, tail), integer(static_cast<int64_t>(items.size())));
    }, [&](const ArrayStorageForm& form, const ArrayStorageForm& made, ArrayElements source, ArrayElements target) {
        copyElements(form, made, { source, integer(0) }, { target, integer(0) }, from, CopyDirection::Forward, miss);
        Integer position = from;
        for (ValueID item : items) {
            storeItem(call, made, target, position, item);
            position = add(position, integer(1));
        }
        copyElements(form, made, { source, behind }, { target, position }, tail, CopyDirection::Forward, miss);
    });
}

CellPointer ArrayBuiltinEmitter::copyRemovedElements(const InlineCall& call, const InlineSplice& splice, const ArrayStorageForm& form)
{
    static_assert(maxRemovedByInlineSplice <= maxArrayElementsInRegisters);
    auto removedElement = [&](int64_t index) {
        return elements().loadRawElement(form, splice.elements, add(splice.from, integer(index)));
    };
    if (form.element == Representation::Double) {
        Vector<Number, maxRemovedByInlineSplice> numbers;
        for (int64_t i = 0; i < splice.removed; ++i)
            numbers.append(removedElement(i).as<Number>());
        return ArrayAllocationEmitter(generator()).newArrayOfNumbers(call.context(), numbers.span());
    }
    Vector<Boxed, maxRemovedByInlineSplice> values;
    for (int64_t i = 0; i < splice.removed; ++i)
        values.append(removedElement(i).as<Boxed>());
    return emitter<CellAllocationEmitter>().newArrayOf(call.context(), values.span());
}

std::optional<ArrayBuiltinEmitter::RawItems> ArrayBuiltinEmitter::rawItemsToInsert(const InlineCall& call, const ArrayStorageForm& form, LBasicBlock doesNotFit)
{
    RawItems inserted;
    for (ValueID item : itemsInsertedBySplice(call)) {
        std::optional<Operand> rawElement = elements().rawElementToStore(form, item, doesNotFit);
        if (!rawElement) {
            m_out.jump(doesNotFit);
            return std::nullopt;
        }
        inserted.append(*rawElement);
    }
    return inserted;
}

void ArrayBuiltinEmitter::replaceElements(const InlineCall& call, const InlineSplice& splice, const ArrayStorageForm& form, std::span<const Operand> inserted)
{
    CellPointer array = splice.elements.array;
    Integer from = splice.from;
    int64_t added = inserted.size();
    int64_t removed = splice.removed;

    ArrayElements storage = elements().elementsOf(array);
    if (added != removed) {
        CopyDirection direction = added > removed ? CopyDirection::Backward : CopyDirection::Forward;
        Integer tailBefore = add(from, removed);
        copyElements(form, { storage, tailBefore }, { storage, add(from, added) }, splice.tail, direction);
        for (int64_t i = 0; i < removed - added; ++i) {
            Integer position = add(splice.newLength, i);
            elements().storeRawElement(form, storage, position, elements().rawHole(form));
        }
    }
    for (size_t i = 0; i < inserted.size(); ++i)
        elements().storeRawElement(form, storage, add(from, integer(i)), inserted[i]);
    if (added != removed)
        elements().setPublicLength(storage, splice.newLength);
    if (form.holdsCells)
        emitter<BarrierEmitter>().barrier(call.context(), array);
}

void ArrayBuiltinEmitter::spliceInline(const InlineCall& call, Integer start, int64_t removed, LBasicBlock outOfLine, const SpliceArm& emitArm)
{
    CellPointer array = receiverOf(call);
    int64_t added = itemsInsertedBySplice(call).size();
    ArrayElements storage = elements().elementsOf(array);
    Integer length = elements().publicLength(storage);
    Integer from = clampToLength(start, length);
    Integer tail = sub(length, add(from, integer(removed)));
    Integer maxTail = added == removed ? length : integer(maxLengthForElementwiseMove);
    branchUnless(belowOrEqual(tail, maxTail), outOfLine);
    Integer newLength = add(length, integer(added - removed));
    if (added > removed)
        branchUnless(belowOrEqual(newLength, elements().vectorLength(storage)), outOfLine);

    InlineSplice splice { storage, from, tail, newLength, removed };
    Word32 mode = elements().indexingMode(array, IndexingShapeAndWritabilityMask);
    elements().forEachFormByShape(mode, &ElementAccessEmitter::hasWritableShape, candidateFormsOf(call), outOfLine, [&](const ArrayStorageForm& form) {
        emitArm(splice, form);
    }, firstCandidateFormOf(call));
}

bool ArrayBuiltinEmitter::hasNumberSpliceRange(const InlineCall& call)
{
    return call.numberOfArguments() >= 2 && typeOf(call.argument(0)).isNumber() && typeOf(call.argument(1)).isNumber();
}

Integer ArrayBuiltinEmitter::wholeNumberOrElse(ValueID id, LBasicBlock isNotWhole)
{
    if (typeOf(id).isInteger())
        return integerOperand(id);
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Number number = numberOperand(id);
    if (std::optional<Integer> exact = conversions.exactIntegerBehind(number))
        return *exact;
    Integer whole = conversions.toInteger(number);
    branchUnless(equal(toNumber(whole), number), isNotWhole);
    return whole;
}

std::optional<int64_t> ArrayBuiltinEmitter::deleteCountOfInlineSplice(const InlineCall& call)
{
    const Type& deleteCount = typeOf(call.argument(1));
    if (!deleteCount.isInteger() || deleteCount.integers.minimum != deleteCount.integers.maximum)
        return std::nullopt;
    if (deleteCount.integers.minimum < 0 || deleteCount.integers.minimum > maxRemovedByInlineSplice)
        return std::nullopt;
    return deleteCount.integers.minimum;
}

Boxed ArrayBuiltinEmitter::spliceByOperation(const InlineCall& call, LBasicBlock otherwise, const CallOptions& options)
{
    auto int32OrElse = [&](ValueID id) {
        Integer operand = wholeNumberOrElse(id, otherwise);
        if (!typeOf(id).isInt32())
            branchUnless(equal(signExtended(low32(operand)), operand), otherwise);
        return low32(operand);
    };
    Word32 start = int32OrElse(call.argument(0));
    Word32 deleteCount = int32OrElse(call.argument(1));
    Vector<Boxed, maxInlineSpliceInsertions> boxedItems;
    for (ValueID item : itemsInsertedBySplice(call))
        boxedItems.append(boxedOperand(call.context(), item));
    Word32 count = word32(static_cast<int32_t>(boxedItems.size()));
    Boxed removedElements = callOperation<Boxed>(call.context(), RuntimeOperation::ArraySplice, { receiverOf(call), start, deleteCount, count, argumentBuffer(boxedItems.span()) }, options);
    branchUnless(isNotEmpty(removedElements), otherwise);
    return removedElements;
}

void ArrayBuiltinEmitter::generateArraySplice(const InlineCall& call)
{
    if (!checkChosenForm(call, call.canMiss() && hasNumberSpliceRange(call) && itemsInsertedBySplice(call).size() <= maxInlineSpliceInsertions))
        return;

    Representation representation = resultIsRead(call) ? resultRepresentationOf(call) : Representation::Void;
    OperandJoin join(m_out, representation);
    std::optional<int64_t> inlineDeleteCount = deleteCountOfInlineSplice(call);
    if (inlineDeleteCount) {
        LBasicBlock outOfLine = newBlock();
        spliceInline(call, wholeNumberOrElse(call.argument(0), outOfLine), *inlineDeleteCount, outOfLine, [&](const InlineSplice& splice, const ArrayStorageForm& form) {
            std::optional<RawItems> inserted = rawItemsToInsert(call, form, outOfLine);
            if (!inserted)
                return;

            Operand removedElements;
            if (representation != Representation::Void) {
                for (int64_t i = 0; i < splice.removed; ++i) {
                    Operand rawElement = elements().loadRawElement(form, splice.elements, add(splice.from, integer(i)));
                    branchUnless(logicalNot(elements().isHole(form, rawElement)), outOfLine);
                }
                removedElements = inRepresentation(call.context(), representation, copyRemovedElements(call, splice, form), resultTypeOf(call));
            }
            replaceElements(call, splice, form, inserted->span());
            join.add(removedElements);
        });
        m_out.appendTo(outOfLine);
    }

    Boxed removedElements = spliceByOperation(call, missBlockOf(call), { .isOnRarePath = inlineDeleteCount.has_value() });
    join.add(inRepresentation(call.context(), representation, removedElements, resultTypeOf(call)));
    finishInlineCall(call, join.finish());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
