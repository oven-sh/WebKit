/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "JSCInlines.h"
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

static constexpr uint16_t cellPrimitives = Primitive::String | Primitive::Symbol | Primitive::BigInt;

static Type onlyCellsOf(const Type& type)
{
    Type result = type;
    result.primitives &= cellPrimitives;
    result.integers = Range::empty();
    return result;
}

static Type withoutCells(const Type& type)
{
    Type result;
    result.primitives = type.primitives & ~cellPrimitives;
    if (result.primitives & Primitive::Int)
        result.integers = type.integers;
    return result;
}

Word32 Lowering::typeOfCell(CellPointer cell)
{
    return loadByte(cell, m_heaps.JSCell_typeInfoType, IsFixed | safeIfReaches(cell, sizeof(JSCell)));
}

Word32 Lowering::shapeWordOf(CellPointer cell)
{
    static_assert(offsetOfShapeWord + sizeof(uint32_t) <= MarkedBlock::atomSize);
    // Where no cast turns an object into one of another layout, it is of its shape for good.
    unsigned flags = safeIfReaches(cell, offsetOfShapeWord + sizeof(uint32_t)) | (m_program.castsMayRetag ? 0 : IsFixed);
    return loadField<Word32>(cell, m_heaps.AOTTypedObject_shapeWord, flags);
}

Word32 Lowering::orderWordOf(CellPointer cell)
{
    return loadField<Word32>(cell, m_heaps.AOTTypedObject_orderWord, safeIfReaches(cell, offsetOfOrderWord + sizeof(uint32_t)));
}

// See ShapeTest. With a block: what is out of the reach of a set goes there, and what is yielded is what is left to test. Without: a value
// that depends on nothing but the word.
Word32 Lowering::passes(Word32 shapeWord, const ShapeTest& test, LBasicBlock otherwise)
{
    if (test.passesNothing())
        return boolean(false);
    Word32 place = add(shapeWord, word32(test.last()));
    if (test.isRange())
        return test.count == 1 ? isZero(place) : below(place, word32(test.count));

    Word32 isWithinReach = below(place, word32(test.count));
    if (otherwise)
        orElse(isWithinReach, otherwise);
    // The back end takes the amount of a shift modulo the width.
    Word32 hasBit;
    if (test.count <= 64) {
        uint64_t bits = 0;
        for (uint32_t i = 0; i < test.count; ++i)
            bits |= static_cast<uint64_t>(m_program.shapeSetBits.get(test.set + i)) << i;
        if (test.count <= 32)
            hasBit = hasBits(shiftRightUnsigned(word32(static_cast<uint32_t>(bits)), place), 1);
        else
            hasBit = hasBits(low32(Integer(m_out.lShr(integer(static_cast<int64_t>(bits)).value(), place.value()))), 1);
    } else {
        // Only the comparison keeps the load within the row of bits.
        Word32 index = otherwise ? place : select(isWithinReach, place, word32(0));
        Pointer words(m_out.add(fixedPointer(Instance::offsetOfTypedShapeSets()), m_out.constIntPtr(test.set / 8)));
        Integer wordIndex = zeroExtended(shiftRightUnsigned(index, word32(5)));
        Word32 word(marked(m_out.load32(TypedPointer(m_heaps.AOTTyped_shapeSets, offsetBy(words, wordIndex, 2).value())), otherwise ? 0 : IsSafe));
        hasBit = hasBits(shiftRightUnsigned(word, index), 1);
    }
    return otherwise ? hasBit : logicalAnd(isWithinReach, hasBit);
}

// Whether the object is of a member of one of the views.
Word32 Lowering::passesOneOf(Word32 shapeWord, const Vector<LayoutID, 1>& views, LBasicBlock otherwise)
{
    Vector<ShapeTest, 4> tests;
    for (LayoutID view : views) {
        if (!m_program.at(view).test.passesNothing())
            tests.append(m_program.at(view).test);
    }
    // Placement tries to number the members of views that are tested together in a row.
    std::ranges::sort(tests, { }, &ShapeTest::first);
    Vector<ShapeTest, 4> merged;
    for (auto& test : tests) {
        if (!merged.isEmpty() && merged.last().isRange() && test.isRange() && test.first <= merged.last().first + merged.last().count) {
            merged.last().count = std::max(merged.last().count, test.first + test.count - merged.last().first);
            continue;
        }
        merged.append(test);
    }
    if (merged.size() == 1)
        return passes(shapeWord, merged[0], otherwise);
    Word32 result;
    for (auto& test : merged)
        result = logicalOr(result, passes(shapeWord, test));
    return result ? result : boolean(false);
}

