/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBarrierEmitter.h"
#include "AOTBuiltinCallEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTFunctionAnalyses.h"
#include "AOTKnownValue.h"
#include "IndexingHeader.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static_assert(sizeof(IndexingHeader) == sizeof(EncodedJSValue));

namespace {

enum class CopyDirection : uint8_t { Forward, Backward };

struct ElementRange {
    ArrayElements elements;
    Integer first;
};

class ElementCopier final : public OperationEmitter {
public:
    explicit ElementCopier(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void copy(const ArrayStorageForm&, const ElementRange& source, const ElementRange& target, Integer count, CopyDirection);
};

void ElementCopier::copy(const ArrayStorageForm& form, const ElementRange& source, const ElementRange& target, Integer count, CopyDirection direction)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    bool isBackward = direction == CopyDirection::Backward;
    LBasicBlock loop = newBlock();
    LBasicBlock element = newBlock();
    LBasicBlock copied = newBlock();
    ValueFromBlock start = m_out.anchor((isBackward ? count : integer(0)).value());
    m_out.jump(loop);

    m_out.appendTo(loop);
    Integer position(m_out.phi(B3::Int64, start));
    branch(isBackward ? isNotZero(position) : below(position, count), { normalEdge(element), normalEdge(copied) });

    m_out.appendTo(element);
    Integer index = isBackward ? sub(position, integer(1)) : position;
    Operand rawElement = elements.loadRawElement(form, source.elements, add(source.first, index));
    elements.storeRawElement(form, target.elements, add(target.first, index), rawElement);
    m_out.addIncomingToPhi(position.value(), m_out.anchor((isBackward ? index : add(position, integer(1))).value()));
    m_out.jump(loop);

    m_out.appendTo(copied);
}

} // anonymous namespace

static std::span<const ValueID> itemsToInsert(const InlineCall& call)
{
    return call.numberOfArguments() > 2 ? call.arguments().subspan(2) : std::span<const ValueID> { };
}

static bool isArray(const Type& type)
{
    const ClassPart* part = type.isObject() ? type.singleBuiltinClass() : nullptr;
    return part && part->builtinClass == BuiltinClasses::Array;
}

ArrayBuiltinEmitter::ConcatOperands ArrayBuiltinEmitter::collectConcatOperands(const InlineCall& call, const ArrayStorageForm& form)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    ConcatOperands operands;
    operands.arrays.append(receiverOf(call));
    for (ValueID argument : call.arguments()) {
        if (!typeOf(argument).hasObjects()) {
            operands.arrays.append(CellPointer());
            continue;
        }
        CellPointer array = cellOperand(argument);
        missUnless(call, elements.isPristineArray(array, { PristineAspects::Storage, PristineAspects::Names }));
        missUnless(call, elements.hasShape(elements.indexingMode(array, arrayStorageTestMask), form));
        operands.arrays.append(array);
    }

    operands.length = integer(0);
    for (CellPointer array : operands.arrays) {
        operands.elements.append(array ? elements.elementsOf(array) : ArrayElements { });
        operands.lengths.append(array ? elements.publicLength(operands.elements.last()) : integer(1));
        operands.length = add(operands.length, operands.lengths.last());
    }
    return operands;
}

void ArrayBuiltinEmitter::copyConcatenatedElements(const InlineCall& call, const ConcatOperands& operands, const ConcatTarget& target, const ArrayStorageForm& form)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    ElementCopier copier(generator());
    LBasicBlock miss = missBlockOf(call);
    // The indexing header is one word, so word `capacity` of the storage is the last element of the vector.
    elements.storeRawElement(form, ArrayElements { CellPointer(), target.storage }, target.capacity, elements.rawHole(form));
    Integer position = integer(0);
    for (unsigned i = 0; i < operands.arrays.size(); ++i) {
        if (operands.arrays[i])
            copier.copy(form, { operands.elements[i], integer(0) }, { target.elements, position }, operands.lengths[i], CopyDirection::Forward);
        else if (std::optional<Operand> rawElement = elements.rawElementToStore(form, call.argument(i - 1), miss))
            elements.storeRawElement(form, target.elements, position, *rawElement);
        else {
            m_out.jump(miss);
            m_out.appendTo(newBlock());
        }
        position = add(position, operands.lengths[i]);
    }
}

