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
    if (call.numberOfArguments()) {
        pushSeveral(call, in<CellPointer>(call.receiver()));
        return;
    }
    m_out.jump(missBlockOf(call));
}

void ArrayStorageBuiltinEmitter::pushSeveral(const InlineCall& call, CellPointer array)
{
    LBasicBlock miss = missBlockOf(call);
    auto values = call.arguments();
    ArrayStorage expected = elements().expectedFormOf(call.receiver()).kind;
    ArrayStorage declared = arrayStorageDeclaredBy(m_program, types(), typeIDOf(call.receiver())).predicted();
    ArrayStorageFormList forms = elements().candidateFormsFor(values.front(), expected);
    for (ValueID value : values.subspan(1)) {
        ArrayStorageFormList formsOfValue = elements().candidateFormsFor(value, expected);
        forms.removeAllMatching([&](const ArrayStorageForm* form) {
            return !formsOfValue.contains(form);
        });
    }
    if (forms.isEmpty()) {
        m_out.jump(miss);
        return;
    }
    if (hasArrayStorageForm(declared) && forms.first()->kind == declared)
        forms.shrink(1);

    build().statistics().count(StatisticID::ArrayPushesInlined);
    Representation representation = resultIsRead(call) ? resultRepresentationOf(call) : Representation::Void;
    std::optional<CodeGenerator::PushesInLoop> loop = generator().findPushesInLoop(call.context().block(), call.receiver());
    LBasicBlock isStored = newBlock();
    LBasicBlock hasOtherKind = newBlock();
    LBasicBlock oneByOne = newBlock();
    LBasicBlock isFull = loop ? newBlock() : oneByOne;
    OperandJoin join(m_out, representation);
    bool mayPushCell = std::ranges::any_of(values, [&](ValueID value) {
        return typeOf(value).hasCells();
    });
    ArrayElements storage = elements().elementsOf(array);
    Word32 length = elements().publicLengthAsWord(storage);
    Word32 newLength = add(length, word32(static_cast<int32_t>(values.size())));
    Word32 mode = elements().indexingMode(array, IndexingShapeAndWritabilityMask);
    elements().forEachFormByShape(mode, &ElementAccessEmitter::hasWritableShape, forms, hasOtherKind, [&](const ArrayStorageForm& form) {
        branchUnless(belowOrEqual(newLength, elements().vectorLengthAsWord(storage)), isFull);
        Vector<Operand, maxInlinePushedValues> rawElements;
        for (ValueID value : values)
            rawElements.append(*elements().rawElementToStore(form, value, oneByOne));
        if (form.holdsCells && mayPushCell)
            branchUnless(emitter<BarrierEmitter>().isBeyondBarrierThreshold(array), oneByOne);
        Integer start = zeroExtended(length);
        for (size_t i = 0; i < rawElements.size(); ++i) {
            elements().stopWhereHoleWouldBeStored(call.context(), form, rawElements[i]);
            elements().storeRawElement(form, storage, i ? add(start, integer(i)) : start, rawElements[i]);
        }
        m_out.jump(isStored);
    }, ElementAccessEmitter::FirstForm::IsLikeTheOthers);

    m_out.appendTo(isStored);
    elements().setPublicLength(storage, newLength);
    for (ValueID value : values)
        prefetchPushedCell(call, value);
    join.add(representation == Representation::Void ? Operand() : lengthResult(call, zeroExtended(newLength)));

    static_assert(UndecidedShape < ContiguousShape && Int32Shape < ContiguousShape && DoubleShape < ContiguousShape && ArrayStorageShape > ContiguousShape && CopyOnWrite > ContiguousShape);
    m_out.appendTo(hasOtherKind);
    branchUnless(belowOrEqual(mode, word32(ContiguousShape)), miss);
    m_out.jump(oneByOne);

    if (loop) {
        m_out.appendTo(isFull);
        reserveForRemainingIterations(call, *loop, array, length, oneByOne);
    }

    m_out.appendTo(oneByOne);
    for (ValueID value : values.first(values.size() - 1))
        elements().appendOutOfLine(call.context(), array, value);
    join.add(pushOutOfLine(call, array, values.back(), representation));
    finishInlineCall(call, join.finish());
}

