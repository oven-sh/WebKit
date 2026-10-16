/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTTypedCompactTable.h"
#include "B3BasicBlockInlines.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"
#include "JSCInlines.h"
#include "PageCount.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

// Arrays, typed arrays and iteration. For strings, maps and sets see AOTTypedLoweringStrings.cpp.
//
// That a receiver is an array or a typed array is what its type says, and is not tested. What is found out when
// the code runs is what no type says: whether it is `undefined` or `null`, how an array keeps its elements, the bounds, whether there is
// a hole. Where there is no element, `undefined` is read: the prototypes of the engine have no elements and cannot be given any.
//
// What a container holds nobody vouches for: it is held to the type of the value that reads it.
//
// An array was as the engine makes them when it was taken for one. Code that is not typed may have given it another prototype, a property
// of its own in the place of a method, or an accessor for an element since. Then it is of another JSType (JSObject::becomeDerivedArray()),
// as an instance of a class that extends Array is from its birth. Of such an array indexingModeOf() yields none of the ways elements are
// kept, so that it comes to the operations by itself wherever that is looked at. An element of its own is read and stored in place all the
// same: indexingModeOfAnyArray(). Everywhere else isPlainArray() is asked. See AOTTypedOperations.h for what is done then.
//
// No path that reads runs code of the program, see keyedAccessOf(). So the back end keeps what it has loaded, and moves out of a loop what
// stays the same there: where the elements are, how many there are and how they are kept.
//
// A receiver that is only expected to be one of these is tested for it, see AOTTypedLoweringGeneric.cpp. Then only what the language says
// is done in place: an element that is there is read, an element is stored where the engine itself would just store it. The functions
// below take a block for that, `otherwise`: with it, whatever else there is to an access goes there, which is the general way.

// Changes elements and lengths, and runs no code of the program.
static constexpr OperationEffects effectsOfStoringElements { Writes::Heap, true, false };

