/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayStorageBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBarrierEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTTypeArena.h"
#include "IndexingType.h"
#include "JSType.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

Operand ArrayStorageBuiltinEmitter::lengthResult(const InlineCall& call, Integer length)
{
    return emitter<ConversionEmitter>().inRepresentation(call.context(), length, types().at(TypeTable::uint32), resultRepresentationOf(call));
}

void ArrayStorageBuiltinEmitter::generateArrayLength(const InlineCall& call)
{
    finishInlineCall(call, lengthResult(call, elements().arrayLength(in<CellPointer>(call.receiver()))));
}

void ArrayStorageBuiltinEmitter::generateArraySetLength(const InlineCall& call)
{
    static constexpr int64_t maxInlineClearedElements = 64;
    RELEASE_ASSERT(call.numberOfArguments());
    CellPointer array = in<CellPointer>(call.receiver());
    ValueID argument = call.argument(0);
    LBasicBlock slowPath = call.canMiss() ? missBlockOf(call) : newBlock();
    LBasicBlock isShorter = newBlock();
    LBasicBlock isNotShorter = newBlock();
    LBasicBlock clearLoop = newBlock();
    LBasicBlock clear = newBlock();
    LBasicBlock setLength = newBlock();

    Integer newLength = elements().indexOrElse(argument, false, slowPath);
    Word32 mode = elements().indexingMode(array, IndexingShapeAndWritabilityMask);
    Condition hasForm;
    for (const ArrayStorageForm& form : arrayStorageForms)
        hasForm = logicalOr(hasForm, elements().hasWritableShape(mode, form));
    branchUnless(hasForm, slowPath);
    ArrayElements storage = elements().elementsOf(array);
    Integer length = elements().publicLength(storage);
    branch(below(newLength, length), { normalEdge(isShorter), normalEdge(isNotShorter) });

    m_out.appendTo(isNotShorter);
    branchUnless(belowOrEqual(newLength, elements().vectorLength(storage)), slowPath);
    m_out.jump(setLength);

    m_out.appendTo(isShorter);
    branchUnless(belowOrEqual(sub(length, newLength), integer(maxInlineClearedElements)), slowPath);
    Integer hole = integer(static_cast<int64_t>(arrayStorageForms.back().hole));
    for (const ArrayStorageForm& form : std::span(arrayStorageForms).first(arrayStorageForms.size() - 1))
        hole = select(elements().hasWritableShape(mode, form), static_cast<int64_t>(form.hole), hole);
    ValueFromBlock firstCleared = m_out.anchor(newLength.value());
    m_out.jump(clearLoop);

    m_out.appendTo(clearLoop);
    Integer index(m_out.phi(Machine::Int64, firstCleared));
    branch(below(index, length), { normalEdge(clear), normalEdge(setLength) });

    m_out.appendTo(clear);
    m_out.store64(hole.value(), elements().elementOfUnknownForm(storage, index));
    m_out.addIncomingToPhi(index.value(), m_out.anchor(add(index, integer(1)).value()));
    m_out.jump(clearLoop);

    m_out.appendTo(setLength);
    elements().setPublicLength(storage, newLength);
    if (!call.canMiss()) {
        LBasicBlock continuation = newBlock();
        m_out.jump(continuation);

        m_out.appendTo(slowPath);
        Boxed boxedLength = emitter<ConversionEmitter>().box(call.context(), get(argument), typeOf(argument));
        emitter<AccessEmitter>().putById(call.context(), boxCell(array), m_program.keyOf(BuiltinNames::length), boxedLength, AccessEmitter::putModeOf(ECMAMode::Strict));
        m_out.jump(continuation);

        m_out.appendTo(continuation);
    }
    Representation representation = resultRepresentationOf(call);
    finishInlineCall(call, representation == Representation::Void ? Operand() : elements().absentElement(call.context(), representation));
}

void ArrayStorageBuiltinEmitter::generateArrayPush(const InlineCall& call)
{
    if (call.numberOfArguments() == 1) {
        pushOne(call, in<CellPointer>(call.receiver()));
        return;
    }
    if (!call.canMiss()) {
        fail(call.context(), InternalErrorKind::OpcodeWithoutCode);
        finishInlineCall(call, emitter<ConstantEmitter>().zeroOf(resultRepresentationOf(call)));
        return;
    }
    m_out.jump(missBlockOf(call));
}