// immediates[1]: the object was allocated with the layout itself. Otherwise: with a member of it.
Word32 Lowering::lowerHasShapeOf(const Value& value)
{
    auto& layout = m_program.at(value.layout());
    auto& type = typeOf(value.child(0));
    Operand operand = get(value.child(0));
    auto test = [&](CellPointer cell) {
        Word32 shapeWord = shapeWordOf(cell);
        Word32 result = passes(shapeWord, value.immediates[1] ? ShapeTest { layout.shape, layout.shape ? 1u : 0u } : layout.test);
        // Where an object has its shape word, a bigint has a hash.
        if (type.primitives & Primitive::BigInt)
            result = logicalAnd(result, notEqual(typeOfCell(cell), word32(HeapBigIntType)));
        return result;
    };
    if (operand.rep == Rep::Cell)
        return test(operand.as<CellPointer>());
    if (operand.rep != Rep::JSValue)
        return boolean(false);
    Boxed boxed = operand.as<Boxed>();
    return either<Word32>(isCell(boxed), [&] {
        return test(asCell(boxed));
    }, [&] {
        return boolean(false);
    });
}

// Whether the cell is the function as a value.
Word32 Lowering::isClosureOf(CellPointer cell, FunctionID function)
{
    // Only a function with a general entry is ever made a value of.
    auto entrypoint = m_program.genericEntrypoints.find(function.index());
    if (entrypoint == m_program.genericEntrypoints.end())
        return boolean(false);
    uint32_t general = entrypoint->value.index();
    // A function that captures nothing is one object, if it has been asked for yet.
    if (!m_program.at(function).environment)
        return equal(cell, CellPointer(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(Instance::offsetOfTypedClosures()), static_cast<ptrdiff_t>(general) * sizeof(void*)))));
    // A closure that was given properties has another Structure.
    Word32 isAsAllocated = equal(loadField<Word32>(cell, m_heaps.JSCell_structureID, safeIfReaches(cell, sizeof(JSCell))), Word32(loadRuntimeState32(Instance::offsetOfTypedClosureStructureID())));
    return either<Word32>(isAsAllocated, [&] {
        return equal(loadField<Word32>(cell, m_heaps.AOTTypedClosure_function), word32(general));
    }, [&] {
        return AOT_TYPED_OPERATION(IsClosureOf, cell, word32(general));
    });
}

Word32 Lowering::lowerIsClosureOf(const Value& value)
{
    Operand operand = get(value.child(0));
    if (operand.rep == Rep::Cell)
        return isClosureOf(operand.as<CellPointer>(), value.function());
    if (operand.rep != Rep::JSValue)
        return boolean(false);
    Boxed boxed = operand.as<Boxed>();
    return either<Word32>(isCell(boxed), [&] {
        return isClosureOf(asCell(boxed), value.function());
    }, [&] {
        return boolean(false);
    });
}

// As a rule what is called is a closure of the program or a function of the engine.
Word32 Lowering::isCallable(CellPointer cell)
{
    Word32 isClosure = equal(loadField<Word32>(cell, m_heaps.JSCell_structureID, safeIfReaches(cell, sizeof(JSCell))), Word32(loadRuntimeState32(Instance::offsetOfTypedClosureStructureID())));
    return either<Word32>(logicalOr(isClosure, equal(typeOfCell(cell), word32(JSFunctionType))), [&] {
        return boolean(true);
    }, [&] {
        return AOT_TYPED_OPERATION(IsCallable, cell);
    });
}

// Whether the cell, which is of type `from`, is of type `to`. What takes a test of its own to turn away has gone to the block.
Word32 Lowering::cellFits(CellPointer cell, const Type& from, const Type& to, LBasicBlock otherwise)
{
    Word32 loadedType;
    auto type = [&] {
        if (!loadedType)
            loadedType = typeOfCell(cell);
        return loadedType;
    };
    // See Layout::admitsForeignObjects.
    bool anyObjectFits = to.anyObject || (!m_foreignObjectsDoNotFit && to.admitsForeignObjects(m_program));
    bool mayBeAnyObject = from.anyObject || from.admitsForeignObjects(m_program);
    // Where an object has its shape word, a bigint has a hash.
    bool mayTakeBigIntForObject = from.primitives & Primitive::BigInt && !to.views.isEmpty() && !anyObjectFits;
    if (mayTakeBigIntForObject && !(to.primitives & Primitive::BigInt)) {
        orElse(notEqual(type(), word32(HeapBigIntType)), otherwise);
        mayTakeBigIntForObject = false;
    }
    Word32 result;
    static constexpr std::pair<uint16_t, JSType> primitives[] = { { Primitive::String, StringType }, { Primitive::Symbol, SymbolType }, { Primitive::BigInt, HeapBigIntType } };
    for (auto [primitive, itsType] : primitives) {
        if (from.primitives & to.primitives & primitive)
            result = logicalOr(result, equal(type(), word32(itsType)));
    }
    if (!from.hasObjects() || !to.hasObjects())
        return result ? result : boolean(false);
    if (anyObjectFits)
        return logicalOr(result, aboveOrEqual(type(), word32(ObjectType)));

    if (to.anyFunction && (from.anyFunction || mayBeAnyObject))
        result = logicalOr(result, isCallable(cell));
    for (uint16_t index : to.engineClasses) {
        if (!mayBeAnyObject && !from.engineClasses.contains(index))
            continue;
        JSType itsType = m_builtins.classes()[index].type;
        result = logicalOr(result, equal(type(), word32(itsType)));
        if (itsType != ArrayType)
            continue;
        // See EngineClass::type. What is an array or one of a few other things for a fact is only told from those.
        if (m_isValidatingFact)
            result = logicalOr(result, isChangedArray(cell));
        else if (!mayBeAnyObject)
            result = logicalOr(result, equal(type(), word32(DerivedArrayType)));
    }
    if (!to.views.isEmpty() && (from.anyObject || !from.views.isEmpty())) {
        // Where nothing else will do, what the first half of a test turns away is done with.
        Word32 isOfAView = passesOneOf(shapeWordOf(cell), to.views, result ? nullptr : otherwise);
        if (mayTakeBigIntForObject)
            isOfAView = logicalAnd(isOfAView, notEqual(type(), word32(HeapBigIntType)));
        result = logicalOr(result, isOfAView);
    }
    return result ? result : boolean(false);
}