void ArrayStorageBuiltinEmitter::prefetchPushedCell(const InlineCall& call, ValueID pushed)
{
    static constexpr OpcodeSet reads { Opcode::ElemGet, Opcode::GetArrayElement, Opcode::PropGet, Opcode::GetSlot, Opcode::GetSlotOrUndefined };
    if (representationOf(pushed) != Representation::JSValue || !typeOf(pushed).hasCells() || !generator().isInLoop(call.context().block()))
        return;
    ValueID root = pushed;
    while (valuePreservingOpcodes.contains(opcodeOf(root)))
        root = operationAt(root).operand(0);
    if (!reads.contains(opcodeOf(root)) || !generator().isPoppedInLoop(call.receiver()))
        return;
    build().statistics().count(StatisticID::PushesOntoPoppedArrays);
#if CPU(X86_64)
    Machine::PatchOperation* prefetch = m_out.patchpoint(Machine::Void);
    prefetch->effects = Machine::Effects::none();
    prefetch->effects.writesLocalState = true;
    prefetch->appendSomeRegister(in<Boxed>(pushed).value());
    prefetch->setRunsWithoutFrame(true);
    prefetch->setGenerator([](CCallHelpers& jit, const Machine::PatchParameters& params) {
        jit.prefetch(CCallHelpers::Address(params[0].gpr()));
    });
#endif
}