// Of the typed arrays for which keyedAccessOf() says TypedArray.
static Store storeOfElements(TypedArrayType type)
{
    switch (type) {
    case TypeInt8:
        return Store::Int8;
    case TypeUint8:
    case TypeUint8Clamped:
        return Store::Uint8;
    case TypeInt16:
        return Store::Int16;
    case TypeUint16:
        return Store::Uint16;
    case TypeInt32:
        return Store::Int32;
    case TypeUint32:
        return Store::Uint32;
    case TypeFloat32:
        return Store::Float;
    case TypeFloat64:
        return Store::Double;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// As keyedAccessOf() has it.
static bool hasElementsReadInPlace(TypedArrayType type)
{
    switch (type) {
    case TypeInt8:
    case TypeUint8:
    case TypeUint8Clamped:
    case TypeInt16:
    case TypeUint16:
    case TypeInt32:
    case TypeUint32:
    case TypeFloat32:
    case TypeFloat64:
        return true;
    default:
        return false;
    }
}

// Operands.

// A key that is a number, or the count of the steps of an iteration, as an integer. It may still be negative or beyond every length.
Integer Lowering::indexOrElse(ValueID key, LBasicBlock otherwise, bool boundsCheckRefusesNegatives)
{
    Operand operand = get(key);
    switch (operand.rep) {
    case Rep::Int64:
        return operand.as<Integer>();
    case Rep::OptionalInt64:
        orElse(logicalNot(isMarker(operand)), otherwise);
        return assumedInteger(operand.as<OptionalInteger>());
    case Rep::Double:
    // What stands for `undefined` or `null` is not a number, and equal to nothing.
    case Rep::OptionalDouble: {
        if (auto exact = integerOfNumber(key)) {
            if (boundsCheckRefusesNegatives)
                return *exact;
            ValueID lowered = valueBeingLowered();
            bool isKey = lowered && (m_function.at(lowered).opcode == Opcode::GetByVal || m_function.at(lowered).opcode == Opcode::PutByVal);
            orElse(isKey ? greaterThanOrEqual(*exact, integer(0)) : notEqual(*exact, integer(INT64_MIN)), otherwise);
            return *exact;
        }
        Number number = operand.rep == Rep::Double ? operand.as<Number>() : assumedNumber(operand.as<OptionalNumber>());
        Integer index = truncateToInteger(number);
        if (isX86_64()) {
            if (!m_wholeNumbers)
                m_wholeNumbers.emplace(m_program, m_function);
            if (m_wholeNumbers->contains(key)) {
                if (!boundsCheckRefusesNegatives)
                    orElse(notEqual(index, integer(INT64_MIN)), otherwise);
                return index;
            }
        }
        orElse(boundsCheckRefusesNegatives && isX86_64() ? equalOrUnordered(convertToNumber(index), number) : equal(convertToNumber(index), number), otherwise);
        return index;
    }
    case Rep::JSValue: {
        Boxed boxed = operand.as<Boxed>();
        LBasicBlock isSmall = m_out.newBlock();
        LBasicBlock isNotSmall = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isInt32(boxed).value(), usually(isSmall), rarely(isNotSmall));
        m_out.appendTo(isSmall);
        ValueFromBlock narrow = m_out.anchor(signExtended(low32(boxed)).value());
        m_out.jump(continuation);
        // The engine may keep an integral number as a double.
        m_out.appendTo(isNotSmall);
        orElse(isNumber(boxed), otherwise);
        Number number = unboxedDouble(boxed);
        Integer index = truncateToInteger(number);
        orElse(equal(convertToNumber(index), number), otherwise);
        ValueFromBlock large = m_out.anchor(index.value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return Integer(m_out.phi(Int64, narrow, large));
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// What is read where there is nothing. Nothing: `undefined` is not of the type, and the block has been ended with a TypeError.
std::optional<Operand> Lowering::absentElement(const Type& type)
{
    auto& undefinedType = m_program.at(m_program.types.undefined);
    if (type.primitives & Primitive::Undefined)
        return ensure(undefined(), undefinedType, type);
    throwTypeMismatch(undefined(), undefinedType, type);
    return std::nullopt;
}

Boxed Lowering::getByValGenerally(ValueID object, ValueID key)
{
    Boxed base = in<Boxed>(object);
    Operand operand = get(key);
    if (operand.rep == Rep::Int64)
        return Boxed(callICStub(Stub::GetByValAtIndex, Int64, { { base.value(), firstStubOperandGPR }, { operand.value, GPRInfo::argumentGPR1 } }));
    if (operand.rep == Rep::Cell && get(object).rep == Rep::Cell)
        return Boxed(callICStub(Stub::GetByValCachedOfCells, Int64, { { base.value(), firstStubOperandGPR }, { operand.value, GPRInfo::argumentGPR1 } }));
    return emitGetByVal(base, convertTo<Boxed>(operand, typeOf(key)), !expectedTypeOf(key).isNumber());
}

void Lowering::putByValGenerally(ValueID object, ValueID key, ValueID stored)
{
    Boxed base = in<Boxed>(object);
    Operand operand = get(key);
    if (operand.rep != Rep::Int64) {
        Boxed boxedKey = convertTo<Boxed>(operand, typeOf(key));
        emitPutByVal(base, boxedKey, in<Boxed>(stored), !expectedTypeOf(key).isNumber());
        return;
    }
    uint32_t isStrict = !isInSloppyCode();
    callICStub(Stub::PutByValAtIndex, Void, { { base.value(), firstStubOperandGPR }, { operand.value, GPRInfo::argumentGPR1 }, { in<Boxed>(stored).value(), GPRInfo::argumentGPR2 } }, std::pair { GPRInfo::argumentGPR3, isStrict });
}

// Arrays.

Pointer Lowering::butterflyOf(CellPointer object)
{
    return loadField<Pointer>(object, m_heaps.JSObject_butterfly, safeIfReaches(object, sizeof(JSObject)));
}

// Of the butterfly of an object that has indexed properties: no other has the lengths.
Integer Lowering::publicLength(Pointer butterfly)
{
    return zeroExtended(loadField<Word32>(butterfly, m_heaps.Butterfly_publicLength));
}

Integer Lowering::vectorLength(Pointer butterfly)
{
    return zeroExtended(loadField<Word32>(butterfly, m_heaps.Butterfly_vectorLength));
}

// Every array has a butterfly, with the length in the same place however the elements are kept.
Integer Lowering::arrayLength(CellPointer array)
{
    return publicLength(butterflyOf(array));
}

// Of what is an array for a fact, see EngineClass::type. Unlike that of any other cell, its JSType may change, once: typeOfCell() tells
// the back end that it never does, which will do wherever the two types of arrays are not told apart.
Word32 Lowering::isPlainArray(CellPointer array)
{
    return equal(loadByte(array, m_heaps.JSCell_typeInfoType, safeIfReaches(array, sizeof(JSCell))), word32(ArrayType));
}

// Of any cell: whether it is what an array that was taken for one becomes.
Word32 Lowering::isChangedArray(CellPointer cell)
{
    static_assert(!NoIndexingShape && UndecidedShape == 2);
    return logicalAnd(equal(loadByte(cell, m_heaps.JSCell_typeInfoType, safeIfReaches(cell, sizeof(JSCell))), word32(DerivedArrayType)), aboveOrEqual(indexingModeOfAnyArray(cell, IndexingShapeMask), word32(UndecidedShape)));
}

Word32 Lowering::indexingModeOf(CellPointer array, unsigned mask)
{
    static_assert(JSCell::typeInfoTypeOffset() == JSCell::indexingTypeAndMiscOffset() + 1 && DerivedArrayType == (ArrayType | 1) && !(ArrayType & 1));
    constexpr unsigned isNoPlainArray = 1 << 8;
    if (mask == IndexingShapeAndWritabilityMask)
        mask = 0xff & ~IsArray;
    return bitAnd(Word32(marked(m_out.load16ZeroExt32(array.value(), m_heaps.JSCell_usefulBytes), safeIfReaches(array, sizeof(JSCell)))), word32(mask | isNoPlainArray));
}

Word32 Lowering::indexingModeOfAnyArray(CellPointer array, unsigned mask)
{
    return bitAnd(loadByte(array, m_heaps.JSCell_indexingTypeAndMisc, safeIfReaches(array, sizeof(JSCell))), word32(mask));
}

void Lowering::findNumberArraysUsedInLoops()
{
    if (m_loops.isEmpty() || (!m_function.convertsArrayStorage && m_function.writes != Writes::Heap))
        return;
    auto isReadAsDouble = [&](const Type& type) {
        return isNumericOperand(type) && (repOf(type) == Rep::Double || repOf(type) == Rep::OptionalDouble);
    };
    auto isStoredAsDouble = [&](const Type& type) {
        return type.primitives & Primitive::Number && isNumericOperand(type) && !(type.isInteger() && type.integers.isWithin(Range::int32()));
    };
    for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
        if (m_innermostLoop[b] == noLoop)
            continue;
        auto& values = m_function.blocks[b].values;
        SetForScope currentBlock(m_currentBlock, &m_function.blocks[b]);
        for (size_t indexInBlock = 0; indexInBlock < values.size(); ++indexInBlock) {
            ValueID id = values[indexInBlock];
            auto& value = m_function.at(id);
            SetForScope currentIndexInBlock(m_currentIndexInBlock, indexInBlock);
            switch (value.opcode) {
            case Opcode::GetByVal:
            case Opcode::IteratorValue:
                if (!isReadAsDouble(m_program.at(value.type)) && !(value.opcode == Opcode::GetByVal && repOf(m_program.at(value.type)) == Rep::JSValue && isReadAsDouble(expectedTypeOf(id)) && isOnlyComparedWithNumbers(id)))
                    continue;
                break;
            case Opcode::PutByVal:
                if (!isStoredAsDouble(typeOf(value.child(2))))
                    continue;
                break;
            default:
                continue;
            }
            ValueID array = value.child(0);
            if (value.opcode == Opcode::IteratorValue) {
                if (keyedAccessOf(m_program, m_function, value) != KeyedAccess::Array)
                    continue;
            } else if (auto* engineClass = typeOf(array).onlyEngineClass(); !engineClass || engineClass->type != ArrayType || !isNumericOperand(typeOf(value.child(1))))
                continue;
            for (unsigned i = 0; i < m_loops.size(); ++i) {
                auto& loop = m_loops[i];
                if (!loop.blocks.get(b) || !loop.header.index() || !isLoopInvariant(loop, array))
                    continue;
                if (m_numberArraysConvertedInFrontOfLoops.isEmpty())
                    m_numberArraysConvertedInFrontOfLoops.grow(m_loops.size());
                if (!m_numberArraysConvertedInFrontOfLoops[i].contains(array))
                    m_numberArraysConvertedInFrontOfLoops[i].append(array);
                break;
            }
        }
    }
}

void Lowering::convertNumberArraysInFrontOfLoopsEnteredBy(const Terminator& terminator, bool isAtHeadOfBlock)
{
    if (m_numberArraysConvertedInFrontOfLoops.isEmpty())
        return;
    for (auto& target : successorsOf(terminator)) {
        for (unsigned i = 0; i < m_loops.size(); ++i) {
            if (m_loops[i].header != target.block || m_loops[i].blocks.get(m_currentBlockID.index()))
                continue;
            for (ValueID array : m_numberArraysConvertedInFrontOfLoops[i]) {
                bool isThereAtHeadOfBlock = m_definingBlock[array.index()] != m_currentBlockID || m_currentBlock->parameters.contains(array);
                if (isThereAtHeadOfBlock == isAtHeadOfBlock)
                    convertInt32sToDoubles(array);
            }
        }
    }
}

bool Lowering::isConvertedInFrontOfLoop(ValueID array) const
{
    if (m_numberArraysConvertedInFrontOfLoops.isEmpty())
        return false;
    for (unsigned i = 0; i < m_loops.size(); ++i) {
        if (m_loops[i].blocks.get(m_currentBlockID.index()) && m_numberArraysConvertedInFrontOfLoops[i].contains(array))
            return true;
    }
    return false;
}

void Lowering::convertInt32sToDoubles(ValueID array)
{
    Operand held = get(array);
    if (held.rep != Rep::Cell && held.rep != Rep::JSValue)
        return;
    auto& type = typeOf(array);
    LBasicBlock keepsInt32s = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    if (held.rep != Rep::Cell && type.mayBeUndefinedOrNull()) {
        LBasicBlock isArray = m_out.newBlock();
        m_out.branch(isCell(held.as<Boxed>(), type).value(), usually(isArray), rarely(continuation));
        m_out.appendTo(isArray);
    }
    CellPointer cell = held.rep == Rep::Cell ? held.as<CellPointer>() : asCell(held.as<Boxed>());
    static_assert(UndecidedShape + 2 == Int32Shape && !(UndecidedShape & 1));
    m_out.branch(belowOrEqual(binary<Word32>(&Output::sub, indexingModeOfAnyArray(cell, IndexingShapeMask), word32(UndecidedShape)), word32(Int32Shape - UndecidedShape)).value(), rarely(keepsInt32s), usually(continuation));
    m_out.appendTo(keepsInt32s);
    OperationEffects effects { Writes::Nothing, false };
    effects.convertsArrayStorage = true;
    callEntry(Entry::operationAOTTypedArrayTryMakeWritableDouble, Int64, false, effects, { cell.value() });
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

// Whether the key of an access, going by its type and by what is expected of it, may be what indexOrElse() takes.
bool Lowering::mayBeIndex(ValueID key) const
{
    Rep rep = repOf(typeOf(key));
    if (rep != Rep::Int64 && rep != Rep::Double && rep != Rep::OptionalInt64 && rep != Rep::OptionalDouble && rep != Rep::JSValue)
        return false;
    return expectedTypeOf(key).primitives & Primitive::Int;
}

// Whether the element that is being read is expected to be a number: not where all that is done with it is a cast, right behind the read, to a type that has none. That decides which ways
// of keeping elements there is code in place for, and nothing else.
bool Lowering::isExpectedToBeNumber(ValueID element)
{
    if (!(expectedTypeOf(element).primitives & Primitive::Number))
        return false;
    auto& values = m_currentBlock->values;
    size_t next = m_currentIndexInBlock + 1;
    if (next >= values.size() || numberOfUsesOf(element) != 1)
        return true;
    auto& cast = m_function.at(values[next]);
    return cast.opcode != Opcode::Cast || cast.child(0) != element || typeOf(values[next]).primitives & Primitive::Number;
}

bool Lowering::isExpectedToBeNoPrimitive(ValueID element)
{
    if (!(expectedTypeOf(element).primitives & ~Primitive::UndefinedOrNull))
        return true;
    auto& values = m_currentBlock->values;
    size_t next = m_currentIndexInBlock + 1;
    if (next >= values.size() || numberOfUsesOf(element) != 1)
        return false;
    auto& cast = m_function.at(values[next]);
    return cast.opcode == Opcode::Cast && cast.child(0) == element && !(typeOf(values[next]).primitives & ~Primitive::UndefinedOrNull);
}

bool Lowering::isOnlyComparedWithNumbers(ValueID element)
{
    unsigned found = 0;
    auto& values = m_currentBlock->values;
    for (size_t i = m_currentIndexInBlock + 1; i < values.size(); ++i) {
        auto& user = m_function.at(values[i]);
        unsigned uses = 0;
        user.forEachChild([&](ValueID child) {
            uses += child == element;
        });
        if (!uses)
            continue;
        if (user.opcode != Opcode::StrictEq || uses != 1 || !typeOf(user.child(0) == element ? user.child(1) : user.child(0)).isNumber())
            return false;
        ++found;
    }
    return found && found == numberOfUsesOf(element);
}

Number Lowering::numberOrNaN(Boxed value)
{
    return select(isInt32(value), convertToNumber(signExtended(low32(value))), select(isNumber(value), unboxedDouble(value), number(PNaN)));
}

Number Lowering::arrayElementAsNumberOrNaN(CellPointer array, Integer index)
{
    LBasicBlock inside = m_out.newBlock();
    LBasicBlock absentOrInherited = newColdBlock();
    LBasicBlock absent = newColdBlock();
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 6> numbers;
    auto yield = [&](Number number) {
        numbers.append(m_out.anchor(number.value()));
        m_out.jump(continuation);
    };
    Pointer butterfly = butterflyOf(array);
    m_out.branch(below(index, publicLength(butterfly)).value(), usually(inside), rarely(absentOrInherited));

    m_out.appendTo(inside);
    Word32 shape = indexingModeOfAnyArray(array, IndexingShapeMask);
    auto whenKeptAs = [&](IndexingType kept, const auto& read) {
        LBasicBlock isKeptSo = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        m_out.branch(equal(shape, word32(kept)).value(), unsure(isKeptSo), unsure(isNot));
        m_out.appendTo(isKeptSo);
        read();
        m_out.appendTo(isNot);
        shape = Word32(m_out.opaque(shape.value()));
    };
    whenKeptAs(DoubleShape, [&] {
        Number asDouble(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value())));
        orElse(equal(asDouble, asDouble), absentOrInherited);
        yield(asDouble);
    });
    whenKeptAs(Int32Shape, [&] {
        Boxed asInt32(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
        orElse(isNotEmpty(asInt32), absentOrInherited);
        yield(convertToNumber(signExtended(low32(asInt32))));
    });
    whenKeptAs(ContiguousShape, [&] {
        Boxed element(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
        orElse(isNotEmpty(element), absentOrInherited);
        yield(numberOrNaN(element));
    });
    m_out.jump(slowPath);

    m_out.appendTo(absentOrInherited);
    m_out.branch(logicalAnd(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), isPlainArray(array)).value(), usually(absent), rarely(slowPath));
    m_out.appendTo(absent);
    yield(number(PNaN));
    m_out.appendTo(slowPath);
    yield(numberOrNaN(AOT_TYPED_OPERATION(ArrayGetAtIndex, array, index)));
    m_out.appendTo(continuation);
    return Number(m_out.phi(Double, numbers));
}

static bool convertsArgumentsToNumbers(InlineBuiltin which)
{
    switch (which) {
    case InlineBuiltin::MathAbs:
    case InlineBuiltin::MathFloor:
    case InlineBuiltin::MathCeil:
    case InlineBuiltin::MathTrunc:
    case InlineBuiltin::MathRound:
    case InlineBuiltin::MathFround:
    case InlineBuiltin::MathF16Round:
    case InlineBuiltin::MathSqrt:
    case InlineBuiltin::MathClz32:
    case InlineBuiltin::MathSign:
    case InlineBuiltin::MathFunction:
    case InlineBuiltin::MathImul:
    case InlineBuiltin::MathATan2:
    case InlineBuiltin::MathHypot:
    case InlineBuiltin::MathMin:
    case InlineBuiltin::MathMax:
    case InlineBuiltin::MathPow:
    case InlineBuiltin::IsNaN:
    case InlineBuiltin::IsFinite:
        return true;
    default:
        return false;
    }
}

bool Lowering::isOperandOfArithmeticAlone(ValueID element)
{
    unsigned found = 0;
    auto& values = m_currentBlock->values;
    for (size_t i = m_currentIndexInBlock + 1; i < values.size(); ++i) {
        auto& user = m_function.at(values[i]);
        unsigned uses = 0;
        user.forEachChild([&](ValueID child) {
            uses += child == element;
        });
        if (!uses)
            continue;
        if (user.opcode == Opcode::CallBuiltin) {
            auto& implementation = m_builtins.at(user.builtin()).implementation;
            if (implementation.kind != BuiltinImplementation::Inline || !convertsArgumentsToNumbers(implementation.inlined))
                return false;
            Vector<Type, 4> arguments;
            for (ValueID argument : user.arguments)
                arguments.append(typeOf(argument));
            if (writesOfInlineBuiltin(implementation.inlined, arguments.span()) != Writes::Pure)
                return false;
            found += uses;
            continue;
        }
        switch (user.opcode) {
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
        case Opcode::Div:
        case Opcode::Mod:
        case Opcode::Pow:
        case Opcode::BitAnd:
        case Opcode::BitOr:
        case Opcode::BitXor:
        case Opcode::LShift:
        case Opcode::RShift:
        case Opcode::URShift:
        case Opcode::Negate:
        case Opcode::BitNot:
        case Opcode::ToNumber:
        case Opcode::ToNumeric:
        case Opcode::Less:
        case Opcode::LessEq:
        case Opcode::Greater:
        case Opcode::GreaterEq:
            break;
        default:
            return false;
        }
        if (!isNumericOperand(typeOf(user.child(0))) || (user.child(1) && !isNumericOperand(typeOf(user.child(1)))))
            return false;
        found += uses;
    }
    return found && found == numberOfUsesOf(element);
}

Operand Lowering::getArrayElement(CellPointer array, ValueID key, const Type& type, LBasicBlock otherwise, bool expectsNumbers)
{
    bool mayKeepInt32s = !std::exchange(m_arrayIsConvertedInFrontOfLoop, false);
    bool isBornKeepingDoubles = std::exchange(m_arrayIsBornKeepingDoubles, false);
    bool leavesNumbersToLoadElement = std::exchange(m_leavesNumbersToLoadElement, false) && expectsNumbers && type.primitives & Primitive::Number && !type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull) && get(key).rep == Rep::Int64;
    bool yieldsAnyValue = !otherwise && !m_watchingHandler && !leavesNumbersToLoadElement && (get(key).rep == Rep::Int64 || get(key).rep == Rep::Double) && m_program.at(m_program.types.anything).isWithin(type, m_program) && !Options::auditAOTTypedAssertions();
    bool looksForNumbers = expectsNumbers && type.primitives & Primitive::Number && !leavesNumbersToLoadElement && !yieldsAnyValue;
    bool looksForNumbersLast = !type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
    Rep rep = repOf(type);
    bool expectsDoubles = looksForNumbers && !looksForNumbersLast && (rep == Rep::Double || rep == Rep::OptionalDouble) && !Options::auditAOTTypedAssertions();
    bool holeMayReadAsNaN = expectsDoubles && !otherwise && rep == Rep::OptionalDouble && type.primitives & Primitive::Undefined && !Options::validateAOTTypedFacts() && isOperandOfArithmeticAlone(m_currentBlock->values[m_currentIndexInBlock]);
    if (expectsDoubles && !otherwise && get(key).rep == Rep::Int64 && rep == Rep::OptionalDouble && type.primitives & Primitive::Undefined && &m_program.at(m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type) == &type && !m_useCastTest && !m_watchingHandler && !handlerEnteredByBranch() && !Options::validateAOTTypedFacts()) {
        LBasicBlock elsewhere = newColdBlock();
        Join join(m_out, rep, b3TypeOf(rep));
        Integer index = in<Integer>(key);
        Pointer butterfly = butterflyOf(array);
        orElse(below(index, publicLength(butterfly)), elsewhere);
        Word32 shape = holeMayReadAsNaN ? indexingModeOf(array, IndexingShapeMask) : indexingModeOfAnyArray(array, IndexingShapeMask);
        LBasicBlock notDoubles = m_isInLoop ? newUnlikelyBlock() : elsewhere;
        orElse(equal(shape, word32(DoubleShape)), notDoubles);
        Number asDouble(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value())));
        if (!holeMayReadAsNaN)
            orElse(equal(asDouble, asDouble), elsewhere);
        join.add(elementOfDoubles(asDouble, type));
        if (m_isInLoop) {
            m_out.appendTo(notDoubles);
            Word32 shapeAskedAnew(m_out.opaque(shape.value()));
            if (mayKeepInt32s) {
                LBasicBlock keepsInt32s = m_out.newBlock();
                LBasicBlock keepsNoInt32s = m_out.newBlock();
                m_out.branch(equal(shapeAskedAnew, word32(Int32Shape)).value(), unsure(keepsInt32s), unsure(keepsNoInt32s));
                m_out.appendTo(keepsInt32s);
                Boxed asInt32(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
                orElse(isNotEmpty(asInt32), elsewhere);
                join.add(elementOfDoubles(convertToNumber(signExtended(low32(asInt32))), type));
                m_out.appendTo(keepsNoInt32s);
            }
            orElse(equal(shapeAskedAnew, word32(ContiguousShape)), elsewhere);
            Boxed asValue(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
            orElse(isNumber(asValue), elsewhere);
            join.add(elementOfDoubles(select(isInt32(asValue), convertToNumber(signExtended(low32(asValue))), unboxedDouble(asValue)), type));
        }
        m_out.appendTo(elsewhere);
        join.add(Operand(Rep::OptionalDouble, callAllocationStub(Stub::LoadElementAsDoubleOrNil, { { array.value(), GPRInfo::argumentGPR1 }, { index.value(), GPRInfo::argumentGPR2 }, { m_out.constInt64(m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type.index()), GPRInfo::argumentGPR3 } }, effectsOfOperation<operationAOTTypedArrayGetAtIndex>(), true, true, true)));
        return join.finish();
    }
    LBasicBlock unboxing = expectsDoubles && !otherwise && get(key).rep == Rep::Int64 ? newUnlikelyBlock() : nullptr;
    ValueFromBlock loadedValue;
    LBasicBlock inside = m_out.newBlock();
    bool takesIndexAsItIs = !otherwise && get(key).rep == Rep::Int64;
    TypeID given = m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type;
    bool holdsInArm = (takesIndexAsItIs || (!otherwise && get(key).rep == Rep::Double)) && !(type.primitives & Primitive::Number) && &m_program.at(given) == &type && !m_function.at(m_currentBlock->values[m_currentIndexInBlock]).readsElementKnownByTheStores() && !Options::auditAOTTypedAssertions();
    bool sharesSlowPath = yieldsAnyValue || (holdsInArm && !m_watchingHandler && (type.primitives & Primitive::UndefinedOrNull) == Primitive::UndefinedOrNull);
    LBasicBlock slowPath = otherwise ? otherwise : newColdBlock();
    LBasicBlock outside = otherwise ? otherwise : sharesSlowPath ? slowPath : m_out.newBlock();
    LBasicBlock absent = otherwise ? otherwise : sharesSlowPath ? slowPath : m_out.newBlock();
    Join join(m_out, rep, b3TypeOf(rep));

    LBasicBlock notAnIndex = (yieldsAnyValue || holdsInArm) && !takesIndexAsItIs ? newColdBlock() : slowPath;
    Integer index = indexOrElse(key, notAnIndex, !yieldsAnyValue);
    Pointer butterfly = butterflyOf(array);
    if (std::exchange(m_indexIsShownToBeInside, false))
        m_out.jump(inside);
    else
        m_out.branch(below(index, publicLength(butterfly)).value(), usually(inside), rarely(outside));

    // An array that shares its elements until it is stored to is read like any other.
    m_out.appendTo(inside);
    bool asksWholeByte = !looksForNumbers && (!(type.primitives & ~Primitive::UndefinedOrNull) || (yieldsAnyValue && isExpectedToBeNoPrimitive(m_currentBlock->values[m_currentIndexInBlock])));
    IndexingType keptAsValues = asksWholeByte ? ArrayWithContiguous : ContiguousShape;
    Word32 shape = asksWholeByte ? loadByte(array, m_heaps.JSCell_indexingTypeAndMisc, safeIfReaches(array, sizeof(JSCell))) : holeMayReadAsNaN ? indexingModeOf(array, IndexingShapeMask) : indexingModeOfAnyArray(array, IndexingShapeMask);
    // Goes on where the elements are kept another way. What is asked first is what the array was born as if it was made as its type says; the back end would
    // make one switch of the questions, in the order of the numbers, if it saw that they are about one value.
    bool isFirst = true;
    auto whenKeptAs = [&](IndexingType kept, const auto& read) {
        LBasicBlock isKeptSo = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        // Where more than a number will do, the first question is for values, and most arrays keep values.
        if (looksForNumbers && (!isFirst || !looksForNumbersLast) && (!expectsDoubles || kept == Int32Shape))
            m_out.branch(equal(shape, word32(kept)).value(), unsure(isKeptSo), unsure(isNot));
        else
            m_out.branch(equal(shape, word32(kept)).value(), usually(isKeptSo), rarely(isNot));
        m_out.appendTo(isKeptSo);
        read();
        m_out.appendTo(isNot);
        if (std::exchange(isFirst, false))
            shape = Word32(m_out.opaque(shape.value()));
    };
    auto readValue = [&] {
        Boxed element(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
        if (unboxing) {
            loadedValue = m_out.anchor(element.value());
            m_out.jump(unboxing);
            return;
        }
        addIfNumber(join, element, type);
        orElse(isNotEmpty(element), outside);
        if (holdsInArm && sharesSlowPath) {
            Type present = type.withoutPrimitives(Primitive::UndefinedOrNull);
            join.add(convert(fitOrElse(element, m_program.at(m_program.types.anything), present, slowPath), present, rep));
            return;
        }
        join.add(holdsInArm ? fitOrElse(element, m_program.at(m_program.types.anything), type, slowPath) : ensureElement(array, index, element, type));
    };
    if (looksForNumbersLast)
        whenKeptAs(keptAsValues, readValue);
    if (isBornKeepingDoubles && yieldsAnyValue && m_isInLoop) {
        whenKeptAs(asksWholeByte ? ArrayWithDouble : DoubleShape, [&] {
            Number asDouble(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value())));
            orElse(equal(asDouble, asDouble), outside);
            join.add(elementOfDoubles(asDouble, type));
        });
        if (mayKeepInt32s)
            whenKeptAs(asksWholeByte ? ArrayWithInt32 : Int32Shape, readValue);
    }
    // Kept as numbers, they are only looked at where a number will do and one is expected. All else is left to the operation.
    // Where values are what is expected, and outside loops, numbers are left to the stub that reads any array at an index: the arms are five times the code of the read.
    bool leavesNumbersToStub = looksForNumbers && looksForNumbersLast && !otherwise && !m_isInLoop;
    if (looksForNumbers && !leavesNumbersToStub) {
        whenKeptAs(DoubleShape, [&] {
            Number asDouble(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value())));
            // Among doubles a hole is what is not a number, and nothing else is.
            if (!holeMayReadAsNaN)
                orElse(equal(asDouble, asDouble), outside);
            join.add(elementOfDoubles(asDouble, type));
        });
        whenKeptAs(Int32Shape, [&] {
            Boxed asInt32(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
            orElse(isNotEmpty(asInt32), outside);
            auto& int32Type = m_program.at(m_program.types.int32);
            if (rep == Rep::JSValue)
                join.add(ensure(asInt32, int32Type, type));
            else
                join.add(ensure(signExtended(low32(asInt32)), int32Type, type));
        });
    }
    if (!looksForNumbersLast)
        whenKeptAs(keptAsValues, readValue);
    if (leavesNumbersToLoadElement && otherwise)
        orElse(isPlainArray(array), otherwise);
    if (leavesNumbersToLoadElement || leavesNumbersToStub)
        join.add(ensure(Boxed(callAllocationStub(Stub::LoadElement, { { array.value(), GPRInfo::argumentGPR1 }, { index.value(), GPRInfo::argumentGPR2 } }, effectsOfOperation<operationAOTTypedArrayGetAtIndex>(), true)), type));
    else
        m_out.jump(slowPath);
    if (otherwise)
        return join.finish();

    // From its length on an array has no elements, and inherits none as long as it is as it was made. What is no index may be the name of a property.
    if (!sharesSlowPath) {
        m_out.appendTo(outside);
        m_out.branch(logicalAnd(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), isPlainArray(array)).value(), usually(absent), rarely(slowPath));
        m_out.appendTo(absent);
        if (auto nothing = absentElement(type))
            join.add(*nothing);
    }

    m_out.appendTo(slowPath);
    SetForScope usesElementStub(m_usesElementStub, sharesSlowPath);
    if (holdsInArm) {
        Writes writes = writesOfElementRead(m_program, type);
        if (!takesIndexAsItIs)
            orElse(greaterThanOrEqual(index, integer(0)), notAnIndex);
        join.add(convert(AOT_TYPED_OPERATION_WITH_EFFECTS(ArrayElementAs, (OperationEffects { writes, true, writes != Writes::Nothing }), array, index, word32(given.index())), type, rep));
        if (!takesIndexAsItIs) {
            m_out.appendTo(notAnIndex);
            SetForScope usesNoElementStub(m_usesElementStub, false);
            join.add(convert(AOT_TYPED_OPERATION_WITH_EFFECTS(ArrayElementAtNumberAs, (OperationEffects { writes, true, writes != Writes::Nothing }), array, in<Number>(key), word32(given.index())), type, rep));
        }
        return join.finish();
    }
    if (unboxing) {
        ValueFromBlock yielded = m_out.anchor(AOT_TYPED_OPERATION(ArrayGetAtIndex, array, index).value());
        m_out.jump(unboxing);
        m_out.appendTo(unboxing);
        Boxed element(m_out.phi(Int64, loadedValue, yielded));
        addIfNumber(join, element, type);
        orElse(isNotEmpty(element), outside);
        join.add(ensureElement(array, index, element, type));
        return join.finish();
    }
    if (takesIndexAsItIs || yieldsAnyValue) {
        join.add(ensure(AOT_TYPED_OPERATION(ArrayGetAtIndex, array, index), type));
        if (!takesIndexAsItIs) {
            m_out.appendTo(notAnIndex);
            join.add(ensure(AOT_TYPED_OPERATION(ArrayGetAtNumber, array, in<Number>(key)), type));
        }
        return join.finish();
    }
    // To box a double takes thirteen instructions, and the operation would undo it.
    if (get(key).rep == Rep::Double)
        join.add(ensure(AOT_TYPED_OPERATION(ArrayGetAtNumber, array, in<Number>(key)), type));
    else
        join.add(ensure(AOT_TYPED_OPERATION(ArrayGet, array, in<Boxed>(key)), type));
    return join.finish();
}