// Whether the value, which is of type `from`, is of type `to` and no cell.
Word32 Lowering::nonCellFits(Boxed value, const Type& from, const Type& to)
{
    auto is = [&](JSValue constant) {
        return equal(value, boxedConstant(constant));
    };
    uint16_t both = from.primitives & to.primitives;
    Word32 result;
    switch (both & Primitive::UndefinedOrNull) {
    case Primitive::UndefinedOrNull:
        result = logicalOr(result, isUndefinedOrNull(value));
        break;
    case Primitive::Undefined:
        result = logicalOr(result, is(jsUndefined()));
        break;
    case Primitive::Null:
        result = logicalOr(result, is(jsNull()));
        break;
    default:
        break;
    }
    switch (both & Primitive::Boolean) {
    case Primitive::Boolean:
        result = logicalOr(result, isBoolean(value));
        break;
    case Primitive::True:
        result = logicalOr(result, is(jsBoolean(true)));
        break;
    case Primitive::False:
        result = logicalOr(result, is(jsBoolean(false)));
        break;
    default:
        break;
    }
    if (from.primitives & Primitive::Number && to.primitives & Primitive::Double)
        result = logicalOr(result, isNumber(value));
    else if (from.primitives & Primitive::Number && to.primitives & Primitive::Int) {
        // The engine may keep an integral number as a double.
        result = logicalOr(result, either<Word32>(isInt32(value), [&] {
            if (Range::int32().isWithin(to.integers))
                return boolean(true);
            return isWithin(signExtended(low32(value)), to.integers);
        }, [&] {
            return either<Word32>(isNumber(value), [&] {
                Number number = unboxedDouble(value);
                Integer truncated = truncateToInteger(number);
                return logicalAnd(equal(bitsOf(convertToNumber(truncated)), bitsOf(number)), isWithin(truncated, to.integers));
            }, [&] {
                return boolean(false);
            });
        }));
    }
    return result ? result : boolean(false);
}

// The number as an integer of the range.
Integer Lowering::integralOrElse(Number number, Range range, LBasicBlock otherwise)
{
    Integer truncated = truncateToInteger(number);
    // By the bits: negative zero is no integer, nor is what is not a number.
    orElse(equal(bitsOf(convertToNumber(truncated)), bitsOf(number)), otherwise);
    orElse(isWithin(truncated, range), otherwise);
    return truncated;
}

// To a type that is kept as an integer, with or without room for `undefined` and `null`.
Operand Lowering::fitBoxedToIntegerOrElse(Boxed value, const Type& from, const Type& to, LBasicBlock otherwise)
{
    Rep rep = repOf(to);
    LBasicBlock int32Case = m_out.newBlock();
    LBasicBlock otherCase = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 4> results;
    m_out.branch(isInt32(value).value(), usually(int32Case), rarely(otherCase));

    m_out.appendTo(int32Case);
    Integer small = signExtended(low32(value));
    if (!Range::int32().isWithin(to.integers))
        orElse(isWithin(small, to.integers), otherwise);
    results.append(m_out.anchor(small.value()));
    m_out.jump(continuation);

    m_out.appendTo(otherCase);
    for (uint16_t primitive : { Primitive::Undefined, Primitive::Null }) {
        if (!(from.primitives & to.primitives & primitive))
            continue;
        LBasicBlock isIt = newColdBlock();
        LBasicBlock isNot = newColdBlock();
        m_out.branch(equal(value, boxedConstant(primitive == Primitive::Undefined ? jsUndefined() : jsNull())).value(), unsure(isIt), unsure(isNot));
        m_out.appendTo(isIt);
        results.append(m_out.anchor(integer(bitsOfMarker(primitive, rep)).value()));
        m_out.jump(continuation);
        m_out.appendTo(isNot);
    }
    orElse(isNumber(value), otherwise);
    results.append(m_out.anchor(integralOrElse(unboxedDouble(value), to.integers, otherwise).value()));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return Operand(rep, m_out.phi(Int64, results));
}

