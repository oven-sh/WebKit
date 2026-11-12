/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTElementAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTBufferBuiltinEmitter.h"
#include "AOTBuiltinTypes.h"
#include "AOTConversionEmitter.h"
#include "AOTGuessedAccessEmitter.h"
#include "AOTPureOperations.h"
#include "AOTResultRules.h"
#include "AOTSlotEmitter.h"
#include "AOTStaticsEmitter.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeArena.h"
#include "AOTTypedArrayKinds.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "JSArrayBufferView.h"
#include "PureNaN.h"
#include <limits>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class TypedArraySupport final : public BuiltinFamilyEmitter {
public:
    explicit TypedArraySupport(CodeGenerator& generator)
        : BuiltinFamilyEmitter(generator)
    {
    }

    using BuiltinFamilyEmitter::cellOperand;
    using BuiltinFamilyEmitter::inRepresentation;
    using BuiltinFamilyEmitter::numberOperand;

    Integer lengthOf(CellPointer view);
};

Integer TypedArraySupport::lengthOf(CellPointer view)
{
    return either<Integer>(hasNoBits(loadByte(view, m_heaps.JSArrayBufferView_mode), resizabilityAndAutoLengthMask), [&] {
        return loadField<Integer>(view, m_heaps.JSArrayBufferView_length);
    }, [&] {
        return callLeaf<Integer>(RuntimeOperation::TypedArrayLength, { view });
    });
}

} // anonymous namespace

TypedArrayElements ElementAccessEmitter::typedArrayElementsOf(CellPointer view, LBasicBlock isNotFixedLength)
{
    branchUnless(hasNoBits(loadByte(view, m_heaps.JSArrayBufferView_mode), resizabilityAndAutoLengthMask), isNotFixedLength);
    return { view, loadField<Integer>(view, m_heaps.JSArrayBufferView_length), loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector) };
}

TypedArrayElements ElementAccessEmitter::typedArrayElementsOf(ValueID id, LBasicBlock isNotFixedLength)
{
    CellPointer view = in<CellPointer>(id);
    const Operation& definition = operationAt(definitionOf(id));
    if (definition.opcode != Opcode::CallInline || definition.inlineBuiltin() != InlineBuiltin::TypedArrayWithLength)
        return typedArrayElementsOf(view, isNotFixedLength);
    return { view, loadField<Integer>(view, m_heaps.JSArrayBufferView_length), loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector) };
}

TypedArrayElements ElementAccessEmitter::typedArrayElementsInsideView(const ValueContext& value, CellPointer view)
{
    std::optional<Pointer> vector = inOptional<Pointer>(value->vectorOperand());
    if (!checksBounds())
        return { view, loadField<Integer>(view, m_heaps.JSArrayBufferView_length), vector ? *vector : loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector) };
    CellPointer asked = askedAnew(view);
    Pointer inCell = loadField<Pointer>(asked, m_heaps.JSArrayBufferView_vector);
    if (vector)
        stopUnlessInside(value, equal(*vector, inCell));
    return { view, TypedArraySupport(generator()).lengthOf(asked), vector ? *vector : inCell };
}

TypedPointer ElementAccessEmitter::elementInsideView(const ValueContext& access)
{
    RELEASE_ASSERT(access->isInsideView());
    Integer index = indexInsideLength(access->positionOperand());
    TypedArrayElements elements = typedArrayElementsInsideView(access, in<CellPointer>(access->viewOperand()));
    holdPositionInsideView(access, index, elements);
    return TypedPointer(m_heaps.TypedArrayProperties, offsetBy(elements.vector, index, logElementSize(access->typedArrayType())).value());
}

TypedPointer ElementAccessEmitter::elementBelowNamedLength(const ValueContext& access)
{
    Integer index = indexInsideLength(access->positionOperand());
    Pointer vector = in<Pointer>(access->vectorOperand());
    return TypedPointer(m_heaps.TypedArrayProperties, offsetBy(vector, index, logElementSize(access->typedArrayType())).value());
}