// Stores an element of which it is not known whether it is a number: that is looked at where the array keeps numbers. See storeArrayElement().
template<typename Functor>
void Lowering::storeBoxedArrayElement(CellPointer array, Integer index, Boxed stored, LBasicBlock otherwise, const Functor& afterwards)
{
    Word32 mode = indexingModeOfAnyArray(array, IndexingShapeAndWritabilityMask);
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    // Once nothing can get in the way of the store. What is between the length and the capacity is kept clear, so that it reads as holes
    // once the array is that long.
    auto makeRoom = [&] {
        LBasicBlock isWithinLength = m_out.newBlock();
        LBasicBlock isBeyondLength = m_out.newBlock();
        m_out.branch(below(index, length).value(), unsure(isWithinLength), unsure(isBeyondLength));
        m_out.appendTo(isBeyondLength);
        orElse(below(index, vectorLength(butterfly)), otherwise);
        storeField(butterfly, m_heaps.Butterfly_publicLength, low32(add(index, integer(1))));
        m_out.jump(isWithinLength);
        m_out.appendTo(isWithinLength);
    };
    LBasicBlock values = m_out.newBlock();
    LBasicBlock notValues = m_out.newBlock();
    LBasicBlock int32s = m_out.newBlock();
    LBasicBlock notInt32s = m_out.newBlock();
    m_out.branch(equal(mode, word32(ContiguousShape)).value(), unsure(values), unsure(notValues));
    m_out.appendTo(notValues);
    m_out.branch(equal(mode, word32(Int32Shape)).value(), unsure(int32s), unsure(notInt32s));
    m_out.appendTo(int32s);
    orElse(isInt32(stored), otherwise);
    m_out.jump(values);

    m_out.appendTo(notInt32s);
    orElse(equal(mode, word32(DoubleShape)), otherwise);
    orElse(isNumber(stored), otherwise);
    Number number = select(isInt32(stored), toNumber(low32(stored)), unboxedDouble(stored));
    // Among doubles it would read as a hole.
    orElse(equal(number, number), otherwise);
    makeRoom();
    m_out.storeDouble(number.value(), m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value()));
    afterwards();

    m_out.appendTo(values);
    makeRoom();
    m_out.store64(stored.value(), m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value()));
    barrier(array, { { stored } });
    afterwards();
}

template<typename Functor>
void Lowering::storeArrayElement(CellPointer array, Integer index, ValueID stored, LBasicBlock otherwise, const Functor& afterwards)
{
    auto& type = typeOf(stored);
    if (Operand operand = get(stored); operand.rep == Rep::JSValue && type.primitives & Primitive::Number) {
        storeBoxedArrayElement(array, index, operand.as<Boxed>(), otherwise, afterwards);
        return;
    }
    Word32 mode = indexingModeOfAnyArray(array, IndexingShapeAndWritabilityMask);
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    // Once nothing can get in the way of the store. What is between the length and the capacity is kept clear, so that it reads as holes
    // once the array is that long.
    auto makeRoom = [&] {
        LBasicBlock isWithinLength = m_out.newBlock();
        LBasicBlock isBeyondLength = m_out.newBlock();
        m_out.branch(below(index, length).value(), usually(isWithinLength), rarely(isBeyondLength));
        m_out.appendTo(isBeyondLength);
        orElse(below(index, vectorLength(butterfly)), otherwise);
        storeField(butterfly, m_heaps.Butterfly_publicLength, low32(add(index, integer(1))));
        m_out.jump(isWithinLength);
        m_out.appendTo(isWithinLength);
    };
    // An array of numbers is born keeping doubles.
    bool isNumberOrMarker = get(stored).rep == Rep::OptionalDouble;
    if (type.isNumber() || isNumberOrMarker) {
        LBasicBlock doubles = m_out.newBlock();
        LBasicBlock notDoubles = m_out.newBlock();
        if (type.isInteger() && type.integers.isWithin(Range::int32()))
            m_out.branch(equal(mode, word32(DoubleShape)).value(), unsure(doubles), unsure(notDoubles));
        else
            m_out.branch(equal(mode, word32(DoubleShape)).value(), usually(doubles), rarely(notDoubles));
        m_out.appendTo(doubles);
        Number number = isNumberOrMarker ? assumedNumber(get(stored).as<OptionalNumber>()) : in<Number>(stored);
        // Among doubles it would read as a hole.
        if (!type.isInteger())
            orElse(equal(number, number), otherwise);
        makeRoom();
        m_out.storeDouble(number.value(), m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value()));
        afterwards();
        m_out.appendTo(notDoubles);
        if (get(stored).rep == Rep::Double && !type.isInteger()) {
            m_out.jump(otherwise);
            return;
        }
    }
    Word32 isTaken = type.isNumber() ? equal(mode, word32(ContiguousShape)) : equal(loadByte(array, m_heaps.JSCell_indexingTypeAndMisc, safeIfReaches(array, sizeof(JSCell))), word32(ArrayWithContiguous));
    if (type.isInteger() && type.integers.isWithin(Range::int32()))
        isTaken = logicalOr(isTaken, equal(mode, word32(Int32Shape)));
    orElse(isTaken, otherwise);
    makeRoom();
    m_out.store64(in<Boxed>(stored).value(), m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value()));
    if (type.hasCells())
        barrier(array, { { in<Boxed>(stored) } });
    afterwards();
}

void Lowering::putArrayElement(CellPointer array, ValueID key, ValueID stored, LBasicBlock otherwise, ValueID receiver)
{
    LBasicBlock slowPath = otherwise ? otherwise : newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Integer index = indexOrElse(key, slowPath);
    LBasicBlock isNotStoredInPlace = otherwise && receiver && !isInSloppyCode() ? newColdBlock() : slowPath;
    storeArrayElement(array, index, stored, isNotStoredInPlace, [&] {
        m_out.jump(continuation);
    });
    if (isNotStoredInPlace != slowPath) {
        m_out.appendTo(isNotStoredInPlace);
        if (LValue expectedAndSite = whatIsExpectedOfArrayStoredTo(receiver, key, false)) {
            AOT_TYPED_OPERATION(ArrayPutAtIndexExpecting, array, in<Boxed>(stored), index, Integer(expectedAndSite));
            m_out.jump(continuation);
        } else
            m_out.jump(slowPath);
    }
    if (!otherwise) {
        m_out.appendTo(slowPath);
        bool isNumberAmongNumbers = get(stored).rep == Rep::Double || (isNumericOperand(typeOf(stored)) && !(typeOf(stored).isInteger() && typeOf(stored).integers.isWithin(Range::int32())));
        LValue expectedAndSite = receiver && get(key).rep == Rep::Int64 ? whatIsExpectedOfArrayStoredTo(receiver, key, isNumberAmongNumbers) : nullptr;
        if (expectedAndSite)
            AOT_TYPED_OPERATION(ArrayPutAtIndexExpecting, array, in<Boxed>(stored), index, Integer(expectedAndSite));
        else if (get(key).rep == Rep::Double)
            AOT_TYPED_OPERATION(ArrayPutAtNumber, array, in<Boxed>(stored), in<Number>(key));
        else if (get(key).rep == Rep::Int64 && get(stored).rep == Rep::Double && !m_watchingHandler && !handlerEnteredByBranch())
            callAllocationStub(Stub::StoreElementOfDouble, { { array.value(), GPRInfo::argumentGPR1 }, { index.value(), GPRInfo::argumentGPR2 } }, effectsOfOperation<operationAOTTypedStoreElement>(), true, true, false, in<Number>(stored).value());
        else if (get(key).rep == Rep::Int64 && isNumericOperand(typeOf(stored)) && !(typeOf(stored).isInteger() && typeOf(stored).integers.isWithin(Range::int32())))
            AOT_TYPED_OPERATION(ArrayPutNumberAtIndex, array, in<Boxed>(stored), index);
        else if (get(key).rep == Rep::Int64)
            AOT_TYPED_OPERATION(ArrayPutAtIndex, array, in<Boxed>(stored), index);
        else
            AOT_TYPED_OPERATION(ArrayPut, array, in<Boxed>(key), in<Boxed>(stored));
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
}

// Whether it was stored. If not, the array is as it was.
Word32 Lowering::tryStoreArrayElement(CellPointer array, Integer index, ValueID stored)
{
    LBasicBlock otherwise = newUnlikelyBlock();
    Join join(m_out, Rep::Int32, Int32);
    storeArrayElement(array, index, stored, otherwise, [&] {
        join.add(boolean(true));
    });
    m_out.appendTo(otherwise);
    join.add(boolean(false));
    return join.finish().as<Word32>();
}

Operand Lowering::pushArrayElement(CellPointer array, ValueID receiver, ValueID pushed, const Type& type)
{
    Rep rep = repOf(type);
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    didEmitWrite(Writes::Heap);
    auto& typeOfPushed = typeOf(pushed);
    bool isUnboxedNumber = typeOfPushed.isNumber() && get(pushed).rep != Rep::JSValue;
    bool isNumberOrMarker = get(pushed).rep == Rep::OptionalDouble;
    auto pushedNumber = [&] {
        return isNumberOrMarker ? assumedNumber(get(pushed).as<OptionalNumber>()) : in<Number>(pushed);
    };
    ValueID literal = emptyLiteralBehind(receiver);
    IndexingType atBirth = indexingTypeAtBirth(arrayTypeAtBirth(literal ? literal : receiver).withoutPrimitives(Primitive::UndefinedOrNull));
    bool isInt32 = typeOfPushed.isInteger() && typeOfPushed.integers.isWithin(Range::int32());

    Word32 mode = indexingModeOf(array, IndexingShapeAndWritabilityMask);
    Pointer butterfly = butterflyOf(array);
    const bool isTight = !(isARM64() && m_isInLoop);
    const bool isYieldUsed = numberOfUsesOf(m_currentBlock->values[m_currentIndexInBlock]);
    LValue bothLengths = nullptr;
    if (isTight && isARM64()) {
        PatchpointValue* pair = m_out.patchpoint(m_proc.addTuple({ Int32, Int32 }));
        pair->effects = Effects::none();
        pair->effects.controlDependent = true;
        m_heaps.decoratePatchpointRead(&m_heaps.root, pair);
        pair->append(ConstrainedValue(butterfly.value(), ValueRep::SomeRegister));
        pair->resultConstraints = { ValueRep::SomeRegister, ValueRep::SomeRegister };
        pair->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
#if CPU(ARM64)
            jit.loadPair32(params[2].gpr(), CCallHelpers::TrustedImm32(Butterfly::offsetOfPublicLength()), params[0].gpr(), params[1].gpr());
#else
            UNUSED_PARAM(jit);
            UNUSED_PARAM(params);
#endif
        });
        bothLengths = pair;
    }
    auto oneOfBothLengths = [&](int32_t which) {
        return Word32(m_out.m_block->appendNew<B3::ExtractValue>(m_proc, m_out.origin(), Int32, bothLengths, which));
    };
    Integer length = bothLengths ? zeroExtended(oneOfBothLengths(0)) : publicLength(butterfly);
    Word32 roomOfBothLengths = bothLengths ? oneOfBothLengths(1) : Word32();
    auto store = [&](bool amongDoubles) {
        if (amongDoubles && !typeOfPushed.isInteger())
            orElse(equal(pushedNumber(), pushedNumber()), slowPath);
        if (typeOfPushed.hasCells())
            orElse(isBeyondBarrierThreshold(array), slowPath);
        if (bothLengths)
            orElse(below(low32(length), roomOfBothLengths), slowPath);
        else if (isTight)
            orElse(below(low32(length), loadField<Word32>(butterfly, m_heaps.Butterfly_vectorLength)), slowPath);
        else
            orElse(below(length, vectorLength(butterfly)), slowPath);
        Integer newLength = add(length, integer(1));
        if (isTight && isX86_64() && !isYieldUsed) {
            PatchpointValue* addsOne = m_out.patchpoint(Void);
            addsOne->effects = Effects::none();
            m_heaps.decoratePatchpointRead(&m_heaps.Butterfly_publicLength, addsOne);
            m_heaps.decoratePatchpointWrite(&m_heaps.Butterfly_publicLength, addsOne);
            addsOne->append(ConstrainedValue(butterfly.value(), ValueRep::SomeRegister));
            addsOne->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
                jit.add32(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(params[0].gpr(), Butterfly::offsetOfPublicLength()));
            });
        } else
            storeField(butterfly, m_heaps.Butterfly_publicLength, low32(newLength));
        if (amongDoubles)
            m_out.storeDouble(pushedNumber().value(), m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), length.value()));
        else
            m_out.store64(in<Boxed>(pushed).value(), m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), length.value()));
        join.add(ensure(newLength, Type::integer(Range::uint32()), type));
    };
    auto keeps = [&](bool doubles) {
        if (doubles)
            return equal(mode, word32(DoubleShape));
        Word32 keepsValues = equal(mode, word32(ContiguousShape));
        return isInt32 ? logicalOr(keepsValues, equal(mode, word32(Int32Shape))) : keepsValues;
    };
    bool isColdByText = !m_isInLoop && !m_program.isHotOutsideItsLoops(m_id);
    if (isColdByText)
        m_out.jump(slowPath);
    else if (!isUnboxedNumber && !isNumberOrMarker) {
        if (isTight && isX86_64() && !isInt32)
            orElse(equal(loadField<Word32>(array, m_heaps.JSCell_structureID), Word32(changing32(Instance::offsetOfNewArrayWithContiguousStructureID()))), slowPath);
        else
            orElse(keeps(false), slowPath);
        store(false);
    } else {
        bool expectsDoubles = atBirth == ArrayWithDouble || (atBirth == ArrayWithInt32 && !isInt32);
        LBasicBlock asExpected = m_out.newBlock();
        LBasicBlock otherwise = newUnlikelyBlock();
        m_out.branch(keeps(expectsDoubles).value(), unsure(asExpected), unsure(otherwise));
        m_out.appendTo(asExpected);
        store(expectsDoubles);
        m_out.appendTo(otherwise);
        orElse(keeps(!expectsDoubles), slowPath);
        store(!expectsDoubles);
    }

    m_out.appendTo(slowPath);
    LValue expectedAndSite = m_out.shl(m_out.zeroExtPtr(turnsLeftInLoop().value()), m_out.constInt32(32));
    if (literal && m_function.at(literal).vectorLengthHintOffset() != Format::none)
        expectedAndSite = m_out.bitOr(expectedAndSite, m_out.constInt64((static_cast<uint64_t>(m_function.at(literal).vectorLengthHintOffset()) + 1) << 1));
    if (m_expectedMethodThatWasRead && m_builtins.at(m_expectedMethodThatWasRead).implementation.inlined == InlineBuiltin::ArrayPush)
        expectedAndSite = m_out.bitOr(expectedAndSite, m_out.constInt64(1));
    static_assert(static_cast<unsigned>(Stub::PushToFullVector) + 1 == static_cast<unsigned>(Stub::LeafPushToFullVector) && static_cast<unsigned>(Stub::PushDoubleToFullVector) + 1 == static_cast<unsigned>(Stub::LeafPushDoubleToFullVector));
    LValue value = isUnboxedNumber ? m_out.bitCast(in<Number>(pushed).value(), Int64) : in<Boxed>(pushed).value();
    Boxed yielded(callAllocationStub(isUnboxedNumber ? Stub::PushDoubleToFullVector : Stub::PushToFullVector, { { array.value(), GPRInfo::toArgumentRegister(1) }, { value, GPRInfo::toArgumentRegister(2) }, { expectedAndSite, GPRInfo::toArgumentRegister(3) } }, effectsOfOperation<operationAOTTypedArrayPush>(), true));
    join.add(ensure(yielded, type));
    return join.finish();
}