Operand Lowering::fitBoxedOrElse(Boxed value, const Type& from, const Type& to, LBasicBlock otherwise)
{
    Rep rep = repOf(to);
    if (rep == Rep::Int64 || rep == Rep::OptionalInt64)
        return fitBoxedToIntegerOrElse(value, from, to, otherwise);

    if (!from.hasCells() || !to.hasCells()) {
        orElse(nonCellFits(value, from, to), otherwise);
        return convert(value, to, rep);
    }

    // What the value may be that is no cell.
    uint16_t others = from.primitives & ~cellPrimitives;
    bool othersFit = withoutCells(from).isWithin(withoutCells(to), m_program);
    CellPointer cell = asCell(value);
    if (onlyCellsOf(from).isWithin(onlyCellsOf(to), m_program, !m_foreignObjectsDoNotFit)) {
        uint16_t gone = others & ~to.primitives;
        Word32 isOne;
        if (!(others & to.primitives))
            isOne = isCell(value);
        else if (!(gone & ~Primitive::UndefinedOrNull) && withoutCells(from).withoutPrimitives(Primitive::UndefinedOrNull).isWithin(withoutCells(to), m_program)) {
            if (gone == Primitive::Undefined)
                isOne = notEqual(value, undefined());
            else if (gone == Primitive::Null)
                isOne = notEqual(value, boxedConstant(jsNull()));
            else
                isOne = logicalNot(isUndefinedOrNull(value));
        } else
            isOne = logicalOr(isCell(value), nonCellFits(value, from, to));
        orElse(isOne, otherwise);
        return convert(value, to, rep);
    }

    if (!others) {
        orElse(cellFits(cell, from, to, otherwise), otherwise);
        return convert(value, to, rep);
    }
    if (!(others & to.primitives)) {
        orElse(isCell(value), otherwise);
        orElse(cellFits(cell, from, to, otherwise), otherwise);
        return convert(value, to, rep);
    }

    // `undefined` and `null` pass for a reference. That is the one way around the test of the cell.
    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    LBasicBlock otherCase = othersFit ? continuation : newColdBlock();
    m_out.branch(isCell(value).value(), usually(cellCase), rarely(otherCase));
    m_out.appendTo(cellCase);
    m_out.branch(cellFits(cell, from, to, otherwise).value(), usually(continuation), rarely(otherwise));
    if (!othersFit) {
        m_out.appendTo(otherCase);
        m_out.branch(nonCellFits(value, from, to).value(), usually(continuation), rarely(otherwise));
    }
    m_out.appendTo(continuation);
    return convert(value, to, rep);
}

Operand Lowering::fitOrElse(Operand operand, const Type& from, const Type& to, LBasicBlock otherwise)
{
    Rep rep = repOf(to);
    if (from.isWithin(to, m_program, !m_foreignObjectsDoNotFit))
        return convert(operand, from, rep);
    // Nothing of the one type is of the other. What follows is not reached.
    auto never = [&] {
        m_out.jump(otherwise);
        m_out.appendTo(m_out.newBlock());
        switch (b3TypeOf(rep).kind()) {
        case Int32:
            return Operand(rep, m_out.int32Zero);
        case Double:
            return Operand(rep, m_out.doubleZero);
        default:
            return Operand(rep, m_out.int64Zero);
        }
    };
    operand = convert(operand, from, repOf(from));
    switch (operand.rep) {
    case Rep::JSValue:
        return fitBoxedOrElse(operand.as<Boxed>(), from, to, otherwise);
    case Rep::Cell:
        if (!to.hasCells())
            return never();
        orElse(cellFits(operand.as<CellPointer>(), from, to, otherwise), otherwise);
        return convert(operand, to, rep);
    case Rep::Double:
        if (!(to.primitives & Primitive::Int))
            return never();
        return convert(integralOrElse(operand.as<Number>(), to.integers, otherwise), to, rep);
    case Rep::Int64:
        if (!(to.primitives & Primitive::Int))
            return never();
        orElse(isWithin(operand.as<Integer>(), to.integers), otherwise);
        return convert(operand, to, rep);
    case Rep::Int32:
        if (!(to.primitives & Primitive::Boolean))
            return never();
        orElse(equal(operand.as<Word32>(), boolean(to.primitives & Primitive::True)), otherwise);
        return convert(operand, to, rep);
    case Rep::OptionalInt64:
    case Rep::OptionalDouble:
        break;
    case Rep::Void:
        RELEASE_ASSERT_NOT_REACHED();
    }

    // A number, or what stands for `undefined` or `null`.
    bool isInteger = operand.rep == Rep::OptionalInt64;
    Operand number(isInteger ? Rep::Int64 : Rep::Double, operand.value);
    Type numbers = from.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent);
    if (!(from.primitives & to.primitives & Primitive::UndefinedOrNull)) {
        orElse(logicalNot(isMarker(operand)), otherwise);
        return fitOrElse(number, numbers, to, otherwise);
    }
    LBasicBlock numberCase = m_out.newBlock();
    LBasicBlock markerCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;
    m_out.branch(isMarker(operand).value(), unsure(markerCase), unsure(numberCase));
    m_out.appendTo(numberCase);
    results.append(m_out.anchor(fitOrElse(number, numbers, to, otherwise).value));
    m_out.jump(continuation);
    m_out.appendTo(markerCase);
    Integer bits = bitsOf(operand);
    for (uint16_t primitive : { Primitive::Undefined, Primitive::Null }) {
        if (!(from.primitives & to.primitives & primitive))
            continue;
        LBasicBlock isIt = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        m_out.branch(equal(bits, integer(bitsOfMarker(primitive, operand.rep))).value(), unsure(isIt), unsure(isNot));
        m_out.appendTo(isIt);
        results.append(m_out.anchor(convert(boxedConstant(primitive == Primitive::Undefined ? jsUndefined() : jsNull()), Type::ofPrimitives(primitive), rep).value));
        m_out.jump(continuation);
        m_out.appendTo(isNot);
    }
    m_out.jump(otherwise);
    m_out.appendTo(continuation);
    return Operand(rep, m_out.phi(b3TypeOf(rep), results));
}