std::optional<TypedArrayElements> ElementAccessEmitter::typedArrayElementsNamedBy(const ValueContext& value, CellPointer view)
{
    if (value->lengthOperands().empty())
        return std::nullopt;
    TypedArrayElements named { view, integerOf(value->lengthOperands()[0]), in<Pointer>(value->vectorOperand()) };
    if (checksBounds()) {
        CellPointer asked = askedAnew(view);
        Condition isVectorOfView = equal(named.vector, loadField<Pointer>(asked, m_heaps.JSArrayBufferView_vector));
        stopUnlessInside(value, value->isMirroredView() ? logicalOr(isVectorOfView, equal(named.length, 0)) : isVectorOfView);
        stopUnlessInside(value, belowOrEqual(named.length, TypedArraySupport(generator()).lengthOf(asked)));
    }
    return named;
}

void ElementAccessEmitter::askViewWherePositionIsNotBelowNamedLength(LBasicBlock isNotBelow, const ScopeFork& fork, const ValueContext& access, CellPointer view, LBasicBlock outside, LBasicBlock otherwise)
{
    m_out.appendToJoin(isNotBelow, fork);
    CellPointer viewAskedAnew = access->isMirroredView() ? viewOnSlowPath(access, view) : CellPointer(m_out.opaque(view.value()));
    branchUnless(hasNoBits(loadByte(viewAskedAnew, m_heaps.JSArrayBufferView_mode), resizabilityAndAutoLengthMask), otherwise);
    m_out.jump(outside);
}

bool ElementAccessEmitter::becomesIndexWithoutTest(const ValueContext& access)
{
    return representationOf(access->positionOperand()) != Representation::Double || (access->keyIsIntegral() && isX86_64());
}

void ElementAccessEmitter::holdPositionInsideView(const ValueContext& access, Integer index, const TypedArrayElements& elements)
{
    if (checksBounds()) {
        if (Operand position = get(access->positionOperand()); position.is<Number>())
            stopUnlessInside(access, equal(position.as<Number>(), position.as<Number>()));
        stopUnlessInside(access, below(index, elements.length));
    } else
        build().statistics().count(StatisticID::RangeChecksRemoved);
}

Operand ElementAccessEmitter::loadBytesOfTypedArrayElement(const TypedArrayElements& elements, Integer index, TypedArrayType type)
{
    RELEASE_ASSERT(hasInlineElementAccess(type));
    TypedPointer element(m_heaps.TypedArrayProperties, offsetBy(elements.vector, index, logElementSize(type)).value());
    return emitter<SlotEmitter>().load(element, elementStore(type));
}

Operand ElementAccessEmitter::loadTypedArrayElement(const TypedArrayElements& elements, Integer index, TypedArrayType type)
{
    Operand loaded = loadBytesOfTypedArrayElement(elements, index, type);
    if (isFloat(type))
        return emitter<ConversionEmitter>().purified(loaded.as<Number>());
    return loaded;
}

Condition ElementAccessEmitter::viewIsOfKind(ValueID view, TypedArrayType kind)
{
    return equal(loadByte(in<CellPointer>(view), m_heaps.JSCell_typeInfoType), static_cast<int32_t>(typeForTypedArrayType(kind)));
}

void ElementAccessEmitter::forEachKindOfView(const ValueContext& value, CellPointer view, NOESCAPE const ScopedLambda<void(TypedArrayType)>& inArm)
{
    uint32_t kinds = kindsOfViewsOfNumbersIn(m_program, types(), typeIDOf(value->viewOperand()));
    RELEASE_ASSERT(kinds);
    Vector<SwitchTarget, 10> targets;
    Vector<std::pair<LBasicBlock, TypedArrayType>, 10> arms;
    for (uint32_t kind = 0; kind < 32; ++kind) {
        if (!(kinds >> kind & 1))
            continue;
        arms.append({ newBlock(), static_cast<TypedArrayType>(kind) });
        targets.append(SwitchTarget { static_cast<int64_t>(typeForTypedArrayType(static_cast<TypedArrayType>(kind))), normalEdge(arms.last().first) });
    }
    Word32 type = loadByte(view, m_heaps.JSCell_typeInfoType);
    LBasicBlock isOfNoListedKind = checksBounds() ? newBlock() : nullptr;
    size_t lastAsked = isOfNoListedKind ? 0 : 1;
    for (size_t index = targets.size(); index-- > lastAsked;) {
        LBasicBlock narrower = index > lastAsked ? newBlock() : nullptr;
        m_out.switchOn(type.value(), targets.subspan(index, 1), narrower ? normalEdge(narrower) : isOfNoListedKind ? rareEdge(isOfNoListedKind) : targets[0].target);
        if (narrower)
            m_out.appendTo(narrower);
    }
    if (isOfNoListedKind) {
        m_out.appendTo(isOfNoListedKind);
        emitter<ThrowEmitter>().crashOnFalseProof(value, std::nullopt, ProofSource::OtherOperation);
    }
    for (auto [arm, kind] : arms) {
        m_out.appendTo(arm);
        inArm(kind);
    }
}

