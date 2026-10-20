/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTElementAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTArrayAllocationProfile.h"
#include "AOTBarrierEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTRuntimeValidationEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStubsArrays.h"
#include <ranges>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class ElementConversion : uint8_t {
    NeverFits,
    UseBoxed,
    BoxInt32,
    BoxDouble,
    CheckBoxedInt32,
    Int32ToDouble,
    UseDouble,
    UnboxNumber,
};

} // anonymous namespace

static ElementConversion conversionTo(const ArrayStorageForm& form, Representation stored, const Type& type)
{
    bool isUnboxedDouble = stored == Representation::Double || stored == Representation::OptionalDouble;
    if (form.element == Representation::Int32) {
        if (stored == Representation::Int32)
            return ElementConversion::BoxInt32;
        if (stored == Representation::JSValue && type.primitives & Primitive::Integer)
            return ElementConversion::CheckBoxedInt32;
        return ElementConversion::NeverFits;
    }
    if (form.element == Representation::Double) {
        if (stored == Representation::Int32)
            return ElementConversion::Int32ToDouble;
        if (!(type.primitives & Primitive::Number))
            return ElementConversion::NeverFits;
        if (isUnboxedDouble)
            return ElementConversion::UseDouble;
        return stored == Representation::JSValue ? ElementConversion::UnboxNumber : ElementConversion::NeverFits;
    }
    RELEASE_ASSERT(form.element == Representation::JSValue);
    if (stored == Representation::JSValue)
        return ElementConversion::UseBoxed;
    if (stored == Representation::Int32)
        return ElementConversion::BoxInt32;
    return isUnboxedDouble ? ElementConversion::BoxDouble : ElementConversion::NeverFits;
}

static ArrayStorage storageExpectedToHold(Representation stored)
{
    return stored == Representation::Double ? ArrayStorage::Double : ArrayStorage::Contiguous;
}

ArrayStorageFormList ElementAccessEmitter::candidateFormsFor(ValueID stored, ArrayStorage expected)
{
    Representation representation = representationOf(stored);
    const Type& type = typeOf(stored);
    ArrayStorageFormList forms;
    auto appendIfPossible = [&](const ArrayStorageForm& form) {
        if (conversionTo(form, representation, type) != ElementConversion::NeverFits)
            forms.appendIfNotContains(&form);
    };
    if (const ArrayStorageForm* form = findArrayStorageForm(expected))
        appendIfPossible(*form);
    for (const ArrayStorageForm& form : arrayStorageForms | std::views::reverse)
        appendIfPossible(form);
    return forms;
}

void ElementAccessEmitter::forEachFormByWritableShape(Word32 mode, const ArrayStorageFormList& forms, LBasicBlock hasNoForm, const ScopedLambda<void(const ArrayStorageForm&)>& emitArm)
{
    for (const ArrayStorageForm* form : forms) {
        LBasicBlock hasForm = newBlock();
        LBasicBlock hasOtherForm = newBlock();
        m_out.branch(hasWritableShape(mode, *form).value(), normalEdge(hasForm), form == forms.last() ? rareEdge(hasOtherForm) : normalEdge(hasOtherForm));
        m_out.appendTo(hasForm);
        emitArm(*form);
        m_out.appendTo(hasOtherForm);
    }
    m_out.jump(hasNoForm);
}