Word32 Lowering::fits(Operand operand, const Type& from, const Type& to)
{
    if (from.isWithin(to, m_program))
        return boolean(true);
    LBasicBlock doesNot = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    fitOrElse(operand, from, to, doesNot);
    ValueFromBlock yes = m_out.anchor(m_out.booleanTrue);
    m_out.jump(continuation);
    m_out.appendTo(doesNot);
    ValueFromBlock no = m_out.anchor(m_out.booleanFalse);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return Word32(m_out.phi(Int32, yes, no));
}

void Lowering::throwTypeMismatch(Operand operand, const Type& from, const Type& to)
{
    uint32_t kinds = to.primitives & Primitive::All;
    if (to.anyObject || !to.views.isEmpty() || !to.engineClasses.isEmpty())
        kinds |= typeAllowsObjects;
    if (to.anyFunction)
        kinds |= typeAllowsFunctions;
    StringID name;
    if (!to.views.isEmpty())
        name = m_program.at(to.views[0]).name;
    else if (!to.engineClasses.isEmpty())
        name = m_program.stringOf(m_builtins.classes()[to.engineClasses[0]].name);
    uint32_t position = m_function.at(m_currentBlock->values[m_currentIndexInBlock]).position;
    AOT_TYPED_OPERATION(ThrowTypeMismatch, convertTo<Boxed>(operand, from), word32(kinds), word32(name.index()), word32(m_id.index()), word32(position));
    m_out.unreachable();
}

Operand Lowering::ensure(Operand operand, const Type& guarantee, const Type& type)
{
    // `never` says nothing.
    if (&guarantee == &type || type.isNever() || guarantee.isWithin(type, m_program))
        return convert(operand, guarantee, repOf(type));
    LBasicBlock mismatch = newColdBlock();
    Operand result = fitOrElse(operand, guarantee, type, mismatch);
    LBasicBlock continuation = m_out.m_block;
    m_out.appendTo(mismatch);
    throwTypeMismatch(operand, guarantee, type);
    m_out.appendTo(continuation);
    return result;
}

void Lowering::validateFact(Operand operand, ValueID id, const Type& type)
{
    if (type.isNever())
        return;
    // An environment and the frame of a coroutine have no shape word to look at.
    for (LayoutID view : type.views) {
        if (!isObjectLayout(m_program.at(view).kind))
            return;
    }
    // All that the representation can hold, so that nothing is taken for granted.
    Type held;
    switch (operand.rep) {
    case Rep::Void:
        return;
    case Rep::Int32:
        held = Type::ofPrimitives(Primitive::Boolean);
        break;
    case Rep::Int64:
    case Rep::OptionalInt64:
        held = Type::integer({ -maxKeptInteger, maxKeptInteger });
        break;
    case Rep::Double:
    case Rep::OptionalDouble:
        held = Type::ofPrimitives(Primitive::Number);
        break;
    case Rep::Cell:
        held = Type::object();
        held.anyFunction = true;
        held.primitives |= Primitive::String | Primitive::Symbol | Primitive::BigInt;
        break;
    case Rep::JSValue:
        held = Type::anything();
        break;
    }
    if (operand.rep == Rep::OptionalInt64 || operand.rep == Rep::OptionalDouble)
        held.primitives |= Primitive::UndefinedOrNull | Primitive::Absent;
    if (held.isWithin(type, m_program))
        return;
    LBasicBlock doesNotHold = newColdBlock();
    {
        SetForScope isValidatingFact(m_isValidatingFact, true);
        fitOrElse(operand, held, type, doesNotHold);
    }
    LBasicBlock continuation = m_out.m_block;
    m_out.appendTo(doesNotHold);
    AOT_TYPED_OPERATION(FactDoesNotHold, convertTo<Boxed>(operand, held), word32(m_id.index()), word32(id.index()));
    m_out.unreachable();
    m_out.appendTo(continuation);
}

// A property of `undefined` or `null` was asked for.
void Lowering::throwNullish(Boxed value, StringID name)
{
    AOT_TYPED_OPERATION(ThrowNullish, value, word32(name.index()));
    m_out.unreachable();
}

CellPointer Lowering::nonNil(Operand operand, const Type& type, StringID name)
{
    RELEASE_ASSERT(!(type.primitives & ~(cellPrimitives | Primitive::UndefinedOrNull)));
    if (operand.rep == Rep::Cell)
        return operand.as<CellPointer>();
    Boxed value = operand.as<Boxed>();
    if (!type.mayBeUndefinedOrNull())
        return asCell(value);
    LBasicBlock isNil = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(value).value(), usually(continuation), rarely(isNil));
    m_out.appendTo(isNil);
    throwNullish(value, name);
    m_out.appendTo(continuation);
    return asCell(value);
}