bool ArrayStorageBuiltinEmitter::isTestedAgain(ValueID array)
{
    while (valuePreservingOpcodes.contains(opcodeOf(array)))
        array = operationAt(array).operand(0);
    return generator().hasArrayStorageTest(array);
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
    bool isInt32OrNothing = typeOf(pushed).isInt32OrNullish() && typeOf(pushed).primitives & Primitive::Integer;
    if (!hasArrayStorageForm(declared) && (representationOf(pushed) == Representation::Int32 || isInt32OrNothing)) {
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
        forms = elements().formsOfListOfNumbersFor(pushed, generator().usesLoopForms(block));
    bool mayPushCell = typeOf(pushed).hasCells();
    std::optional<CodeGenerator::PushesInLoop> loop = generator().findPushesInLoop(block, call.receiver());
    bool joinsForms = forms.size() == 1 || !isTestedAgain(call.receiver());
    LBasicBlock isStored = joinsForms ? newBlock() : nullptr;
    LBasicBlock slowPath = newBlock();
    LBasicBlock isFull = loop ? newBlock() : slowPath;
    OperandJoin join(m_out, representation);

    build().statistics().count(StatisticID::ArrayPushesInlined);
    const auto* known = call.canMiss() ? generator().listStorageForCall(block) : nullptr;
    auto* behind = call.canMiss() ? generator().listStorageBehindCall(block) : nullptr;
    Vector<ValueFromBlock, 4> butterflies;
    Vector<ValueFromBlock, 4> lengths;
    Vector<ValueFromBlock, 4> modes;
    auto carry = [&](const ArrayElements& elements, Integer length, Word32 mode) {
        butterflies.append(m_out.anchor(elements.butterfly.value()));
        lengths.append(m_out.anchor(length.value()));
        modes.append(m_out.anchor(mode.value()));
    };
    ArrayElements storage = known ? ArrayElements { array, Pointer(known->butterfly) } : elements().elementsOf(array);
    Word32 length = known ? low32(Integer(known->length)) : elements().publicLengthAsWord(storage);
    std::optional<Word32> knownMode;
    if (known)
        knownMode = Word32(known->mode);
    else if (behind)
        knownMode = elements().indexingMode(array, IndexingShapeAndWritabilityMask);
    auto indexingMode = [&] {
        return knownMode ? *knownMode : elements().indexingMode(array, IndexingShapeAndWritabilityMask);
    };
    branchUnless(below(length, elements().vectorLengthAsWord(storage)), isFull);
    auto finishStore = [&] {
        Word32 newLength = add(length, word32(1));
        elements().setPublicLength(storage, newLength);
        prefetchPushedCell(call, pushed);
        Operand result = representation == Representation::Void ? Operand() : lengthResult(call, zeroExtended(newLength));
        if (behind)
            carry(storage, zeroExtended(newLength), *knownMode);
        join.add(result);
    };
    auto store = [&](const ArrayStorageForm& form) {
        Operand rawElement = *elements().rawElementToStore(form, pushed, slowPath);
        if (form.holdsCells && mayPushCell)
            branchUnless(emitter<BarrierEmitter>().isBeyondBarrierThreshold(array), slowPath);
        elements().stopWhereHoleWouldBeStored(call.context(), form, rawElement);
        elements().storeRawElement(form, storage, known ? Integer(known->length) : zeroExtended(length), rawElement);
        if (joinsForms)
            m_out.jump(isStored);
        else
            finishStore();
    };
    if (!goesByKindOfNumbers && forms.size() > 1) {
        LBasicBlock hasOtherKind = newBlock();
        ArrayStorageFormList expectedForms;
        expectedForms.append(forms.first());
        forms.removeAt(0);
        size_t placeOfInt32s = forms.findIf([](const ArrayStorageForm* form) {
            return form->kind == ArrayStorage::Int32;
        });
        bool expectsInt32s = expectedForms.first()->kind == ArrayStorage::Contiguous && placeOfInt32s != notFound && forms.size() > 1;
        if (expectsInt32s) {
            expectedForms.append(forms[placeOfInt32s]);
            forms.removeAt(placeOfInt32s);
        }
        elements().forEachFormByShape(indexingMode(), &ElementAccessEmitter::hasWritableShape, expectedForms, hasOtherKind, store, expectsInt32s ? ElementAccessEmitter::FirstForm::IsLikeTheOthers : ElementAccessEmitter::FirstForm::IsExpected);
        m_out.appendTo(hasOtherKind);
    }
    elements().forEachFormByShape(indexingMode(), &ElementAccessEmitter::hasWritableShape, forms, slowPath, store, ElementAccessEmitter::FirstForm::IsLikeTheOthers);

    if (joinsForms) {
        m_out.appendTo(isStored);
        finishStore();
    }

    if (loop) {
        m_out.appendTo(isFull);
        reserveForRemainingIterations(call, *loop, array, length, slowPath);
    }

    m_out.appendTo(slowPath);
    Operand lengthBehindSlowPath = pushOutOfLine(call, array, pushed, representation);
    if (behind) {
        ArrayElements grown = elements().elementsOf(array);
        carry(grown, elements().publicLength(grown), elements().indexingMode(array, IndexingShapeAndWritabilityMask));
    }
    join.add(lengthBehindSlowPath);
    Operand result = join.finish();
    if (behind) {
        behind->butterfly = m_out.phi(Machine::Int64, butterflies);
        behind->length = m_out.phi(Machine::Int64, lengths);
        behind->mode = m_out.phi(Machine::Int32, modes);
    }
    finishInlineCall(call, result);
}

void ArrayStorageBuiltinEmitter::reserveForRemainingIterations(const InlineCall& call, const CodeGenerator::PushesInLoop& loop, CellPointer array, Word32 length, LBasicBlock done)
{
    static constexpr int64_t growthFactor = 16;
    static constexpr int64_t smallestGrowth = 64;
    auto numberOf = [&](ValueID id) -> std::optional<Number> {
        Operand operand = get(id);
        switch (operand.representation) {
        case Representation::Int32:
            return toNumber(operand.as<Word32>());
        case Representation::Int64:
            return toNumber(operand.as<Integer>());
        case Representation::Double:
            return operand.as<Number>();
        case Representation::JSValue:
            branchUnless(isInt32(operand.as<Boxed>()), done);
            return toNumber(unboxInt32(operand.as<Boxed>()));
        default:
            return std::nullopt;
        }
    };
    std::optional<Number> bound = numberOf(loop.bound);
    std::optional<Number> counter = bound ? numberOf(loop.counter) : std::nullopt;
    if (!counter) {
        m_out.jump(done);
        return;
    }
    build().statistics().count(StatisticID::PushesReservingForLoops);
    Number remaining = sub(*bound, *counter);
    if (loop.includesBound)
        remaining = add(remaining, number(1));
    if (loop.valuesPerIteration > 1)
        remaining = mul(remaining, number(loop.valuesPerIteration));
    Number held = toNumber(zeroExtended(length));
    if (loop.startOfCounter) {
        std::optional<Number> start = numberOf(loop.startOfCounter);
        if (!start) {
            m_out.jump(done);
            return;
        }
        branchUnless(greaterThanOrEqual(held, mul(sub(*counter, *start), number(loop.valuesPerIteration))), done);
    }
    Number mostCapacity = add(mul(held, number(growthFactor)), number(smallestGrowth));
    callStub(call.context(), reserveArrayCapacity(), { array.value(), min(add(held, remaining), mostCapacity).value() });
    m_out.jump(done);
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
