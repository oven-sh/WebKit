/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayStorageBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTArrayRules.h"
#include "AOTBarrierEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTFunctionAnalyses.h"
#include "AOTVector.h"
#include "IndexingType.h"
#include "JSType.h"
#include <algorithm>
#include <optional>
#include <ranges>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr int64_t invalidArrayLength = -1;

static ArrayStorageFormList formsStartingWith(const ArrayStorageForm& expected)
{
    ArrayStorageFormList forms { &expected };
    for (const ArrayStorageForm& form : arrayStorageForms | std::views::reverse) {
        if (form.kind != expected.kind)
            forms.append(&form);
    }
    return forms;
}

Operand ArrayStorageBuiltinEmitter::lengthResult(const InlineCall& call, Integer length)
{
    return emitter<ConversionEmitter>().inRepresentation(call.context(), length, Type::integer(Range::uint32()), resultRepresentationOf(call));
}

void ArrayStorageBuiltinEmitter::generateArrayLength(const InlineCall& call)
{
    finishInlineCall(call, lengthResult(call, elements().arrayLength(in<CellPointer>(call.receiver()))));
}

void ArrayStorageBuiltinEmitter::generateArraySetLength(const InlineCall& call)
{
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
    Integer maxClearedElements = integer(static_cast<int64_t>(build().limit(LimitID::MaxInlineClearedElements)));
    branchUnless(belowOrEqual(sub(length, newLength), maxClearedElements), slowPath);
    Integer hole = integer(static_cast<int64_t>(arrayStorageForms.back().hole));
    for (const ArrayStorageForm& form : std::span(arrayStorageForms).first(arrayStorageForms.size() - 1))
        hole = select(elements().hasWritableShape(mode, form), integer(static_cast<int64_t>(form.hole)), hole);
    ValueFromBlock firstCleared = m_out.anchor(newLength.value());
    m_out.jump(clearLoop);

    m_out.appendTo(clearLoop);
    Integer index(m_out.phi(B3::Int64, firstCleared));
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
        emitter<AccessEmitter>().putById(call.context(), boxCell(array), m_program.keyOf(BuiltinNames::length), boxedLength, AccessEmitter::putModeOf(ECMAMode::Strict), SiteCacheID());
        m_out.jump(continuation);

        m_out.appendTo(continuation);
    }
    Representation representation = resultRepresentationOf(call);
    finishInlineCall(call, representation == Representation::Void ? Operand() : elements().absentElement(call.context(), representation));
}

void ArrayStorageBuiltinEmitter::generateArrayPush(const InlineCall& call)
{
    CellPointer array = in<CellPointer>(call.receiver());
    if (call.numberOfArguments() == 1) {
        pushOne(call, array);
        return;
    }
    pushSeveral(call, array);
}