// Whether what the elements of an array of the type are expected to be is objects of the program, however deep.
static bool expectsViewsInElements(const Program& program, const Type& type)
{
    for (TypeID elements = type.elements; elements; elements = program.at(elements).elements) {
        if (!program.at(elements).views.isEmpty())
            return true;
    }
    return false;
}

// What enters a place whose type is written down. A value that does not fit is for the run time to turn into one that does, or to turn
// away. Also where a value is narrowed to a type that casts turn objects into where they are: see narrowingMayRetag(). That looks at
// the object alone.
Operand Lowering::coerce(Operand operand, const Type& from, const Value& cast)
{
    auto& to = m_program.at(cast.type);
    Rep rep = repOf(to);
    bool isNarrowing = cast.castReason() == CastReason::Flow;
    // That it is an array says nothing of its elements, which are turned into what they are expected to be where they are.
    uint16_t arrayClass = m_builtins.arrayClass().index;
    bool looksIntoArrays = !isNarrowing && to.engineClasses.contains(arrayClass) && !from.engineClasses.contains(arrayClass) && expectsViewsInElements(m_program, to);
    // Only an object is ever turned into anything.
    if (!from.hasObjects() || (to.views.isEmpty() && !looksIntoArrays) || (!looksIntoArrays && m_program.isHeldAlready(from, to, cast.castReason())))
        return ensure(operand, from, to);

    Type passing = to;
    if (looksIntoArrays) {
        passing.engineClasses.removeFirst(arrayClass);
        passing.elements = { };
    }
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    // Where the type admits foreign objects, only an object of a member is done with here. Any other is for the run time, though it is of
    // the type as it is: that lays out what a plain object refers to, and hands back the object itself. See Coercion in AOTTypedObject.cpp.
    Operand fitted;
    {
        SetForScope isCoerced(m_foreignObjectsDoNotFit, !isNarrowing);
        fitted = fitOrElse(operand, from, passing, slowPath);
    }
    ValueFromBlock fast = m_out.anchor(convert(fitted, passing, rep).value);
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    OperationEffects effects { writesOfCast(m_program, m_function, cast), true, castWritesShapeWords(m_program, m_function, cast) };
    Boxed coerced = AOT_TYPED_OPERATION_WITH_EFFECTS(Coerce, effects, convertTo<Boxed>(operand, from), word32(cast.type.index()), word32(static_cast<uint32_t>(cast.castReason())), word32(m_id.index()), word32(cast.position));
    ValueFromBlock slow = m_out.anchor(convert(coerced, to, rep).value);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return Operand(rep, m_out.phi(b3TypeOf(rep), fast, slow));
}

// Whether an object of the type can be of a shape that passes.
bool Lowering::mayPass(const Type& type, const ShapeTest& test) const
{
    if (test.passesNothing())
        return false;
    if (type.anyObject || type.admitsForeignObjects(m_program))
        return true;
    for (LayoutID view : type.views) {
        for (LayoutID member : m_program.at(view).members) {
            if (m_program.passes(test, m_program.at(member).shape))
                return true;
        }
    }
    return false;
}