Word32 ElementAccessEmitter::clampedToUint8(Integer value)
{
    Integer nonNegative = select(lessThan(value, 0), 0, value);
    return low32(select(greaterThan(nonNegative, UINT8_MAX), UINT8_MAX, nonNegative));
}

Word32 ElementAccessEmitter::clampedToUint8(Word32 value)
{
    Word32 nonNegative = select(lessThan(value, 0), 0, value);
    return select(greaterThan(nonNegative, UINT8_MAX), UINT8_MAX, nonNegative);
}

Word32 ElementAccessEmitter::clampedToUint8(Number value)
{
#if CPU(X86_64)
    Machine::PureFunction function = pureFunction(PureOperation::ClampToByte, 0, true);
    std::array<LValue, 3> operands { value.value(), m_out.constDouble(0), m_out.constDouble(UINT8_MAX) };
    LValue clamped = m_out.findPurePatch(function, Machine::Double, operands);
    if (!clamped) {
        Machine::PatchOperation* clampedNumber = m_out.patchpoint(Machine::Double);
        for (LValue operand : operands)
            clampedNumber->appendSomeRegister(operand);
        clampedNumber->effects = Machine::Effects::none();
        clampedNumber->pureFunction = function;
        clampedNumber->setRunsWithoutFrame(true);
        clampedNumber->setGenerator([](CCallHelpers& jit, const Machine::PatchParameters& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            FPRReg destination = params[0].fpr();
            FPRReg zero = params[2].fpr();
            FPRReg greatest = params[3].fpr();
            if (destination == greatest) {
                jit.moveDouble(greatest, MacroAssembler::fpTempRegister);
                greatest = MacroAssembler::fpTempRegister;
            }
            jit.maxOrSecondDouble(params[1].fpr(), zero, destination);
            jit.minOrSecondDouble(destination, greatest, destination);
        });
        m_out.recordPurePatch(clampedNumber);
        clamped = clampedNumber;
    }
    return low32(Integer(m_out.bitCast(m_out.doubleAdd(clamped, m_out.constDouble(0x1p52)), Machine::Int64)));
#else
    Number nonNegative = select(greaterThan(value, number(0)), value, number(0));
    Number clamped = select(lessThan(nonNegative, number(UINT8_MAX)), nonNegative, number(UINT8_MAX));
    return low32(emitter<ConversionEmitter>().toInteger(sub(add(clamped, number(0x1p52)), number(0x1p52))));
#endif
}

void ElementAccessEmitter::storeTypedArrayElement(const TypedArrayElements& elements, Integer index, TypedArrayType type, Operand stored)
{
    SlotRepresentation slot = elementStore(type);
    if (hasIntegerElements(type)) {
        Word32 bits;
        static_assert(ConversionEmitter::numberOfLowestBits >= 8);
        if (stored.is<Number>())
            bits = emitter<ConversionEmitter>().toInt32(stored.as<Number>(), elementSize(type) == 1 && isX86_64() ? ConversionEmitter::BitsOfInt32::Lowest : ConversionEmitter::BitsOfInt32::All);
        else
            bits = stored.is<Integer>() ? low32InWords(stored.as<Integer>()) : stored.as<Word32>();
        stored = slot == SlotRepresentation::Uint32 ? Operand(zeroExtended(bits)) : Operand(bits);
    } else if (isClamped(type))
        stored = stored.is<Number>() ? clampedToUint8(stored.as<Number>()) : stored.is<Word32>() ? clampedToUint8(stored.as<Word32>()) : clampedToUint8(stored.as<Integer>());

    TypedPointer element(m_heaps.TypedArrayProperties, offsetBy(elements.vector, index, logElementSize(type)).value());
    emitter<SlotEmitter>().store(element, slot, stored);
}