void ArrayBuiltinEmitter::generateArrayConcat(const InlineCall& call)
{
    bool areElementsOrArrays = std::ranges::all_of(call.arguments(), [&](ValueID argument) {
        const Type& type = typeOf(argument);
        return !type.hasObjects() || isArray(type);
    });
    if (!checkChosenForm(call, call.canMiss() && areElementsOrArrays && call.numberOfArguments() <= maxInlineConcatOperands))
        return;

    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    ArrayAllocationEmitter allocation(generator());
    LBasicBlock miss = missBlockOf(call);
    Join<CellPointer> join(m_out);
    forEachReadableFormOrMiss(call, receiverOf(call), [&](const ArrayStorageForm& form) {
        ConcatOperands operands = collectConcatOperands(call, form);
        missUnless(call, belowOrEqual(operands.length, integer(maxLengthForElementwiseCopy)));
        Integer header = allocation.headerOfNewArray(form);
        missUnless(call, isNotZero(header));
        Integer capacity = bitOr(operands.length, integer(1));
        CellPointer array = allocation.allocateEmptyArrayWithStorage(call.context(), header, capacity, miss);
        Pointer storage = offsetBy(asPointer(array), sizeof(JSArray));
        ConcatTarget target { storage, capacity, { CellPointer(), offsetBy(storage, sizeof(IndexingHeader)) } };
        {
            CodeGenerator::InitializationScope scope(generator());
            copyConcatenatedElements(call, operands, target, form);
            elements.setPublicLength(target.elements, operands.length);
        }
        emitter<BarrierEmitter>().fenceAfterInitialization();
        join.add(array);
    });
    finish(call, join.finish());
}

CellPointer ArrayBuiltinEmitter::copyRemovedElements(const InlineCall& call, const InlineSplice& splice, const ArrayStorageForm& form)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    auto removedElement = [&](int64_t index) {
        return elements.loadRawElement(form, splice.elements, add(splice.from, integer(index)));
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
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    RawItems inserted;
    for (ValueID item : itemsToInsert(call)) {
        std::optional<Operand> rawElement = elements.rawElementToStore(form, item, doesNotFit);
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
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    CellPointer array = splice.elements.array;
    Integer from = splice.from;
    int64_t added = inserted.size();
    int64_t removed = splice.removed;

    ArrayElements storage = elements.elementsOf(array);
    if (added != removed) {
        CopyDirection direction = added > removed ? CopyDirection::Backward : CopyDirection::Forward;
        ElementCopier(generator()).copy(form, { storage, add(from, integer(removed)) }, { storage, add(from, integer(added)) }, splice.tail, direction);
        for (int64_t i = 0; i < removed - added; ++i)
            elements.storeRawElement(form, storage, add(splice.newLength, integer(i)), elements.rawHole(form));
    }
    for (size_t i = 0; i < inserted.size(); ++i)
        elements.storeRawElement(form, storage, add(from, integer(i)), inserted[i]);
    if (added != removed)
        elements.setPublicLength(storage, splice.newLength);
    if (form.holdsCells)
        emitter<BarrierEmitter>().barrier(call.context(), array);
}

void ArrayBuiltinEmitter::spliceElements(const InlineCall& call, OperandJoin& join, const InlineSplice& splice, const ArrayStorageForm& form, LBasicBlock outOfLine)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    std::optional<RawItems> inserted = rawItemsToInsert(call, form, outOfLine);
    if (!inserted)
        return;

    Representation representation = resultRepresentationOf(call);
    Operand removedElements;
    if (representation != Representation::Void) {
        for (int64_t i = 0; i < splice.removed; ++i) {
            Operand rawElement = elements.loadRawElement(form, splice.elements, add(splice.from, integer(i)));
            branchUnless(logicalNot(elements.isHoleIn(call.receiver(), form, rawElement)), outOfLine);
        }
        removedElements = inRepresentation(call.context(), representation, copyRemovedElements(call, splice, form), resultTypeOf(call));
    }
    replaceElements(call, splice, form, inserted->span());
    join.add(removedElements);
}