std::optional<Operand> ElementAccessEmitter::rawElementToStore(const ArrayStorageForm& form, ValueID stored, LBasicBlock doesNotFit)
{
    const Type& type = typeOf(stored);
    Operand operand = get(stored);
    auto numberToStore = [&](Number number) -> Operand {
        if (!canStoreWithoutConversion(form.kind, type))
            branchUnless(equal(number, number), doesNotFit);
        return number;
    };

    switch (conversionTo(form, operand.representation, type)) {
    case ElementConversion::NeverFits:
        return std::nullopt;
    case ElementConversion::UseBoxed:
        return operand;
    case ElementConversion::BoxInt32:
        return Operand(boxInt32(operand.as<Word32>()));
    case ElementConversion::BoxDouble: {
        Number number(operand.value);
        if (operand.is<OptionalNumber>())
            branchUnless(equal(number, number), doesNotFit);
        return Operand(emitter<ConversionEmitter>().boxNumber(number));
    }
    case ElementConversion::CheckBoxedInt32:
        branchUnless(isInt32(operand.as<Boxed>()), doesNotFit);
        return operand;
    case ElementConversion::Int32ToDouble:
        return Operand(toNumber(operand.as<Word32>()));
    case ElementConversion::UseDouble:
        return numberToStore(Number(operand.value));
    case ElementConversion::UnboxNumber: {
        Boxed boxed = operand.as<Boxed>();
        if (!type.isNumber())
            branchUnless(isNumber(boxed), doesNotFit);
        return numberToStore(numberToDouble(boxed));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ElementAccessEmitter::storeInVector(const ValueContext& store, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, Operand rawElement, LBasicBlock isBeyondVector)
{
    LBasicBlock isBeyondLength = newBlock();
    LBasicBlock isWithinLength = newBlock();
    m_out.branch(below(index, publicLength(elements)).value(), normalEdge(isWithinLength), unlikelyEdge(isBeyondLength));

    m_out.appendTo(isBeyondLength);
    branchUnless(below(index, vectorLength(elements)), isBeyondVector);
    setPublicLength(elements, add(index, integer(1)));
    if (!replacesBoxedInt32(store, form)) {
        m_out.jump(isWithinLength);
        m_out.appendTo(isWithinLength);
        replaceElement(store, form, elements, index, rawElement);
        return;
    }

    LBasicBlock continuation = newBlock();
    storeRawElement(form, elements, index, rawElement);
    m_out.jump(continuation);

    m_out.appendTo(isWithinLength);
    replaceElement(store, form, elements, index, rawElement);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

// Inside the length of an array of int32 without holes every element is a boxed int32, and all of them have the same high half.
bool ElementAccessEmitter::replacesBoxedInt32(const ValueContext& store, const ArrayStorageForm& form)
{
    return form.element == Representation::Int32 && representationOf(store->valueOperand()) == Representation::Int32 && isDense(store->arrayOperand());
}

void ElementAccessEmitter::replaceElement(const ValueContext& store, const ArrayStorageForm& form, const ArrayElements& elements, Integer index, Operand rawElement)
{
    if (replacesBoxedInt32(store, form)) {
        m_out.store32(in<Word32>(store->valueOperand()).value(), elementAt(form, elements, index));
        return;
    }
    storeRawElement(form, elements, index, rawElement);
    if (form.holdsCells && typeOf(store->valueOperand()).hasCells()) {
        Boxed stored = rawElement.as<Boxed>();
        emitter<BarrierEmitter>().elementBarrierAfterStore(store, elements.array, singleElementSpan(stored));
    }
}

// The type of the value says that the storage takes it as it is, and the guard that the storage is the array's own.
void ElementAccessEmitter::storeInsideLength(const ValueContext& value, const ArrayStorageForm& form)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    ArrayElements elements = elementsOf(array);
    Integer index = indexInsideLength(value->positionOperand());
    if (generator().validates()) {
        RuntimeValidationEmitter& validation = emitter<RuntimeValidationEmitter>();
        validation.checkPrecondition(value, hasWritableShape(indexingMode(array, IndexingShapeAndWritabilityMask), form));
        validation.checkPrecondition(value, below(index, publicLength(elements)));
    }
    replaceElement(value, form, elements, index, rawElementThatFits(form, value->valueOperand()));
}

// A boxed integer may be boxed as a double, which storage of int32 does not hold.
Operand ElementAccessEmitter::rawElementThatFits(const ArrayStorageForm& form, ValueID stored)
{
    if (form.element == Representation::Int32 && representationOf(stored) == Representation::JSValue)
        return boxInt32(emitter<ConversionEmitter>().unbox(in<Boxed>(stored), typeOf(stored), Representation::Int32).as<Word32>());
    return *rawElementToStore(form, stored, nullptr);
}

void ElementAccessEmitter::generatePutArrayElement(const ValueContext& value)
{
    if (const ArrayStorageForm* form = findArrayStorageForm(value->arrayStorage())) {
        storeWithKnownStorage(value, *form);
        return;
    }
    storeWithAnyStorage(value);
}

void ElementAccessEmitter::storeWithKnownStorage(const ValueContext& value, const ArrayStorageForm& form)
{
    if (value->isInBounds()) {
        storeInsideLength(value, form);
        return;
    }

    CellPointer array = in<CellPointer>(value->arrayOperand());
    ValueID stored = value->valueOperand();
    if (conversionTo(form, representationOf(stored), typeOf(stored)) == ElementConversion::NeverFits) {
        storeOutOfLine(value, array);
        return;
    }

    LBasicBlock slowPath = newBlock();
    LBasicBlock continuation = newBlock();
    Integer index = indexOrElse(value->positionOperand(), value->keyIsIntegral(), slowPath);
    // The guard accepts copy-on-write storage, which only the operation may replace.
    branchUnless(hasWritableShape(indexingMode(array, IndexingShapeAndWritabilityMask), form), slowPath);
    Operand rawElement = *rawElementToStore(form, stored, slowPath);
    storeInVector(value, form, elementsOf(array), index, rawElement, slowPath);
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    storeOutOfLine(value, array);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void ElementAccessEmitter::storeWithAnyStorage(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    if (value.shape() == CodeShape::Compact) {
        storeOutOfLine(value, array);
        return;
    }

    ValueID stored = value->valueOperand();
    LBasicBlock slowPath = newBlock();
    LBasicBlock continuation = newBlock();
    Integer index = indexOrElse(value->positionOperand(), value->keyIsIntegral(), slowPath);
    Word32 mode = indexingMode(array, IndexingShapeAndWritabilityMask);
    ArrayElements elements = elementsOf(array);
    forEachFormByWritableShape(mode, candidateFormsFor(stored, storageExpectedToHold(representationOf(stored))), slowPath, [&](const ArrayStorageForm& form) {
        Operand rawElement = *rawElementToStore(form, stored, slowPath);
        storeInVector(value, form, elements, index, rawElement, slowPath);
        m_out.jump(continuation);
    });

    m_out.appendTo(slowPath);
    storeOutOfLine(value, array);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void ElementAccessEmitter::storeOutOfLine(const ValueContext& value, CellPointer array)
{
    ValueID position = value->positionOperand();
    ValueID stored = value->valueOperand();
    const Type& storedType = typeOf(stored);
    Boxed boxedValue = emitter<ConversionEmitter>().box(value, get(stored), storedType);
    if (value->mode() == ECMAMode::Sloppy) {
        emitter<AccessEmitter>().putByVal(value, boxCell(array), boxedPosition(position), boxedValue, AccessEmitter::putModeOf(ECMAMode::Sloppy));
        return;
    }

    if (Operand key = get(position); key.is<Number>()) {
        callVoidOperation(value, RuntimeOperation::ArrayPutAtNumber, { array, boxedValue, key }, { .isOnRarePath = true });
        return;
    }
    bool mayConvertToDouble = storedType.isNumberLike() && !storedType.isInt32();
    RuntimeOperation operation = mayConvertToDouble ? RuntimeOperation::ArrayPutNumberAtIndex : RuntimeOperation::ArrayPutAtIndex;
    callVoidOperation(value, operation, { array, boxedValue, integerOf(position) }, { .isOnRarePath = true });
}

void ElementAccessEmitter::appendElement(const ValueContext& store, CellPointer array, ValueID element)
{
    if (store.shape() != CodeShape::Fast) {
        appendOutOfLine(store, array, element);
        return;
    }

    ArrayStorage expected = ArrayAllocationEmitter(generator()).formAtAllocation(typeOf(store->arrayOperand())).kind;
    LBasicBlock slowPath = newBlock();
    LBasicBlock continuation = newBlock();
    Word32 mode = indexingMode(array, IndexingShapeAndWritabilityMask);
    ArrayElements elements = elementsOf(array);
    forEachFormByWritableShape(mode, candidateFormsFor(element, expected), slowPath, [&](const ArrayStorageForm& form) {
        appendWithMode(store, form, elements, element, slowPath);
        m_out.jump(continuation);
    });

    m_out.appendTo(slowPath);
    appendOutOfLine(store, array, element);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void ElementAccessEmitter::appendWithMode(const ValueContext& store, const ArrayStorageForm& form, const ArrayElements& elements, ValueID element, LBasicBlock slowPath)
{
    Operand rawElement = *rawElementToStore(form, element, slowPath);
    Integer length = publicLength(elements);
    branchUnless(below(length, vectorLength(elements)), slowPath);

    setPublicLength(elements, add(length, integer(1)));
    storeRawElement(form, elements, length, rawElement);
    if (form.holdsCells && typeOf(element).hasCells()) {
        Boxed stored = rawElement.as<Boxed>();
        emitter<BarrierEmitter>().elementBarrierAfterStore(store, elements.array, singleElementSpan(stored));
    }
}

void ElementAccessEmitter::appendOutOfLine(const ValueContext& store, CellPointer array, ValueID element)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Operand operand = get(element);
    if (operand.is<Number>()) {
        callStub(store, arrayPushDouble(), { array.value(), conversions.bitsOf(operand).value() });
        return;
    }
    callStub(store, arrayPush(), { array.value(), conversions.box(store, operand, typeOf(element)).value() });
}

Condition ElementAccessEmitter::tryStoreElement(const ValueContext& store, CellPointer array, Integer index, ValueID stored)
{
    RELEASE_ASSERT(store->valueOperand() == stored);
    LBasicBlock otherwise = newBlock();
    Join<Condition> join(m_out);
    Word32 mode = indexingMode(array, IndexingShapeAndWritabilityMask);
    ArrayElements elements = elementsOf(array);
    forEachFormByWritableShape(mode, candidateFormsFor(stored, storageExpectedToHold(representationOf(stored))), otherwise, [&](const ArrayStorageForm& form) {
        Operand rawElement = *rawElementToStore(form, stored, otherwise);
        storeInVector(store, form, elements, index, rawElement, otherwise);
        join.add(boolean(true));
    });

    m_out.appendTo(otherwise);
    join.add(boolean(false));
    return join.finish();
}

namespace {

struct ElementAppend {
    ArrayElements source;
    ArrayElements target;
    Integer count;
    Integer targetLength;
};

class AppendedElementCopier final : public OperationEmitter {
public:
    explicit AppendedElementCopier(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    ArrayStorageFormList sourceFormsFor(ValueID source, const ArrayStorageForm& targetForm);
    void copy(const ValueContext&, const ArrayStorageForm& sourceForm, const ArrayStorageForm& targetForm, const ElementAppend&, LBasicBlock outOfLine);

private:
    void copyIntoContiguousStorage(const ValueContext&, const ArrayStorageForm& sourceForm, const ElementAppend&);
    void forEachPosition(Integer count, NOESCAPE const Invocable<void(Integer)> auto& emitElement);
};

} // anonymous namespace

static bool canHoldElementsOf(const ArrayStorageForm& target, const ArrayStorageForm& source)
{
    if (target.kind == source.kind || target.kind == ArrayStorage::Contiguous)
        return true;
    return target.kind == ArrayStorage::Double && source.kind == ArrayStorage::Int32;
}

static bool mayHoldElementsOfType(const ArrayStorageForm& form, const Type& elements)
{
    if (form.element == Representation::Int32)
        return elements.primitives & Primitive::Integer;
    if (form.element == Representation::Double)
        return elements.primitives & Primitive::Number;
    return true;
}

ArrayStorageFormList AppendedElementCopier::sourceFormsFor(ValueID source, const ArrayStorageForm& targetForm)
{
    const ClassPart* part = typeOf(source).singleBuiltinClass();
    TypeID contents = part ? part->contentsAt(m_builtins, ContentsSlot::Element) : TypeID();
    const Type& elements = m_program.at(contents ? contents : TypeTable::anything);
    ArrayStorageFormList forms;
    auto appendIfPossible = [&](const ArrayStorageForm& form) {
        if (canHoldElementsOf(targetForm, form) && mayHoldElementsOfType(form, elements))
            forms.appendIfNotContains(&form);
    };
    if (const ArrayStorageForm* expected = findArrayStorageForm(arrayStorageHolding(elements)))
        appendIfPossible(*expected);
    for (const ArrayStorageForm& form : arrayStorageForms | std::views::reverse)
        appendIfPossible(form);
    return forms;
}

void AppendedElementCopier::forEachPosition(Integer count, NOESCAPE const Invocable<void(Integer)> auto& emitElement)
{
    LBasicBlock loop = newBlock();
    LBasicBlock element = newBlock();
    LBasicBlock done = newBlock();
    ValueFromBlock start = m_out.anchor(integer(0).value());
    m_out.jump(loop);

    m_out.appendTo(loop);
    Integer position(m_out.phi(B3::Int64, start));
    branch(below(position, count), { normalEdge(element), normalEdge(done) });

    m_out.appendTo(element);
    emitElement(position);
    m_out.addIncomingToPhi(position.value(), m_out.anchor(add(position, integer(1)).value()));
    m_out.jump(loop);

    m_out.appendTo(done);
}

void AppendedElementCopier::copyIntoContiguousStorage(const ValueContext& value, const ArrayStorageForm& sourceForm, const ElementAppend& append)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    const ArrayStorageForm& targetForm = arrayStorageFormOf(ArrayStorage::Contiguous);
    // The collector visits the elements below the length, which are holes until they are copied.
    elements.setPublicLength(append.target, add(append.targetLength, append.count));
    forEachPosition(append.count, [&](Integer position) {
        Operand rawElement = elements.loadRawElement(sourceForm, append.source, position);
        Boxed element = select(elements.isHoleIn(value->sourceOperand(), sourceForm, rawElement), undefined(), elements.boxRawElement(sourceForm, rawElement));
        elements.storeRawElement(targetForm, append.target, add(append.targetLength, position), element);
    });
    if (sourceForm.holdsCells)
        emitter<BarrierEmitter>().barrier(value, append.target.array);
}

// The iterator of an array reads a hole as undefined, which only contiguous storage holds.
void AppendedElementCopier::copy(const ValueContext& value, const ArrayStorageForm& sourceForm, const ArrayStorageForm& targetForm, const ElementAppend& append, LBasicBlock outOfLine)
{
    RELEASE_ASSERT(canHoldElementsOf(targetForm, sourceForm));
    if (targetForm.kind == ArrayStorage::Contiguous) {
        copyIntoContiguousStorage(value, sourceForm, append);
        return;
    }

    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    LBasicBlock hasHole = newBlock();
    LBasicBlock copied = newBlock();
    Integer numberOfCopiedElements;
    auto storeAt = [&](Integer position, Operand rawElement) {
        elements.storeRawElement(targetForm, append.target, add(append.targetLength, position), rawElement);
    };
    forEachPosition(append.count, [&](Integer position) {
        Operand rawElement = elements.loadRawElement(sourceForm, append.source, position);
        numberOfCopiedElements = position;
        branchUnless(logicalNot(elements.isHoleIn(value->sourceOperand(), sourceForm, rawElement)), hasHole);
        storeAt(position, sourceForm.kind == targetForm.kind ? rawElement : Operand(toNumber(unboxInt32(rawElement.as<Boxed>()))));
    });
    elements.setPublicLength(append.target, add(append.targetLength, append.count));
    m_out.jump(copied);

    // Whatever lies between the length and the end of the vector is a hole.
    m_out.appendTo(hasHole);
    forEachPosition(numberOfCopiedElements, [&](Integer position) {
        storeAt(position, elements.rawHole(targetForm));
    });
    m_out.jump(outOfLine);

    m_out.appendTo(copied);
}

void ElementAccessEmitter::generateHasSpareArrayCapacity(const ValueContext& value)
{
    ArrayElements elements = elementsOf(in<CellPointer>(value->arrayOperand()));
    defineTest(value, below(publicLength(elements), vectorLength(elements)));
}

void ElementAccessEmitter::generateReserveArrayCapacity(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::ReserveArrayCapacity, { in<CellPointer>(value->arrayOperand()), integerOf(value->additionalOperand()) });
}

void ElementAccessEmitter::generateAppendElements(const ValueContext& value)
{
    CellPointer array = in<CellPointer>(value->arrayOperand());
    CellPointer source = in<CellPointer>(value->sourceOperand());
    if (value.shape() == CodeShape::Compact) {
        callVoidOperation(value, RuntimeOperation::AppendElements, { array, source });
        return;
    }

    AppendedElementCopier copier(generator());
    LBasicBlock hasElements = newBlock();
    LBasicBlock outOfLine = newBlock();
    LBasicBlock continuation = newBlock();
    ElementAppend append { elementsOf(source), elementsOf(array), { }, { } };
    append.count = publicLength(append.source);
    m_out.branch(isZero(append.count).value(), normalEdge(continuation), normalEdge(hasElements));

    m_out.appendTo(hasElements);
    append.targetLength = publicLength(append.target);
    const ArrayStorageForm& targetForm = ArrayAllocationEmitter(generator()).formAtAllocation(typeOf(value->arrayOperand()));
    branchUnless(belowOrEqual(append.count, integer(maxLengthForElementwiseCopy)), outOfLine);
    branchUnless(belowOrEqual(add(append.targetLength, append.count), vectorLength(append.target)), outOfLine);
    branchUnless(hasWritableShape(indexingMode(array, IndexingShapeAndWritabilityMask), targetForm), outOfLine);
    Word32 sourceShape = indexingMode(source, arrayStorageTestMask);
    ArrayStorageFormList sourceForms = copier.sourceFormsFor(value->sourceOperand(), targetForm);
    for (const ArrayStorageForm* sourceForm : sourceForms) {
        LBasicBlock hasForm = newBlock();
        LBasicBlock hasOtherForm = newBlock();
        m_out.branch(hasShape(sourceShape, *sourceForm).value(), normalEdge(hasForm), sourceForm == sourceForms.last() ? rareEdge(hasOtherForm) : normalEdge(hasOtherForm));
        m_out.appendTo(hasForm);
        copier.copy(value, *sourceForm, targetForm, append, outOfLine);
        m_out.jump(continuation);
        m_out.appendTo(hasOtherForm);
    }
    m_out.jump(outOfLine);

    m_out.appendTo(outOfLine);
    callVoidOperation(value, RuntimeOperation::AppendElements, { array, source }, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