static bool comparesWordWithLength(Integer index, TypedArrayType type)
{
    return index.value()->opcode() == Machine::SExt32 && type != NotTypedArray && logElementSize(type);
}

Condition ElementAccessEmitter::isBelowLengthOfView(Integer index, Integer length, TypedArrayType type)
{
    static_assert(MAX_ARRAY_BUFFER_SIZE >> 1 <= 1ull << 31);
    if (!comparesWordWithLength(index, type))
        return below(index, length);
    return below(Word32(index.value()->child(0)), low32(length));
}

Integer ElementAccessEmitter::indexFoundBelowLength(Integer index, TypedArrayType type)
{
    if (!comparesWordWithLength(index, type))
        return index;
    return zeroExtended(Word32(index.value()->child(0)));
}

void ElementAccessEmitter::generateGetTypedArrayLength(const ValueContext& value)
{
    define(value, TypedArraySupport(generator()).lengthOf(in<CellPointer>(value->viewOperand())));
}

void ElementAccessEmitter::generateGetTypedArrayVector(const ValueContext& value)
{
    build().statistics().count(StatisticID::VectorsOfViewsRead);
    define(value, loadField<Pointer>(in<CellPointer>(value->viewOperand()), m_heaps.JSArrayBufferView_vector));
}

void ElementAccessEmitter::generateGetMirroredLength(const ValueContext& value)
{
    define(value, emitter<StaticsEmitter>().loadMirroredLength(value->variable(), value->isOfBuffer()));
}

void ElementAccessEmitter::generateIsBelowLength(const ValueContext& value)
{
    static_assert(MAX_ARRAY_BUFFER_SIZE >> 1 <= 1ull << 31);
    Operand position = get(value->positionOperand());
    bool fitsWord = typeOf(value->lengthOperand()).integers.isSubsetOf(Range { 0, 1ll << 31 });
    bool comparesWords = position.representation == Representation::Int32 && fitsWord;
    Condition isBelow = comparesWords ? below(position.as<Word32>(), low32(integerOf(value->lengthOperand()))) : below(integerOf(value->positionOperand()), integerOf(value->lengthOperand()));
    defineTestByBranching(value, [&](BranchTargets targets) {
        branch(isBelow, targets);
    });
}

void ElementAccessEmitter::generateGetDataViewLimit(const ValueContext& value)
{
    CellPointer view = in<CellPointer>(value->viewOperand());
    Integer limit = loadField<Integer>(view, m_heaps.JSArrayBufferView_length);
    if (unsigned size = elementSize(value->typedArrayType()); size > 1) {
        Integer lessLastBytes = sub(limit, integer(size - 1));
        limit = select(lessThan(lessLastBytes, 0), 0, lessLastBytes);
    }
    define(value, select(hasNoBits(loadByte(view, m_heaps.JSArrayBufferView_mode), resizabilityAndAutoLengthMask), limit, 0));
}

Pointer ElementAccessEmitter::bytesInsideDataView(const ValueContext& access)
{
    Integer offset = indexInsideLength(access->positionOperand());
    Pointer vector = in<Pointer>(access->vectorOperand());
    if (checksBounds()) {
        CellPointer asked = askedAnew(in<CellPointer>(access->viewOperand()));
        stopUnlessInside(access, hasNoBits(loadByte(asked, m_heaps.JSArrayBufferView_mode), resizabilityAndAutoLengthMask));
        stopUnlessInside(access, equal(vector, loadField<Pointer>(asked, m_heaps.JSArrayBufferView_vector)));
        stopUnlessInside(access, greaterThanOrEqual(offset, integer(0)));
        stopUnlessInside(access, below(add(offset, integer(elementSize(access->typedArrayType()) - 1)), loadField<Integer>(asked, m_heaps.JSArrayBufferView_length)));
    } else
        build().statistics().count(StatisticID::RangeChecksRemoved);
    return offsetBy(vector, offset, 0);
}