void ArrayStorageBuiltinEmitter::pushOne(const InlineCall& call, CellPointer array)
{
    ValueID pushed = call.argument(0);
    ArrayStorage expected = ArrayAllocationEmitter(generator()).formAtAllocation(typeOf(call.receiver())).kind;
    ArrayStorage declared = arrayStorageDeclaredBy(m_program, types(), typeIDOf(call.receiver())).predicted();
    ArrayStorageFormList forms = elements().candidateFormsFor(pushed, expected);
    if (hasArrayStorageForm(declared) && forms.size() > 1 && forms.first()->kind == declared)
        forms.shrink(1);
    bool goesByKindOfNumbers = !hasArrayStorageForm(declared) && elements().goesByKindOfListOfNumbers(call.receiver(), pushed);
    if (goesByKindOfNumbers)
        forms = elements().formsOfListOfNumbersFor(pushed, false);
    bool mayPushCell = typeOf(pushed).hasCells();
    LBasicBlock isStored = newBlock();
    LBasicBlock slowPath = newBlock();
    OperandJoin join(m_out, resultRepresentationOf(call));

    ArrayElements storage = elements().elementsOf(array);
    Integer length = elements().publicLength(storage);
    branchUnless(below(length, elements().vectorLength(storage)), slowPath);
    Word32 mode = elements().indexingMode(array, IndexingShapeAndWritabilityMask);
    elements().forEachFormByShape(mode, &ElementAccessEmitter::hasWritableShape, forms, slowPath, [&](const ArrayStorageForm& form) {
        Operand rawElement = *elements().rawElementToStore(form, pushed, slowPath);
        if (form.holdsCells && mayPushCell)
            branchUnless(emitter<BarrierEmitter>().isBeyondBarrierThreshold(array), slowPath);
        elements().storeRawElement(form, storage, length, rawElement);
        m_out.jump(isStored);
    }, goesByKindOfNumbers ? ElementAccessEmitter::FirstForm::IsLikeTheOthers : ElementAccessEmitter::FirstForm::IsExpected);

    m_out.appendTo(isStored);
    Integer newLength = add(length, integer(1));
    elements().setPublicLength(storage, newLength);
    join.add(lengthResult(call, newLength));

    m_out.appendTo(slowPath);
    join.add(pushOutOfLine(call, array, pushed));
    finishInlineCall(call, join.finish());
}

Operand ArrayStorageBuiltinEmitter::pushOutOfLine(const InlineCall& call, CellPointer array, ValueID pushed)
{
    Boxed newLength = elements().appendOutOfLine(call.context(), array, pushed);
    return emitter<ConversionEmitter>().unbox(newLength, resultTypeOf(call), resultRepresentationOf(call));
}

void ArrayStorageBuiltinEmitter::generateArrayIsArray(const InlineCall& call)
{
    static_assert(DerivedArrayType == ArrayType + 1);
    ValueID argument = call.argumentOrNull(0);
    if (!argument || !typeOf(argument).hasObjects()) {
        finishInlineCall(call, boolean(false));
        return;
    }
    if (!call.canMiss()) {
        fail(call.context(), InternalErrorKind::OpcodeWithoutCode);
        finishInlineCall(call, emitter<ConstantEmitter>().zeroOf(resultRepresentationOf(call)));
        return;
    }

    LBasicBlock isCellCase = newBlock();
    LBasicBlock isArray = newBlock();
    LBasicBlock isOtherCell = newBlock();
    LBasicBlock isNotArray = newBlock();
    Join<Condition> join(m_out);
    Boxed value = in<Boxed>(argument);
    branch(isCell(value), { normalEdge(isCellCase), normalEdge(isNotArray) });

    m_out.appendTo(isCellCase);
    CellPointer cell = unboxCell(value);
    branch(hasCellTypeIn(cell, { ArrayType, DerivedArrayType }), { normalEdge(isArray), normalEdge(isOtherCell) });

    m_out.appendTo(isArray);
    join.add(boolean(true));

    m_out.appendTo(isOtherCell);
    Condition isProxy = hasCellType(cell, ProxyObjectType);
    branch(isProxy, { rareEdge(missBlockOf(call)), normalEdge(isNotArray) });

    m_out.appendTo(isNotArray);
    join.add(boolean(false));
    finishInlineCall(call, join.finish());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