// `x as T`, see Format::CastReason::Coerce. As a rule the object is not of T and nothing can be made of it: people cast first and look
// afterwards. That costs no call. Only these go to the run time: a value of a kind T has not got, to be converted; an object of a
// layout that is turned into a member of T where it is (Layout::retagSources); where the operand may be any object, an ordinary
// object, which may be plain data, and an array whose elements may be.
Operand Lowering::lowerCoercion(ValueID result, const Value& cast)
{
    Operand operand = get(cast.child(0));
    auto& from = typeOf(cast.child(0));
    auto& type = typeOf(result);
    auto& to = m_program.at(cast.coercedType());
    Rep rep = repOf(type);
    bool mayChangeObjects = holdsCells(rep) && coercionMayChangeObjects(m_program, from, to);
    bool mayBeAnyObject = from.anyObject || from.admitsForeignObjects(m_program);
    bool mayCopy = mayChangeObjects && mayBeAnyObject && !to.views.isEmpty();
    bool looksIntoArrays = mayChangeObjects && mayBeAnyObject && to.engineClasses.contains(m_builtins.arrayClass().index) && expectsViewsInElements(m_program, to);
    Vector<ShapeTest, 4> sources;
    for (LayoutID view : to.views) {
        if (mayChangeObjects && mayPass(from, m_program.at(view).retagSources))
            sources.append(m_program.at(view).retagSources);
    }
    bool reports = mayChangeObjects && Options::reportAOTTypedCoercions();
    bool asksObjects = reports || !sources.isEmpty() || mayCopy || looksIntoArrays;
    if (!asksObjects && from.isWithin(type, m_program))
        return convert(operand, from, rep);

    LBasicBlock slowPath = newColdBlock();
    LBasicBlock asItIs = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    // Every object is of the type of the cast, if T has objects at all.
    Operand fitted = fitOrElse(operand, from, type, slowPath);
    if (reports) [[unlikely]] {
        SetForScope isStrict(m_foreignObjectsDoNotFit, true);
        fitOrElse(fitted, type, to, slowPath);
        m_out.jump(asItIs);
    } else if (!asksObjects)
        m_out.jump(asItIs);
    else {
        CellPointer cell;
        if (rep == Rep::Cell)
            cell = fitted.as<CellPointer>();
        else {
            LBasicBlock cellCase = m_out.newBlock();
            m_out.branch(isCell(fitted.as<Boxed>()).value(), usually(cellCase), rarely(asItIs));
            m_out.appendTo(cellCase);
            cell = asCell(fitted.as<Boxed>());
        }
        Word32 isForRunTime;
        if (!sources.isEmpty()) {
            Word32 shapeWord = shapeWordOf(cell);
            // Where that is all there is to ask, a shape out of the reach of a set is done with at the first comparison.
            bool isOnlyTest = sources.size() == 1 && !mayCopy && !looksIntoArrays;
            for (auto& test : sources)
                isForRunTime = logicalOr(isForRunTime, passes(shapeWord, test, isOnlyTest ? asItIs : nullptr));
            // Where an object has its shape word, a bigint has a hash.
            if (type.primitives & Primitive::BigInt)
                isForRunTime = logicalAnd(isForRunTime, notEqual(typeOfCell(cell), word32(HeapBigIntType)));
        }
        if (mayCopy)
            isForRunTime = logicalOr(isForRunTime, equal(typeOfCell(cell), word32(FinalObjectType)));
        if (looksIntoArrays)
            isForRunTime = logicalOr(isForRunTime, equal(typeOfCell(cell), word32(ArrayType)));
        m_out.branch(isForRunTime.value(), rarely(slowPath), usually(asItIs));
    }

    m_out.appendTo(asItIs);
    ValueFromBlock fast = m_out.anchor(fitted.value);
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    OperationEffects effects { writesOfCast(m_program, m_function, cast), true, castWritesShapeWords(m_program, m_function, cast) };
    Boxed coerced = AOT_TYPED_OPERATION_WITH_EFFECTS(Coerce, effects, convertTo<Boxed>(operand, from), word32(cast.coercedType().index()), word32(static_cast<uint32_t>(CastReason::Coerce)), word32(m_id.index()), word32(cast.position));
    ValueFromBlock slow = m_out.anchor(convert(coerced, type, rep).value);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return Operand(rep, m_out.phi(b3TypeOf(rep), fast, slow));
}

// `x!`. Its type is without `undefined` and `null`, so the test for the type is the test for them. With
// Options::reportAOTTypedCoercions() the reader has put them back where there is room for them: they are reported and let through.
Operand Lowering::lowerNonNull(ValueID result, const Value& cast)
{
    Operand operand = get(cast.child(0));
    auto& from = typeOf(cast.child(0));
    auto& to = typeOf(result);
    if (!to.mayBeUndefinedOrNull() && from.isWithin(to, m_program))
        return convert(operand, from, repOf(to));
    auto fail = [&](bool goesOn) {
        AOT_TYPED_OPERATION(NonNullFailed, convertTo<Boxed>(operand, from), word32(cast.type.index()), word32(goesOn), word32(m_id.index()), word32(cast.position));
    };
    LBasicBlock mismatch = newColdBlock();
    Operand fitted = fitOrElse(operand, from, to, mismatch);
    if (to.mayBeUndefinedOrNull() && fitted.rep == Rep::JSValue) [[unlikely]] {
        LBasicBlock isNil = newColdBlock();
        LBasicBlock isNot = m_out.newBlock();
        m_out.branch(isUndefinedOrNull(fitted.as<Boxed>()).value(), rarely(isNil), usually(isNot));
        m_out.appendTo(isNil);
        fail(true);
        m_out.jump(isNot);
        m_out.appendTo(isNot);
    }
    LBasicBlock continuation = m_out.m_block;
    m_out.appendTo(mismatch);
    fail(false);
    m_out.unreachable();
    m_out.appendTo(continuation);
    return fitted;
}

Operand Lowering::lowerCast(ValueID result, const Value& value)
{
    Operand operand = get(value.child(0));
    auto& from = typeOf(value.child(0));
    auto& to = typeOf(result);
    switch (value.castReason()) {
    case CastReason::Upcast:
        return convert(operand, from, repOf(to));
    case CastReason::As:
        return coerce(operand, from, value);
    case CastReason::Coerce:
        return lowerCoercion(result, value);
    case CastReason::NonNull:
        return lowerNonNull(result, value);
    case CastReason::Flow:
        if (!value.string() && narrowingMayRetag(m_program, to))
            return coerce(operand, from, value);
        break;
    case CastReason::Store:
    case CastReason::Claim:
        break;
    }
    // The receiver of a call that was resolved, with the name of what is called: what is left of the read of the property. See resolveCalls() and resolveBuiltins().
    if (StringID name = value.string(); name && !(from.primitives & ~(cellPrimitives | Primitive::UndefinedOrNull)) && from.withoutPrimitives(Primitive::UndefinedOrNull).isWithin(to, m_program))
        return convert(nonNil(operand, from, name), to, repOf(to));
    return ensure(operand, from, to);
}