Operand Lowering::pushArrayElements(CellPointer array, const Value& value, const Type& type)
{
    ValueID receiver = value.child(0);
    auto& pushed = value.arguments;
    Rep rep = repOf(type);
    Type lengthType = Type::integer(Range::uint32());
    bool areNumbers = true;
    bool areInt32 = true;
    bool haveCells = false;
    for (ValueID id : pushed) {
        auto& element = typeOf(id);
        areNumbers &= element.isNumber();
        areInt32 &= element.isInteger() && element.integers.isWithin(Range::int32());
        haveCells |= element.hasCells();
    }
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    didEmitWrite(Writes::Heap);
    Word32 mode = indexingModeOf(array, IndexingShapeAndWritabilityMask);
    Pointer butterfly = butterflyOf(array);
    Integer length = publicLength(butterfly);
    Integer newLength = add(length, integer(pushed.size()));
    // Where there is none the vector grows and all goes on here. Yields where the elements are then.
    auto makeRoom = [&] {
        LBasicBlock hasRoom = m_out.newBlock();
        LBasicBlock isFull = newColdBlock();
        ValueFromBlock asItIs = m_out.anchor(butterfly.value());
        m_out.branch(belowOrEqual(newLength, vectorLength(butterfly)).value(), usually(hasRoom), rarely(isFull));
        m_out.appendTo(isFull);
        updateVectorLengthHint(array, receiver);
        orElse(logicalNot(isZero(AOT_TYPED_OPERATION(ArrayGrow, array, low32(newLength)))), slowPath);
        ValueFromBlock grown = m_out.anchor(butterflyOf(array).value());
        m_out.jump(hasRoom);
        m_out.appendTo(hasRoom);
        Pointer elements(m_out.phi(pointerType(), asItIs, grown));
        storeField(elements, m_heaps.Butterfly_publicLength, low32(newLength));
        return elements;
    };
    auto placeOf = [&](unsigned i) {
        return i ? add(length, integer(i)) : length;
    };
    if (areNumbers) {
        LBasicBlock doubles = m_out.newBlock();
        LBasicBlock notDoubles = m_out.newBlock();
        m_out.branch(equal(mode, word32(DoubleShape)).value(), unsure(doubles), unsure(notDoubles));
        m_out.appendTo(doubles);
        Vector<Number, 8> numbers;
        for (ValueID id : pushed) {
            numbers.append(in<Number>(id));
            if (!typeOf(id).isInteger())
                orElse(equal(numbers.last(), numbers.last()), slowPath);
        }
        Pointer elements = makeRoom();
        for (unsigned i = 0; i < pushed.size(); ++i)
            m_out.storeDouble(numbers[i].value(), m_out.baseIndex(m_heaps.indexedDoubleProperties, elements.value(), placeOf(i).value()));
        join.add(ensure(newLength, lengthType, type));
        m_out.appendTo(notDoubles);
    }
    Word32 isTaken = equal(mode, word32(ContiguousShape));
    if (areInt32)
        isTaken = logicalOr(isTaken, equal(mode, word32(Int32Shape)));
    orElse(isTaken, slowPath);
    Vector<Boxed, 8> values;
    for (ValueID id : pushed)
        values.append(in<Boxed>(id));
    Pointer elements = makeRoom();
    for (unsigned i = 0; i < pushed.size(); ++i)
        m_out.store64(values[i].value(), m_out.baseIndex(m_heaps.indexedContiguousProperties, elements.value(), placeOf(i).value()));
    if (haveCells)
        barrier(array, values.span());
    join.add(ensure(newLength, lengthType, type));

    m_out.appendTo(slowPath);
    join.add(ensure(callArrayMethodOnSlowPath(value, array), type));
    return join.finish();
}

// The vector of a typed array and the characters of a string are freed with their owner, and a pointer to them keeps nothing alive. The back
// end holds on to one across calls that collect, so after each use of one the collector still has to find the owner. No code comes of this.
bool Lowering::isForcedForTesting(const char* file, unsigned line)
{
    static const char* const places = byteCast<char>(Options::aotGuardsToFailForTesting());
    if (!places) [[likely]]
        return false;
    const char* slash = strrchr(file, '/');
    return StringView::fromLatin1(places).contains(makeString(',', StringView::fromLatin1(slash ? slash + 1 : file), ':', line, ','));
}

bool Lowering::isDroppedForTesting(const char* file, unsigned line)
{
    if (!Options::aotGuardToDropForTesting()) [[likely]]
        return false;
    StringView path = StringView::fromLatin1(file);
    size_t slash = path.reverseFind('/');
    return makeString(slash == notFound ? path : path.substring(slash + 1), ':', line) == StringView::fromLatin1(byteCast<char>(Options::aotGuardToDropForTesting()));
}

void Lowering::ensureStillAliveHere(CellPointer owner, const char* file, unsigned line)
{
    if (isDroppedForTesting(file, line)) [[unlikely]]
        return;
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    patchpoint->effects = Effects::none();
    patchpoint->effects.writesLocalState = true;
    patchpoint->effects.reads = HeapRange::top();
    patchpoint->append(owner.value(), ValueRep::ColdAny);
    patchpoint->setGenerator(m_generatorOfNothing);
}

void Lowering::ensureStillAliveHereForWord(CellPointer owner, LValue word, const char* file, unsigned line)
{
    if (isDroppedForTesting(file, line)) [[unlikely]]
        return;
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    patchpoint->effects = Effects::none();
    patchpoint->effects.writesLocalState = true;
    patchpoint->effects.reads = HeapRange::top();
    patchpoint->append(owner.value(), ValueRep::ColdAny);
    patchpoint->append(word, ValueRep::ColdAny);
    patchpoint->setGenerator(m_generatorOfNothing);
    m_context.keepsCellsAliveForWords = true;
}

// Typed arrays.

// The receiver of an access by a type that says it is a typed array, `undefined` or `null`. See findTypedArrayElement() for the last two.
Lowering::TypedArrayReceiver Lowering::typedArrayReceiver(ValueID object)
{
    Operand operand = get(object);
    if (operand.rep == Rep::Cell)
        return typedArrayReceiver(operand.as<CellPointer>());
    // In the place of what is no view, something is loaded from that is always there. What is found there is not looked at.
    static_assert(sizeof(Instance) >= sizeof(JSArrayBufferView));
    Boxed value = operand.as<Boxed>();
    Word32 isView = isCell(value);
    return { CellPointer(m_out.select(isView.value(), value.value(), m_instance)), IsSafe, isView, value };
}

Lowering::TypedArrayReceiver Lowering::typedArrayReceiver(CellPointer view)
{
    return { view, safeIfReaches(view, sizeof(JSArrayBufferView)), { }, { } };
}

void Lowering::turnAwayNil(const TypedArrayReceiver& receiver)
{
    if (!receiver.isView)
        return;
    LBasicBlock isNil = newColdBlock();
    LBasicBlock continuation = newColdBlock();
    m_out.branch(receiver.isView.value(), usually(continuation), rarely(isNil));
    m_out.appendTo(isNil);
    throwNullish(receiver.value, { });
    m_out.appendTo(continuation);
}

// Otherwise the length depends on that of the buffer. Of the mode only these two bits are looked at, which stay as the view was made with them.
Word32 Lowering::hasLengthOfItsOwn(const TypedArrayReceiver& receiver)
{
    return lacksBits(loadByte(receiver.view, m_heaps.JSArrayBufferView_mode, IsFixed | receiver.flags), isResizableOrGrowableSharedMode);
}

// Goes on where the index is inside, and yields where the elements are. Of a typed array that has been detached the length is zero.
Pointer Lowering::findTypedArrayElement(const TypedArrayReceiver& receiver, Integer index, LBasicBlock otherwise, LBasicBlock outside)
{
    Word32 hasOwnLength = hasLengthOfItsOwn(receiver);
    // One test in the usual case: `undefined`, `null` and a view that goes by its buffer count as empty. None of this changes around a loop that only reads and stores elements.
    Word32 isAsLongAsItSays = receiver.isView ? logicalAnd(receiver.isView, hasOwnLength) : hasOwnLength;
    Integer length = select(isAsLongAsItSays, loadField<Integer>(receiver.view, m_heaps.JSArrayBufferView_length, receiver.flags), integer(0));
    LBasicBlock inside = m_out.newBlock();
    LBasicBlock notInside = newColdBlock();
    m_out.branch(below(index, length).value(), usually(inside), rarely(notInside));
    m_out.appendTo(notInside);
    turnAwayNil(receiver);
    m_out.branch(hasOwnLength.value(), usually(outside), rarely(otherwise));
    m_out.appendTo(inside);
    return loadField<Pointer>(receiver.view, m_heaps.JSArrayBufferView_vector, receiver.flags);
}

Operand Lowering::getTypedArrayElement(const TypedArrayReceiver& receiver, ValueID key, TypedArrayType arrayType, const Type& type, LBasicBlock otherwise)
{
    Rep rep = repOf(type);
    Type elementType = elementTypeOf(arrayType);
    LBasicBlock slowPath = otherwise ? otherwise : newColdBlock();
    LBasicBlock outside = newColdBlock();
    LBasicBlock notIndex = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));

    // No number is the name of a property of a typed array that is looked for anywhere else. A key that is only expected to be one may be any name.
    Integer index = indexOrElse(key, otherwise ? otherwise : notIndex);
    Pointer vector = findTypedArrayElement(receiver, index, slowPath, outside);
    Operand element = load(storeOfElements(arrayType), TypedPointer(m_heaps.TypedArrayProperties, offsetBy(vector, index, logElementSize(arrayType)).value()), 0);
    element = convert(element, elementType, repOf(elementType));
    ensureStillAliveHere(receiver.view);
    join.add(ensure(element, elementType, type));

    m_out.appendTo(notIndex);
    turnAwayNil(receiver);
    m_out.jump(outside);
    m_out.appendTo(outside);
    if (auto nothing = absentElement(type))
        join.add(*nothing);
    if (otherwise)
        return join.finish();
    m_out.appendTo(slowPath);
    join.add(ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(GetByKey, (OperationEffects { Writes::Nothing, false }), box(receiver.view), in<Boxed>(key)), type));
    return join.finish();
}