void ElementAccessEmitter::generateGetDataViewElement(const ValueContext& value)
{
    TypedArrayType elements = value->typedArrayType();
    BufferBuiltinEmitter::ByteOrder order { value->isLittleEndian(), { } };
    if (value->littleEndianOperand())
        order = { std::nullopt, get(value->littleEndianOperand()).as<Condition>() };
    Pointer bytes = bytesInsideDataView(value);
    if (readsNumberOutOfBytes(value.value())) {
        Operand number = emitter<BufferBuiltinEmitter>().loadBytes(value, bytes, elements, order, BufferBuiltinEmitter::NaNBits::AsStored);
        define(value, generator().settleNaNOf(value, number.as<Number>()));
        return;
    }
    Operand element = emitter<BufferBuiltinEmitter>().loadBytes(value, bytes, elements, order);
    define(value, TypedArraySupport(generator()).inRepresentation(value, value.representation(), element, types().at(elementTypeOf(elements))));
}

void ElementAccessEmitter::generatePutDataViewElement(const ValueContext& value)
{
    TypedArrayType elements = value->typedArrayType();
    BufferBuiltinEmitter::ByteOrder order { value->isLittleEndian(), { } };
    if (value->littleEndianOperand())
        order = { std::nullopt, get(value->littleEndianOperand()).as<Condition>() };
    ValueID storedValue = value->valueOperand();
    Operand stored = takesAnyNaN(value.value(), OperandIndex::PutDataViewElement::value) ? generator().getWithAnyNaN(storedValue) : get(storedValue);
    if (isFloat(elements))
        stored = stored.is<Number>() ? stored : Operand(TypedArraySupport(generator()).numberOperand(storedValue));
    else if (stored.is<Number>())
        stored = emitter<ConversionEmitter>().toInt32(stored.as<Number>(), elementSize(elements) == 1 && isX86_64() ? ConversionEmitter::BitsOfInt32::Lowest : ConversionEmitter::BitsOfInt32::All);
    else if (stored.is<Integer>())
        stored = low32InWords(stored.as<Integer>());
    emitter<BufferBuiltinEmitter>().storeBytes(bytesInsideDataView(value), elements, stored, order);
}

void ElementAccessEmitter::generateGetMirroredVector(const ValueContext& value)
{
    build().statistics().count(StatisticID::VectorsOfViewsRead);
    define(value, emitter<StaticsEmitter>().loadMirroredVector(value->variable()));
}

CellPointer ElementAccessEmitter::viewOnSlowPath(const ValueContext& access, CellPointer view)
{
    if (!access->isMirroredView())
        return view;
    VariableID variable = operationAt(definitionOf(access->vectorOperand())).variable();
    return emitter<StaticsEmitter>().loadMirroredView(variable);
}

