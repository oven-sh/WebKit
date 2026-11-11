/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayStorageBuiltinEmitter.h"

#if ENABLE(AOT)

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
    Representation representation = call.canMiss() && !resultIsRead(call) ? Representation::Void : resultRepresentationOf(call);
    BlockID block = call.context().block();
    if (m_code.at(block).frequency == BlockFrequency::Rare && !generator().isInLoop(block)) {
        finishInlineCall(call, pushOutOfLine(call, array, pushed, representation));
        return;
    }

    ArrayStorage expected = elements().expectedFormOf(call.receiver()).kind;
    ArrayStorage declared = arrayStorageDeclaredBy(m_program, types(), typeIDOf(call.receiver())).predicted();
    ArrayStorageFormList forms = elements().candidateFormsFor(pushed, expected);
    if (!hasArrayStorageForm(declared) && representationOf(pushed) == Representation::Int32) {
        ValueID birth = call.receiver();
        while (valuePreservingOpcodes.contains(opcodeOf(birth)))
            birth = operationAt(birth).operand(0);
        bool isBornInSightWithElements = opcodeOf(birth) == Opcode::NewArray && operationAt(birth).numberOfVariadicOperands();
        size_t place = forms.findIf([](const ArrayStorageForm* form) {
            return form->kind == ArrayStorage::Int32;
        });
        size_t wanted = isBornInSightWithElements ? 1 : 0;
        if (place != notFound && place > wanted) {
            const ArrayStorageForm* ofInt32s = forms[place];
            forms.removeAt(place);
            forms.insert(wanted, ofInt32s);
        }
    }
    if (hasArrayStorageForm(declared) && forms.size() > 1 && forms.first()->kind == declared)
        forms.shrink(1);
    bool goesByKindOfNumbers = !hasArrayStorageForm(declared) && elements().goesByKindOfListOfNumbers(call.receiver(), pushed);
    if (goesByKindOfNumbers)
        forms = elements().formsOfListOfNumbersFor(pushed, false);
    bool mayPushCell = typeOf(pushed).hasCells();
    LBasicBlock isStored = newBlock();
    LBasicBlock slowPath = newBlock();
    OperandJoin join(m_out, representation);

    build().statistics().count(StatisticID::ArrayPushesInlined);
    ArrayElements storage = elements().elementsOf(array);
    Word32 length = elements().publicLengthAsWord(storage);
    branchUnless(below(length, elements().vectorLengthAsWord(storage)), slowPath);
    auto store = [&](const ArrayStorageForm& form) {
        Operand rawElement = *elements().rawElementToStore(form, pushed, slowPath);
        if (form.holdsCells && mayPushCell)
            branchUnless(emitter<BarrierEmitter>().isBeyondBarrierThreshold(array), slowPath);
        elements().stopWhereHoleWouldBeStored(call.context(), form, rawElement);
        elements().storeRawElement(form, storage, zeroExtended(length), rawElement);
        m_out.jump(isStored);
    };
    if (!goesByKindOfNumbers && forms.size() > 1) {
        LBasicBlock hasOtherKind = newBlock();
        ArrayStorageFormList expectedForm;
        expectedForm.append(forms.first());
        forms.removeAt(0);
        elements().forEachFormByShape(elements().indexingMode(array, IndexingShapeAndWritabilityMask), &ElementAccessEmitter::hasWritableShape, expectedForm, hasOtherKind, store, ElementAccessEmitter::FirstForm::IsExpected);
        m_out.appendTo(hasOtherKind);
    }
    elements().forEachFormByShape(elements().indexingMode(array, IndexingShapeAndWritabilityMask), &ElementAccessEmitter::hasWritableShape, forms, slowPath, store, ElementAccessEmitter::FirstForm::IsLikeTheOthers);

    m_out.appendTo(isStored);
    Word32 newLength = add(length, word32(1));
    elements().setPublicLength(storage, newLength);
    join.add(representation == Representation::Void ? Operand() : lengthResult(call, zeroExtended(newLength)));

    m_out.appendTo(slowPath);
    join.add(pushOutOfLine(call, array, pushed, representation));
    finishInlineCall(call, join.finish());
}

Operand ArrayStorageBuiltinEmitter::pushOutOfLine(const InlineCall& call, CellPointer array, ValueID pushed, Representation representation)
{
    Boxed newLength = elements().appendOutOfLine(call.context(), array, pushed);
    if (representation == Representation::Void)
        return { };
    return emitter<ConversionEmitter>().unbox(newLength, resultTypeOf(call), representation);
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