void Lowering::putTypedArrayElement(const TypedArrayReceiver& receiver, ValueID key, TypedArrayType arrayType, ValueID stored, LBasicBlock otherwise)
{
    Store store = storeOfElements(arrayType);
    LBasicBlock slowPath = otherwise ? otherwise : newColdBlock();
    LBasicBlock notIndex = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    // A store outside does nothing.
    Integer index = indexOrElse(key, otherwise ? otherwise : notIndex);
    // What is stored is made a number of first, which only what is one already leaves to be done in place.
    Operand element;
    if (store == Store::Double || store == Store::Float)
        element = otherwise ? expectOrElse(Expectation { stored, &m_program.at(m_program.types.number) }, otherwise) : Operand(in<Number>(stored));
    else {
        Word32 bits = otherwise ? toInt32OrElse(stored, otherwise) : toInt32(stored);
        element = store == Store::Uint32 ? Operand(zeroExtended(bits)) : Operand(bits);
    }
    Pointer vector = findTypedArrayElement(receiver, index, slowPath, continuation);
    this->store(store, element, TypedPointer(m_heaps.TypedArrayProperties, offsetBy(vector, index, logElementSize(arrayType)).value()));
    ensureStillAliveHere(receiver.view);
    m_out.jump(continuation);
    m_out.appendTo(notIndex);
    turnAwayNil(receiver);
    m_out.jump(continuation);
    if (!otherwise) {
        m_out.appendTo(slowPath);
        AOT_TYPED_OPERATION_WITH_EFFECTS(TypedArrayPut, (OperationEffects { Writes::TypedArrayProperties, false, false, false, true }), receiver.view, index, in<Number>(stored));
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
}

LBasicBlock Lowering::appendSpreadOfTypedArray(const Value& value)
{
    static constexpr unsigned maxLengthToCopyInPlace = 512;
    ValueID literal = originOf(value.arguments[0]);
    while (m_function.at(literal).opcode == Opcode::CallBuiltin && m_function.at(literal).builtin() == value.builtin())
        literal = originOf(m_function.at(literal).arguments[0]);
    Operand iterable = get(value.arguments[1]);
    auto& engineClass = *typeOf(value.arguments[1]).onlyEngineClass();
    TypedArrayType arrayType = engineClass.typedArrayType;
    if (m_function.at(literal).opcode != Opcode::NewArray || !holdsCells(iterable.rep) || !hasElementsReadInPlace(arrayType))
        return nullptr;
    Type elementType = elementTypeOf(arrayType);
    IndexingType indexingType = indexingTypeOfLiteral(m_function.at(literal).arguments, arrayTypeAtBirth(literal));
    if (indexingType == ArrayWithInt32 && !(elementType.isInteger() && elementType.integers.isWithin(Range::int32())))
        return nullptr;
    LBasicBlock otherwise = newUnlikelyBlock();
    auto& elements = elementsOf(indexingType);

    CellPointer target = asCell(in<Boxed>(value.arguments[0]));
    CellPointer source;
    if (iterable.rep == Rep::Cell)
        source = iterable.as<CellPointer>();
    else {
        orElse(isCell(iterable.as<Boxed>(), typeOf(value.arguments[1])), otherwise);
        source = asCell(iterable.as<Boxed>());
    }
    TypedArrayReceiver receiver = typedArrayReceiver(source);
    orElse(hasOriginalStructure(source, engineClass), otherwise);
    orElse(hasLengthOfItsOwn(receiver), otherwise);
    orElse(equal(indexingModeOf(target, IndexingShapeAndWritabilityMask), word32(indexingType & IndexingShapeMask)), otherwise);
    Integer count = loadField<Integer>(source, m_heaps.JSArrayBufferView_length, receiver.flags);
    orElse(below(sub(count, integer(1)), integer(maxLengthToCopyInPlace)), otherwise);

    Pointer old = butterflyOf(target);
    Integer length = publicLength(old);
    Integer newLength = add(length, count);
    unsigned toCome = numberOfElementsPushedTo(m_currentBlock->values[m_currentIndexInBlock]);
    Integer needed = add(newLength, integer(toCome));
    LBasicBlock grow = m_out.newBlock();
    LBasicBlock clearLoop = m_out.newBlock();
    LBasicBlock clear = m_out.newBlock();
    LBasicBlock moveLoop = m_out.newBlock();
    LBasicBlock move = m_out.newBlock();
    LBasicBlock hasRoom = m_out.newBlock();
    LBasicBlock copyLoop = m_out.newBlock();
    LBasicBlock copy = m_out.newBlock();
    LBasicBlock copied = m_out.newBlock();
    LBasicBlock publish = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    didEmitWrite(effectsOfBuiltinCall(value).writes);
    ValueFromBlock inPlace = m_out.anchor(old.value());
    m_out.branch(belowOrEqual(needed, vectorLength(old)).value(), unsure(hasRoom), unsure(grow));

    m_out.appendTo(grow);
    LValue capacity = m_out.bitOr(needed.value(), m_out.constInt64(1));
    LValue size = m_out.add(m_out.shl(capacity, m_out.constInt32(3)), m_out.constIntPtr(sizeof(IndexingHeader)));
    LValue storage = allocateStorage(size, otherwise);
    LValue grown = m_out.add(storage, m_out.constIntPtr(sizeof(IndexingHeader)));
    m_out.store32(m_out.castToInt32(length.value()), grown, m_heaps.Butterfly_publicLength);
    m_out.store32(m_out.castToInt32(capacity), grown, m_heaps.Butterfly_vectorLength);
    ValueFromBlock firstCleared = m_out.anchor(newLength.value());
    m_out.jump(clearLoop);
    m_out.appendTo(clearLoop);
    LValue slot = m_out.phi(pointerType(), firstCleared);
    ValueFromBlock firstMoved = m_out.anchor(m_out.intPtrZero);
    m_out.branch(m_out.below(slot, capacity), unsure(clear), unsure(moveLoop));
    m_out.appendTo(clear);
    m_out.store64(m_out.constInt64(holeIn(indexingType)), m_out.baseIndex(elements, grown, slot, FTL::ScaleEight));
    m_out.addIncomingToPhi(slot, m_out.anchor(m_out.add(slot, m_out.intPtrOne)));
    m_out.jump(clearLoop);
    m_out.appendTo(moveLoop);
    LValue moved = m_out.phi(pointerType(), firstMoved);
    ValueFromBlock outOfPlace = m_out.anchor(grown);
    m_out.branch(m_out.below(moved, length.value()), unsure(move), unsure(hasRoom));
    m_out.appendTo(move);
    m_out.store64(m_out.load64(m_out.baseIndex(elements, old.value(), moved, FTL::ScaleEight)), m_out.baseIndex(elements, grown, moved, FTL::ScaleEight));
    m_out.addIncomingToPhi(moved, m_out.anchor(m_out.add(moved, m_out.intPtrOne)));
    m_out.jump(moveLoop);

    m_out.appendTo(hasRoom);
    LValue butterfly = m_out.phi(pointerType(), inPlace, outOfPlace);
    LValue to = m_out.add(butterfly, m_out.shl(length.value(), m_out.constInt32(3)));
    Pointer vector = loadField<Pointer>(source, m_heaps.JSArrayBufferView_vector, receiver.flags);
    ValueFromBlock firstCopied = m_out.anchor(m_out.intPtrZero);
    m_out.jump(copyLoop);
    m_out.appendTo(copyLoop);
    LValue index = m_out.phi(pointerType(), firstCopied);
    m_out.branch(m_out.below(index, count.value()), unsure(copy), unsure(copied));
    m_out.appendTo(copy);
    Operand element = load(storeOfElements(arrayType), TypedPointer(m_heaps.TypedArrayProperties, offsetBy(vector, Integer(index), logElementSize(arrayType)).value()), 0);
    element = convert(element, elementType, repOf(elementType));
    if (indexingType == ArrayWithDouble) {
        Number number = convertTo<Number>(element, elementType);
        if (!elementType.isInteger()) {
            LBasicBlock takeBackLoop = newColdBlock();
            LBasicBlock takeBack = newColdBlock();
            LBasicBlock isElement = m_out.newBlock();
            ValueFromBlock reached = m_out.anchor(index);
            m_out.branch(equal(number, number).value(), usually(isElement), rarely(takeBackLoop));
            m_out.appendTo(takeBackLoop);
            LValue left = m_out.phi(pointerType(), reached);
            m_out.branch(m_out.isZero64(left), unsure(otherwise), unsure(takeBack));
            m_out.appendTo(takeBack);
            LValue previous = m_out.sub(left, m_out.intPtrOne);
            m_out.store64(m_out.constInt64(holeIn(indexingType)), m_out.baseIndex(elements, to, previous, FTL::ScaleEight));
            m_out.addIncomingToPhi(left, m_out.anchor(previous));
            m_out.jump(takeBackLoop);
            m_out.appendTo(isElement);
        }
        m_out.storeDouble(number.value(), m_out.baseIndex(elements, to, index, FTL::ScaleEight));
    } else
        m_out.store64(convertTo<Boxed>(element, elementType).value(), m_out.baseIndex(elements, to, index, FTL::ScaleEight));
    m_out.addIncomingToPhi(index, m_out.anchor(m_out.add(index, m_out.intPtrOne)));
    m_out.jump(copyLoop);

    m_out.appendTo(copied);
    ensureStillAliveHere(source);
    m_out.store32(m_out.castToInt32(newLength.value()), butterfly, m_heaps.Butterfly_publicLength);
    m_out.branch(m_out.equal(butterfly, old.value()), unsure(done), unsure(publish));
    m_out.appendTo(publish);
    fenceAfterInitialization();
    m_out.storePtr(butterfly, target.value(), m_heaps.JSObject_butterfly);
    m_out.jump(done);
    m_out.appendTo(done);
    barrier(target);
    return otherwise;
}

Operand Lowering::lowerArrayFromTypedArray(ValueID result, const Value& value, const Type& type)
{
    static constexpr unsigned maxLengthToCopyInPlace = 512;
    ValueID items = value.arguments[0];
    auto& engineClass = *typeOf(items).onlyEngineClass();
    TypedArrayType arrayType = engineClass.typedArrayType;
    Type elementType = elementTypeOf(arrayType);
    Type made = Type::engineClass(m_builtins.arrayClass());
    IndexingType indexingType = indexingTypeAtBirth(arrayTypeAtBirth(result));
    if (indexingType == ArrayWithInt32 && !(elementType.isInteger() && elementType.integers.isWithin(Range::int32())))
        indexingType = ArrayWithDouble;
    auto& elements = elementsOf(indexingType);
    Rep rep = repOf(type);
    LBasicBlock generally = newBlockForGeneralCall(value);
    LBasicBlock outOfLine = newUnlikelyBlock();
    LBasicBlock copyLoop = m_out.newBlock();
    LBasicBlock copy = m_out.newBlock();
    LBasicBlock copied = m_out.newBlock();
    Join join(m_out, rep, b3TypeOf(rep));

    Operand iterable = get(items);
    CellPointer source;
    if (iterable.rep == Rep::Cell)
        source = iterable.as<CellPointer>();
    else {
        Boxed boxed = in<Boxed>(items);
        orElse(isCell(boxed, typeOf(items)), generally);
        source = asCell(boxed);
    }
    TypedArrayReceiver receiver = typedArrayReceiver(source);
    orElse(hasOriginalStructure(source, engineClass), generally);
    orElse(hasLengthOfItsOwn(receiver), generally);
    Integer count = loadField<Integer>(source, m_heaps.JSArrayBufferView_length, receiver.flags);
    orElse(isNotZero(count), generally);
    orElse(belowOrEqual(count, integer(maxLengthToCopyInPlace)), outOfLine);
    LValue header = changing64(Instance::offsetOfNewArrayHeader(indexingType));
    orElse(m_out.notZero64(header), outOfLine);

    LValue capacity = m_out.bitOr(count.value(), m_out.constInt64(1));
    LValue storage = allocateStorage(m_out.add(m_out.shl(capacity, m_out.constInt32(3)), m_out.constIntPtr(sizeof(IndexingHeader))), outOfLine);
    LValue butterfly = m_out.add(storage, m_out.constIntPtr(sizeof(IndexingHeader)));
    m_out.store32(m_out.castToInt32(count.value()), butterfly, m_heaps.Butterfly_publicLength);
    m_out.store32(m_out.castToInt32(capacity), butterfly, m_heaps.Butterfly_vectorLength);
    m_out.store64(m_out.constInt64(holeIn(indexingType)), TypedPointer(elements, m_out.add(storage, m_out.shl(capacity, m_out.constInt32(3)))));
    Pointer vector = loadField<Pointer>(source, m_heaps.JSArrayBufferView_vector, receiver.flags);
    ValueFromBlock firstCopied = m_out.anchor(m_out.intPtrZero);
    m_out.jump(copyLoop);
    m_out.appendTo(copyLoop);
    LValue index = m_out.phi(pointerType(), firstCopied);
    m_out.branch(m_out.below(index, count.value()), unsure(copy), unsure(copied));
    m_out.appendTo(copy);
    Operand element = load(storeOfElements(arrayType), TypedPointer(m_heaps.TypedArrayProperties, offsetBy(vector, Integer(index), logElementSize(arrayType)).value()), 0);
    element = convert(element, elementType, repOf(elementType));
    if (indexingType == ArrayWithDouble) {
        Number number = convertTo<Number>(element, elementType);
        if (!elementType.isInteger())
            orElse(equal(number, number), outOfLine);
        m_out.storeDouble(number.value(), m_out.baseIndex(elements, butterfly, index, FTL::ScaleEight));
    } else
        m_out.store64(convertTo<Boxed>(element, elementType).value(), m_out.baseIndex(elements, butterfly, index, FTL::ScaleEight));
    m_out.addIncomingToPhi(index, m_out.anchor(m_out.add(index, m_out.intPtrOne)));
    m_out.jump(copyLoop);

    m_out.appendTo(copied);
    ensureStillAliveHere(source);
    LValue array = allocateHeapCell(fixedPointer(Instance::offsetOfArrayAllocator()), sizeof(JSArray), outOfLine);
    applyEffects(array->as<PatchpointValue>(), { Writes::RuntimeState, false });
    storeHeader(array, header);
    m_out.storePtr(butterfly, array, m_heaps.JSObject_butterfly);
    fenceAfterInitialization();
    join.add(ensure(CellPointer(array), made, type));

    m_out.appendTo(outOfLine);
    join.add(ensure(AOT_TYPED_OPERATION(ArrayFromTypedArray, source, word32(indexingType)), made, type));

    if (!isGeneralArmOnOffer(generally)) {
        m_out.appendTo(generally);
        join.add(ensure(callBuiltinFunction(value, OperationEffects { Writes::Heap, true }), type));
    }
    return join.finish();
}

// Access by value.

Operand Lowering::lowerGetByVal(ValueID result, const Value& value)
{
    ValueID object = value.child(0);
    ValueID key = value.child(1);
    auto& type = typeOf(result);
    if (value.readsForInList() && get(key).rep == Rep::Int64) {
        CellPointer list = nonNil(object, { });
        Rep rep = repOf(type);
        LBasicBlock isNot = newColdBlock();
        Join join(m_out, rep, b3TypeOf(rep));
        orElse(equal(indexingModeOf(list, IndexingShapeMask), word32(ContiguousShape)), isNot);
        Boxed element(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterflyOf(list).value(), in<Integer>(key).value())));
        if (Options::validateAOTTypedFacts()) [[unlikely]] {
            LBasicBlock doesNotHold = newColdBlock();
            orElse(below(in<Integer>(key), publicLength(butterflyOf(list))), doesNotHold);
            orElse(isNotEmpty(element), doesNotHold);
            orElse(isCell(element), doesNotHold);
            orElse(Word32(isCellOfType(asCell(element).value(), StringType)), doesNotHold);
            LBasicBlock continuation = m_out.m_block;
            m_out.appendTo(doesNotHold);
            AOT_TYPED_OPERATION(FactDoesNotHold, element, word32(m_id.index()), word32(result.index()));
            m_out.jump(isNot);
            m_out.appendTo(continuation);
        }
        join.add(ensure(asCell(element), m_program.at(m_program.types.string), type));
        m_out.appendTo(isNot);
        join.add(ensure(AOT_TYPED_OPERATION(ArrayGet, list, in<Boxed>(key)), type));
        return join.finish();
    }
    switch (keyedAccessOf(m_program, m_function, value)) {
    case KeyedAccess::Array:
        static_assert(static_cast<unsigned>(Stub::LoadElement) + 1 == static_cast<unsigned>(Stub::LeafLoadElement));
        if (Writes writes = m_function.at(result).readsElementKnownByTheStores() && !Options::validateAOTTypedFacts() && !Options::auditAOTTypedAssertions() ? Writes::Nothing : writesOfElementRead(m_program, type); !m_isInLoop && !m_program.isHotOutsideItsLoops(m_id) && get(key).rep == Rep::Int64 && (writes == Writes::Nothing || (!(type.primitives & Primitive::Number) && &m_program.at(m_function.at(result).type) == &type && !Options::auditAOTTypedAssertions()))) {
            CellPointer array = nonNil(object, { });
            Integer index = in<Integer>(key);
            static_assert(static_cast<unsigned>(Stub::LoadElementAsDoubleOrNil) + 1 == static_cast<unsigned>(Stub::LeafLoadElementAsDoubleOrNil));
            if (writes == Writes::Nothing && repOf(type) == Rep::OptionalDouble && type.primitives & Primitive::Undefined && &m_program.at(m_function.at(result).type) == &type && !m_useCastTest && !handlerEnteredByBranch() && !Options::validateAOTTypedFacts() && !Options::auditAOTTypedAssertions())
                return Operand(Rep::OptionalDouble, callAllocationStub(Stub::LoadElementAsDoubleOrNil, { { array.value(), GPRInfo::argumentGPR1 }, { index.value(), GPRInfo::argumentGPR2 }, { m_out.constInt64(m_function.at(result).type.index()), GPRInfo::argumentGPR3 } }, effectsOfOperation<operationAOTTypedArrayGetAtIndex>(), true, false, true));
            Boxed element(callAllocationStub(Stub::LoadElement, { { array.value(), GPRInfo::argumentGPR1 }, { index.value(), GPRInfo::argumentGPR2 } }, effectsOfOperation<operationAOTTypedArrayGetAtIndex>(), true));
            if (writes == Writes::Nothing)
                return ensure(element, type);
            Rep rep = repOf(type);
            LBasicBlock misfit = newColdBlock();
            Join join(m_out, rep, b3TypeOf(rep));
            join.add(fitOrElse(element, m_program.at(m_program.types.anything), type, misfit));
            m_out.appendTo(misfit);
            join.add(convert(AOT_TYPED_OPERATION_WITH_EFFECTS(ArrayElementAs, (OperationEffects { writes, true, true }), array, index, word32(m_function.at(result).type.index())), type, rep));
            return join.finish();
        }
        if (m_isInLoop && !m_watchingHandler && get(key).rep == Rep::Int64 && repOf(type) == Rep::JSValue && expectedTypeOf(result).hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull) && expectedTypeOf(result).primitives & Primitive::Number && isOnlyComparedWithNumbers(result)) {
            m_elementsReadAsNumbers.set(result.index(), arrayElementAsNumberOrNaN(nonNil(object, { }), in<Integer>(key)).value());
            return placeholder(repOf(type));
        }
        m_arrayIsConvertedInFrontOfLoop = isConvertedInFrontOfLoop(object);
        m_arrayIsBornKeepingDoubles = indexingTypeAtBirth(typeOf(object)) == ArrayWithDouble;
        return getArrayElement(nonNil(object, { }), key, type, nullptr, isExpectedToBeNumber(result));
    case KeyedAccess::TypedArray:
        return getTypedArrayElement(typedArrayReceiver(object), key, typeOf(object).onlyEngineClass()->typedArrayType, type);
    case KeyedAccess::String:
        return getStringElement(stringReceiver(object), key, type);
    case KeyedAccess::Generic:
        break;
    }
    auto generally = [&] {
        return ensure(getByValGenerally(object, key), type);
    };
    if (value.readsListedKey()) {
        auto& keyRead = m_function.at(originOf(key));
        ValueID index = keyRead.child(1);
        const ForInStamp* stamp = nullptr;
        for (auto& candidate : m_forInStamps) {
            if (candidate.keys == originOf(keyRead.child(0)))
                stamp = &candidate;
        }
        if (stamp && m_function.at(index).rep == Rep::Int64) {
            CellPointer cell = asCell(in<Boxed>(object));
            Integer at = in<Integer>(index);
            Integer inlineCapacity = stamp->inlineCapacity;
            auto load = [&] {
                return either<Boxed>(lessThan(at, inlineCapacity), [&] {
                    return Boxed(m_out.load64(TypedPointer(m_heaps.root, m_out.add(m_out.add(cell.value(), m_out.constIntPtr(JSObject::offsetOfInlineStorage())), m_out.shl(at.value(), m_out.constInt32(3))))));
                }, [&] {
                    constexpr intptr_t offsetOfFirstProperty = offsetInButterfly(firstOutOfLineOffset) * static_cast<intptr_t>(sizeof(EncodedJSValue));
                    LValue butterfly = m_out.loadPtr(cell.value(), m_heaps.JSObject_butterfly);
                    return Boxed(m_out.load64(TypedPointer(m_heaps.root, m_out.sub(m_out.add(butterfly, m_out.constIntPtr(offsetOfFirstProperty)), m_out.shl(sub(at, inlineCapacity).value(), m_out.constInt32(3))))));
                });
            };
            if (Options::validateAOTTypedFacts()) [[unlikely]] {
                Boxed general = getByValGenerally(object, key);
                LBasicBlock isNotAsListed = newColdBlock();
                LBasicBlock continuation = m_out.newBlock();
                orElse(isCell(in<Boxed>(object), typeOf(object)), isNotAsListed);
                orElse(equal(loadField<Word32>(cell, m_heaps.JSCell_structureID), stamp->finalStructureID), isNotAsListed);
                orElse(equal(load(), general), isNotAsListed);
                m_out.jump(continuation);
                m_out.appendTo(isNotAsListed);
                AOT_TYPED_OPERATION(FactDoesNotHold, in<Boxed>(object), word32(m_id.index()), word32(result.index()));
                m_out.jump(continuation);
                m_out.appendTo(continuation);
                return ensure(general, type);
            }
            return ensure(load(), type);
        }
    }
    if (auto read = tryLookupWithSunkKey(key, type, [&](Word32 count, Pointer parts) -> Boxed {
        return AOT_TYPED_OPERATION(TryGetByValWithKeyParts, in<Boxed>(object), count, parts);
    }, [&]() -> Operand {
        return generally();
    }))
        return *read;
    // The receiver, or the key, is only expected to be what an access in place takes.
    bool isColdByText = !m_isInLoop && !m_program.isHotOutsideItsLoops(m_id);
    if (isColdByText || !mayBeIndex(key))
        return generally();
    Type expected = expectedTypeOf(object).withoutPrimitives(Primitive::UndefinedOrNull);
    auto inPlace = [&](const auto& read) {
        return ifAsExpected({ { object, &expected } }, repOf(type), [&](LBasicBlock otherwise) {
            return read(get(object).as<CellPointer>(), otherwise);
        }, generally);
    };
    B3::Variable* isShownToBeThere = get(object).rep == Rep::JSValue ? m_elementsShownToBeThere.get(result.index()) : nullptr;
    auto whereShownToBeThere = [&](const auto& read, const auto& elsewhere) {
        LBasicBlock otherwise = newColdBlock();
        Join join(m_out, repOf(type), b3TypeOf(repOf(type)));
        orElse(Word32(m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), isShownToBeThere)), otherwise);
        size_t numberOfAssumptions = m_assumptions.size();
        m_assumptions.append(Assumption { object, asCell(in<Boxed>(object)), &expected });
        {
            SetForScope isInArm(m_isInArm, true);
            m_indexIsShownToBeInside = true;
            join.add(read(otherwise));
        }
        m_assumptions.shrink(numberOfAssumptions);
        m_out.appendTo(otherwise);
        join.add(elsewhere());
        return join.finish();
    };
    if (expected.isString()) {
        B3::Variable* codeUnit = m_proc.addVariable(Int64);
        auto readCharacter = [&](LBasicBlock otherwise) {
            return getStringElement(stringReceiver(get(object).as<CellPointer>()), key, type, otherwise, codeUnit);
        };
        auto generallyWithoutCodeUnit = [&] {
            m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), codeUnit, m_out.constInt64(codeUnitIsNotKept));
            return generally();
        };
        Operand element = isShownToBeThere ? whereShownToBeThere(readCharacter, generallyWithoutCodeUnit) : ifAsExpected({ { object, &expected } }, repOf(type), readCharacter, generallyWithoutCodeUnit);
        m_codeUnitsOfStrings.set(result.index(), CodeUnitOfString { element.value, m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), codeUnit), true });
        return element;
    }
    auto* engineClass = expected.onlyEngineClass();
    if (!engineClass)
        return generally();
    if (engineClass->type == ArrayType) {
        bool expectsNumbers = (!expected.elements || m_program.at(expected.elements).primitives & Primitive::Number) && isExpectedToBeNumber(result);
        if (isShownToBeThere) {
            return whereShownToBeThere([&](LBasicBlock otherwise) {
                m_leavesNumbersToLoadElement = m_function.at(result).type == m_program.types.anything && !predictionOf(result);
                return getArrayElement(get(object).as<CellPointer>(), key, type, otherwise, expectsNumbers);
            }, generally);
        }
        return inPlace([&](CellPointer array, LBasicBlock otherwise) {
            m_leavesNumbersToLoadElement = m_function.at(result).type == m_program.types.anything && !predictionOf(result);
            return getArrayElement(array, key, type, otherwise, expectsNumbers);
        });
    }
    if (hasElementsReadInPlace(engineClass->typedArrayType)) {
        return inPlace([&](CellPointer view, LBasicBlock otherwise) {
            return getTypedArrayElement(typedArrayReceiver(view), key, engineClass->typedArrayType, type, otherwise);
        });
    }
    return generally();
}