void ElementAccessEmitter::generateGetTypedArrayElement(const ValueContext& value)
{
    generator().forgetBoxed(value.id());
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateElementRead(value);
        return;
    }

    TypedArrayType type = value->typedArrayType();
    CellPointer view = in<CellPointer>(value->viewOperand());
    Representation representation = value.representation();
    auto readByValue = [&] {
        return emitter<ConversionEmitter>().unbox(readOutOfLine(value, viewOnSlowPath(value, view)), typeOf(value.id()), representation);
    };
    bool isOfSeveralKinds = type == NotTypedArray;
    if (!isOfSeveralKinds && !hasInlineElementAccess(type)) {
        define(value, readByValue());
        return;
    }

    Type elementType = types().at(isOfSeveralKinds ? TypeID(TypeTable::number) : elementTypeOf(type));
    auto someKindHoldsFloats = [&] {
        if (std::optional<TypedArrayType> found = generator().kindOfViewFoundInFrontOfTurnsLaidOut(value->viewOperand()))
            return isFloat(*found);
        uint32_t kinds = kindsOfViewsOfNumbersIn(m_program, types(), typeIDOf(value->viewOperand()));
        for (uint32_t kind = 0; kind < 32; ++kind) {
            if (kinds >> kind & 1 && isFloat(static_cast<TypedArrayType>(kind)))
                return true;
        }
        return false;
    };
    auto loadElement = [&](const TypedArrayElements& elements, Integer index, bool keepsBitsOfNaN = false) -> Operand {
        if (!isOfSeveralKinds)
            return loadTypedArrayElement(elements, index, type);
        auto loadOfKind = [&](TypedArrayType kind) {
            Operand loaded = keepsBitsOfNaN ? loadBytesOfTypedArrayElement(elements, index, kind) : loadTypedArrayElement(elements, index, kind);
            return TypedArraySupport(generator()).inRepresentation(value, Representation::Double, loaded, types().at(elementTypeOf(kind)));
        };
        if (std::optional<TypedArrayType> found = kindOfViewFoundInFrontOfTurns(value, value->viewOperand()))
            return loadOfKind(*found);
        Join<Number> ofAnyKind(m_out);
        forEachKindOfView(value, elements.view, scopedLambda<void(TypedArrayType)>([&](TypedArrayType kind) {
            ofAnyKind.add(loadOfKind(kind).as<Number>());
        }));
        return ofAnyKind.finish();
    };
    if (value->isInsideView()) {
        build().statistics().count(StatisticID::AccessesInsideViews);
        Integer index = indexInsideLength(value->positionOperand());
        TypedArrayElements elements = typedArrayElementsInsideView(value, view);
        holdPositionInsideView(value, index, elements);
        if (readsNumberOutOfBytes(value.value())) {
            if (isOfSeveralKinds && !someKindHoldsFloats()) {
                generator().noteThatNumberIsNoNaN(value);
                define(value, loadElement(elements, index));
                return;
            }
            define(value, generator().settleNaNOf(value, (isOfSeveralKinds ? loadElement(elements, index, true) : loadBytesOfTypedArrayElement(elements, index, type)).as<Number>()));
            return;
        }
        Operand element = loadElement(elements, index);
        define(value, TypedArraySupport(generator()).inRepresentation(value, representation, element, elementType));
        return;
    }

    bool nothingIsNaN = !isOfSeveralKinds && generator().takesNothingForNaN(value);
    bool isBoxed = !isOfSeveralKinds && representation == Representation::OptionalInt64 && hasIntegerElements(type) && generator().isOnlyBoxed(value.id());
    if (isBoxed)
        representation = Representation::JSValue;
    LBasicBlock outside = newBlock();
    LBasicBlock otherwise = newBlock();
    OperandJoin join(m_out, nothingIsNaN ? Representation::Double : representation);
    std::optional<Integer> indexMadeInFront = becomesIndexWithoutTest(value) ? std::optional(indexOfAccessOrElse(value, outside, NaNIndexMode::ProduceOutOfBoundsIndex)) : std::nullopt;
    ScopeFork fork = m_out.forkScope();

    Integer index = indexMadeInFront ? *indexMadeInFront : indexOfAccessOrElse(value, outside, NaNIndexMode::ProduceOutOfBoundsIndex);
    std::optional<TypedArrayElements> named = typedArrayElementsNamedBy(value, view);
    LBasicBlock isNotBelowNamedLength = named ? newBlock() : nullptr;
    TypedArrayElements elements = named ? *named : typedArrayElementsOf(value->viewOperand(), otherwise);
    branchUnless(isBelowLengthOfView(index, elements.length, type), named ? isNotBelowNamedLength : outside);
    if (nothingIsNaN && isFloat(type))
        join.add(loadBytesOfTypedArrayElement(elements, indexFoundBelowLength(index, type), type));
    else {
        Operand element = loadElement(elements, indexFoundBelowLength(index, type));
        join.add(TypedArraySupport(generator()).inRepresentation(value, nothingIsNaN ? Representation::Double : representation, element, elementType));
    }

    if (named)
        askViewWherePositionIsNotBelowNamedLength(isNotBelowNamedLength, fork, value, view, outside, otherwise);
    m_out.appendToJoin(outside, fork);
    join.add(nothingIsNaN ? Operand(number(PNaN)) : absentElement(value, representation));

    m_out.appendToJoin(otherwise, fork);
    join.add(nothingIsNaN ? Operand(emitter<ConversionEmitter>().withoutPayload(Number(readByValue().value))) : readByValue());
    Operand read = join.finishBehind(fork);
    if (isBoxed) {
        build().statistics().count(StatisticID::NumberElementsReadBoxed);
        generator().defineBoxed(value, read.as<Boxed>());
        return;
    }
    define(value, nothingIsNaN ? Operand(OptionalNumber(read.value)) : read);
}