Word32 Lowering::lowerIsUndefinedOrNull(ValueID id)
{
    auto& type = typeOf(id);
    Operand operand = get(id);
    if (operand.rep == Rep::OptionalInt64 || operand.rep == Rep::OptionalDouble)
        return isMarker(operand);
    if (operand.rep != Rep::JSValue)
        return boolean(false);
    Boxed value = operand.as<Boxed>();
    // The test an access makes, so that one of the two is folded.
    if (!(type.primitives & ~(cellPrimitives | Primitive::UndefinedOrNull)))
        return logicalNot(isCell(value));
    switch (type.primitives & Primitive::UndefinedOrNull) {
    case Primitive::Undefined:
        return equal(value, undefined());
    case Primitive::Null:
        return equal(value, boxedConstant(jsNull()));
    default:
        return isUndefinedOrNull(value);
    }
}

Word32 Lowering::lowerToBoolean(ValueID id)
{
    auto& type = typeOf(id);
    Operand operand = get(id);
    // There is one empty string.
    auto isTruthy = [&](CellPointer cell) {
        return type.primitives & Primitive::String ? notEqual(cell, CellPointer(emptyString())) : boolean(true);
    };
    switch (operand.rep) {
    case Rep::Int32:
        return operand.as<Word32>();
    case Rep::Int64:
        return isNotZero(operand.as<Integer>());
    case Rep::Double:
        return greaterThan(abs(operand.as<Number>()), number(0));
    case Rep::OptionalDouble:
        // What stands for `undefined` and `null` is not a number, and so not greater than zero.
        return greaterThan(abs(assumedNumber(operand.as<OptionalNumber>())), number(0));
    case Rep::OptionalInt64:
        return logicalNot(logicalOr(isMarker(operand), isZero(assumedInteger(operand.as<OptionalInteger>()))));
    case Rep::Cell:
        if (!(type.primitives & Primitive::BigInt))
            return isTruthy(operand.as<CellPointer>());
        break;
    case Rep::JSValue:
        if (type.primitives & (Primitive::BigInt | Primitive::Number | Primitive::True))
            break;
        if (!(type.primitives & Primitive::String))
            return isCell(operand.as<Boxed>());
        return either<Word32>(isCell(operand.as<Boxed>()), [&] {
            return isTruthy(asCell(operand.as<Boxed>()));
        }, [&] {
            return boolean(false);
        });
    case Rep::Void:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return toBooleanGenerally(id);
}

bool mayWalkPrototypeChain(const Program& program, const Function& function, const Value& value)
{
    auto& type = program.typeOf(function, value.child(0));
    return type.anyObject || type.anyFunction || !type.engineClasses.isEmpty() || type.admitsForeignObjects(program) || program.at(value.layout()).instancesMayBecomeRecords;
}

// An object of the program keeps its prototype, so its shape says what it is an instance of. The chain of any other object is walked.
Word32 Lowering::lowerIsInstanceOfClass(const Value& value)
{
    ValueID object = value.child(0);
    auto& type = typeOf(object);
    Operand operand = get(object);
    if (!type.hasObjects() || !holdsCells(operand.rep))
        return boolean(false);
    bool hasOnlyObjectsOfTheProgram = !type.anyObject && !type.anyFunction && type.engineClasses.isEmpty() && !type.admitsForeignObjects(m_program);
    auto test = [&](CellPointer cell) {
        Word32 shapeWord = shapeWordOf(cell);
        // Where an object has its shape word, a bigint has a hash.
        auto ofAnObject = [&](Word32 isAmongThem) {
            return type.primitives & Primitive::BigInt ? logicalAnd(isAmongThem, notEqual(typeOfCell(cell), word32(HeapBigIntType))) : isAmongThem;
        };
        Word32 isInstance = ofAnObject(passes(shapeWord, m_program.at(value.layout()).instances));
        // Then a shape that is not of the class says nothing.
        if (m_program.at(value.layout()).instancesMayBecomeRecords) {
            return either<Word32>(isInstance, [&] {
                return boolean(true);
            }, [&] {
                return AOT_TYPED_OPERATION(InheritsFromClass, cell, word32(value.layout().index()));
            });
        }
        if (hasOnlyObjectsOfTheProgram)
            return isInstance;
        Word32 hasShape = ofAnObject(passes(shapeWord, ShapeTest { 1, m_program.numberOfShapes }));
        return either<Word32>(hasShape, [&] {
            return isInstance;
        }, [&] {
            return AOT_TYPED_OPERATION(InheritsFromClass, cell, word32(value.layout().index()));
        });
    };
    if (operand.rep == Rep::Cell)
        return test(operand.as<CellPointer>());
    Boxed boxed = operand.as<Boxed>();
    return either<Word32>(isCell(boxed), [&] {
        return test(asCell(boxed));
    }, [&] {
        return boolean(false);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