void Lowering::lowerPutByVal(const Value& value)
{
    if (auto* run = propertyRunAt(m_currentIndexInBlock)) {
        if (m_currentIndexInBlock == run->stores.last().index)
            lowerPropertyRun(*run);
        return;
    }
    ValueID object = value.child(0);
    ValueID key = value.child(1);
    ValueID stored = value.child(2);
    KeyedAccess access = keyedAccessOf(m_program, m_function, value);
    didEmitWrite(access == KeyedAccess::TypedArray ? Writes::TypedArrayProperties : Writes::Heap);
    bool isColdByText = !m_isInLoop && !m_program.isHotOutsideItsLoops(m_id);
    switch (access) {
    case KeyedAccess::Array:
        static_assert(static_cast<unsigned>(Stub::StoreElement) + 1 == static_cast<unsigned>(Stub::LeafStoreElement));
        static_assert(static_cast<unsigned>(Stub::StoreElementOfDouble) + 1 == static_cast<unsigned>(Stub::LeafStoreElementOfDouble));
        if (isColdByText && get(key).rep == Rep::Int64 && get(stored).rep == Rep::Double) {
            callAllocationStub(Stub::StoreElementOfDouble, { { nonNil(object, { }).value(), GPRInfo::argumentGPR1 }, { in<Integer>(key).value(), GPRInfo::argumentGPR2 } }, effectsOfOperation<operationAOTTypedStoreElement>(), true, false, false, in<Number>(stored).value());
            return;
        }
        if (isColdByText && get(key).rep == Rep::Int64) {
            callAllocationStub(Stub::StoreElement, { { nonNil(object, { }).value(), GPRInfo::argumentGPR1 }, { in<Integer>(key).value(), GPRInfo::argumentGPR2 }, { in<Boxed>(stored).value(), GPRInfo::argumentGPR3 } }, effectsOfOperation<operationAOTTypedStoreElement>(), true);
            return;
        }
        putArrayElement(nonNil(object, { }), key, stored, nullptr, object);
        return;
    case KeyedAccess::TypedArray:
        putTypedArrayElement(typedArrayReceiver(object), key, typeOf(object).onlyEngineClass()->typedArrayType, stored);
        return;
    case KeyedAccess::String:
    case KeyedAccess::Generic:
        break;
    }
    auto generally = [&]() -> Operand {
        putByValGenerally(object, key, stored);
        return { };
    };
    Type expected = expectedTypeOf(object).withoutPrimitives(Primitive::UndefinedOrNull);
    auto* engineClass = expected.onlyEngineClass();
    if (!engineClass || !mayBeIndex(key) || isColdByText) {
        generally();
        return;
    }
    auto inPlace = [&](const auto& write) {
        ifAsExpected({ { object, &expected } }, Rep::Void, [&](LBasicBlock otherwise) -> Operand {
            write(get(object).as<CellPointer>(), otherwise);
            return { };
        }, generally);
    };
    if (engineClass->type == ArrayType) {
        inPlace([&](CellPointer array, LBasicBlock otherwise) {
            putArrayElement(array, key, stored, otherwise, object);
        });
        return;
    }
    // Rounding to a clamped byte is left to the engine.
    bool mayStoreNumber = typeOf(stored).withoutPrimitives(Primitive::UndefinedOrNull).isNumber() || (repOf(typeOf(stored)) == Rep::JSValue && expectedTypeOf(stored).primitives & Primitive::Number);
    if (hasElementsReadInPlace(engineClass->typedArrayType) && engineClass->typedArrayType != TypeUint8Clamped && mayStoreNumber) {
        inPlace([&](CellPointer view, LBasicBlock otherwise) {
            putTypedArrayElement(typedArrayReceiver(view), key, engineClass->typedArrayType, stored, otherwise);
        });
        return;
    }
    generally();
}

// Iteration.
//
// An array whose iteration nothing can observe stands for its own iterator, and compiled code steps through it by index. Every value
// that is typed as an array is one. Of any other value operationAOTTypedIterOpen finds out: what it returns is such an array, or a
// record of the iterator, which is a cell of another type. Where it does not show how the iterator was made, that is looked at.
// Of a value that is expected to be an array it is found out in place first. An array that was not typed as one is asked for its
// elements the way the language has it: one that is there is read, for anything else the array is asked the general way.

// It was of ArrayType when the iteration was opened.
Word32 Lowering::isArrayStandingForItsIterator(ValueID iterator)
{
    static_assert(DerivedArrayType == ArrayType + 1);
    return below(binary<Word32>(&Output::sub, typeOfCell(asCell(in<Boxed>(iterator))), word32(ArrayType)), word32(2));
}

// A string stands for its own iterator too: its iteration cannot be replaced either, and has nothing to close. It goes by code points, so that where it is is not what the steps count: that is kept in a variable of the
// back end's, for an iterator that nothing uses but its own IteratorHasNext, IteratorValue and IteratorClose.
void Lowering::findIterationsOfStrings()
{
    Vector<std::pair<ValueID, bool>, 4> found;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            if (value.opcode != Opcode::IteratorOpen)
                continue;
            auto& type = m_program.at(m_function.at(value.child(0)).type);
            bool isCertain = type.primitives & Primitive::String && type.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
            auto expected = predictionOf(value.child(0));
            if (isCertain || (expected && expected->isString()))
                found.append({ id, isCertain });
        }
    }
    if (found.isEmpty())
        return;
    BitVector hasOtherUse = valuesUsedOtherwiseThanAsIteratorsOfSteps();
    for (auto [id, isCertain] : found) {
        if (!hasOtherUse.get(id.index()))
            m_iterationsOfStrings.add(id.index(), IterationOfString { m_proc.addVariable(Int64), m_proc.addVariable(Int64), m_proc.addVariable(Int64), m_proc.addVariable(Int32), isCertain });
    }
}

BitVector Lowering::valuesUsedOtherwiseThanAsIteratorsOfSteps() const
{
    BitVector hasOtherUse;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& user = m_function.at(id);
            bool stepsThrough = user.opcode == Opcode::IteratorHasNext || user.opcode == Opcode::IteratorValue || user.opcode == Opcode::IteratorClose;
            unsigned index = 0;
            user.forEachChild([&](ValueID child) {
                if (!stepsThrough || index++)
                    hasOtherUse.set(child.index());
            });
        }
        if (block.terminator.value)
            hasOtherUse.set(block.terminator.value.index());
        for (auto& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                hasOtherUse.set(argument.index());
        }
    }
    return hasOtherUse;
}

const Lowering::ElementsTakenAtOnce* Lowering::elementsTakenAtOnce(ValueID iterator) const
{
    if (m_elementsTakenAtOnce.isEmpty())
        return nullptr;
    auto found = m_elementsTakenAtOnce.find(iterator.index());
    return found == m_elementsTakenAtOnce.end() ? nullptr : &found->value;
}

void Lowering::findElementsTakenAtOnce()
{
    static constexpr unsigned maxElementsTakenAtOnce = 8;
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return;
    Vector<std::pair<unsigned, unsigned>, 4> found;
    for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
        auto& values = m_function.blocks[b].values;
        for (unsigned i = 0; i < values.size(); ++i) {
            auto& value = m_function.at(values[i]);
            if (value.opcode == Opcode::IteratorOpen && keyedAccessOf(m_program, m_function, value) == KeyedAccess::Generic && !iterationOfString(values[i]) && !m_iterationsOfEntries.get(values[i].index()) && !m_pairsOfEntries.contains(values[i].index()))
                found.append({ b, i });
        }
    }
    if (found.isEmpty())
        return;
    BitVector hasOtherUse = valuesUsedOtherwiseThanAsIteratorsOfSteps();
    Vector<Vector<unsigned, 2>> predecessors(m_function.blocks.size());
    for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
        for (auto& target : m_function.blocks[b].terminator.targets)
            predecessors[target.block.index()].append(b);
    }
    auto isStepOf = [&](const Value& value, ValueID iterator) {
        return (value.opcode == Opcode::IteratorHasNext || value.opcode == Opcode::IteratorValue || value.opcode == Opcode::IteratorClose) && value.child(0) == iterator;
    };
    auto nobodyCanTell = [&](const Value& value) {
        switch (value.opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::BigInt:
        case Opcode::StrictEq:
        case Opcode::LogicalNot:
        case Opcode::IsUndefinedOrNull:
        case Opcode::ToBoolean:
            return true;
        case Opcode::Cast:
            return !mayThrow(m_program, m_function, value);
        default:
            return false;
        }
    };
    UncheckedKeyHashMap<uint32_t, Vector<std::pair<unsigned, ValueID>, 8>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> stepsOf;
    for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
        for (ValueID id : m_function.blocks[b].values) {
            auto& step = m_function.at(id);
            if (step.opcode == Opcode::IteratorHasNext || step.opcode == Opcode::IteratorValue || step.opcode == Opcode::IteratorClose)
                stepsOf.add(step.child(0).index(), Vector<std::pair<unsigned, ValueID>, 8> { }).iterator->value.append({ b, id });
        }
    }
    Vector<unsigned> waysInLeft;
    waysInLeft.fill(0, m_function.blocks.size());
    for (auto [opened, indexOfOpen] : found) {
        ValueID iterator = m_function.blocks[opened].values[indexOfOpen];
        if (hasOtherUse.get(iterator.index()))
            continue;
        auto steps = stepsOf.find(iterator.index());
        if (steps == stepsOf.end())
            continue;
        BitVector leadsToStep(m_function.blocks.size());
        Vector<unsigned, 16> worklist;
        uint32_t asked = 0;
        uint32_t read = 0;
        bool isRegular = true;
        for (auto [b, id] : steps->value) {
            auto& step = m_function.at(id);
            if (!leadsToStep.set(b))
                worklist.append(b);
            if (step.opcode == Opcode::IteratorClose) {
                isRegular &= !step.immediates[0];
                continue;
            }
            auto& place = m_function.at(step.child(1));
            if (place.opcode != Opcode::Integer || place.integer() < 0 || place.integer() >= maxElementsTakenAtOnce) {
                isRegular = false;
                break;
            }
            uint32_t& seen = step.opcode == Opcode::IteratorHasNext ? asked : read;
            isRegular &= !(seen >> place.integer() & 1);
            seen |= 1u << place.integer();
            if (step.opcode == Opcode::IteratorValue)
                isRegular &= step.type == m_program.types.anything;
        }
        if (!isRegular || !asked || asked & (asked + 1) || read & ~asked)
            continue;
        while (!worklist.isEmpty()) {
            unsigned b = worklist.takeLast();
            if (b == opened)
                continue;
            for (unsigned predecessor : predecessors[b]) {
                if (!leadsToStep.set(predecessor))
                    worklist.append(predecessor);
            }
        }
        if (!leadsToStep.get(opened))
            continue;
        for (unsigned b : leadsToStep) {
            for (unsigned predecessor : predecessors[b])
                waysInLeft[b] += b != opened && leadsToStep.get(predecessor);
        }
        unsigned numberInOrder = 0;
        worklist.append(opened);
        while (!worklist.isEmpty()) {
            unsigned b = worklist.takeLast();
            ++numberInOrder;
            for (auto& target : m_function.blocks[b].terminator.targets) {
                unsigned next = target.block.index();
                if (next != opened && leadsToStep.get(next) && !--waysInLeft[next])
                    worklist.append(next);
            }
        }
        for (unsigned b : leadsToStep)
            waysInLeft[b] = 0;
        if (numberInOrder != leadsToStep.bitCount())
            continue;
        for (unsigned b : leadsToStep) {
            auto& block = m_function.blocks[b];
            bool goesOnToStep = false;
            for (auto& target : block.terminator.targets)
                goesOnToStep |= target.block.index() != opened && leadsToStep.get(target.block.index());
            if (block.terminator.kind == TerminatorKind::Invoke) {
                auto& atOpen = m_function.blocks[opened].terminator;
                isRegular &= atOpen.kind == TerminatorKind::Invoke && atOpen.targets[1].block == block.terminator.targets[1].block && atOpen.targets[1].arguments == block.terminator.targets[1].arguments;
            } else if (goesOnToStep && block.terminator.kind != TerminatorKind::Jump && block.terminator.kind != TerminatorKind::Branch)
                isRegular = false;
            unsigned end = block.values.size();
            while (!goesOnToStep && end && !isStepOf(m_function.at(block.values[end - 1]), iterator))
                --end;
            for (unsigned i = b == opened ? indexOfOpen + 1 : 0; i < end; ++i) {
                auto& value = m_function.at(block.values[i]);
                isRegular &= isStepOf(value, iterator) || nobodyCanTell(value);
            }
        }
        if (!isRegular)
            continue;
        m_elementsTakenAtOnce.add(iterator.index(), ElementsTakenAtOnce { nullptr, static_cast<unsigned>(std::popcount(asked)) });
    }
    unsigned most = 0;
    for (auto& taken : m_elementsTakenAtOnce.values())
        most = std::max(most, taken.count);
    if (!most)
        return;
    auto* slot = m_proc.addStackSlot(most * sizeof(EncodedJSValue));
    for (auto& taken : m_elementsTakenAtOnce.values())
        taken.slot = slot;
}

void Lowering::findIterationsInFrame()
{
    Vector<ValueID, 4> found;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            if (value.opcode == Opcode::IteratorOpen && keyedAccessOf(m_program, m_function, value) == KeyedAccess::Generic && !iterationOfString(id) && !m_iterationsOfEntries.get(id.index()) && !m_pairsOfEntries.contains(id.index()) && !elementsTakenAtOnce(id))
                found.append(id);
        }
    }
    if (found.isEmpty())
        return;
    BitVector hasOtherUse = valuesUsedOtherwiseThanAsIteratorsOfSteps();
    for (ValueID id : found) {
        if (!hasOtherUse.get(id.index()))
            m_iterationsInFrame.add(id.index(), m_proc.addStackSlot(sizeof(CollectionIteration::InFrame)));
    }
}

std::optional<Pointer> Lowering::recordInFrameOf(ValueID iterator)
{
    if (m_iterationsInFrame.isEmpty())
        return std::nullopt;
    auto found = m_iterationsInFrame.find(iterator.index());
    if (found == m_iterationsInFrame.end())
        return std::nullopt;
    return Pointer(m_out.m_block->appendNew<B3::SlotBaseValue>(m_proc, m_out.origin(), found->value));
}

const Lowering::IterationOfString* Lowering::iterationOfString(ValueID iterator) const
{
    if (m_iterationsOfStrings.isEmpty())
        return nullptr;
    auto found = m_iterationsOfStrings.find(iterator.index());
    return found == m_iterationsOfStrings.end() ? nullptr : &found->value;
}

// What operationAOTTypedIterOpen yields is an array or a record, which is a cell of another type.
Word32 Lowering::isStringStandingForItsIterator(ValueID iterator)
{
    return equal(typeOfCell(asCell(in<Boxed>(iterator))), word32(StringType));
}

Integer Lowering::positionIn(const IterationOfString& iteration)
{
    return Integer(m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), iteration.position));
}

void Lowering::setPositionIn(const IterationOfString& iteration, Integer position)
{
    m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), iteration.position, position.value());
}

Operand Lowering::openIterationOfString(const IterationOfString& iteration, ValueID result, ValueID iterable)
{
    setPositionIn(iteration, integer(0));
    auto& type = typeOf(iterable);
    if (!type.isString()) {
        m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), iteration.data, m_out.int64Zero);
        m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), iteration.length, m_out.int64Zero);
        m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), iteration.flags, m_out.int32Zero);
    }
    auto inPlace = [&](CellPointer string) {
        // Every rope is made flat here, once: a substring that is looked at in its base lets go of the base when something in the loop makes it flat.
        Pointer fiber = fiberOf(stringReceiver(string));
        Pointer impl = eitherPointer(lacksBits(fiber, JSString::isRopeInPointer), [&] {
            return fiber;
        }, [&] {
            return AOT_TYPED_OPERATION(Flatten, string);
        });
        auto set = [&](B3::Variable* variable, LValue value) {
            m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), variable, value);
        };
        set(iteration.data, loadField<Pointer>(impl, m_heaps.StringImpl_data).value());
        set(iteration.length, zeroExtended(loadField<Word32>(impl, m_heaps.StringImpl_length)).value());
        set(iteration.flags, loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags, IsFixed).value());
        ensureStillAliveHere(string);
        // The word that was read above may have said "rope": whoever reads it from here on does so through a pointer of its own.
        return box(CellPointer(m_out.opaque(string.value())));
    };
    if (type.isString())
        return ensure(inPlace(in<CellPointer>(iterable)), typeOf(result));
    Boxed boxed = in<Boxed>(iterable);
    LBasicBlock isCellCase = m_out.newBlock();
    LBasicBlock isStringCase = iteration.isCertain ? isCellCase : m_out.newBlock();
    LBasicBlock otherwise = newColdBlock();
    Join join(m_out, Rep::JSValue, Int64);
    m_out.branch((iteration.isCertain ? isCell(boxed, type) : isCell(boxed)).value(), usually(isCellCase), rarely(otherwise));
    m_out.appendTo(isCellCase);
    if (!iteration.isCertain) {
        m_out.branch(equal(typeOfCell(asCell(boxed)), word32(StringType)).value(), usually(isStringCase), rarely(otherwise));
        m_out.appendTo(isStringCase);
    }
    join.add(inPlace(asCell(boxed)));
    m_out.appendTo(otherwise);
    join.add(AOT_TYPED_OPERATION(IterOpen, boxed));
    return ensure(join.finish().as<Boxed>(), typeOf(result));
}

Lowering::Characters Lowering::charactersOfIteratedString(const IterationOfString& iteration, CellPointer string)
{
    auto get = [&](B3::Variable* variable) {
        return m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), variable);
    };
    return { Pointer(get(iteration.data)), Word32(get(iteration.flags)), Integer(get(iteration.length)), string };
}

Word32 Lowering::iteratedStringHasNext(const IterationOfString& iteration, ValueID iterator)
{
    return below(positionIn(iteration), charactersOfIteratedString(iteration, asCell(in<Boxed>(iterator))).length);
}