void ArrayBuiltinEmitter::spliceInline(const InlineCall& call, Integer start, int64_t removed, LBasicBlock outOfLine, const SpliceArm& emitArm)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    CellPointer array = receiverOf(call);
    int64_t added = itemsToInsert(call).size();
    ArrayElements storage = elements.elementsOf(array);
    Integer length = elements.publicLength(storage);
    Integer fromEnd = add(start, length);
    Integer clampedFromEnd = select(lessThan(fromEnd, integer(0)), integer(0), fromEnd);
    Integer from = select(lessThan(start, integer(0)), clampedFromEnd, select(lessThan(start, length), start, length));
    Integer tail = sub(length, add(from, integer(removed)));
    Integer maxTail = added == removed ? length : integer(maxLengthForElementwiseMove);
    branchUnless(belowOrEqual(tail, maxTail), outOfLine);
    Integer newLength = add(length, integer(added - removed));
    if (added > removed)
        branchUnless(belowOrEqual(newLength, elements.vectorLength(storage)), outOfLine);

    InlineSplice splice { storage, from, tail, newLength, removed };
    elements.forEachFormByWritableShape(elements.indexingMode(array, IndexingShapeAndWritabilityMask), candidateFormsOf(call), outOfLine, [&](const ArrayStorageForm& form) {
        emitArm(splice, form);
    });
}

// `let at: number` that only ever holds whole numbers is a double for all that its type says: the code asks it.
bool ArrayBuiltinEmitter::hasIntegerSpliceRange(const InlineCall& call)
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
    KnownValue deleteCount = knownValueOf(m_program, m_function, call.argument(1));
    if (deleteCount.kind() != KnownValue::Kind::Integer || deleteCount.asInteger() < 0 || deleteCount.asInteger() > maxRemovedByInlineSplice)
        return std::nullopt;
    return deleteCount.asInteger();
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
    for (ValueID item : itemsToInsert(call))
        boxedItems.append(boxedOperand(call.context(), item));
    Word32 count = word32(static_cast<int32_t>(boxedItems.size()));
    Boxed removedElements = callOperation<Boxed>(call.context(), RuntimeOperation::ArraySplice, { receiverOf(call), start, deleteCount, count, argumentBuffer(boxedItems.span()) }, options);
    branchUnless(isNotEmpty(removedElements), otherwise);
    return removedElements;
}

void ArrayBuiltinEmitter::generateArraySplice(const InlineCall& call)
{
    if (!checkChosenForm(call, call.canMiss() && hasIntegerSpliceRange(call) && itemsToInsert(call).size() <= maxInlineSpliceInsertions))
        return;

    Representation representation = resultRepresentationOf(call);
    OperandJoin join(m_out, representation);
    // The operation is a call like the call of the function, and a fifth of its instructions. The elements are moved in place where the code is fast.
    std::optional<int64_t> inlineDeleteCount = call.context().shape() == CodeShape::Fast ? deleteCountOfInlineSplice(call) : std::nullopt;
    if (inlineDeleteCount) {
        LBasicBlock outOfLine = newBlock();
        spliceInline(call, wholeNumberOrElse(call.argument(0), outOfLine), *inlineDeleteCount, outOfLine, [&](const InlineSplice& splice, const ArrayStorageForm& form) {
            spliceElements(call, join, splice, form, outOfLine);
        });
        m_out.appendTo(outOfLine);
    }

    Boxed removedElements = spliceByOperation(call, missBlockOf(call), { .isOnRarePath = inlineDeleteCount.has_value() });
    join.add(inRepresentation(call.context(), representation, removedElements, resultTypeOf(call)));
    finishInlineCall(call, join.finish());
}