void ArrayStorageBuiltinEmitter::pushOne(const InlineCall& call, CellPointer array)
{
    ValueID pushed = call.argument(0);
    if (call.context().shape() == CodeShape::Compact) {
        finishInlineCall(call, pushOutOfLine(call, array, pushed));
        return;
    }

    ArrayStorage expected = ArrayAllocationEmitter(generator()).formAtAllocation(typeOf(call.receiver())).kind;
    ArrayStorage declared = arrayStorageDeclaredBy(m_program, typeOf(call.receiver())).predicted();
    ArrayStorageFormList forms = elements().candidateFormsFor(pushed, expected);
    // An array of any other kind than its element type declares takes the stub, which knows them all.
    if (hasArrayStorageForm(declared) && forms.size() > 1 && forms.first()->kind == declared)
        forms.shrink(1);
    bool mayPushCell = typeOf(pushed).hasCells();
    LBasicBlock isStored = newBlock();
    LBasicBlock slowPath = newBlock();
    OperandJoin join(m_out, resultRepresentationOf(call));

    ArrayElements storage = elements().elementsOf(array);
    Integer length = elements().publicLength(storage);
    branchUnless(below(length, elements().vectorLength(storage)), slowPath);
    // Read in the block that asks for it: a compare reads the byte from memory.
    Word32 mode = elements().indexingMode(array, IndexingShapeAndWritabilityMask);
    elements().forEachFormByShape(mode, &ElementAccessEmitter::hasWritableShape, forms, slowPath, [&](const ArrayStorageForm& form) {
        Operand rawElement = *elements().rawElementToStore(form, pushed, slowPath);
        // An array that needs a barrier takes the stub, which remembers the pushed element and not the array.
        if (form.holdsCells && mayPushCell)
            branchUnless(emitter<BarrierEmitter>().isBeyondBarrierThreshold(array), slowPath);
        elements().storeRawElement(form, storage, length, rawElement);
        m_out.jump(isStored);
    });

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

void ArrayStorageBuiltinEmitter::pushSeveral(const InlineCall& call, CellPointer array)
{
    if (!call.canMiss()) {
        fail(call.context(), InternalErrorKind::OpcodeWithoutCode);
        finishInlineCall(call, emitter<ConstantEmitter>().zeroOf(resultRepresentationOf(call)));
        return;
    }

    std::span<const ValueID> pushed = call.arguments();
    ArrayStorageFormList forms = formsStartingWith(ArrayAllocationEmitter(generator()).formAtAllocation(typeOf(call.receiver())));
    forms.removeAllMatching([&](const ArrayStorageForm* form) {
        return std::ranges::any_of(pushed, [&](ValueID id) {
            return !elements().candidateFormsFor(id, form->kind).contains(form);
        });
    });
    bool mayPushCell = std::ranges::any_of(pushed, [&](ValueID id) {
        return typeOf(id).hasCells();
    });
    LBasicBlock miss = missBlockOf(call);
    LBasicBlock isStored = newBlock();

    Word32 mode = elements().indexingMode(array, IndexingShapeAndWritabilityMask);
    ArrayElements currentStorage = elements().elementsOf(array);
    Integer length = elements().publicLength(currentStorage);
    Integer newLength = add(length, integer(pushed.size()));
    // No program can observe the capacity, so the call may still miss after the vector has grown.
    Pointer butterfly = either<Pointer>(belowOrEqual(newLength, elements().vectorLength(currentStorage)), [&] {
        return currentStorage.butterfly;
    }, [&] {
        Integer hasGrown = callOperation<Integer>(call.context(), RuntimeOperation::ArrayGrow, { array, low32(newLength) }, { .isOnRarePath = true });
        branchUnless(isNotZero(hasGrown), miss);
        return elements().elementsOf(array).butterfly;
    });
    ArrayElements storage { array, butterfly };
    elements().forEachFormByShape(mode, &ElementAccessEmitter::hasWritableShape, forms, miss, [&](const ArrayStorageForm& form) {
        // Nothing is stored before every element is known to fit.
        Vector<Operand, maxInlinePushOperands> rawElements;
        for (ValueID id : pushed)
            rawElements.append(*elements().rawElementToStore(form, id, miss));
        for (size_t i = 0; i < rawElements.size(); ++i)
            elements().storeRawElement(form, storage, add(length, integer(i)), rawElements[i]);
        elements().setPublicLength(storage, newLength);
        if (form.holdsCells && mayPushCell)
            emitter<BarrierEmitter>().barrier(call.context(), array);
        m_out.jump(isStored);
    });

    m_out.appendTo(isStored);
    finishInlineCall(call, lengthResult(call, newLength));
}

// What flow found in the arrays of the site stands in the cast behind the call. Their stores and reads expect the kind of storage that holds it.
const Type& ArrayStorageBuiltinEmitter::typeOfArraysCreatedBy(const InlineCall& call)
{
    if (const ValueContext* value = call.value()) {
        std::optional<Use> use = analyses().useLists().singleUse(value->id());
        if (use && use->kind == Use::Kind::Operand && m_function.at(use->user).opcode == Opcode::AssumeType)
            return typeOf(use->user);
    }
    return resultTypeOf(call);
}

void ArrayStorageBuiltinEmitter::generateArrayWithLength(const InlineCall& call)
{
    RELEASE_ASSERT(call.numberOfArguments());
    ArrayAllocationEmitter allocation(generator());
    LBasicBlock isNotInteger = newBlock();
    Join<Integer> join(m_out);
    join.add(elements().indexOrElse(call.argument(0), false, isNotInteger));

    m_out.appendTo(isNotInteger);
    join.add(integer(invalidArrayLength));
    Integer length = join.finish();
    bool isDoneInPlace = call.value() && (*call.value())->isDoneInPlace();
    finishInlineCall(call, allocation.newArrayOfHoles(call.context(), length, allocation.formAtAllocation(typeOfArraysCreatedBy(call)), isDoneInPlace ? BuiltinID() : functionOf(call.overload())));
}

void ArrayStorageBuiltinEmitter::generateArrayOf(const InlineCall& call)
{
    finishInlineCall(call, ArrayAllocationEmitter(generator()).newArrayOf(call.context(), call.arguments(), typeOfArraysCreatedBy(call)));
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
    branch(hasCellType(cell, ProxyObjectType), { rareEdge(missBlockOf(call)), normalEdge(isNotArray) });

    m_out.appendTo(isNotArray);
    join.add(boolean(false));
    finishInlineCall(call, join.finish());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