// What the front end holds the value of an iteration to is the type of the loop's variable. The iteration of a string yields strings.
std::optional<Operand> Lowering::castOfIteratedCharacter(const Value& value)
{
    if (m_iterationsOfStrings.isEmpty() || value.opcode != Opcode::Cast || value.castReason() == CastReason::Coerce)
        return std::nullopt;
    auto& from = m_function.at(value.child(0));
    if (from.opcode != Opcode::IteratorValue || !(m_program.at(value.type).primitives & Primitive::String))
        return std::nullopt;
    auto* iteration = iterationOfString(from.child(0));
    if (!iteration || !iteration->isCertain)
        return std::nullopt;
    // There every value is looked at by itself.
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts()) [[unlikely]]
        return std::nullopt;
    return convert(asCell(in<Boxed>(value.child(0))), m_program.at(m_program.types.string), value.rep);
}

// The position is inside.
CellPointer Lowering::nextOfIteratedString(const IterationOfString& iteration, ValueID iterator, Word32& codePoint)
{
    StringReceiver receiver = stringReceiver(asCell(in<Boxed>(iterator)));
    Integer position = positionIn(iteration);
    Characters characters = charactersOfIteratedString(iteration, receiver.string);
    LBasicBlock narrow = m_out.newBlock();
    LBasicBlock wide = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(hasBits(characters.flags, StringImpl::flagIs8Bit()).value(), unsure(narrow), unsure(wide));

    static_assert(maxSingleCharacterString == 0xff);
    m_out.appendTo(narrow);
    LValue unit = marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), offsetBy(characters.data, position, 0).value())), IsFixed);
    ensureStillAliveHere(receiver.string);
    setPositionIn(iteration, add(position, integer(1)));
    ValueFromBlock narrowUnit = m_out.anchor(unit);
    ValueFromBlock narrowString = m_out.anchor(singleCharacterString(unit));
    m_out.jump(continuation);

    m_out.appendTo(wide);
    Word32 wideCodePoint = codePointAt(characters, position);
    Integer next = add(position, select(below(word32(0xffff), wideCodePoint), integer(2), integer(1)));
    setPositionIn(iteration, next);
    CellPointer wideString = either<CellPointer>(belowOrEqual(wideCodePoint, word32(maxSingleCharacterString)), [&] {
        return CellPointer(singleCharacterString(wideCodePoint.value()));
    }, [&] {
        return partOfString(receiver, characters.length, position, next);
    });
    ValueFromBlock wideUnit = m_out.anchor(wideCodePoint.value());
    ValueFromBlock wideResult = m_out.anchor(wideString.value());
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    codePoint = Word32(m_out.phi(Int32, narrowUnit, wideUnit));
    return CellPointer(m_out.phi(Int64, narrowString, wideResult));
}

// `for (const [i, x] of array.entries())`. The call of `entries`, on an array that has its original structure, goes to one place: the IteratorOpen of a loop, straight or through parameters of blocks
// and upcasts that nothing else uses. Nothing uses that but its IteratorHasNext, IteratorValue and IteratorClose, and what each IteratorValue yields is only taken apart: by an IteratorOpen that
// nothing uses but IteratorHasNext and IteratorValue with constant indices and IteratorClose, or by casts that nothing uses but GetByVal with 0 or 1 and an IteratorOpen that nothing uses. Then nobody can tell whether there is an
// iterator, or a pair: the call makes a record (CollectionIteration::isOfEntriesOfArrayByIndex), and each of the others asks the iterator of the loop whether it is one: see partOfEntriesOfArray().
void Lowering::findEntriesOfArrays()
{
    Vector<ValueID, 2> calls;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            if (value.opcode != Opcode::CallBuiltin)
                continue;
            auto& implementation = m_builtins.at(value.builtin()).implementation;
            if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ArrayEntries && value.arguments.isEmpty())
                calls.append(id);
        }
    }
    if (calls.isEmpty())
        return;

    // The value that has it for its first operand, or the parameter it is handed to, if that is its only use.
    auto whereItGoes = [&](ValueID id) -> ValueID {
        if (numberOfUsesOf(id) != 1)
            return { };
        for (auto& block : m_function.blocks) {
            for (ValueID user : block.values) {
                if (m_function.at(user).child(0) == id)
                    return user;
            }
            for (auto& target : block.terminator.targets) {
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    if (target.arguments[i] == id)
                        return m_function.at(target.block).parameters[i];
                }
            }
        }
        return { };
    };
    // Whether every use of the value is as the first operand of a value the functor accepts.
    auto isOnlyUsedBy = [&](ValueID id, const auto& accepts) {
        for (auto& block : m_function.blocks) {
            for (ValueID user : block.values) {
                unsigned index = 0;
                bool isAccepted = true;
                m_function.at(user).forEachChild([&](ValueID child) {
                    if (child == id && (index || !accepts(user, m_function.at(user))))
                        isAccepted = false;
                    ++index;
                });
                if (!isAccepted)
                    return false;
            }
            if (block.terminator.value == id)
                return false;
            for (auto& target : block.terminator.targets) {
                if (target.arguments.contains(id))
                    return false;
            }
        }
        return true;
    };
    auto isConstantIndex = [&](ValueID id, int64_t limit) {
        auto& index = m_function.at(id);
        return index.opcode == Opcode::Integer && index.integer() >= 0 && index.integer() < limit;
    };

    for (ValueID call : calls) {
        ValueID iterator = whereItGoes(call);
        for (unsigned steps = 0; steps < 4 && iterator; ++steps) {
            auto& value = m_function.at(iterator);
            if (value.opcode != Opcode::Parameter && (value.opcode != Opcode::Cast || value.castReason() != CastReason::Upcast))
                break;
            iterator = whereItGoes(iterator);
        }
        if (!iterator || m_function.at(iterator).opcode != Opcode::IteratorOpen || iterationOfString(iterator))
            continue;
        BlockID made = m_definingBlock[call.index()];
        BlockID opened = m_definingBlock[iterator.index()];
        bool mayBeOpenedAgain = made != opened && m_loopBlocks.get(opened.index()) && m_innermostLoop[opened.index()] == noLoop;
        for (auto& loop : m_loops)
            mayBeOpenedAgain |= loop.blocks.get(opened.index()) && !loop.blocks.get(made.index());
        if (mayBeOpenedAgain)
            continue;
        if (m_iterationsOfEntries.get(iterator.index())) {
            m_entriesMadeAsRecords.set(call.index());
            continue;
        }

        PairsOfEntries pairs;
        bool isTakenApart = isOnlyUsedBy(iterator, [&](ValueID user, const Value& step) {
            if (step.opcode == Opcode::IteratorValue)
                pairs.add(user.index(), PairOfEntries { iterator, user });
            return step.opcode == Opcode::IteratorHasNext || step.opcode == Opcode::IteratorValue || step.opcode == Opcode::IteratorClose;
        });
        Vector<ValueID, 2> values;
        for (auto& pair : pairs.values())
            values.append(pair.pair);
        for (ValueID pair : values) {
            if (!isTakenApart)
                break;
            isTakenApart = m_function.at(pair).rep == Rep::JSValue && isOnlyUsedBy(pair, [&](ValueID user, const Value& use) {
                if (m_function.at(user).rep != Rep::JSValue)
                    return false;
                pairs.add(user.index(), PairOfEntries { iterator, pair });
                if (use.opcode == Opcode::IteratorOpen) {
                    return numberOfUsesOf(pair) == 1 && !iterationOfString(user) && isOnlyUsedBy(user, [&](ValueID, const Value& step) {
                        if (step.opcode == Opcode::IteratorHasNext || step.opcode == Opcode::IteratorValue)
                            return isConstantIndex(step.child(1), INT32_MAX);
                        return step.opcode == Opcode::IteratorClose;
                    });
                }
                if (use.opcode != Opcode::Cast || use.castReason() == CastReason::Coerce || use.castReason() == CastReason::As)
                    return false;
                return isOnlyUsedBy(user, [&](ValueID reader, const Value& read) {
                    if (read.opcode == Opcode::IteratorOpen && !numberOfUsesOf(reader) && !iterationOfString(reader) && m_function.at(reader).rep == Rep::JSValue) {
                        pairs.add(reader.index(), PairOfEntries { iterator, pair });
                        return true;
                    }
                    return read.opcode == Opcode::GetByVal && isConstantIndex(read.child(1), 2);
                });
            });
        }
        if (!isTakenApart)
            continue;
        m_entriesMadeAsRecords.set(call.index());
        m_iterationsOfEntries.set(iterator.index());
        for (auto& [id, pair] : pairs)
            m_pairsOfEntries.add(id, pair);
    }
}

TypedPointer Lowering::wordOfIterationRecord(CellPointer record, unsigned word)
{
    return m_out.address(m_heaps.AOTTyped_runtimeState, record.value(), JSCellButterfly::offsetOfData() + word * sizeof(EncodedJSValue));
}

// Of what an IteratorOpen has yielded, which is a cell: an array or a string that stands for its own iterator, or a record. The first two words of a record stay what they are.
Word32 Lowering::iteratesEntriesOfArray(ValueID iterator)
{
    CellPointer cell = asCell(in<Boxed>(iterator));
    return either<Word32>(equal(typeOfCell(cell), word32(JSCellButterflyType)), [&] {
        return Word32(m_out.equal(marked(m_out.load64(wordOfIterationRecord(cell, 1)), IsFixed), m_out.constInt64(JSValue::encode(jsNumber(CollectionIteration::isOfEntriesOfArrayByIndex)))));
    }, [&] {
        return boolean(false);
    });
}

CellPointer Lowering::arrayOfEntries(ValueID iterator)
{
    return CellPointer(marked(m_out.load64(wordOfIterationRecord(asCell(in<Boxed>(iterator)), 0)), IsFixed));
}

// Nothing: the value has no part in such a loop.
std::optional<Operand> Lowering::partOfEntriesOfArray(ValueID id, const Value& value)
{
    if (m_iterationsOfEntries.isEmpty())
        return std::nullopt;
    auto& type = typeOf(id);
    Rep rep = repOf(type);
    auto pairOf = [&](ValueID operand) -> const PairOfEntries* {
        auto found = m_pairsOfEntries.find(operand.index());
        return found == m_pairsOfEntries.end() ? nullptr : &found->value;
    };
    // The counter of the loop, which is the index of a fresh iterator, and the element.
    auto part = [&](const PairOfEntries& pair, int64_t index) -> Operand {
        if (!index) {
            ValueID counter = m_function.at(pair.pair).child(1);
            return ensure(in<Integer>(counter), typeOf(counter), type);
        }
        if (index == 1) {
            SetForScope isElementOfEntries(m_isElementOfEntries, true);
            return ensure(in<Boxed>(pair.pair), type);
        }
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    };

    switch (value.opcode) {
    case Opcode::IteratorOpen: {
        Boxed operand = in<Boxed>(value.child(0));
        if (m_iterationsOfEntries.get(id.index())) {
            // No value of the program is such a cell.
            Word32 isRecord = either<Word32>(isCell(operand), [&] {
                return equal(typeOfCell(asCell(operand)), word32(JSCellButterflyType));
            }, [&] {
                return boolean(false);
            });
            return eitherOperand(isRecord, rep, [&] {
                return convert(operand, type, rep);
            }, [&] {
                return lowerIteratorOpen(id, value);
            });
        }
        auto* pair = pairOf(id);
        if (!pair)
            return std::nullopt;
        return eitherOperand(iteratesEntriesOfArray(pair->iterator), rep, [&] {
            return convert(operand, type, rep);
        }, [&] {
            return lowerIteratorOpen(id, value);
        });
    }
    case Opcode::IteratorHasNext: {
        if (m_iterationsOfEntries.get(value.child(0).index())) {
            return Operand(either<Word32>(iteratesEntriesOfArray(value.child(0)), [&] {
                return below(in<Integer>(value.child(1)), arrayLength(arrayOfEntries(value.child(0))));
            }, [&] {
                return lowerIteratorHasNext(value);
            }));
        }
        auto* pair = pairOf(value.child(0));
        if (!pair)
            return std::nullopt;
        return Operand(either<Word32>(iteratesEntriesOfArray(pair->iterator), [&] {
            return boolean(m_function.at(value.child(1)).integer() < 2);
        }, [&] {
            return lowerIteratorHasNext(value);
        }));
    }
    case Opcode::IteratorValue: {
        if (m_iterationsOfEntries.get(value.child(0).index())) {
            return eitherOperand(iteratesEntriesOfArray(value.child(0)), rep, [&] {
                auto& anything = m_program.at(m_program.types.anything);
                return convert(getArrayElement(arrayOfEntries(value.child(0)), value.child(1), anything, nullptr, false), anything, rep);
            }, [&] {
                return lowerIteratorValue(id, value);
            });
        }
        auto* pair = pairOf(value.child(0));
        if (!pair)
            return std::nullopt;
        return eitherOperand(iteratesEntriesOfArray(pair->iterator), rep, [&] {
            return part(*pair, m_function.at(value.child(1)).integer());
        }, [&] {
            return lowerIteratorValue(id, value);
        });
    }
    case Opcode::IteratorClose: {
        auto* pair = pairOf(value.child(0));
        if (!pair)
            return std::nullopt;
        ifElse(iteratesEntriesOfArray(pair->iterator), [] { }, [&] {
            lowerIteratorClose(value);
        });
        return Operand(undefined());
    }
    case Opcode::Cast: {
        auto* pair = pairOf(id);
        if (!pair)
            return std::nullopt;
        return eitherOperand(iteratesEntriesOfArray(pair->iterator), rep, [&] {
            return convert(undefined(), m_program.at(m_program.types.undefined), rep);
        }, [&] {
            return lowerCast(id, value);
        });
    }
    case Opcode::GetByVal: {
        auto* pair = pairOf(value.child(0));
        if (!pair)
            return std::nullopt;
        return eitherOperand(iteratesEntriesOfArray(pair->iterator), rep, [&] {
            return part(*pair, m_function.at(value.child(1)).integer());
        }, [&] {
            return lowerGetByVal(id, value);
        });
    }
    default:
        return std::nullopt;
    }
}

bool Lowering::arraysMayChangeBetweenStepsOf(ValueID iterator) const
{
    unsigned loopOfSteps = noLoop;
    for (unsigned b = 0; b < m_function.blocks.size() && loopOfSteps == noLoop; ++b) {
        if (m_innermostLoop[b] == noLoop)
            continue;
        for (ValueID id : m_function.blocks[b].values) {
            auto& step = m_function.at(id);
            if (step.opcode == Opcode::IteratorHasNext && step.child(0) == iterator) {
                loopOfSteps = m_innermostLoop[b];
                break;
            }
        }
    }
    if (loopOfSteps == noLoop)
        return true;
    for (size_t b : m_loops[loopOfSteps].blocks) {
        auto& block = m_function.blocks[b];
        for (size_t i = 0; i < block.values.size(); ++i) {
            auto& value = m_function.at(block.values[i]);
            if ((value.opcode == Opcode::CallDirect ? m_program.at(value.function()).writes : writesOf(m_program, m_function, block, i)) == Writes::Heap)
                return true;
        }
    }
    return false;
}

Operand Lowering::lowerIteratorOpen(ValueID result, const Value& value)
{
    ValueID iterable = value.child(0);
    if (auto* taken = elementsTakenAtOnce(result)) {
        LValue words = m_out.m_block->appendNew<B3::SlotBaseValue>(m_proc, m_out.origin(), taken->slot);
        callICStub(Stub::TakeElements, Void, { { in<Boxed>(iterable).value(), firstStubOperandGPR }, { words, GPRInfo::argumentGPR1 }, { m_out.constInt64(taken->count), GPRInfo::argumentGPR2 } });
        return ensure(undefined(), m_program.at(m_program.types.undefined), typeOf(result));
    }
    if (auto* iteration = iterationOfString(result))
        return openIterationOfString(*iteration, result, iterable);
    if (keyedAccessOf(m_program, m_function, value) == KeyedAccess::Array) {
        CellPointer array = nonNil(iterable, { });
        constexpr bool mayThrow = OperationSignature<std::remove_pointer_t<decltype(operationAOTTypedOpenChangedArray)>>::mayThrow;
        if (canCallOnLatePath(Entry::operationAOTTypedOpenChangedArray, mayThrow, { array.value() })) {
            callOnLatePath(Entry::operationAOTTypedOpenChangedArray, mayThrow, effectsOfOperation<operationAOTTypedOpenChangedArray>(), { array.value() }, { array.value() }, createSharedTask<CCallHelpers::JumpList(CCallHelpers&, GPRReg, std::span<const GPRReg>)>([](CCallHelpers& jit, GPRReg, std::span<const GPRReg> checked) {
                return CCallHelpers::JumpList(jit.branch8(CCallHelpers::NotEqual, CCallHelpers::Address(checked[0], JSCell::typeInfoTypeOffset()), CCallHelpers::TrustedImm32(ArrayType)));
            }), nullptr, 0);
        } else {
            LBasicBlock hasChanged = newColdBlock();
            LBasicBlock continuation = m_out.newBlock();
            m_out.branch(isPlainArray(array).value(), usually(continuation), rarely(hasChanged));
            m_out.appendTo(hasChanged);
            AOT_TYPED_OPERATION(OpenChangedArray, array);
            m_out.jump(continuation);
            m_out.appendTo(continuation);
        }
        // Read here, so that a loop that changes no array does not read them again for every element.
        if (arraysMayChangeBetweenStepsOf(result))
            return ensure(array, typeOf(iterable), typeOf(result));
        Pointer butterfly = butterflyOf(array);
        ensureStillAliveHere(CellPointer(publicLength(butterfly).value()));
        ensureStillAliveHere(CellPointer(m_out.zeroExtPtr(loadByte(array, m_heaps.JSCell_indexingTypeAndMisc, safeIfReaches(array, sizeof(JSCell))).value())));
        return ensure(array, typeOf(iterable), typeOf(result));
    }
    if (keyedAccessOf(m_program, m_function, value) == KeyedAccess::TypedArray) {
        CellPointer view = nonNil(iterable, { });
        TypedArrayReceiver receiver = typedArrayReceiver(view);
        LBasicBlock slowPath = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        orElse(hasOriginalStructure(view, *typeOf(iterable).onlyEngineClass()), slowPath);
        orElse(hasLengthOfItsOwn(receiver), slowPath);
        m_out.branch(equal(loadField<Integer>(view, m_heaps.JSArrayBufferView_length, receiver.flags), integer(0)).value(), rarely(slowPath), usually(continuation));
        m_out.appendTo(slowPath);
        AOT_TYPED_OPERATION(TypedArrayIteratorOpen, view);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return ensure(view, typeOf(iterable), typeOf(result));
    }
    auto generally = [&] {
        if (auto record = recordInFrameOf(result))
            return AOT_TYPED_OPERATION(IterOpenInFrame, in<Boxed>(iterable), *record);
        return AOT_TYPED_OPERATION(IterOpen, in<Boxed>(iterable));
    };
    // An array as the engine makes them finds the iteration of arrays, which cannot be replaced.
    auto expected = predictionOf(iterable);
    if (!expected || !expected->onlyEngineClass() || expected->onlyEngineClass()->type != ArrayType)
        return ensure(generally(), typeOf(result));
    return ensure(ifAsExpected({ { iterable, &*expected, true } }, Rep::JSValue, [&](LBasicBlock) {
        return in<Boxed>(iterable);
    }, generally).as<Boxed>(), typeOf(result));
}