void ElementAccessEmitter::generatePutTypedArrayElement(const ValueContext& value)
{
    if (GuessedAccessEmitter::testsForItself(value)) {
        emitter<GuessedAccessEmitter>().generateElementStore(value);
        return;
    }

    TypedArrayType type = value->typedArrayType();
    ValueID storedValue = value->valueOperand();
    if (!typeOf(storedValue).hasOnlyPrimitives(isBigInt(type) ? Primitive::BigInt : Primitive::Number)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }

    TypedArraySupport support(generator());
    CellPointer view = in<CellPointer>(value->viewOperand());
    Operand stored = takesAnyNaN(value.value(), OperandIndex::PutTypedArrayElement::value) ? generator().getWithAnyNaN(storedValue) : get(storedValue);
    bool isOfSeveralKinds = type == NotTypedArray;
    bool storesInline = (isOfSeveralKinds || hasInlineElementAccess(type)) && !stored.is<Boxed>();
    auto elementToStore = [&] {
        return (isOfSeveralKinds || isFloat(type)) && !stored.is<Number>() ? Operand(support.numberOperand(storedValue)) : stored;
    };
    auto storeElement = [&](const TypedArrayElements& elements, Integer index) {
        Operand element = elementToStore();
        if (!isOfSeveralKinds) {
            bool isByte = isClamped(type) && typeOf(storedValue).isInteger() && typeOf(storedValue).integers.isSubsetOf(Range { 0, UINT8_MAX }) && !element.is<Number>();
            storeTypedArrayElement(elements, index, isByte ? TypeUint8 : type, element);
            return;
        }
        if (std::optional<TypedArrayType> found = kindOfViewFoundInFrontOfTurns(value, value->viewOperand())) {
            storeTypedArrayElement(elements, index, *found, element);
            return;
        }
        LBasicBlock isStored = newBlock();
        forEachKindOfView(value, elements.view, scopedLambda<void(TypedArrayType)>([&](TypedArrayType kind) {
            storeTypedArrayElement(elements, index, kind, element);
            m_out.jump(isStored);
        }));
        m_out.appendTo(isStored);
    };
    if (value->isInsideView() && storesInline) {
        build().statistics().count(StatisticID::AccessesInsideViews);
        Integer index = indexInsideLength(value->positionOperand());
        TypedArrayElements elements = typedArrayElementsInsideView(value, view);
        holdPositionInsideView(value, index, elements);
        storeElement(elements, index);
        return;
    }

    LBasicBlock done = newBlock();
    std::optional<Integer> indexMadeInFront = becomesIndexWithoutTest(value) ? std::optional(indexOfAccessOrElse(value, done, NaNIndexMode::ProduceOutOfBoundsIndex)) : std::nullopt;
    ScopeFork fork = m_out.forkScope();
    Integer index = indexMadeInFront ? *indexMadeInFront : indexOfAccessOrElse(value, done, NaNIndexMode::ProduceOutOfBoundsIndex);
    if (isBigInt(type)) {
        Integer length = support.lengthOf(view);
        branchUnless(below(index, length), done);
        TypedArrayElements elements { view, length, loadField<Pointer>(view, m_heaps.JSArrayBufferView_vector) };
        storeTypedArrayElement(elements, index, type, emitter<BufferBuiltinEmitter>().low64Of(support.cellOperand(storedValue)));
        m_out.jump(done);

        m_out.appendToJoin(done, fork);
        return;
    }

    LBasicBlock otherwise = newBlock();
    if (storesInline) {
        std::optional<TypedArrayElements> named = typedArrayElementsNamedBy(value, view);
        LBasicBlock isNotBelowNamedLength = named ? newBlock() : nullptr;
        TypedArrayElements elements = named ? *named : typedArrayElementsOf(value->viewOperand(), otherwise);
        branchUnless(isBelowLengthOfView(index, elements.length, type), named ? isNotBelowNamedLength : done);
        storeElement(elements, indexFoundBelowLength(index, type));
        m_out.jump(done);
        if (named)
            askViewWherePositionIsNotBelowNamedLength(isNotBelowNamedLength, fork, value, view, done, otherwise);
    } else
        m_out.jump(otherwise);

    m_out.appendToJoin(otherwise, fork);
    bool storedBearsAnyNaN = stored.is<Number>() && generator().holdsAnyNaN(storedValue);
    Number toStore = storedBearsAnyNaN ? emitter<ConversionEmitter>().purified(stored.as<Number>()) : support.numberOperand(storedValue);
    callVoidOperation(value, RuntimeOperation::TypedArrayPut, { viewOnSlowPath(value, view), index, toStore }, { .isOnRarePath = storesInline });
    m_out.jump(done);

    m_out.appendToJoin(done, fork);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