// A read of a hole among the removed elements finds nothing further up either: undefined.
ArrayBuiltinEmitter::RemovedPrefix ArrayBuiltinEmitter::loadRemovedPrefix(const InlineSplice& splice, const ArrayStorageForm& form, unsigned count)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    RemovedPrefix prefix { integer(splice.removed), { } };
    for (unsigned i = 0; i < count; ++i) {
        if (i >= splice.removed) {
            prefix.elements.append(undefined());
            continue;
        }
        Operand rawElement = elements.loadRawElement(form, splice.elements, add(splice.from, integer(i)));
        prefix.elements.append(select(elements.isHole(form, rawElement), undefined(), elements.boxRawElement(form, rawElement)));
    }
    return prefix;
}

Boxed ArrayBuiltinEmitter::spliceByFunction(const InlineCall& call)
{
    Vector<Boxed, 2 + maxInlineSpliceInsertions> arguments;
    for (ValueID argument : call.arguments())
        arguments.append(boxedOperand(call.context(), argument));
    return emitter<BuiltinCallEmitter>().callBuiltinFunction(call.context(), call.overload(), boxCell(receiverOf(call)), arguments.span());
}

ArrayBuiltinEmitter::RemovedPrefix ArrayBuiltinEmitter::readRemovedPrefix(const InlineCall& call, Boxed removedElements, unsigned count)
{
    RemovedPrefix prefix { emitter<ElementAccessEmitter>().arrayLength(unboxCell(removedElements)), { } };
    for (unsigned i = 0; i < count; ++i)
        prefix.elements.append(emitter<AccessEmitter>().getByVal(call.context(), removedElements, boxInt32(word32(static_cast<int32_t>(i)))));
    return prefix;
}

void ArrayBuiltinEmitter::generateArraySplicePrefix(const InlineCall& call)
{
    RELEASE_ASSERT(call.value() && !call.canMiss());
    unsigned count = *(*call.value())->prefixLength();
    LBasicBlock continuation = newBlock();
    Vector<Vector<ValueFromBlock, 4>, 4> arms;
    auto endArm = [&](const RemovedPrefix& prefix) {
        Vector<ValueFromBlock, 4> anchors { m_out.anchor(prefix.length.value()) };
        for (Boxed element : prefix.elements)
            anchors.append(m_out.anchor(element.value()));
        arms.append(WTF::move(anchors));
        m_out.jump(continuation);
    };

    bool hasFastPath = hasIntegerSpliceRange(call) && !build().exceeds(LimitID::MaxInlineSpliceInsertions, itemsToInsert(call).size(), analyses().subject(call.context().valueID()));
    std::optional<int64_t> inlineDeleteCount = hasFastPath && call.context().shape() != CodeShape::Compact ? deleteCountOfInlineSplice(call) : std::nullopt;
    if (inlineDeleteCount) {
        LBasicBlock outOfLine = newBlock();
        spliceInline(call, wholeNumberOrElse(call.argument(0), outOfLine), *inlineDeleteCount, outOfLine, [&](const InlineSplice& splice, const ArrayStorageForm& form) {
            std::optional<RawItems> inserted = rawItemsToInsert(call, form, outOfLine);
            if (!inserted)
                return;
            RemovedPrefix prefix = loadRemovedPrefix(splice, form, count);
            replaceElements(call, splice, form, inserted->span());
            endArm(prefix);
        });
        m_out.appendTo(outOfLine);
    }
    if (hasFastPath) {
        LBasicBlock byFunction = newBlock();
        endArm(readRemovedPrefix(call, spliceByOperation(call, byFunction, { .isOnRarePath = inlineDeleteCount.has_value() }), count));
        m_out.appendTo(byFunction);
    }
    endArm(readRemovedPrefix(call, spliceByFunction(call), count));

    m_out.appendTo(continuation);
    Vector<Operand, 4> results;
    for (unsigned index = 0; index <= count; ++index) {
        Vector<ValueFromBlock, 4> incoming = arms.map<Vector<ValueFromBlock, 4>>([&](const Vector<ValueFromBlock, 4>& anchors) {
            return anchors[index];
        });
        LValue joined = m_out.phi(B3::Int64, incoming);
        results.append(index ? Operand(Boxed(joined)) : Operand(Integer(joined)));
    }
    finishWithResults(call, results.span());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