Word32 Lowering::lowerIteratorHasNext(const Value& value)
{
    ValueID iterator = value.child(0);
    if (elementsTakenAtOnce(iterator))
        return boolean(true);
    auto isInsideArray = [&] {
        return below(in<Integer>(value.child(1)), arrayLength(asCell(in<Boxed>(iterator))));
    };
    if (keyedAccessOf(m_program, m_function, value) == KeyedAccess::Array)
        return isInsideArray();
    if (keyedAccessOf(m_program, m_function, value) == KeyedAccess::TypedArray) {
        TypedArrayReceiver receiver = typedArrayReceiver(asCell(in<Boxed>(iterator)));
        Integer index = in<Integer>(value.child(1));
        Integer length = select(hasLengthOfItsOwn(receiver), loadField<Integer>(receiver.view, m_heaps.JSArrayBufferView_length, receiver.flags), integer(0));
        LBasicBlock isInside = m_out.newBlock();
        LBasicBlock isNotInside = newUnlikelyBlock();
        LBasicBlock isAtEnd = newUnlikelyBlock();
        LBasicBlock slowPath = newColdBlock();
        Join join(m_out, Rep::Int32, Int32);
        m_out.branch(below(index, length).value(), usually(isInside), rarely(isNotInside));
        m_out.appendTo(isInside);
        join.add(boolean(true));
        m_out.appendTo(isNotInside);
        m_out.branch(equal(length, integer(0)).value(), rarely(slowPath), usually(isAtEnd));
        m_out.appendTo(isAtEnd);
        join.add(boolean(false));
        m_out.appendTo(slowPath);
        join.add(logicalNot(isZero(AOT_TYPED_OPERATION(TypedArrayIteratorHasNext, receiver.view, index))));
        return join.finish().as<Word32>();
    }
    auto generally = [&] {
        return either<Word32>(isArrayStandingForItsIterator(iterator), isInsideArray, [&] {
            if (auto record = recordInFrameOf(iterator))
                return AOT_TYPED_OPERATION(IterHasInFrame, *record);
            return AOT_TYPED_OPERATION(IterHas, in<Boxed>(iterator));
        });
    };
    auto* iteration = iterationOfString(iterator);
    if (!iteration)
        return generally();
    if (iteration->isCertain)
        return iteratedStringHasNext(*iteration, iterator);
    return either<Word32>(isStringStandingForItsIterator(iterator), [&] {
        return iteratedStringHasNext(*iteration, iterator);
    }, generally);
}

Operand Lowering::lowerIteratorValue(ValueID result, const Value& value)
{
    ValueID iterator = value.child(0);
    auto& type = typeOf(result);
    if (auto* taken = elementsTakenAtOnce(iterator)) {
        LValue words = m_out.m_block->appendNew<B3::SlotBaseValue>(m_proc, m_out.origin(), taken->slot);
        return ensure(Boxed(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, words, m_function.at(value.child(1)).integer() * sizeof(EncodedJSValue)))), type);
    }
    if (keyedAccessOf(m_program, m_function, value) == KeyedAccess::Array) {
        m_arrayIsConvertedInFrontOfLoop = isConvertedInFrontOfLoop(iterator);
        m_arrayIsBornKeepingDoubles = indexingTypeAtBirth(typeOf(m_function.at(iterator).child(0))) == ArrayWithDouble;
        return getArrayElement(asCell(in<Boxed>(iterator)), value.child(1), type, nullptr, isExpectedToBeNumber(result));
    }
    if (keyedAccessOf(m_program, m_function, value) == KeyedAccess::TypedArray)
        return getTypedArrayElement(typedArrayReceiver(asCell(in<Boxed>(iterator))), value.child(1), typeOf(m_function.at(iterator).child(0)).onlyEngineClass()->typedArrayType, type);
    auto* iteration = iterationOfString(iterator);
    auto& stringType = m_program.at(m_program.types.string);
    if (iteration && iteration->isCertain) {
        Word32 codePoint;
        Operand string = ensure(nextOfIteratedString(*iteration, iterator, codePoint), stringType, type);
        // No code unit is above 0xffff: a pair is equal to no string of one.
        m_codeUnitsOfStrings.set(result.index(), CodeUnitOfString { string.value, zeroExtended(codePoint).value() });
        return string;
    }
    if (iteration) {
        return eitherOperand(isStringStandingForItsIterator(iterator), repOf(type), [&] {
            Word32 codePoint;
            return ensure(nextOfIteratedString(*iteration, iterator, codePoint), stringType, type);
        }, [&] {
            return valueOfArrayOrRecord(result, value);
        });
    }
    return valueOfArrayOrRecord(result, value);
}

Operand Lowering::valueOfArrayOrRecord(ValueID result, const Value& value)
{
    ValueID iterator = value.child(0);
    auto& type = typeOf(result);
    bool wasJustFoundInside = [&] {
        for (size_t i = m_currentIndexInBlock; i--;) {
            if (m_function.at(m_currentBlock->values[i]).opcode != Opcode::LoopHint)
                return false;
        }
        if (m_onlyPredecessors.isEmpty()) {
            m_onlyPredecessors.fill(UINT32_MAX, m_function.blocks.size());
            for (uint32_t i = 0; i < m_function.blocks.size(); ++i) {
                for (auto& target : m_function.blocks[i].terminator.targets) {
                    uint32_t& only = m_onlyPredecessors[target.block.index()];
                    only = only == UINT32_MAX ? i : UINT32_MAX - 1;
                }
            }
        }
        uint32_t previous = m_onlyPredecessors[m_currentBlockID.index()];
        if (previous >= UINT32_MAX - 1)
            return false;
        auto& asking = m_function.blocks[previous];
        if (asking.terminator.kind != TerminatorKind::Branch || asking.values.isEmpty() || asking.terminator.value != asking.values.last() || asking.terminator.targets[0].block != m_currentBlockID)
            return false;
        auto& asked = m_function.at(asking.values.last());
        return asked.opcode == Opcode::IteratorHasNext && asked.child(0) == iterator && asked.child(1) == value.child(1);
    }();
    // The iterator of arrays asks the array for the element, whatever has become of the array since.
    return eitherOperand(isArrayStandingForItsIterator(iterator), repOf(type), [&] {
        return ifAsExpected({ }, repOf(type), [&](LBasicBlock otherwise) {
            m_leavesNumbersToLoadElement = m_function.at(result).type == m_program.types.anything && !predictionOf(result);
            m_indexIsShownToBeInside = wasJustFoundInside;
            Operand element = getArrayElement(asCell(in<Boxed>(iterator)), value.child(1), type, otherwise, isExpectedToBeNumber(result));
            m_indexIsShownToBeInside = false;
            return element;
        }, [&] {
            return ensure(getByValGenerally(iterator, value.child(1)), type);
        });
    }, [&] {
        // Every record has the value of the step there: see operationAOTTypedIterHas.
        if (auto record = recordInFrameOf(iterator))
            return ensure(Boxed(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, record->value(), 2 * sizeof(EncodedJSValue)))), type);
        auto place = m_out.address(m_heaps.AOTTyped_runtimeState, asCell(in<Boxed>(iterator)).value(), JSCellButterfly::offsetOfData() + 2 * sizeof(EncodedJSValue));
        Boxed held(m_out.load64(place));
        m_out.store64(m_out.constInt64(JSValue::encode(jsUndefined())), place);
        return ensure(held, type);
    });
}

void Lowering::lowerIteratorClose(const Value& value)
{
    if (keyedAccessOf(m_program, m_function, value) != KeyedAccess::Generic || elementsTakenAtOnce(value.child(0)))
        return;
    auto generally = [&] {
        ifElse(isArrayStandingForItsIterator(value.child(0)), [] { }, [&] {
            auto record = recordInFrameOf(value.child(0));
            if (record && value.immediates[0])
                AOT_TYPED_OPERATION(IterCloseForThrowInFrame, *record);
            else if (record)
                AOT_TYPED_OPERATION(IterCloseInFrame, *record);
            else if (value.immediates[0])
                AOT_TYPED_OPERATION(IterCloseForThrow, in<Boxed>(value.child(0)));
            else
                AOT_TYPED_OPERATION(IterClose, in<Boxed>(value.child(0)));
        });
    };
    auto* iteration = iterationOfString(value.child(0));
    if (!iteration)
        return generally();
    if (!iteration->isCertain)
        ifElse(isStringStandingForItsIterator(value.child(0)), [] { }, generally);
}

// The table.

// The entries that have to do with the above, for arguments writesOfInlineBuiltin() says there is code for. `inPlace`: what it says.
Operand Lowering::lowerContainerBuiltin(InlineBuiltin which, ValueID result, const Value& value, Writes inPlace)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    auto& booleanType = m_program.at(m_program.types.boolean);
    // Of what takes one.
    auto receiver = [&] {
        return nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    };
    auto yieldsNew = [&](CellPointer cell) {
        return ensure(cell, Type::engineClass(m_builtins.classes()[value.engineClass()]), type);
    };
    auto yieldsLength = [&](Integer length, int64_t most) {
        return ensure(length, Type::integer({ 0, most }), type);
    };

    switch (which) {
    case InlineBuiltin::ArrayLength: {
        Integer length = arrayLength(receiver());
        if (repOf(type) != Rep::JSValue || !m_generalArmOnOffer || value.builtin() != m_builtinOfGeneralArmOnOffer)
            return yieldsLength(length, UINT32_MAX);
        orElse(below(length, integer(1ll << 31)), m_generalArmOnOffer);
        return yieldsLength(length, INT32_MAX);
    }
    // An own property of every array that cannot be redefined as anything else, so that nothing gets in the way of its name.
    case InlineBuiltin::ArraySetLength: {
        CellPointer array = receiver();
        // Of an array with its elements in place, which it may write to, the length can be written. What is between the length and the capacity is kept clear.
        constexpr unsigned maxLengthToClearOneByOne = 64;
        LBasicBlock slowPath = newColdBlock();
        LBasicBlock shorter = m_out.newBlock();
        LBasicBlock notShorter = m_out.newBlock();
        LBasicBlock clearLoop = m_out.newBlock();
        LBasicBlock clear = m_out.newBlock();
        LBasicBlock setLength = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        didEmitWrite(Writes::Heap);
        Integer newLength = indexOrElse(arguments[0], slowPath);
        static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
        Word32 mode = indexingModeOf(array, IndexingShapeAndWritabilityMask);
        orElse(belowOrEqual(binary<Word32>(&Output::sub, mode, word32(Int32Shape)), word32(ContiguousShape - Int32Shape)), slowPath);
        Pointer butterfly = butterflyOf(array);
        Integer length = publicLength(butterfly);
        m_out.branch(below(newLength, length).value(), unsure(shorter), unsure(notShorter));
        m_out.appendTo(notShorter);
        orElse(belowOrEqual(newLength, vectorLength(butterfly)), slowPath);
        m_out.jump(setLength);
        m_out.appendTo(shorter);
        orElse(belowOrEqual(sub(length, newLength), integer(maxLengthToClearOneByOne)), slowPath);
        LValue hole = m_out.select(equal(mode, word32(DoubleShape)).value(), m_out.constInt64(std::bit_cast<int64_t>(PNaN)), m_out.int64Zero);
        ValueFromBlock first = m_out.anchor(newLength.value());
        m_out.jump(clearLoop);
        m_out.appendTo(clearLoop);
        LValue place = m_out.phi(Int64, first);
        m_out.branch(m_out.below(place, length.value()), unsure(clear), unsure(setLength));
        m_out.appendTo(clear);
        // Whichever way the elements are kept.
        m_out.store64(hole, m_out.baseIndex(m_heaps.root, butterfly.value(), place, FTL::ScaleEight));
        m_out.addIncomingToPhi(place, m_out.anchor(m_out.add(place, m_out.constInt64(1))));
        m_out.jump(clearLoop);
        m_out.appendTo(setLength);
        storeField(butterfly, m_heaps.Butterfly_publicLength, low32(newLength));
        m_out.jump(continuation);
        m_out.appendTo(slowPath);
        if (isInSloppyCode())
            AOT_TYPED_OPERATION_WITH_EFFECTS(SetByNameSloppy, effectsOfStoringElements, box(array), word32(m_program.stringOf(m_builtins.at(value.builtin()).nameName).index()), in<Boxed>(arguments[0]));
        else
            AOT_TYPED_OPERATION_WITH_EFFECTS(SetByName, effectsOfStoringElements, box(array), word32(m_program.stringOf(m_builtins.at(value.builtin()).nameName).index()), in<Boxed>(arguments[0]));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    case InlineBuiltin::ArrayPush:
        if (arguments.size() == 1)
            return pushArrayElement(receiver(), value.child(0), arguments[0], type);
        return pushArrayElements(receiver(), value, type);
    case InlineBuiltin::ArrayWithLength: {
        Integer length;
        if (typeOf(arguments[0]).isInteger())
            length = get(arguments[0]).as<Integer>();
        else {
            // What is no integer is no length, as little as what is negative.
            Number number = in<Number>(arguments[0]);
            length = select(isIntegral(number), truncateToInteger(number), integer(-1));
        }
        LBasicBlock slowPath = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock fast = m_out.anchor(allocateArrayWithHoles(length, indexingTypeAtBirth(arrayTypeAtBirth(result)), slowPath).value());
        m_out.jump(continuation);
        m_out.appendTo(slowPath);
        ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(NewArrayWithHoles, CellPointer(m_out.int64Zero), length, word32(indexingTypeAtBirth(arrayTypeAtBirth(result))), word32(false)).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return yieldsNew(CellPointer(m_out.phi(Int64, fast, slow)));
    }
    case InlineBuiltin::ArrayOf:
        return yieldsNew(newArrayOfType(arguments, expectedTypeOf(result)));
    case InlineBuiltin::ArrayIsArray: {
        static_assert(DerivedArrayType == ArrayType + 1);
        ValueID argument = arguments[0];
        if (!typeOf(argument).hasObjects())
            return ensure(boolean(false), booleanType, type);
        LBasicBlock isCellCase = m_out.newBlock();
        LBasicBlock isArray = m_out.newBlock();
        LBasicBlock isOtherCell = m_out.newBlock();
        LBasicBlock isNotArray = m_out.newBlock();
        LBasicBlock isProxy = newBlockForGeneralCall(value);
        Join join(m_out, Rep::Int32, Int32);
        Boxed boxed = in<Boxed>(argument);
        m_out.branch(isCell(boxed).value(), unsure(isCellCase), unsure(isNotArray));
        m_out.appendTo(isCellCase);
        Word32 cellType = typeOfCell(asCell(boxed));
        m_out.branch(below(binary<Word32>(&Output::sub, cellType, word32(ArrayType)), word32(2)).value(), unsure(isArray), unsure(isOtherCell));
        m_out.appendTo(isArray);
        join.add(boolean(true));
        m_out.appendTo(isOtherCell);
        m_out.branch(equal(cellType, word32(ProxyObjectType)).value(), rarely(isProxy), usually(isNotArray));
        m_out.appendTo(isNotArray);
        join.add(boolean(false));
        // It is what its target is. That runs no code of the program, but there is no target once the proxy has been revoked: that throws.
        if (!isGeneralArmOnOffer(isProxy)) {
            m_out.appendTo(isProxy);
            join.add(unboxedBoolean(callBuiltinFunction(value, OperationEffects { inPlace, true })));
        }
        return ensure(join.finish(), booleanType, type);
    }

    case InlineBuiltin::TypedArrayWithLength:
        return yieldsNew(AOT_TYPED_OPERATION(NewTypedArray, word32(m_builtins.classes()[value.engineClass()].typedArrayType), in<Number>(arguments[0])));
    case InlineBuiltin::TypedArrayLength: {
        TypedArrayReceiver view = typedArrayReceiver(receiver());
        Type lengthType = Type::integer({ 0, static_cast<int64_t>(MAX_ARRAY_BUFFER_SIZE) });
        return ensure(either<Integer>(hasLengthOfItsOwn(view), [&] {
            return loadField<Integer>(view.view, m_heaps.JSArrayBufferView_length, view.flags);
        }, [&] {
            return convertTo<Integer>(callBuiltinFunction(value, OperationEffects { inPlace, true }), lengthType);
        }), lengthType, type);
    }

    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
