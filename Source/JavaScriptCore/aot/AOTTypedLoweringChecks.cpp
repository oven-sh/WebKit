/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "B3BasicBlockInlines.h"
#include "B3Dominators.h"
#include "B3InsertionSetInlines.h"
#include "B3MemoryValueInlines.h"
#include "B3StackmapGenerationParams.h"
#include "B3UseCounts.h"
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
    return passes(shapeWord, MergedShapeTest { test, !test.isRange() && test.count <= 64 ? std::optional(bitsOf(test)) : std::nullopt }, otherwise);
}

// Of a set that reaches over at most 64 shapes.
uint64_t Lowering::bitsOf(const ShapeTest& test) const
{
    RELEASE_ASSERT(!test.isRange() && test.count <= 64);
    uint64_t bits = 0;
    for (uint32_t i = 0; i < test.count; ++i)
        bits |= static_cast<uint64_t>(m_program.shapeSetBits.get(test.set + i)) << i;
    return bits;
}

Word32 Lowering::passes(Word32 shapeWord, const MergedShapeTest& merged, LBasicBlock otherwise)
{
    auto& test = merged.test;
    if (test.passesNothing())
        return boolean(false);
    if (test.isRange() && !merged.bits && test.count == 1)
        return equal(shapeWord, word32(0u - test.last()));
    Word32 place = add(shapeWord, word32(test.last()));
    if (test.isRange() && !merged.bits)
        return below(place, word32(test.count));

    Word32 isWithinReach = below(place, word32(test.count));
    if (otherwise)
        orElse(isWithinReach, otherwise);
    // The back end takes the amount of a shift modulo the width.
    Word32 hasBit;
    if (merged.bits) {
        uint64_t bits = *merged.bits;
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

// About how many instructions a test takes that is branched on.
static unsigned costOfRange(uint32_t count)
{
    return count == 1 ? 2 : 3;
}

static unsigned costOfBits(uint64_t bits)
{
    unsigned halfwords = 0;
    for (unsigned i = 0; i < 64; i += 16)
        halfwords += !!(bits >> i & 0xffff);
    return 5 + halfwords;
}

static unsigned costOf(const ShapeTest& test, const Program& program)
{
    if (test.isRange())
        return costOfRange(test.count);
    if (test.count > 64)
        return 9;
    uint64_t bits = 0;
    for (uint32_t i = 0; i < test.count; ++i)
        bits |= static_cast<uint64_t>(program.shapeSetBits.get(test.set + i)) << i;
    return costOfBits(bits);
}

// As few and as short as they get. Ranges that touch are one range: placement tries to number the members of views that are tested together in a row. Tests that lie within 64 shapes are
// one set, or one range, where that is shorter than one test after the other.
Vector<Lowering::MergedShapeTest, 4> Lowering::merged(Vector<ShapeTest, 4> given) const
{
    given.removeAllMatching([](auto& test) {
        return test.passesNothing();
    });
    std::ranges::sort(given, { }, &ShapeTest::first);
    Vector<ShapeTest, 4> tests;
    for (auto& test : given) {
        if (!tests.isEmpty() && tests.last().isRange() && test.isRange() && test.first <= tests.last().first + tests.last().count) {
            tests.last().count = std::max(tests.last().count, test.first + test.count - tests.last().first);
            continue;
        }
        tests.append(test);
    }
    Vector<MergedShapeTest, 4> result;
    for (size_t i = 0; i < tests.size();) {
        uint32_t first = tests[i].first;
        uint32_t last = tests[i].last();
        unsigned oneByOne = costOf(tests[i], m_program);
        size_t end = i + 1;
        for (; end < tests.size() && std::max(last, tests[end].last()) - first < 64; ++end) {
            last = std::max(last, tests[end].last());
            oneByOne += costOf(tests[end], m_program);
        }
        if (last - first < 64 && end - i > 1) {
            uint32_t count = last - first + 1;
            uint64_t bits = 0;
            for (uint32_t place = 0; place < count; ++place) {
                for (size_t k = i; k < end; ++k)
                    bits |= static_cast<uint64_t>(m_program.passes(tests[k], last - place)) << place;
            }
            // The first and the last shape of a test pass it.
            RELEASE_ASSERT(bits & 1 && bits >> (count - 1) == 1);
            bool isRange = !(bits & (bits + 1));
            if ((isRange ? costOfRange(count) : costOfBits(bits)) < oneByOne) {
                result.append(MergedShapeTest { ShapeTest { first, count }, isRange ? std::nullopt : std::optional(bits) });
                i = end;
                continue;
            }
        }
        result.append(MergedShapeTest { tests[i], !tests[i].isRange() && tests[i].count <= 64 ? std::optional(bitsOf(tests[i])) : std::nullopt });
        ++i;
    }
    return result;
}

Word32 Lowering::passesOneOf(Word32 shapeWord, const Vector<ShapeTest, 4>& tests, LBasicBlock otherwise)
{
    auto all = merged(tests);
    if (all.size() == 1)
        return passes(shapeWord, all[0], otherwise);
    Word32 result;
    for (auto& test : all)
        result = logicalOr(result, passes(shapeWord, test));
    return result ? result : boolean(false);
}

// Whether the object is of a member of one of the views.
Word32 Lowering::passesOneOf(Word32 shapeWord, const Vector<LayoutID, 1>& views, LBasicBlock otherwise)
{
    Vector<ShapeTest, 4> tests;
    for (LayoutID view : views)
        tests.append(m_useCastTest ? m_program.at(view).castTest : m_program.at(view).test);
    return passesOneOf(shapeWord, tests, otherwise);
}

// Each test branches: no answer is made into a value.
void Lowering::branchOnPassingOneOf(Word32 shapeWord, const Vector<ShapeTest, 4>& tests, FTL::WeightedTarget passing, FTL::WeightedTarget failing)
{
    auto all = merged(tests);
    if (all.isEmpty()) {
        m_out.jump(failing.target());
        return;
    }
    for (size_t i = 0; i < all.size(); ++i) {
        bool isLast = i + 1 == all.size();
        LBasicBlock next = isLast ? failing.target() : m_out.newBlock();
        m_out.branch(passes(shapeWord, all[i], next).value(), passing, FTL::WeightedTarget(next, failing.weight()));
        if (!isLast)
            m_out.appendTo(next);
    }
}

Word32 Lowering::lowerIsClassObjectAsDeclared(const Value& value)
{
    return isZero(Word32(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTInstance_mutableFields, fixedPointer(Instance::offsetOfTypedAlteredClassObjects()), value.layout().index()))));
}

void Lowering::accessStaticField(const Value& value, const Invocable<void(CellPointer classObject, TypedPointer slot)> auto& inPlace, const Invocable<void(Word32 ofClass, Word32 field)> auto& generally)
{
    auto& layout = m_program.at(value.layout());
    uint32_t place = layout.staticFields[value.immediates[1]].place;
    auto word = [&](uint32_t number) {
        return inStatics(m_heaps.AOTInstance_mutableFields, m_program.staticFieldPlacesOffset + number * sizeof(int32_t));
    };
    LBasicBlock isReady = m_out.newBlock();
    LBasicBlock asks = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue distance = m_out.load32(word(place));
    m_out.branch(m_out.notZero32(distance), usually(isReady), rarely(asks));
    m_out.appendTo(isReady);
    CellPointer classObject(m_out.load64(word(layout.classObjectPlace)));
    LValue butterfly = m_out.loadPtr(classObject.value(), m_heaps.JSObject_butterfly);
    inPlace(classObject, TypedPointer(m_heaps.properties.atAnyNumber(), m_out.add(butterfly, m_out.signExt32ToPtr(distance))));
    m_out.jump(continuation);
    m_out.appendTo(asks);
    generally(word32(value.layout().index()), word32(place - layout.classObjectPlace - 2));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

Operand Lowering::lowerGetStaticField(ValueID result, const Value& value)
{
    Rep rep = repOf(typeOf(result));
    Vector<ValueFromBlock, 2> found;
    accessStaticField(value, [&](CellPointer, TypedPointer slot) {
        found.append(m_out.anchor(ensure(Boxed(m_out.load64(slot)), typeOf(result)).value));
    }, [&](Word32 ofClass, Word32 field) {
        found.append(m_out.anchor(ensure(AOT_TYPED_OPERATION(GetStaticField, ofClass, field), typeOf(result)).value));
    });
    return Operand(rep, m_out.phi(b3TypeOf(rep), found));
}

void Lowering::lowerPutStaticField(const Value& value)
{
    Boxed stored = in<Boxed>(value.child(0));
    accessStaticField(value, [&](CellPointer classObject, TypedPointer slot) {
        didEmitWrite(Writes::Heap);
        m_out.store64(stored.value(), slot);
        if (typeOf(value.child(0)).hasCells())
            barrier(classObject);
    }, [&](Word32 ofClass, Word32 field) {
        AOT_TYPED_OPERATION(SetStaticField, ofClass, field, stored);
    });
}

// immediates[1]: the object was allocated with the layout itself. Otherwise: with a member of it.
Word32 Lowering::lowerHasShapeOf(const Value& value)
{
    auto& layout = m_program.at(value.layout());
    Operand operand = get(value.child(0));
    auto test = [&](CellPointer cell) {
        return passes(shapeWordOf(cell), value.immediates[1] ? ShapeTest { layout.shape, layout.shape ? 1u : 0u } : layout.test);
    };
    if (operand.rep == Rep::Cell)
        return test(operand.as<CellPointer>());
    if (operand.rep != Rep::JSValue)
        return boolean(false);
    Boxed boxed = operand.as<Boxed>();
    return either<Word32>(isCell(boxed, typeOf(value.child(0))), [&] {
        return test(asCell(boxed));
    }, [&] {
        return boolean(false);
    });
}

// Whether the cell is the function as a value.
Word32 Lowering::isClosureOf(CellPointer cell, FunctionID function, bool mustBeAsMade, bool evenIfAltered)
{
    // Only a function with a general entry is ever made a value of.
    auto entrypoint = m_program.genericEntrypoints.find(function.index());
    if (entrypoint == m_program.genericEntrypoints.end())
        return boolean(false);
    uint32_t general = entrypoint->value.index();
    // Which Structure its closures are born with. A class object has one of its own.
    ptrdiff_t structureID = offsetOfStructureIDOfClosuresOf(m_program.at(function));
    bool isSlim = m_program.makesSlimClosures(m_program.at(function));
    // One object, if it has been asked for yet.
    if (m_program.at(function).captureTypes.isEmpty() && !m_program.at(function).hasManyClosures) {
        Word32 isTheOne = isTheClosure(cell, general);
        if (!mustBeAsMade)
            return isTheOne;
        return either<Word32>(isTheOne, [&] {
            return equal(loadField<Word32>(cell, m_heaps.JSCell_structureID), Word32(loadRuntimeState32(structureID)));
        }, [&] {
            return boolean(false);
        });
    }
    Word32 isAsAllocated = equal(loadField<Word32>(cell, m_heaps.JSCell_structureID, safeIfReaches(cell, sizeof(JSCell))), Word32(loadRuntimeState32(structureID)));
    return either<Word32>(isAsAllocated, [&] {
        return equal(functionOfClosure(cell, isSlim), word32(general));
    }, [&] {
        // A closure that was given properties has another Structure. The test is a guard: it is taken for none, and goes the way that is right for every value. A class object has a Structure of its own from the start.
        if (mustBeAsMade || (m_program.at(function).kind != FunctionKind::Constructor && !evenIfAltered))
            return boolean(false);
        // Zero: it is no closure. The table has that too, for as long as there is no closure of the function.
        Integer entry(callAllocationStub(Stub::TypedClosureEntry, { { cell.value(), stubTemporaryGPRs[0] } }, { Writes::Nothing, false }, false));
        if (m_program.at(function).kind != FunctionKind::Constructor) {
            Word32 functionOfEntry(m_out.bitAnd(m_out.castToInt32(m_out.lShr(entry.value(), m_out.constInt32(ClosureWord::functionShift))), m_out.constInt32(static_cast<int32_t>(ClosureWord::functionMask))));
            return logicalAnd(logicalNot(isZero(entry)), equal(functionOfEntry, word32(general)));
        }
        Integer entryOfFunction(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(Instance::offsetOfTypedFunctions()), static_cast<ptrdiff_t>(general) * sizeof(void*))));
        return logicalAnd(logicalNot(isZero(entry)), equal(entry, entryOfFunction));
    });
}

// ClosureWord::functionOf().
ptrdiff_t Lowering::offsetOfStructureIDOfClosuresOf(const Function& function)
{
    if (m_program.makesSlimClosures(function))
        return Instance::offsetOfTypedSlimClosureHeader(slimClosureKindOf(static_cast<uint8_t>(function.kind) | function.declaredAs));
    return Instance::offsetOfTypedClosureStructureID(function.kind == FunctionKind::Constructor ? 0 : closureStructureOf(function.declaredAs));
}

Word32 Lowering::isUnreifiedSlimClosure(CellPointer cell)
{
    static_assert(SlimClosureWithPropertiesType == SlimClosureType + 1);
    return equal(typeOfCell(cell), word32(SlimClosureType));
}

Word32 Lowering::functionOfClosure(CellPointer closure, bool isSlim)
{
    LValue upperHalf = m_out.castToInt32(m_out.lShr(loadField<Integer>(closure, isSlim ? m_heaps.AOTTypedSlimClosure_word : m_heaps.AOTTypedClosure_entry).value(), m_out.constInt32(ClosureWord::functionShift)));
    return Word32(m_out.bitAnd(upperHalf, m_out.constInt32(static_cast<int32_t>(ClosureWord::functionMask))));
}

// Goes on where the cell is the function as a value. Elsewhere: to the block. Each test branches: no answer is made into a value.
void Lowering::closureOfOrElse(CellPointer cell, FunctionID function, bool mustBeAsMade, LBasicBlock otherwise, bool evenIfAltered)
{
    auto entrypoint = m_program.genericEntrypoints.find(function.index());
    auto& callee = m_program.at(function);
    if (entrypoint == m_program.genericEntrypoints.end() || (callee.captureTypes.isEmpty() && !callee.hasManyClosures) || callee.kind == FunctionKind::Constructor || evenIfAltered) {
        orElse(isClosureOf(cell, function, mustBeAsMade, evenIfAltered), otherwise);
        return;
    }
    orElse(equal(loadField<Word32>(cell, m_heaps.JSCell_structureID, safeIfReaches(cell, sizeof(JSCell))), Word32(loadRuntimeState32(offsetOfStructureIDOfClosuresOf(callee)))), otherwise);
    orElse(equal(functionOfClosure(cell, m_program.makesSlimClosures(callee)), word32(entrypoint->value.index())), otherwise);
}

// Ends the block.
void Lowering::branchOnClosureOf(const Value& value, LBasicBlock isOne, LBasicBlock isNone)
{
    Operand operand = get(value.child(0));
    if (operand.rep != Rep::Cell && operand.rep != Rep::JSValue) {
        m_out.jump(isNone);
        return;
    }
    if (operand.rep == Rep::JSValue)
        orElse(isCell(operand.as<Boxed>(), typeOf(value.child(0))), isNone);
    if (value.immediates[1] == closureIsTheOneThatConstructs) {
        orElse(isTheClosure(operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>()), m_program.genericEntrypoints.get(value.function().index()).index()), isNone);
        m_out.jump(isOne);
        return;
    }
    closureOfOrElse(operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>()), value.function(), value.immediates[1] == closureIsAsMade, isNone, value.immediates[1] == closureEvenIfAltered);
    m_out.jump(isOne);
}

// The tests for a closure that nothing looks at but the branch that ends their block.
void Lowering::findClosureTestsCarriedOutByBranches()
{
    BitVector isLookedAtOtherwise(m_function.values.size());
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            m_function.at(id).forEachChild([&](ValueID child) {
                isLookedAtOtherwise.set(child.index());
            });
        }
        for (auto& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                isLookedAtOtherwise.set(argument.index());
        }
        if (block.terminator.value && (block.terminator.kind != TerminatorKind::Branch || block.values.isEmpty() || block.values.last() != block.terminator.value))
            isLookedAtOtherwise.set(block.terminator.value.index());
    }
    for (auto& block : m_function.blocks) {
        if (block.terminator.kind != TerminatorKind::Branch || isLookedAtOtherwise.get(block.terminator.value.index()))
            continue;
        auto& condition = m_function.at(block.terminator.value);
        if (condition.opcode == Opcode::IsClosureOf || isAwaitMayContinue(condition) || isGuardOfReceiver(condition) || isTestOfForInStamp(condition))
            m_isCarriedOutByBranch.set(block.terminator.value.index());
    }
}

Word32 Lowering::lowerIsClosureOf(const Value& value)
{
    Operand operand = get(value.child(0));
    bool mustBeAsMade = value.immediates[1] == closureIsAsMade;
    bool evenIfAltered = value.immediates[1] == closureEvenIfAltered;
    auto test = [&](CellPointer cell) {
        if (value.immediates[1] == closureIsTheOneThatConstructs)
            return isTheClosure(cell, m_program.genericEntrypoints.get(value.function().index()).index());
        return isClosureOf(cell, value.function(), mustBeAsMade, evenIfAltered);
    };
    if (operand.rep == Rep::Cell)
        return test(operand.as<CellPointer>());
    if (operand.rep != Rep::JSValue)
        return boolean(false);
    Boxed boxed = operand.as<Boxed>();
    return either<Word32>(isCell(boxed, typeOf(value.child(0))), [&] {
        return test(asCell(boxed));
    }, [&] {
        return boolean(false);
    });
}

// As a rule what is called is a closure of the program or a function of the engine.
Word32 Lowering::isCallable(CellPointer cell)
{
    return either<Word32>(hasTypeOfFunctionFlag(cell), [&] {
        return boolean(true);
    }, [&] {
        return AOT_TYPED_OPERATION(IsCallable, cell);
    });
}

// `typeof x === "function"` and the like. The string `typeof` yields is never looked at.
Word32 Lowering::lowerTypeOfIs(const Value& value)
{
    TypeofType wanted = value.typeofType();
    ValueID tested = value.child(0);
    while (m_function.at(tested).opcode == Opcode::Cast && m_function.at(tested).castReason() == CastReason::Upcast)
        tested = m_function.at(tested).child(0);
    auto& staticType = typeOf(tested);
    if (staticType.isNumber() || staticType == Type::function())
        return boolean(wanted == (staticType.isNumber() ? TypeofType::Number : TypeofType::Function));
    if (wanted == TypeofType::Number && !(staticType.primitives & Primitive::Number))
        return boolean(false);
    Boxed operand = in<Boxed>(value.child(0));
    if (wanted == TypeofType::Number)
        return isNumber(operand);
    if (wanted == TypeofType::Boolean)
        return isBoolean(operand);

    Join join(m_out, Rep::Int32, Int32);
    // Goes on where the condition does not hold.
    auto when = [&](Word32 condition, Word32 answer) {
        LBasicBlock holds = m_out.newBlock();
        LBasicBlock next = m_out.newBlock();
        m_out.branch(condition.value(), unsure(holds), unsure(next));
        m_out.appendTo(holds);
        join.add(answer);
        m_out.appendTo(next);
    };
    Word32 ofNonCell = boolean(false);
    if (wanted == TypeofType::Undefined)
        ofNonCell = equal(operand, undefined());
    else if (wanted == TypeofType::Object)
        ofNonCell = equal(operand, boxedConstant(jsNull()));
    when(logicalNot(isCell(operand, typeOf(value.child(0)))), ofNonCell);

    CellPointer cell = asCell(operand);
    Word32 type = typeOfCell(cell);
    switch (wanted) {
    case TypeofType::String:
        join.add(equal(type, word32(StringType)));
        return join.finish().as<Word32>();
    case TypeofType::Symbol:
        join.add(equal(type, word32(SymbolType)));
        return join.finish().as<Word32>();
    case TypeofType::BigInt:
        join.add(equal(type, word32(HeapBigIntType)));
        return join.finish().as<Word32>();
    default:
        break;
    }
    auto is = [&](TypeofType what) {
        return boolean(what == wanted);
    };
    Word32 flags = loadByte(cell, m_heaps.JSCell_typeInfoFlags);
    when(hasBits(flags, TypeOfYieldsFunction), is(TypeofType::Function));
    if (wanted != TypeofType::Function)
        when(below(type, word32(ObjectType)), boolean(false));
    when(lacksBits(flags, MasqueradesAsUndefined | OverridesGetCallData), is(TypeofType::Object));
    join.add(equal(AOT_TYPED_OPERATION(TypeOfObject, cell), word32(static_cast<int32_t>(wanted))));
    return join.finish().as<Word32>();
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
    bool mayBeChangedArray = false;
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
        JSType itsType = m_builtins.classes()[index].type;
        if (!mayBeAnyObject && !std::ranges::any_of(from.engineClasses, [&](uint16_t other) { return m_builtins.classes()[other].type == itsType; }))
            continue;
        result = logicalOr(result, equal(type(), word32(itsType)));
        if (itsType != ArrayType)
            continue;
        // See EngineClass::type. What is an array or one of a few other things for a fact is only told from those.
        if (m_isValidatingFact)
            result = logicalOr(result, isChangedArray(cell));
        else if (!mayBeAnyObject)
            result = logicalOr(result, equal(type(), word32(DerivedArrayType)));
        else
            mayBeChangedArray = true;
    }
    if (!to.views.isEmpty() && (from.anyObject || !from.views.isEmpty())) {
        // Where nothing else will do, what the first half of a test turns away is done with.
        result = logicalOr(result, passesOneOf(shapeWordOf(cell), to.views, result || mayBeChangedArray ? nullptr : otherwise));
    }
    if (!mayBeChangedArray)
        return result ? result : boolean(false);
    // An heir of Array, an array that has been changed: asked only of what all else has turned away.
    LBasicBlock fits = m_out.newBlock();
    LBasicBlock doesNot = newColdBlock();
    m_out.branch(result.value(), usually(fits), rarely(doesNot));
    m_out.appendTo(doesNot);
    m_out.branch(isChangedArray(cell).value(), unsure(fits), unsure(otherwise));
    m_out.appendTo(fits);
    return boolean(true);
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
        result = logicalOr(result, isUndefinedOrNull(value, from));
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
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics() && range.isWithin(Range::int32())) {
        LBasicBlock continuation = m_out.newBlock();
        PatchpointValue* patchpoint = m_out.patchpoint(Int32);
        patchpoint->effects = Effects::none();
        patchpoint->effects.terminal = true;
        patchpoint->append(ConstrainedValue(number.value(), ValueRep::SomeRegister));
        m_out.appendSuccessor(rarely(otherwise));
        m_out.appendSuccessor(usually(continuation));
        patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
            CCallHelpers::Jump isNoInt32 = jit.branchOnFlags(CCallHelpers::NotEqual);
            CCallHelpers::Jump isInt32;
            if (!params.fallsThroughToSuccessor(1))
                isInt32 = jit.jump();
            Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
            params.addLatePath([=](CCallHelpers& jit) {
                isNoInt32.linkTo(*labels[0], &jit);
                if (isInt32.isSet())
                    isInt32.linkTo(*labels[1], &jit);
            });
        });
        m_out.appendTo(continuation);
        Integer exact = signExtended(Word32(patchpoint));
        if (!Range::int32().isWithin(range))
            orElse(isWithin(exact, range), otherwise);
        return exact;
    }
#endif
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
    bool mayBeNumber = from.primitives & Primitive::Number;
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 4> results;
    if (mayBeNumber) {
        LBasicBlock int32Case = m_out.newBlock();
        LBasicBlock otherCase = newColdBlock();
        m_out.branch(isInt32(value).value(), usually(int32Case), rarely(otherCase));

        m_out.appendTo(int32Case);
        Integer narrowed = signExtended(low32(value));
        if (!Range::int32().isWithin(to.integers))
            orElse(isWithin(narrowed, to.integers), otherwise);
        results.append(m_out.anchor(narrowed.value()));
        m_out.jump(continuation);

        m_out.appendTo(otherCase);
    }
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
    if (mayBeNumber) {
        // The engine may keep an integral number as a double.
        results.append(m_out.anchor(integralOrElse(unboxedDouble(value), to.integers, otherwise).value()));
        m_out.jump(continuation);
    } else
        m_out.jump(otherwise);

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
            isOne = isCell(value, from);
        else if (!(gone & ~Primitive::UndefinedOrNull) && withoutCells(from).withoutPrimitives(Primitive::UndefinedOrNull).isWithin(withoutCells(to), m_program)) {
            if (gone == Primitive::Undefined)
                isOne = notEqual(value, undefined());
            else if (gone == Primitive::Null)
                isOne = notEqual(value, boxedConstant(jsNull()));
            else
                isOne = logicalNot(isUndefinedOrNull(value, from));
        } else
            isOne = logicalOr(isCell(value, from), nonCellFits(value, from, to));
        orElse(isOne, otherwise);
        return convert(value, to, rep);
    }

    if (!others) {
        orElse(cellFits(cell, from, to, otherwise), otherwise);
        return convert(value, to, rep);
    }
    if (!(others & to.primitives)) {
        orElse(isCell(value, from), otherwise);
        orElse(cellFits(cell, from, to, otherwise), otherwise);
        return convert(value, to, rep);
    }

    // `undefined` and `null` pass for a reference. That is the one way around the test of the cell.
    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    // Nothing rare: a call that may yield `undefined` does.
    LBasicBlock otherCase = othersFit ? continuation : newUnlikelyBlock();
    if (othersFit)
        m_out.branch(isCell(value, from).value(), usually(cellCase), rarely(continuation));
    else
        m_out.branch(isCell(value, from).value(), unsure(cellCase), unsure(otherCase));
    m_out.appendTo(cellCase);
    m_out.branch(cellFits(cell, from, to, otherwise).value(), usually(continuation), rarely(otherwise));
    if (!othersFit) {
        m_out.appendTo(otherCase);
        m_out.branch(nonCellFits(value, from, to).value(), usually(continuation), rarely(otherwise));
    }
    m_out.appendTo(continuation);
    return convert(value, to, rep);
}

// Objects are not looked into.
static bool mayHaveValuesInCommon(const Type& a, const Type& b)
{
    uint16_t both = a.primitives & b.primitives;
    if (both & ~Primitive::Int || (both & Primitive::Int && !a.integers.overlap(b.integers).isEmpty()))
        return true;
    return a.hasObjects() && b.hasObjects();
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
    if (!mayHaveValuesInCommon(from, to))
        return never();
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

// The operand of the type as throwAt() takes it, in a block that is rarely run.
Operand Lowering::thrown(Operand operand, const Type& type)
{
    // Of a value that is kept in parts: the word where it is no cell. The run time reads `undefined` where that is zero, so nothing else is made of it here.
    if (operand.rep == Rep::JSValue) {
        auto found = m_decoded.find(operand.value);
        if (found == m_decoded.end())
            return operand;
        // Opaque: see remade().
        LValue word = m_out.opaque(found->value.loaded);
        return Boxed(m_out.select(m_out.below(word, m_out.constInt64(firstCellWord)), word, m_out.opaque(found->value.cell)));
    }
    // The run time takes 32 bits for a boolean.
    if (operand.rep == Rep::Int32 && !(type.primitives & Primitive::Boolean))
        return convert(operand, type, Rep::Int64);
    return operand;
}

// The same of what is `undefined` or `null` here: the word says which.
Boxed Lowering::thrownNil(Boxed value)
{
    auto found = m_decoded.find(value.value());
    return found == m_decoded.end() ? value : Boxed(found->value.loaded);
}

void Lowering::throwTypeMismatch(Operand operand, const Type& from, const Type& to)
{
    operand = thrown(operand, from);
    auto& current = m_function.at(m_currentBlock->values[m_currentIndexInBlock]);
    // The run time knows a type by its TypeID, and can then say which property is wrong. Nearly always it is the type of the current value.
    if (&m_program.at(current.type) == &to) {
        throwAt(ThrowKind::TypeMismatch, operand, current.type.index());
        m_out.unreachable();
        return;
    }
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
    throwAt(ThrowKind::KindMismatch, operand, kinds, name.index());
    m_out.unreachable();
}

bool Lowering::resultBecomesItsType(const Type& guarantee, const Type& type) const
{
    auto& value = m_function.at(m_currentBlock->values[m_currentIndexInBlock]);
    if (!value.type || &m_program.at(value.type) != &type || !(guarantee.anyObject || guarantee.admitsForeignObjects(m_program)) || writesOfElementRead(m_program, type) == Writes::Nothing)
        return false;
    if (Options::auditAOTTypedAssertions() || m_useCastTest || m_foreignObjectsDoNotFit || handlerEnteredByBranch() || m_environmentAwaitingBarrier || m_isFillingArrayAtBirth || m_isFillingReusedEnvironment || !m_fieldsAwaitingFirstStore.isEmpty())
        return false;
    if (value.opcode == Opcode::Cast)
        return claimMayMakeObjects(m_program, m_function, value);
    if (m_isElementOfEntries && (value.opcode == Opcode::GetByVal || value.opcode == Opcode::IteratorValue) && keyedAccessOf(m_program, m_function, value) == KeyedAccess::Array)
        return !value.readsElementKnownByTheStores() || Options::validateAOTTypedFacts();
    return value.opcode == Opcode::CallBuiltin || fieldReadMayMakeObjects(m_program, m_function, value) || writesOf(m_program, m_function, *m_currentBlock, m_currentIndexInBlock) == Writes::Heap;
}

Operand Lowering::ensure(Operand operand, const Type& guarantee, const Type& type)
{
    if (m_fusedCasts && &type == &m_fusedCasts->type && !m_fusedCasts->areBeingCarriedOut)
        return carryOutFusedCasts(operand, guarantee);
    // `never` says nothing.
    if (&guarantee == &type || type.isNever() || guarantee.isWithin(type, m_program, !m_isHoldingToTypeOpenedForLog))
        return convert(operand, guarantee, repOf(type));
    if (resultBecomesItsType(guarantee, type)) {
        TypeID given = m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type;
        OperationEffects effects { Writes::RuntimeState, true, true };
        if (m_program.isColdByGraph(m_id) && !m_isInLoop && repOf(guarantee) == Rep::JSValue)
            return convert(AOT_TYPED_OPERATION_WITH_EFFECTS(HoldOrBecome, effects, remade(convert(operand, guarantee, Rep::JSValue).as<Boxed>()), word32(given.index())), type, repOf(type));
        Rep rep = repOf(type);
        LBasicBlock misfit = newColdBlock();
        Join join(m_out, rep, b3TypeOf(rep));
        join.add(fitOrElse(operand, guarantee, type, misfit));
        m_out.appendTo(misfit);
        join.add(convert(AOT_TYPED_OPERATION_WITH_EFFECTS(HoldOrBecome, effects, remade(convert(operand, guarantee, Rep::JSValue).as<Boxed>()), word32(given.index())), type, rep));
        return join.finish();
    }
    bool isInColdBlockOfColdText = !m_out.m_block->frequency() && m_currentBlock && (m_program.isColdByGraph(m_id) || (goesGenerallyWhereTextIsCold() && !m_holdsWhatStrangerYields));
    if ((m_program.isColdByGraph(m_id) || isInColdBlockOfColdText) && !m_isInLoop && !m_function.isBodyOfSeveralSites
        && !m_environmentAwaitingBarrier && !m_isFillingArrayAtBirth && m_fieldsAwaitingFirstStore.isEmpty() && repOf(guarantee) == Rep::JSValue && holdsCells(repOf(type)) && &m_program.at(m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type) == &type
        && !m_useCastTest && !m_foreignObjectsDoNotFit && !handlerEnteredByBranch() && !Options::auditAOTTypedAssertions() && !Options::validateAOTTypedFacts()) {
        Boxed given = remade(convert(operand, guarantee, Rep::JSValue).as<Boxed>());
        LValue number = m_out.constInt64(m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type.index());
        LValue held = callAllocationStub(Stub::HoldToType, { { given.value(), GPRInfo::argumentGPR1 }, { number, GPRInfo::argumentGPR2 } }, { Writes::Nothing, true }, true, !m_out.m_block->frequency());
        return convert(Boxed(held), type, repOf(type));
    }
    if (isInColdBlockOfColdText && repOf(guarantee) == Rep::JSValue && holdsCells(repOf(type)) && &m_program.at(m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type) == &type
        && !m_useCastTest && !m_foreignObjectsDoNotFit && !handlerEnteredByBranch() && !Options::auditAOTTypedAssertions() && !Options::validateAOTTypedFacts()) {
        Boxed given = convert(operand, guarantee, Rep::JSValue).as<Boxed>();
        return convert(AOT_TYPED_OPERATION(Hold, remade(given), word32(m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type.index())), type, repOf(type));
    }
    LBasicBlock mismatch = newColdBlock();
    Operand result = fitOrElse(operand, guarantee, type, mismatch);
    // Of the types an operation holds its result to only those without room for whatever comes are left then: see Reader::typeReliedOn().
    // The run time knows a type by its number, so this is for the type of the value that is being lowered. Any other test throws as
    // ever, and the message says that the audit ends there.
    if (auto& value = m_function.at(m_currentBlock->values[m_currentIndexInBlock]); Options::auditAOTTypedAssertions() && (!holdsCells(result.rep) || type.isUndefinedOrNull()) && &m_program.at(value.type) == &type) [[unlikely]] {
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock fast = m_out.anchor(result.value);
        m_out.jump(continuation);
        m_out.appendTo(mismatch);
        ValueFromBlock slow = m_out.anchor(convert(assertionFailed(operand, guarantee, value.type, assertionIsOnResult | static_cast<uint32_t>(value.opcode)), type, result.rep).value);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return Operand(result.rep, m_out.phi(b3TypeOf(result.rep), fast, slow));
    }
    LBasicBlock continuation = m_out.m_block;
    m_out.appendTo(mismatch);
    if (LBasicBlock handler = handlerEnteredByBranch()) {
        if (auto& value = m_function.at(m_currentBlock->values[m_currentIndexInBlock]); m_function.has(FunctionLogsMisfits) && &m_program.at(value.type) == &type) {
            auto& held = m_function.at(value.child(0));
            uint32_t first = m_function.isGenericEntrypoint() ? 2 : m_function.captureTypes.size() + m_function.has(Format::FunctionHasThis);
            uint32_t parameter = UINT32_MAX;
            if (held.opcode == Opcode::Parameter && held.immediates[1] >= first)
                parameter = held.immediates[1] - first;
            else if (held.opcode == Opcode::GetArgument)
                parameter = held.immediates[0];
            AOT_TYPED_OPERATION(NoteMisfit, remade(convert(operand, guarantee, Rep::JSValue).as<Boxed>()), word32(value.type.index()), word32(parameter), word32(!m_function.isGenericEntrypoint() && !m_function.has(FunctionIsEnteredByValue)));
        }
        m_out.jump(handler);
    } else
        throwTypeMismatch(operand, guarantee, type);
    m_out.appendTo(continuation);
    return result;
}

Operand Lowering::ensureElement(CellPointer array, Integer index, Boxed element, const Type& type)
{
    // The run time knows a type by its number, so this is for the type of the value that is being lowered.
    TypeID given = m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type;
    if (m_function.at(m_currentBlock->values[m_currentIndexInBlock]).readsElementKnownByTheStores() && &m_program.at(given) == &type && !Options::validateAOTTypedFacts() && !Options::auditAOTTypedAssertions())
        return ensure(element, type, type);
    Writes writes = writesOfElementRead(m_program, type);
    if (writes == Writes::Nothing || &m_program.at(given) != &type || Options::auditAOTTypedAssertions())
        return ensure(element, type);
    Rep rep = repOf(type);
    LBasicBlock misfit = newColdBlock();
    LBasicBlock cellCase = newColdBlock();
    LBasicBlock plainObject = newColdBlock();
    LBasicBlock mismatch = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    join.add(fitOrElse(element, m_program.at(m_program.types.anything), type, misfit));
    m_out.appendTo(misfit);
    m_out.branch(isCell(element).value(), usually(cellCase), rarely(mismatch));
    m_out.appendTo(cellCase);
    m_out.branch(logicalOr(equal(typeOfCell(asCell(element)), word32(FinalObjectType)), equal(typeOfCell(asCell(element)), word32(ObjectType))).value(), usually(plainObject), rarely(mismatch));
    m_out.appendTo(plainObject);
    OperationEffects effects { writes, true, true };
    join.add(convert(AOT_TYPED_OPERATION_WITH_EFFECTS(CoerceElement, effects, array, low32(index), word32(given.index())), type, rep));
    m_out.appendTo(mismatch);
    throwTypeMismatch(element, m_program.at(m_program.types.anything), type);
    return join.finish();
}

Operand Lowering::ensureProperty(CellPointer holder, StringID name, Boxed yielded, const Type& type)
{
    TypeID given = m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type;
    Writes writes = writesOfElementRead(m_program, type);
    SetForScope holdsWhatStrangerYields(m_holdsWhatStrangerYields, true);
    if (writes == Writes::Nothing || &m_program.at(given) != &type || Options::auditAOTTypedAssertions())
        return ensure(yielded, type);
    Rep rep = repOf(type);
    LBasicBlock misfit = newColdBlock();
    LBasicBlock cellCase = newColdBlock();
    LBasicBlock plainObject = newColdBlock();
    LBasicBlock mismatch = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    join.add(fitOrElse(yielded, m_program.at(m_program.types.anything), type, misfit));
    m_out.appendTo(misfit);
    m_out.branch(isCell(yielded).value(), usually(cellCase), rarely(mismatch));
    m_out.appendTo(cellCase);
    m_out.branch(logicalOr(equal(typeOfCell(asCell(yielded)), word32(FinalObjectType)), equal(typeOfCell(asCell(yielded)), word32(ObjectType))).value(), usually(plainObject), rarely(mismatch));
    m_out.appendTo(plainObject);
    OperationEffects effects { writes, true, true };
    join.add(convert(AOT_TYPED_OPERATION_WITH_EFFECTS(CoerceProperty, effects, holder, word32(name.index()), yielded, word32(given.index())), type, rep));
    m_out.appendTo(mismatch);
    throwTypeMismatch(yielded, m_program.at(m_program.types.anything), type);
    return join.finish();
}

// The second half of an AuditSite for the value that is being lowered. Nothing is inlined in an audit build.
Word32 Lowering::auditLineAndColumn()
{
    LineColumn where = m_program.lineColumnFor(m_function.module, m_function.at(m_currentBlock->values[m_currentIndexInBlock]).position);
    return word32(AuditSite::pack(where.line, where.column));
}

// The cast that is being lowered holds a value to the type of a property. The object the next operation stores it to, if that is what the next
// operation does. The front end makes such a cast while it emits the store, so the object is at hand here.
ValueID Lowering::receiverOfNextStore(ValueID stored) const
{
    const Block* block = m_currentBlock;
    size_t next = m_currentIndexInBlock + 1;
    if (next == block->values.size()) {
        if (block->terminator.kind != TerminatorKind::Invoke)
            return { };
        block = &m_function.at(block->terminator.targets[0].block);
        next = 0;
    }
    if (next >= block->values.size())
        return { };
    auto& store = m_function.at(block->values[next]);
    bool isStoreOfIt = (store.opcode == Opcode::PutById && store.child(1) == stored) || (store.opcode == Opcode::PutByVal && store.child(2) == stored);
    if (!isStoreOfIt || (block != m_currentBlock && block->parameters.contains(store.child(0))))
        return { };
    return store.child(0);
}

Boxed Lowering::assertionFailed(Operand operand, const Type& from, TypeID wanted, uint32_t what, ValueID storedTo)
{
    if (storedTo)
        return AOT_TYPED_OPERATION(StoreAssertionFailed, convertTo<Boxed>(operand, from), in<Boxed>(storedTo), word32(wanted.index()), word32(m_id.index()), auditLineAndColumn());
    return AOT_TYPED_OPERATION(AssertionFailed, convertTo<Boxed>(operand, from), word32(wanted.index()), word32(what), word32(m_id.index()), auditLineAndColumn());
}

// The value is anything, and the operation would have held it to Value::assertedType. What is not of that is reported, and goes on.
void Lowering::auditResult(Operand operand, const Value& value)
{
    auto& type = m_program.at(value.type);
    auto& asserted = m_program.at(value.assertedType);
    if (type.isWithin(asserted, m_program))
        return;
    LBasicBlock mismatch = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    fitOrElse(operand, type, asserted, mismatch);
    m_out.jump(continuation);
    m_out.appendTo(mismatch);
    assertionFailed(operand, type, value.assertedType, assertionIsOnResult | static_cast<uint32_t>(value.opcode));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::validateOnlyFunction(Operand stored, ValueID id, FunctionID function, bool mayBeNil, FunctionID other)
{
    if ((stored.rep != Rep::Cell && stored.rep != Rep::JSValue) || m_function.at(id).opcode == Opcode::Unspecified)
        return;
    if (stored.value->hasInt64() && !stored.value->asInt64())
        return;
    Boxed boxed(stored.value);
    LBasicBlock isNotEmpty = m_out.newBlock();
    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock notCellCase = m_out.newBlock();
    LBasicBlock doesNotHold = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.notZero64(boxed.value()), unsure(isNotEmpty), unsure(continuation));
    m_out.appendTo(isNotEmpty);
    m_out.branch(isCell(boxed).value(), unsure(cellCase), unsure(notCellCase));
    m_out.appendTo(notCellCase);
    if (mayBeNil)
        m_out.branch(isUndefinedOrNull(boxed).value(), unsure(continuation), rarely(doesNotHold));
    else
        m_out.jump(doesNotHold);
    m_out.appendTo(cellCase);
    auto entrypoint = m_program.genericEntrypoints.find(function.index());
    if (other) {
        SetForScope isReporting(m_isReportingThatFactDoesNotHold, true);
        m_out.branch(logicalOr(isClosureOf(asCell(boxed), function, false, true), isClosureOf(asCell(boxed), other, false, true)).value(), unsure(continuation), rarely(doesNotHold));
    } else if (entrypoint == m_program.genericEntrypoints.end())
        m_out.jump(doesNotHold);
    else {
        SetForScope isReporting(m_isReportingThatFactDoesNotHold, true);
        Integer word(callAllocationStub(Stub::TypedClosureEntry, { { boxed.value(), stubTemporaryGPRs[0] } }, { Writes::Nothing, false }, false));
        Word32 functionOfWord(m_out.bitAnd(m_out.castToInt32(m_out.lShr(word.value(), m_out.constInt32(ClosureWord::functionShift))), m_out.constInt32(static_cast<int32_t>(ClosureWord::functionMask))));
        m_out.branch(logicalAnd(logicalNot(isZero(word)), equal(functionOfWord, word32(entrypoint->value.index()))).value(), unsure(continuation), rarely(doesNotHold));
    }
    m_out.appendTo(doesNotHold);
    AOT_TYPED_OPERATION(FactDoesNotHold, boxed, word32(m_id.index()), word32(id.index()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::validateFact(Operand operand, ValueID id, const Type& type)
{
    if (type.isNever() || operand.value == placeholder(operand.rep).value)
        return;
    ValueID source = id;
    while (m_function.at(source).opcode == Opcode::Cast && m_function.at(source).castReason() == CastReason::Upcast)
        source = m_function.at(source).child(0);
    if (m_entriesMadeAsRecords.get(source.index()))
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
        operand = Operand(Rep::JSValue, operand.value);
        [[fallthrough]];
    case Rep::JSValue:
        if (operand.value->hasInt64() && !operand.value->asInt64())
            return;
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
        if (operand.rep == Rep::JSValue)
            Emitter::orElse(m_out.notZero64(operand.value), doesNotHold);
        fitOrElse(operand, held, type, doesNotHold);
    }
    LBasicBlock continuation = m_out.newBlock();
    m_out.jump(continuation);
    m_out.appendTo(doesNotHold);
    AOT_TYPED_OPERATION(FactDoesNotHold, convertTo<Boxed>(operand, held), word32(m_id.index()), word32(id.index()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

// A property of `undefined` or `null` was asked for.
// Only of a value whose type admits nothing but cells, `undefined` and `null`: nonNil() asserts it, turnAwayNil() asks. As an address nil is below 16 (or that far into the first page of the
// cage), where nothing is mapped. A boxed number is not: ARM64 ignores the top byte of an address, so one in 512 doubles is a low address that may be mapped. What may be a number keeps its test.
bool Lowering::markNilTest(CellPointer accessed)
{
    if (!accessed || m_watchingHandler)
        return false;
    LValue number = m_out.constInt32(m_context.nilTests.size());
    PatchpointValue* marker = m_out.patchpoint(Void);
    marker->effects = Effects::none();
    marker->effects.writesLocalState = true;
    marker->append(ConstrainedValue(accessed.value(), ValueRep::ColdAny));
    marker->append(ConstrainedValue(number, ValueRep::ColdAny));
    marker->setGenerator(m_context.nilTestMarker);
    return true;
}

void Lowering::validateIsNotReleased(CellPointer environment, const Value& access)
{
    auto& layout = m_program.at(access.layout());
    if (!layout.reuseVariable || access.field() + 1 >= layout.fields.size())
        return;
    auto& hasEscaped = layout.fields[layout.fields.size() - 2];
    auto& isReleased = layout.fields[layout.fields.size() - 1];
    LBasicBlock released = m_out.newBlock();
    LBasicBlock doesNotHold = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(load(isReleased.store, fieldAt(environment, isReleased), 0, environment.value()).value, rarely(released), usually(continuation));
    m_out.appendTo(released);
    m_out.branch(load(hasEscaped.store, fieldAt(environment, hasEscaped), 0, environment.value()).value, usually(continuation), rarely(doesNotHold));
    m_out.appendTo(doesNotHold);
    {
        SetForScope isReporting(m_isReportingThatFactDoesNotHold, true);
        AOT_TYPED_OPERATION(FactDoesNotHold, Boxed(m_out.int64Zero), word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
    }
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::validateIsNotEmpty(Boxed value)
{
    LBasicBlock doesNotHold = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.notZero64(value.value()), usually(continuation), rarely(doesNotHold));
    m_out.appendTo(doesNotHold);
    AOT_TYPED_OPERATION(FactDoesNotHold, value, word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::throwNullish(Boxed value, StringID name, CellPointer accessed)
{
    ThrowKind kind = ThrowKind::NullishRead;
    switch (m_function.at(m_currentBlock->values[m_currentIndexInBlock]).opcode) {
    case Opcode::PutById:
    case Opcode::PutByVal:
    case Opcode::PutField:
        kind = ThrowKind::NullishWrite;
        break;
    case Opcode::IteratorOpen:
        kind = ThrowKind::NullishIterate;
        break;
    default:
        break;
    }
    bool mayFault = markNilTest(accessed);
    uint32_t callSite = throwAt(kind, Operand(thrownNil(value)), name.index());
    if (mayFault)
        m_context.nilTests.append({ kind, name ? name.index() : ThrowSite::none, std::nullopt, callSite });
    m_out.unreachable();
}

static bool isPlainAccess(const MemoryValue& memory)
{
    switch (memory.opcode()) {
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
    case Store8:
    case Store16:
    case B3::Store:
        break;
    default:
        return false;
    }
    int64_t offset = memory.offset();
    return !memory.hasFence() && offset >= 0 && offset + static_cast<int64_t>(memory.accessByteSize()) <= reachOfFaultingAccess && memory.isLegalOffset(offset);
}

static void emitAccess(CCallHelpers& jit, B3::Opcode opcode, B3::Type type, bool isSignExtended, const ValueRep& value, CCallHelpers::Address address)
{
    if (isSignExtended) {
        jit.load32SignedExtendTo64(address, value.gpr());
        return;
    }
    switch (opcode) {
    case Load8Z:
        jit.load8(address, value.gpr());
        return;
    case Load8S:
        jit.load8SignedExtendTo32(address, value.gpr());
        return;
    case Load16Z:
        jit.load16(address, value.gpr());
        return;
    case Load16S:
        jit.load16SignedExtendTo32(address, value.gpr());
        return;
    case Store8:
        jit.store8(value.gpr(), address);
        return;
    case Store16:
        jit.store16(value.gpr(), address);
        return;
    case Load:
    case B3::Store:
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    bool isLoad = opcode == Load;
    switch (type.kind()) {
    case Int32:
        isLoad ? jit.load32(address, value.gpr()) : jit.store32(value.gpr(), address);
        return;
    case Int64:
        isLoad ? jit.load64(address, value.gpr()) : jit.store64(value.gpr(), address);
        return;
    case Float:
        isLoad ? jit.loadFloat(address, value.fpr()) : jit.storeFloat(value.fpr(), address);
        return;
    case Double:
        isLoad ? jit.loadDouble(address, value.fpr()) : jit.storeDouble(value.fpr(), address);
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void askMemoryAtLoopPolls(Procedure& proc)
{
#if CPU(X86_64)
    if (!proc.loopPollGenerator())
        return;
    UseCounts useCounts(proc);
    for (BasicBlock* block : proc) {
        B3::Value* branch = block->last();
        if (branch->opcode() != B3::Branch)
            continue;
        B3::Value* comparison = branch->child(0);
        if (comparison->opcode() != B3::LessThan || !comparison->child(1)->isInt64(0) || useCounts.numUses(comparison) != 1)
            continue;
        auto* poll = comparison->child(0)->as<PatchpointValue>();
        if (!poll || poll->generator() != proc.loopPollGenerator() || useCounts.numUses(poll) != 1 || poll->owner != block || comparison->owner != block)
            continue;
        PatchpointValue* folded = proc.add<PatchpointValue>(B3::Void, branch->origin());
        folded->effects = poll->effects;
        folded->effects.terminal = true;
        folded->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            CCallHelpers::Jump isInterrupted = jit.branch64(CCallHelpers::LessThan, CCallHelpers::Address(instanceGPR, Instance::offsetOfTrapAwareSoftStackLimit()), CCallHelpers::TrustedImm32(0));
            CCallHelpers::Jump goesOn;
            if (!params.fallsThroughToSuccessor(1))
                goesOn = jit.jump();
            Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
            params.addLatePath([=](CCallHelpers& jit) {
                isInterrupted.linkTo(*labels[0], &jit);
                if (goesOn.isSet())
                    goesOn.linkTo(*labels[1], &jit);
            });
        });
        block->replaceLast(proc, folded);
        comparison->replaceWithNopIgnoringType();
        poll->replaceWithNopIgnoringType();
    }
#else
    UNUSED_PARAM(proc);
#endif
}

void askZeroExtendedWordsIn32Bits(Procedure& proc)
{
    auto isZeroExtended = [](B3::Value* value) {
        return value->opcode() == B3::ZExt32;
    };
    auto isWord = [](B3::Value* value) {
        return value->hasInt64() && value->asInt64() >= 0 && value->asInt64() <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max());
    };
    auto unsignedOpcode = [](B3::Opcode opcode) {
        switch (opcode) {
        case B3::LessThan:
            return B3::Below;
        case B3::LessEqual:
            return B3::BelowEqual;
        case B3::GreaterThan:
            return B3::Above;
        case B3::GreaterEqual:
            return B3::AboveEqual;
        default:
            return opcode;
        }
    };
    std::optional<B3::UseCountsWithoutUsingInstructions> useCounts;
    InsertionSet insertionSet(proc);
    for (BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            B3::Value* value = block->at(index);
            auto narrowConstant = [&](B3::Value* constant) {
                return insertionSet.insert<Const32Value>(index, value->origin(), static_cast<int32_t>(static_cast<uint32_t>(constant->asInt64())));
            };
            auto narrowMask = [&](B3::Value* masked) -> B3::Value* {
                if (masked->opcode() != B3::BitAnd || masked->type() != Int64 || !isZeroExtended(masked->child(0)) || !masked->child(1)->hasInt64())
                    return nullptr;
                return insertionSet.insert<B3::Value>(index, B3::BitAnd, value->origin(), masked->child(0)->child(0), narrowConstant(masked->child(1)));
            };
            switch (value->opcode()) {
            case B3::Branch:
            case B3::Select:
                if (B3::Value* masked = narrowMask(value->child(0)))
                    value->child(0) = masked;
                else if (isZeroExtended(value->child(0)))
                    value->child(0) = value->child(0)->child(0);
                break;
            case B3::Equal:
            case B3::NotEqual:
                if (value->child(1)->isInt64(0)) {
                    if (B3::Value* masked = narrowMask(value->child(0))) {
                        value->child(0) = masked;
                        value->child(1) = insertionSet.insert<Const32Value>(index, value->origin(), 0);
                        break;
                    }
                }
                [[fallthrough]];
            case B3::LessThan:
            case B3::GreaterThan:
            case B3::LessEqual:
            case B3::GreaterEqual:
            case B3::Below:
            case B3::BelowEqual:
            case B3::Above:
            case B3::AboveEqual: {
                if (!isZeroExtended(value->child(0)))
                    break;
                B3::Value* right = nullptr;
                if (isZeroExtended(value->child(1)))
                    right = value->child(1)->child(0);
                else if (isWord(value->child(1)))
                    right = narrowConstant(value->child(1));
                if (!right)
                    break;
                value->setOpcodeUnsafely(unsignedOpcode(value->opcode()));
                value->child(0) = value->child(0)->child(0);
                value->child(1) = right;
                break;
            }
            case B3::ZShr: {
                auto* memory = value->child(0)->as<MemoryValue>();
                if (value->type() != Int64 || !value->child(1)->isInt32(32) || !memory || memory->opcode() != B3::Load || memory->hasFence() || memory->offset() > std::numeric_limits<int32_t>::max() - 4)
                    break;
                unsigned place = index;
                while (place && index - place < 8 && block->at(place) != memory)
                    --place;
                if (block->at(place) != memory)
                    break;
                if (!useCounts)
                    useCounts.emplace(proc);
                if (useCounts->numUses(memory) != 1)
                    break;
                auto* upperHalf = insertionSet.insert<MemoryValue>(place, B3::Load, Int32, memory->origin(), memory->lastChild(), memory->offset() + 4, memory->range(), memory->fenceRange());
                upperHalf->setReadsMutability(memory->readsMutability());
                upperHalf->setControlDependent(memory->controlDependent());
                value->replaceWithIdentity(insertionSet.insert<B3::Value>(index, B3::ZExt32, value->origin(), upperHalf));
                break;
            }
            default:
                break;
            }
        }
        insertionSet.execute(block);
    }
}

void askSignExtendedWordsIn32Bits(Procedure& proc)
{
    auto isSignExtended = [](B3::Value* value) {
        return value->opcode() == B3::SExt32;
    };
    auto isWithin = [](B3::Value* value, int64_t least, int64_t greatest) {
        return value->hasInt64() && value->asInt64() >= least && value->asInt64() <= greatest;
    };
    InsertionSet insertionSet(proc);
    for (BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            B3::Value* value = block->at(index);
            auto narrowConstant = [&](B3::Value* constant) {
                return insertionSet.insert<Const32Value>(index, value->origin(), static_cast<int32_t>(constant->asInt64()));
            };
            auto narrowMask = [&](B3::Value* masked) -> B3::Value* {
                if (masked->opcode() != B3::BitAnd || masked->type() != Int64 || !isSignExtended(masked->child(0)) || !isWithin(masked->child(1), 0, std::numeric_limits<int32_t>::max()))
                    return nullptr;
                return insertionSet.insert<B3::Value>(index, B3::BitAnd, value->origin(), masked->child(0)->child(0), narrowConstant(masked->child(1)));
            };
            switch (value->opcode()) {
            case B3::Branch:
            case B3::Select:
                if (B3::Value* masked = narrowMask(value->child(0)))
                    value->child(0) = masked;
                else if (isSignExtended(value->child(0)))
                    value->child(0) = value->child(0)->child(0);
                break;
            case B3::Equal:
            case B3::NotEqual:
                if (value->child(1)->isInt64(0)) {
                    if (B3::Value* masked = narrowMask(value->child(0))) {
                        value->child(0) = masked;
                        value->child(1) = insertionSet.insert<Const32Value>(index, value->origin(), 0);
                        break;
                    }
                }
                [[fallthrough]];
            case B3::LessThan:
            case B3::GreaterThan:
            case B3::LessEqual:
            case B3::GreaterEqual:
                if (!isSignExtended(value->child(0)))
                    break;
                if (isSignExtended(value->child(1))) {
                    value->child(0) = value->child(0)->child(0);
                    value->child(1) = value->child(1)->child(0);
                } else if (isWithin(value->child(1), std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max())) {
                    value->child(1) = narrowConstant(value->child(1));
                    value->child(0) = value->child(0)->child(0);
                }
                break;
            case B3::Below:
            case B3::BelowEqual:
            case B3::Above:
            case B3::AboveEqual:
                if (isSignExtended(value->child(0)) && isWithin(value->child(1), 0, std::numeric_limits<int32_t>::max())) {
                    value->child(1) = narrowConstant(value->child(1));
                    value->child(0) = value->child(0)->child(0);
                }
                break;
            default:
                break;
            }
        }
        insertionSet.execute(block);
    }
}

void foldNumberBoxings(Procedure& proc, LoweringContext& context)
{
    if (!context.numberBoxingGenerator)
        return;
    InsertionSet insertionSet(proc);
    for (BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            auto* boxing = block->at(index)->as<PatchpointValue>();
            if (!boxing || boxing->generator() != context.numberBoxingGenerator.get() || !boxing->child(0)->hasDouble())
                continue;
            boxing->replaceWithIdentity(insertionSet.insert<Const64Value>(index, boxing->origin(), JSValue::encode(jsNumber(purifyNaN(boxing->child(0)->asDouble())))));
        }
        insertionSet.execute(block);
    }
}

#if CPU(ARM64)
static bool joinsCompareChain(B3::Value* value)
{
    switch (value->opcode()) {
    case B3::BitAnd:
    case B3::BitOr:
        return joinsCompareChain(value->child(0)) && joinsCompareChain(value->child(1));
    case B3::Equal:
    case B3::NotEqual:
        if (value->child(1)->isInt(0) && value->child(0)->opcode() == B3::BitAnd && value->child(0)->child(1)->hasInt() && hasOneBitSet(static_cast<uint64_t>(value->child(0)->child(1)->asInt())))
            return false;
        return true;
    case B3::LessThan:
    case B3::GreaterThan:
    case B3::LessEqual:
    case B3::GreaterEqual:
    case B3::Above:
    case B3::Below:
    case B3::AboveEqual:
    case B3::BelowEqual:
        return true;
    default:
        return false;
    }
}
#endif

static bool comparesWithConstant(B3::Value* value)
{
    return (value->opcode() == B3::Equal || value->opcode() == B3::NotEqual) && value->child(1)->hasInt();
}

void splitBranchConditions(Procedure& proc)
{
    Vector<B3::BasicBlock*> worklist;
    for (B3::BasicBlock* block : proc) {
        B3::Value* last = block->last();
        if (last->opcode() == B3::Branch && (last->child(0)->opcode() == B3::BitAnd || last->child(0)->opcode() == B3::BitOr))
            worklist.append(block);
    }
    if (worklist.isEmpty())
        return;
    B3::UseCountsWithoutUsingInstructions useCounts(proc);
    bool changed = false;
    while (!worklist.isEmpty()) {
        B3::BasicBlock* block = worklist.takeLast();
        B3::Value* branch = block->last();
        if (branch->opcode() != B3::Branch)
            continue;
        B3::Value* condition = branch->child(0);
        if ((condition->opcode() != B3::BitAnd && condition->opcode() != B3::BitOr) || condition->owner != block || useCounts.numUses(condition) != 1)
            continue;
        B3::Value* first = condition->child(0);
        B3::Value* second = condition->child(1);
        if (!first->returnsBool() || !second->returnsBool() || first->isConstant() || second->isConstant() || first == second)
            continue;
        if (useCounts.numUses(first) != 1 || useCounts.numUses(second) != 1)
            continue;
#if CPU(ARM64)
        if (joinsCompareChain(condition))
            continue;
#endif
        if (comparesWithConstant(first) && comparesWithConstant(second) && first->child(0) == second->child(0))
            continue;

        UncheckedKeyHashMap<B3::Value*, unsigned> usesThatSink;
        usesThatSink.add(second, 1);
        Vector<B3::Value*, 8> sunk;
        bool crossesWrite = false;
        auto& values = block->values();
        for (unsigned i = values.size() - 1; i--;) {
            B3::Value* value = values[i];
            if (value == condition)
                continue;
            B3::Effects effects = value->effects();
            auto found = usesThatSink.find(value);
            bool reads = effects.reads || effects.readsPinned;
            if (found != usesThatSink.end() && found->value == useCounts.numUses(value) && !effects.mustExecute() && !effects.readsLocalState && !(reads && crossesWrite)) {
                sunk.append(value);
                for (B3::Value* child : value->children())
                    usesThatSink.add(child, 0).iterator->value++;
                continue;
            }
            crossesWrite |= effects.isWrite();
        }

        B3::FrequentedBlock taken = block->taken();
        B3::FrequentedBlock notTaken = block->notTaken();
        B3::BasicBlock* next = proc.addBlock(std::max(taken.block()->frequency(), notTaken.block()->frequency()));
        B3::FrequentedBlock toNext(next, taken.isRare() && notTaken.isRare() ? B3::FrequencyClass::Rare : B3::FrequencyClass::Normal);
        values.removeAllMatching([&](B3::Value* value) {
            return value == condition || sunk.contains(value);
        });
        for (unsigned i = sunk.size(); i--;)
            next->append(sunk[i]);
        next->appendNew<B3::Value>(proc, B3::Branch, branch->origin(), second);
        next->setSuccessors(taken, notTaken);
        branch->child(0) = first;
        if (condition->opcode() == B3::BitAnd)
            block->setSuccessors(toNext, notTaken);
        else
            block->setSuccessors(taken, toNext);
        proc.deleteValue(condition);
        worklist.append(next);
        worklist.append(block);
        changed = true;
    }
    if (changed)
        proc.invalidateCFG();
}

// A test for nil whose failure throws, followed by an access through the pointer with nothing in between that anybody could see: the access is the test.
void useFaultsForNilTests(Procedure& proc, LoweringContext& context)
{
    InsertionSet insertionSet(proc);
    bool didChange = false;
    // The back end makes one instruction of a load of 32 bits and its extension. So does the access that takes the place of the load.
    UncheckedKeyHashMap<B3::Value*, Vector<B3::Value*, 1>> extensionsOf;
    for (B3::Value* value : proc.values()) {
        if ((value->opcode() == ZExt32 || value->opcode() == SExt32) && value->child(0)->as<MemoryValue>())
            extensionsOf.add(value->child(0), Vector<B3::Value*, 1> { }).iterator->value.append(value);
    }
    for (B3::BasicBlock* arm : proc) {
        PatchpointValue* marker = nullptr;
        for (B3::Value* value : *arm) {
            if (auto* patchpoint = value->as<PatchpointValue>(); patchpoint && patchpoint->generator() == context.nilTestMarker.get()) {
                marker = patchpoint;
                break;
            }
        }
        if (!marker)
            continue;
        B3::Value* pointer = marker->child(0);
        uint32_t nilTest = marker->child(1)->asInt32();
        marker->replaceWithNop();
        if (arm->numPredecessors() != 1)
            continue;
        B3::BasicBlock* block = arm->predecessor(0);
        if (block->last()->opcode() != Branch)
            continue;
        B3::BasicBlock* next = block->successorBlock(block->successorBlock(0) == arm);
        if (next == arm || next->numPredecessors() != 1)
            continue;
        for (unsigned index = 0; index < next->size(); ++index) {
            B3::Value* value = next->at(index);
            auto* memory = value->as<MemoryValue>();
            if (!memory || memory->lastChild() != pointer || !isPlainAccess(*memory)) {
                Effects effects = value->effects();
                if (effects.terminal || effects.exitsSideways || effects.writesLocalState || effects.writesPinned || effects.fence || effects.writes)
                    break;
                continue;
            }
            bool isStore = memory->isStore();
            B3::Type type = isStore ? memory->child(0)->type() : memory->type();
            bool isExtended = type == Int32 && (memory->opcode() == Load || memory->opcode() == Load8Z || memory->opcode() == Load16Z);
            Vector<B3::Value*, 1> extensions = isExtended ? extensionsOf.get(memory) : Vector<B3::Value*, 1> { };
            auto isExtendedBy = [&](B3::Opcode opcode) {
                return extensions.containsIf([&](B3::Value* extension) {
                    return extension->opcode() == opcode;
                });
            };
            bool isSignExtended = memory->opcode() == Load && isExtendedBy(SExt32) && !isExtendedBy(ZExt32);
            PatchpointValue* access = insertionSet.insert<PatchpointValue>(index, isExtended ? B3::Type(Int64) : memory->type(), memory->origin());
            if (isStore)
                access->append(ConstrainedValue(memory->child(0), ValueRep::SomeRegister));
            else
                access->resultConstraints = { ValueRep::SomeRegister };
            access->append(ConstrainedValue(pointer, ValueRep::SomeRegister));
            access->effects = memory->effects();
            access->effects.exitsSideways = true;
            access->effects.reads = HeapRange::top();
            access->setGenerator([context = &context, opcode = memory->opcode(), type, isSignExtended, offset = memory->offset(), nilTest](CCallHelpers& jit, const StackmapGenerationParams& params) {
                CCallHelpers::Label instruction = jit.labelIgnoringWatchpoints();
                emitAccess(jit, opcode, type, isSignExtended, params[0], CCallHelpers::Address(params[1].gpr(), offset));
                context->faultingAccesses.append({ instruction, params[1].gpr(), nilTest });
            });
            if (isStore)
                memory->replaceWithNop();
            else if (isExtended) {
                memory->replaceWithIdentity(insertionSet.insert<B3::Value>(index, Trunc, memory->origin(), access));
                for (B3::Value* extension : extensions) {
                    if (extension->opcode() == (isSignExtended ? SExt32 : ZExt32))
                        extension->replaceWithIdentity(access);
                }
            } else
                memory->replaceWithIdentity(access);
            insertionSet.execute(next);
            block->replaceLastWithNew<B3::Value>(proc, Jump, block->last()->origin());
            block->setSuccessors(FrequentedBlock(next));
            arm->removePredecessor(block);
            didChange = true;
            break;
        }
    }
    if (!didChange)
        return;
    proc.resetReachability();
    proc.invalidateCFG();
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
    m_out.branch(isCell(value, type).value(), usually(continuation), rarely(isNil));
    m_out.appendTo(isNil);
    throwNullish(value, name, asCell(value));
    m_out.appendTo(continuation);
    return asCell(value);
}

// What enters a place whose type is written down. A value that does not fit is for the run time to turn into one that does, or to turn
// away. Also where a value is narrowed to a type that casts turn objects into where they are: see narrowingMayRetag(). That looks at
// the object alone.
Operand Lowering::coerce(Operand operand, const Type& from, const Value& cast)
{
    auto& to = m_program.at(cast.type);
    Rep rep = repOf(to);
    bool isNarrowing = cast.castReason() == CastReason::Flow;
    // Only an object is ever converted. A container passes on its kind: see ensureElement().
    if (!from.hasObjects() || to.views.isEmpty() || m_program.isHeldAlready(from, to, cast.castReason()))
        return ensure(operand, from, to);

    LBasicBlock slowPath = newColdBlock();
    OperandJoin join(*this, joinedAs(to));
    // Where the type admits foreign objects, only an object of a member is done with here. Any other is for the run time, though it is of
    // the type as it is: that lays out what a plain object refers to, and hands back the object itself. See Coercion in AOTTypedObject.cpp.
    Operand fitted;
    {
        SetForScope isCoerced(m_foreignObjectsDoNotFit, (!isNarrowing && m_program.annotatedPlaceLooksAtForeignObjects(to)) || m_program.isOpenedForLog(to));
        // Nothing becomes of what is only tested: a member that could still become more is done with here.
        SetForScope useCastTest(m_useCastTest, cast.castReason() == CastReason::As || isNarrowing);
        fitted = fitOrElse(operand, from, to, slowPath);
    }
    join.add(convert(fitted, to, rep));

    m_out.appendTo(slowPath);
    OperationEffects effects { writesOfCast(m_program, m_function, cast), true, castWritesShapeWords(m_program, m_function, cast) };
    bool onlyCellsComeHere = operand.rep == Rep::JSValue && withoutCells(from).isWithin(withoutCells(to), m_program);
    Boxed passed = onlyCellsComeHere ? Boxed(partsOf(operand.as<Boxed>()).cell) : operand.rep == Rep::JSValue ? remade(operand.as<Boxed>()) : convertTo<Boxed>(operand, from);
    // An object of the program becomes what it becomes where it is, and what is no object is handed back or turned away: the value goes on as it was, and is of the type now.
    if (!from.anyObject && !from.admitsForeignObjects(m_program) && !Options::auditAOTTypedAssertions()) {
        AOT_TYPED_OPERATION_WITH_EFFECTS(CoerceInPlace, effects, passed, typeAndReason(cast.type, cast.castReason()));
        join.add(convert(convert(operand, from, repOf(from)), to, rep));
        return join.finish();
    }
    RELEASE_ASSERT(isNarrowing || !m_program.castsMayRetag || effects.writesShapeWords || cast.castReason() != CastReason::As);
    Boxed coerced = AOT_TYPED_OPERATION_WITH_EFFECTS(Coerce, effects, passed, typeAndReason(cast.type, cast.castReason() == CastReason::As && cast.coercesFreshData() ? CastReason::AsFreshObject : cast.castReason()));
    join.add(convert(coerced, to, rep));
    return join.finish();
}

// How fresh the operand of a coercion is goes with the reason.
static CastReason reasonForRunTime(const Value& cast)
{
    if (cast.castReason() != CastReason::Coerce || !cast.coercesFreshData())
        return cast.castReason();
    return cast.freshness() == Format::Freshness::WholeGraph ? CastReason::CoerceFreshGraph : CastReason::CoerceFreshObject;
}

Word32 Lowering::typeAndReason(TypeID type, CastReason reason)
{
    static_assert(static_cast<uint32_t>(CastReason::AsFreshObject) < 16);
    RELEASE_ASSERT(type.index() < 1u << 28 && static_cast<uint32_t>(reason) < 16);
    return word32(type.index() << 4 | static_cast<uint32_t>(reason));
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
// object, which may be plain data.
Operand Lowering::lowerCoercion(ValueID result, const Value& cast)
{
    Operand operand = get(cast.child(0));
    auto& from = typeOf(cast.child(0));
    auto& type = typeOf(result);
    auto& to = m_program.at(cast.coercedType());
    Rep rep = repOf(type);
    if (!m_isCarryingOutSunkCoercion && (coercionSinks(m_currentIndexInBlock) || coercionIsOnlyReadThrough(m_currentIndexInBlock)))
        return convert(operand, from, rep);
    bool mayChangeObjects = holdsCells(rep) && coercionMayChangeObjects(m_program, from, to, cast.coercesFreshData());
    bool mayBeAnyObject = from.anyObject || from.admitsForeignObjects(m_program);
    bool mayCopy = mayChangeObjects && mayBeAnyObject && !to.views.isEmpty() && cast.coercesFreshData();
    Vector<ShapeTest, 4> sources;
    for (LayoutID view : to.views) {
        if (mayChangeObjects && mayPass(from, m_program.at(view).retagSources))
            sources.append(m_program.at(view).retagSources);
    }
    bool asksObjects = !sources.isEmpty() || mayCopy;
    if (!asksObjects && from.isWithin(type, m_program))
        return convert(operand, from, rep);

    LBasicBlock slowPath = newColdBlock();
    LBasicBlock asItIs = m_out.newBlock();
    OperandJoin join(*this, joinedAs(type));
    // Every object is of the type of the cast, if T has objects at all.
    Operand fitted = fitOrElse(operand, from, type, slowPath);
    if (!asksObjects)
        m_out.jump(asItIs);
    else {
        CellPointer cell;
        if (rep == Rep::Cell)
            cell = fitted.as<CellPointer>();
        else {
            LBasicBlock cellCase = m_out.newBlock();
            m_out.branch(isCell(fitted.as<Boxed>(), type).value(), usually(cellCase), rarely(asItIs));
            m_out.appendTo(cellCase);
            cell = asCell(fitted.as<Boxed>());
        }
        if (!sources.isEmpty()) {
            LBasicBlock isNoSource = mayCopy ? m_out.newBlock() : asItIs;
            // Where an object has its shape word, a bigint has a hash.
            LBasicBlock isSource = type.primitives & Primitive::BigInt ? newColdBlock() : slowPath;
            branchOnPassingOneOf(shapeWordOf(cell), sources, rarely(isSource), usually(isNoSource));
            if (isSource != slowPath) {
                m_out.appendTo(isSource);
                m_out.branch(equal(typeOfCell(cell), word32(HeapBigIntType)).value(), unsure(isNoSource), unsure(slowPath));
            }
            if (mayCopy)
                m_out.appendTo(isNoSource);
        }
        if (mayCopy)
            m_out.branch(equal(typeOfCell(cell), word32(FinalObjectType)).value(), rarely(slowPath), usually(asItIs));
    }

    m_out.appendTo(asItIs);
    join.add(Operand(rep, fitted.value));

    m_out.appendTo(slowPath);
    OperationEffects effects { writesOfCast(m_program, m_function, cast), true, castWritesShapeWords(m_program, m_function, cast) };
    bool onlyCellsComeHere = operand.rep == Rep::JSValue && withoutCells(from).isWithin(withoutCells(type), m_program);
    Boxed passed = onlyCellsComeHere ? Boxed(partsOf(operand.as<Boxed>()).cell) : operand.rep == Rep::JSValue ? remade(operand.as<Boxed>()) : convertTo<Boxed>(operand, from);
    // See coerce(). Nothing is converted either: only an object of a layout that is turned into another comes here.
    if (!mayBeAnyObject && from.isWithin(type, m_program) && !Options::auditAOTTypedAssertions()) {
        AOT_TYPED_OPERATION_WITH_EFFECTS(CoerceInPlace, effects, passed, typeAndReason(cast.coercedType(), CastReason::Coerce));
        join.add(Operand(rep, fitted.value));
        return join.finish();
    }
    RELEASE_ASSERT(!mayCopy || !m_program.castsMayRetag || effects.writesShapeWords);
    Boxed coerced = AOT_TYPED_OPERATION_WITH_EFFECTS(Coerce, effects, passed, typeAndReason(cast.coercedType(), reasonForRunTime(cast)));
    join.add(convert(coerced, type, rep));
    return join.finish();
}

// `x!`. Its type is without `undefined` and `null`, so the test for the type is the test for them.
Operand Lowering::lowerNonNull(ValueID result, const Value& cast)
{
    Operand operand = get(cast.child(0));
    auto& from = typeOf(cast.child(0));
    auto& to = typeOf(result);
    if (from.isWithin(to, m_program))
        return convert(operand, from, repOf(to));
    LBasicBlock mismatch = newColdBlock();
    Operand fitted = fitOrElse(operand, from, to, mismatch);
    LBasicBlock continuation = m_out.m_block;
    m_out.appendTo(mismatch);
    // As a rule the type is that of the operand without `undefined` and `null`: then nothing else gets here.
    bool isNil = operand.rep == Rep::JSValue && from.withoutPrimitives(Primitive::UndefinedOrNull).isWithin(to, m_program);
    throwAt(ThrowKind::NonNullFailed, isNil ? Operand(thrownNil(operand.as<Boxed>())) : thrown(operand, from), cast.type.index(), cast.operandText().index());
    m_out.unreachable();
    m_out.appendTo(continuation);
    return fitted;
}

// Options::auditAOTTypedAssertions(). What the cast holds its operand to is Value::assertedType, or its own type where that is relied on, as
// that of a bare number is. What is of that goes on. Anything else is for the run time, which makes of it what the cast makes of such
// a value, reports what the cast would turn away, and hands back what goes on: the value, or what is of the type in its stead.
Operand Lowering::lowerAuditedCast(ValueID result, const Value& cast)
{
    Operand operand = get(cast.child(0));
    auto& from = typeOf(cast.child(0));
    CastReason reason = cast.castReason();
    TypeID wanted = reason == CastReason::Coerce ? cast.coercedType() : cast.assertedType ? cast.assertedType : cast.type;
    // These look into an object that is of the type only because the type admits whatever comes.
    bool looksIntoObjects = reason == CastReason::As || reason == CastReason::Coerce;
    // `x as string`: an object, `undefined` and `null` stay what they are: the type of the result has them. As in lowerCoercion().
    bool goesByResult = reason == CastReason::Coerce && !m_program.at(wanted).hasObjects();
    auto& passing = m_program.at(goesByResult ? (cast.assertedType ? cast.assertedType : cast.type) : wanted);
    if (from.isWithin(passing, m_program, !looksIntoObjects))
        return convert(operand, from, cast.rep);

    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Operand fitted;
    {
        SetForScope isStrict(m_foreignObjectsDoNotFit, looksIntoObjects);
        SetForScope useCastTest(m_useCastTest, looksIntoObjects || reason == CastReason::Flow);
        fitted = fitOrElse(operand, from, passing, slowPath);
    }
    ValueFromBlock fast = m_out.anchor(convert(fitted, passing, cast.rep).value);
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    Boxed goesOn;
    // Behind a cast to `undefined` or `null` alone there is no room for anything else: the run time reports and throws.
    if (looksIntoObjects || reason == CastReason::Flow || m_program.at(wanted).isUndefinedOrNull()) {
        OperationEffects effects { writesOfCast(m_program, m_function, cast), true, castWritesShapeWords(m_program, m_function, cast) };
        goesOn = AOT_TYPED_OPERATION_WITH_EFFECTS(AuditCoerce, effects, convertTo<Boxed>(operand, from), word32(wanted.index()), word32(static_cast<uint32_t>(reasonForRunTime(cast))), word32(m_id.index()), auditLineAndColumn());
    } else
        goesOn = assertionFailed(operand, from, wanted, static_cast<uint32_t>(reason), reason == CastReason::Store ? receiverOfNextStore(result) : ValueID());
    ValueFromBlock slow = m_out.anchor(convert(goesOn, m_program.at(cast.type), cast.rep).value);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return Operand(cast.rep, m_out.phi(b3TypeOf(cast.rep), fast, slow));
}

unsigned Lowering::numberOfUsesOf(ValueID id)
{
    if (m_numberOfUses.isEmpty()) {
        m_numberOfUses.fill(0, m_function.values.size());
        for (auto& block : m_function.blocks) {
            for (ValueID user : block.values) {
                m_function.at(user).forEachChild([&](ValueID child) {
                    ++m_numberOfUses[child.index()];
                });
            }
            if (block.terminator.value)
                ++m_numberOfUses[block.terminator.value.index()];
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    ++m_numberOfUses[argument.index()];
            }
        }
    }
    return m_numberOfUses[id.index()];
}

void Lowering::findUsers()
{
    if (!m_firstUser.isEmpty())
        return;
    unsigned count = m_function.values.size();
    m_firstUser.fill(0, count + 1);
    m_placeOfValue.fill({ UINT32_MAX, UINT32_MAX }, count);
    m_isUsedByTerminator.ensureSize(count);
    for (uint32_t b = 0; b < m_function.blocks.size(); ++b) {
        auto& block = m_function.blocks[b];
        for (uint32_t i = 0; i < block.values.size(); ++i) {
            m_placeOfValue[block.values[i].index()] = { b, i };
            m_function.at(block.values[i]).forEachChild([&](ValueID child) {
                ++m_firstUser[child.index() + 1];
            });
        }
        if (block.terminator.value)
            m_isUsedByTerminator.quickSet(block.terminator.value.index());
        for (auto& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                m_isUsedByTerminator.quickSet(argument.index());
        }
    }
    for (unsigned i = 0; i < count; ++i)
        m_firstUser[i + 1] += m_firstUser[i];
    m_users.grow(m_firstUser[count]);
    Vector<uint32_t> next(m_firstUser.span().first(count));
    for (auto& block : m_function.blocks) {
        for (ValueID user : block.values) {
            m_function.at(user).forEachChild([&](ValueID child) {
                m_users[next[child.index()]++] = user;
            });
        }
    }
}

std::span<const ValueID> Lowering::usersOf(ValueID id)
{
    findUsers();
    return m_users.span().subspan(m_firstUser[id.index()], m_firstUser[id.index() + 1] - m_firstUser[id.index()]);
}

bool Lowering::isUsedByTerminator(ValueID id)
{
    findUsers();
    return m_isUsedByTerminator.quickGet(id.index());
}

std::pair<uint32_t, uint32_t> Lowering::placeOf(ValueID id)
{
    findUsers();
    return m_placeOfValue[id.index()];
}

bool Lowering::mayFuseWithRead(size_t indexOfRead)
{
    // There every value is looked at by itself.
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || m_currentBlock->terminator.kind == TerminatorKind::Invoke)
        return false;
    auto& read = m_function.at(m_currentBlock->values[indexOfRead]);
    return read.opcode == Opcode::GetById && !read.readsMethodFor() && fieldFor(typeOf(read.child(0)), read.string());
}

static bool isConstant(Opcode opcode)
{
    return opcode == Opcode::Undefined || opcode == Opcode::Null || opcode == Opcode::Boolean || opcode == Opcode::Integer || opcode == Opcode::Double;
}

// `(t as O).flags & 3`: a written cast asserts nothing, so what is read through it is boxed and may be anything. The arms of the read know more: an integer as it is kept; `undefined`, where the object lacks the field. An operation on
// numbers that is the only use of what the read yields, or the last of the casts behind it, is carried out in the arms as the casts are: on the integer; folded; the general way only where the object is asked by the name. Between the
// two stand constants only.
std::optional<size_t> Lowering::indexOfUseFusedWithRead(size_t indexOfRead, unsigned numberOfCasts)
{
    if (!mayFuseWithRead(indexOfRead))
        return std::nullopt;
    auto& values = m_currentBlock->values;
    ValueID operand = values[indexOfRead + numberOfCasts];
    if (m_function.at(operand).rep != Rep::JSValue || numberOfUsesOf(operand) != 1)
        return std::nullopt;
    // Where the field keeps numbers: there the arms have something to gain. A text would be joined to the other operand once in every arm.
    auto& read = m_function.at(values[indexOfRead]);
    if (auto& stored = m_program.at(fieldFor(typeOf(read.child(0)), read.string())->stored); stored.hasObjects() || stored.primitives & ~(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent))
        return std::nullopt;
    for (size_t index = indexOfRead + numberOfCasts + 1; index < values.size(); ++index) {
        auto& value = m_function.at(values[index]);
        if (isConstant(value.opcode))
            continue;
        switch (value.opcode) {
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
        case Opcode::BitAnd:
        case Opcode::BitOr:
        case Opcode::BitXor:
        case Opcode::LShift:
        case Opcode::RShift:
        case Opcode::URShift:
        case Opcode::Less:
        case Opcode::LessEq:
        case Opcode::Greater:
        case Opcode::GreaterEq:
            if ((value.child(0) == operand) == (value.child(1) == operand))
                return std::nullopt;
            // What may be anything takes the general way in every arm.
            if (!isNumericOperand(m_program.at(m_function.at(value.child(value.child(0) == operand)).type)))
                return std::nullopt;
            return index;
        default:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// How many of the values behind the read, which is of a field through a view, are casts of what comes right before them that nothing else uses.
unsigned Lowering::numberOfCastsFusedWithRead(size_t indexOfRead)
{
    return mayFuseWithRead(indexOfRead) ? numberOfCastsBehind(indexOfRead) : 0;
}

// How many of the values behind the one at the index are casts of what comes right before them that nothing else uses.
unsigned Lowering::numberOfCastsBehind(size_t index)
{
    auto& values = m_currentBlock->values;
    unsigned count = 0;
    for (size_t next = index + 1; next < values.size(); ++next, ++count) {
        auto& cast = m_function.at(values[next]);
        // What is defined as a cell is taken note of.
        if (cast.opcode != Opcode::Cast || cast.child(0) != values[next - 1] || numberOfUsesOf(values[next - 1]) != 1 || m_function.at(values[next - 1]).rep == Rep::Cell)
            break;
    }
    return count;
}

Operand Lowering::placeholder(Rep rep)
{
    switch (b3TypeOf(rep).kind()) {
    case Int32:
        return Operand(rep, m_out.int32Zero);
    case Double:
        return Operand(rep, m_out.doubleZero);
    default:
        return Operand(rep, m_out.int64Zero);
    }
}

// Of the cast at the index. Nothing: it is lowered by itself.
std::optional<Operand> Lowering::lowerFusedCast(size_t index)
{
    if (m_fusedCasts)
        return std::nullopt;
    auto& values = m_currentBlock->values;
    size_t indexOfRead = index;
    while (indexOfRead && m_function.at(values[indexOfRead]).opcode == Opcode::Cast && m_function.at(values[indexOfRead]).child(0) == values[indexOfRead - 1])
        --indexOfRead;
    // What the casts come behind is an operation that is itself carried out in the arms of a read.
    if (auto read = readFusedWithUse(indexOfRead)) {
        size_t indexOfUse = indexOfRead;
        unsigned behind = numberOfCastsFusedBehindUse(indexOfUse);
        if (index > indexOfUse + behind)
            return std::nullopt;
        if (index < indexOfUse + behind)
            return placeholder(m_function.at(values[index]).rep);
        return lowerReadFusedWithUse(*read, indexOfUse, behind);
    }
    unsigned count = numberOfCastsFusedWithRead(indexOfRead);
    if (!count || index > indexOfRead + count)
        return std::nullopt;
    if (index < indexOfRead + count || indexOfUseFusedWithRead(indexOfRead, count))
        return placeholder(m_function.at(values[index]).rep);
    m_fusedCasts = FusedCasts { indexOfRead, count, m_program.at(m_function.at(values[index]).type) };
    SetForScope currentIndex(m_currentIndexInBlock, indexOfRead);
    Operand result = lowerGetById(values[indexOfRead], m_function.at(values[indexOfRead]));
    m_fusedCasts = std::nullopt;
    return result;
}

// The read in whose arms the operation at the index is carried out: see indexOfUseFusedWithRead().
std::optional<Lowering::ReadFusedWithUse> Lowering::readFusedWithUse(size_t indexOfUse)
{
    auto& values = m_currentBlock->values;
    size_t indexOfOperand = indexOfUse;
    while (indexOfOperand && isConstant(m_function.at(values[indexOfOperand - 1]).opcode))
        --indexOfOperand;
    if (!indexOfOperand--)
        return std::nullopt;
    size_t indexOfRead = indexOfOperand;
    while (indexOfRead && m_function.at(values[indexOfRead]).opcode == Opcode::Cast && m_function.at(values[indexOfRead]).child(0) == values[indexOfRead - 1])
        --indexOfRead;
    unsigned count = numberOfCastsFusedWithRead(indexOfRead);
    if (indexOfRead + count != indexOfOperand || indexOfUseFusedWithRead(indexOfRead, count) != indexOfUse)
        return std::nullopt;
    return ReadFusedWithUse { indexOfRead, count };
}

// `const n: number = (t as O).op + 1`: a sum may be a text by its type, so it is boxed, and the cast behind it takes it apart again. A cast that only tests, for a type without objects that is within that of the operation, asks
// what ensure() asks at the end of the operation's own lowering: so the operation is lowered for that type.
unsigned Lowering::numberOfCastsFusedBehindUse(size_t indexOfUse)
{
    auto& values = m_currentBlock->values;
    auto& use = m_function.at(values[indexOfUse]);
    if (use.rep != Rep::JSValue)
        return 0;
    unsigned count = numberOfCastsBehind(indexOfUse);
    for (unsigned i = 1; i <= count; ++i) {
        auto& cast = m_function.at(values[indexOfUse + i]);
        auto& type = m_program.at(cast.type);
        bool onlyTests = cast.castReason() == CastReason::As || cast.castReason() == CastReason::Store || cast.castReason() == CastReason::Claim || cast.castReason() == CastReason::Flow || cast.castReason() == CastReason::Upcast;
        if (!onlyTests || cast.string() || type.hasObjects() || !type.isWithin(m_program.at(m_function.at(values[indexOfUse + i - 1]).type), m_program))
            return i - 1;
    }
    return count;
}

Operand Lowering::lowerReadFusedWithUse(ReadFusedWithUse read, size_t indexOfUse, unsigned numberOfCastsBehindUse)
{
    auto& values = m_currentBlock->values;
    // The other operand of an operation on bits becomes an int32 once, ahead of the arms. It is a number by its type: nothing sees when.
    size_t numberOfAssumptions = m_assumptions.size();
    auto& use = m_function.at(values[indexOfUse]);
    ValueID other = use.child(use.child(0) == values[read.indexOfRead + read.numberOfCasts]);
    bool isOnBits = use.opcode == Opcode::BitAnd || use.opcode == Opcode::BitOr || use.opcode == Opcode::BitXor || use.opcode == Opcode::LShift || use.opcode == Opcode::RShift || use.opcode == Opcode::URShift;
    if (isOnBits && !typeOf(other).isInteger())
        m_assumptions.append(Assumption { other, signExtended(toInt32(other)), &m_program.at(m_program.types.int32) });
    m_fusedCasts = FusedCasts { read.indexOfRead, read.numberOfCasts, m_program.at(m_function.at(values[indexOfUse + numberOfCastsBehindUse]).type), false, indexOfUse, numberOfCastsBehindUse };
    SetForScope currentIndex(m_currentIndexInBlock, read.indexOfRead);
    Operand result = lowerGetById(values[read.indexOfRead], m_function.at(values[read.indexOfRead]));
    m_fusedCasts = std::nullopt;
    m_assumptions.shrink(numberOfAssumptions);
    return result;
}

// Of the operation at the index. Nothing: it is lowered by itself.
std::optional<Operand> Lowering::lowerFusedUse(size_t index)
{
    if (m_fusedCasts)
        return std::nullopt;
    auto read = readFusedWithUse(index);
    if (!read)
        return std::nullopt;
    if (numberOfCastsFusedBehindUse(index))
        return placeholder(m_function.at(m_currentBlock->values[index]).rep);
    return lowerReadFusedWithUse(*read, index, 0);
}

// What an arm of the read has come by, which is of the type `from`.
Operand Lowering::carryOutFusedCasts(Operand operand, const Type& from)
{
    SetForScope areBeingCarriedOut(m_fusedCasts->areBeingCarriedOut, true);
    SetForScope isInArm(m_isInArm, true);
    // What is laid down in an arm that is rarely run is rarely run: whoever asks the block whether a call may take the form for cold code is told so.
    SetForScope frequency(m_out.m_frequency, m_out.m_block->frequency() ? m_out.m_frequency : 0);
    auto& values = m_currentBlock->values;
    size_t indexOfRead = m_fusedCasts->indexOfRead;
    size_t numberOfAssumptions = m_assumptions.size();
    const Type* known = &from;
    if (auto& typeOfRead = m_program.at(m_function.at(values[indexOfRead]).type); !from.isWithin(typeOfRead, m_program)) {
        operand = ensure(operand, from, typeOfRead);
        known = &typeOfRead;
    }
    m_assumptions.append(Assumption { values[indexOfRead], operand, known });
    for (unsigned i = 1; i <= m_fusedCasts->count; ++i) {
        ValueID id = values[indexOfRead + i];
        auto& cast = m_function.at(id);
        auto& to = m_program.at(cast.type);
        // `x as number`: a claim that it is a number or nil, and the conversion of nil right behind it. Neither makes anything of a number: one test, and both casts where that fails. Not in an arm that is rarely run: there it would only add code.
        if (i < m_fusedCasts->count && m_out.m_frequency && operand.rep == Rep::JSValue && cast.castReason() == CastReason::Claim && !cast.accessedName() && !cast.string()) {
            ValueID idOfNext = values[indexOfRead + i + 1];
            auto& next = m_function.at(idOfNext);
            auto& wanted = m_program.at(next.type);
            if (next.castReason() == CastReason::Coerce && !wanted.hasObjects() && !wanted.mayBeUndefinedOrNull() && wanted.isWithin(to, m_program) && !known->isWithin(wanted, m_program) && mayHaveValuesInCommon(*known, wanted)) {
                LBasicBlock otherwise = newColdBlock();
                Join join(m_out, repOf(wanted), b3TypeOf(repOf(wanted)));
                join.add(fitOrElse(operand, *known, wanted, otherwise));
                m_out.appendTo(otherwise);
                {
                    SetForScope currentIndex(m_currentIndexInBlock, indexOfRead + i);
                    m_assumptions.append(Assumption { id, lowerCast(id, cast), &to });
                }
                {
                    SetForScope currentIndex(m_currentIndexInBlock, indexOfRead + i + 1);
                    join.add(convert(lowerCast(idOfNext, next), wanted, repOf(wanted)));
                }
                m_assumptions.removeLast();
                operand = join.finish();
                known = &wanted;
                m_assumptions.append(Assumption { id, operand, known });
                m_assumptions.append(Assumption { idOfNext, operand, known });
                ++i;
                continue;
            }
        }
        // A cast of what is of its type and no object does nothing, unless it stands for an access. Then more stays known than its type says.
        bool turnsAwayNil = (cast.accessedName() || cast.castReason() == CastReason::NonNull) && known->mayBeUndefinedOrNull();
        if (!known->isWithin(to, m_program) || turnsAwayNil || (known->hasObjects() && cast.castReason() != CastReason::Upcast)) {
            SetForScope currentIndex(m_currentIndexInBlock, indexOfRead + i);
            operand = lowerCast(id, cast);
            known = &to;
        }
        m_assumptions.append(Assumption { id, operand, known });
    }
    if (m_fusedCasts->indexOfUse) {
        // Where a cast behind it fails, it fails as that cast does by itself: throwTypeMismatch() goes by the current value.
        size_t indexOfLast = *m_fusedCasts->indexOfUse + m_fusedCasts->countBehindUse;
        SetForScope currentIndex(m_currentIndexInBlock, indexOfLast);
        ValueID id = values[*m_fusedCasts->indexOfUse];
        auto& use = m_function.at(id);
        // An operation takes an operand as a value of its type is kept; an arm has it as the field keeps it.
        m_assumptions.append(Assumption { values[indexOfRead + m_fusedCasts->count], convert(operand, *known, repOf(*known)), known });
        known = &m_program.at(m_function.at(values[indexOfLast]).type);
        if (m_fusedCasts->countBehindUse)
            m_assumptions.append(Assumption { id, placeholder(use.rep), known });
        bool isRelational = use.opcode == Opcode::Less || use.opcode == Opcode::LessEq || use.opcode == Opcode::Greater || use.opcode == Opcode::GreaterEq;
        operand = isRelational ? Operand(lowerRelational(use)) : lowerBinary(id, use);
    }
    m_assumptions.shrink(numberOfAssumptions);
    return convert(operand, *known, repOf(m_fusedCasts->type));
}

// Which of the two tests of a view lowerCast() has the cast go by.
bool Lowering::usesCastTest(const Value& cast) const
{
    return cast.castReason() == CastReason::As || (cast.castReason() == CastReason::Flow && !cast.string() && narrowingMayRetag(m_program, m_program.at(cast.type)));
}

// The written cast whose work the value at the index does, where its own test of the shape fails. That value is the only use of the cast and comes right behind it, with nothing but Upcasts
// between; it is a cast that asserts, or a read of a field that some members lack. All the written cast does is to look for layouts that it turns into others where they are. And no layout
// that passes the test of the use is one of those: then the cast has nothing to do for an object that passes. By the IR and the program alone: the cast and its use both ask.
std::optional<Lowering::SunkCoercion> Lowering::sunkCoercionAt(size_t indexOfUse)
{
    // There every value is looked at by itself.
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || m_currentBlock->terminator.kind == TerminatorKind::Invoke)
        return std::nullopt;
    auto& values = m_currentBlock->values;
    auto& use = m_function.at(values[indexOfUse]);
    auto staticTypeOf = [&](ValueID id) -> const Type& {
        return m_program.at(m_function.at(id).type);
    };
    bool isRead = use.opcode == Opcode::GetById && !use.readsMethodFor();
    bool isCast = use.opcode == Opcode::Cast && !use.string()
        && (use.castReason() == CastReason::As || use.castReason() == CastReason::Flow || use.castReason() == CastReason::Store || use.castReason() == CastReason::Claim);
    if (!isRead && !isCast)
        return std::nullopt;
    size_t index = indexOfUse;
    ValueID operand = use.child(0);
    for (;; operand = m_function.at(operand).child(0)) {
        if (!index || values[index - 1] != operand || numberOfUsesOf(operand) != 1 || m_function.at(operand).opcode != Opcode::Cast)
            return std::nullopt;
        --index;
        if (m_function.at(operand).castReason() == CastReason::Coerce)
            break;
        if (m_function.at(operand).castReason() != CastReason::Upcast || isCast)
            return std::nullopt;
    }
    auto& cast = m_function.at(operand);
    auto& from = staticTypeOf(cast.child(0));
    auto& type = m_program.at(cast.type);
    auto& coercedTo = m_program.at(cast.coercedType());
    // Nothing is copied, nothing is converted, and where an object has its shape word a bigint has a hash.
    if (!holdsCells(repOf(type)) || from.anyObject || from.admitsForeignObjects(m_program) || !from.isWithin(type, m_program) || type.primitives & Primitive::BigInt
        || !coercionMayChangeObjects(m_program, from, coercedTo, cast.coercesFreshData()))
        return std::nullopt;
    SunkCoercion sunk { operand, { } };
    for (LayoutID view : coercedTo.views) {
        if (mayPass(from, m_program.at(view).retagSources))
            sunk.sources.append(&m_program.at(view).retagSources);
    }
    if (sunk.sources.isEmpty())
        return std::nullopt;

    Vector<const ShapeTest*, 4> tests;
    if (isRead) {
        const Field* field = fieldFor(staticTypeOf(use.child(0)), use.string());
        if (!field || !field->isLackedBySomeMember || field->membersWithIt.passesNothing())
            return std::nullopt;
        tests.append(&field->membersWithIt);
    } else {
        auto& to = m_program.at(use.type);
        if (to.views.isEmpty() || to.anyObject || to.admitsForeignObjects(m_program) || m_program.isHeldAlready(type, to, use.castReason()))
            return std::nullopt;
        for (LayoutID view : to.views)
            tests.append(usesCastTest(use) ? &m_program.at(view).castTest : &m_program.at(view).test);
    }
    // An object that has the field is read the same whether or not the cast has made anything of it.
    for (LayoutID view : isRead ? Vector<LayoutID, 1> { } : from.views) {
        for (LayoutID member : m_program.at(view).members) {
            uint32_t shape = m_program.at(member).shape;
            auto passesIt = [&](const ShapeTest* test) {
                return m_program.passes(*test, shape);
            };
            if (shape && std::ranges::any_of(tests, passesIt) && std::ranges::any_of(sunk.sources, passesIt))
                return std::nullopt;
        }
    }
    return sunk;
}

bool Lowering::coercionSinks(size_t indexOfCast)
{
    auto& values = m_currentBlock->values;
    size_t use = indexOfCast + 1;
    while (use < values.size() && m_function.at(values[use]).opcode == Opcode::Cast && m_function.at(values[use]).castReason() == CastReason::Upcast && m_function.at(values[use]).child(0) == values[use - 1])
        ++use;
    if (use >= values.size())
        return false;
    auto sunk = sunkCoercionAt(use);
    return sunk && sunk->cast == values[indexOfCast];
}

// All the written cast does is to look for layouts that it turns into others where they are, and all that is made of what it yields is a read of a field that every object it may be has.
bool Lowering::coercionIsOnlyReadThrough(size_t indexOfCast)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return false;
    auto& values = m_currentBlock->values;
    auto staticTypeOf = [&](ValueID id) -> const Type& {
        return m_program.at(m_function.at(id).type);
    };
    size_t use = indexOfCast + 1;
    for (;; ++use) {
        if (use >= values.size() || numberOfUsesOf(values[use - 1]) != 1 || m_function.at(values[use]).child(0) != values[use - 1])
            return false;
        if (m_function.at(values[use]).opcode != Opcode::Cast || m_function.at(values[use]).castReason() != CastReason::Upcast)
            break;
    }
    auto& read = m_function.at(values[use]);
    if (read.opcode != Opcode::GetById || read.readsMethodFor())
        return false;
    auto& cast = m_function.at(values[indexOfCast]);
    auto& from = staticTypeOf(cast.child(0));
    auto& type = m_program.at(cast.type);
    if (!holdsCells(repOf(type)) || from.anyObject || from.admitsForeignObjects(m_program) || !from.isWithin(type, m_program) || type.primitives & Primitive::BigInt)
        return false;
    auto& receiver = staticTypeOf(read.child(0));
    LayoutID view = receiver.onlyView();
    const Field* field = fieldFor(receiver, read.string());
    return view && field && !field->isLackedBySomeMember && !m_program.at(view).admitsForeignObjects;
}

void Lowering::branchOnTransitionSource(CellPointer cell, const SunkCoercion& sunk, FTL::WeightedTarget isSource, FTL::WeightedTarget isNoSource)
{
    Vector<ShapeTest, 4> sources;
    for (const ShapeTest* test : sunk.sources)
        sources.append(*test);
    branchOnPassingOneOf(shapeWordOf(cell), sources, isSource, isNoSource);
}

// Of an object that is a source.
void Lowering::carryOut(CellPointer cell, const SunkCoercion& sunk)
{
    auto& cast = m_function.at(sunk.cast);
    OperationEffects effects { writesOfCast(m_program, m_function, cast), true, castWritesShapeWords(m_program, m_function, cast) };
    AOT_TYPED_OPERATION_WITH_EFFECTS(CoerceInPlace, effects, box(cell), typeAndReason(cast.coercedType(), CastReason::Coerce));
}

// The test of this cast first. Where it fails: the written cast, then this one again, each as it is lowered by itself.
Operand Lowering::lowerCastBehindCoercion(ValueID result, const Value& cast, const SunkCoercion& sunk)
{
    auto& to = typeOf(result);
    Rep rep = repOf(to);
    LBasicBlock otherwise = newColdBlock();
    OperandJoin join(*this, joinedAs(to));
    {
        SetForScope useCastTest(m_useCastTest, usesCastTest(cast));
        join.add(fitOrElse(get(cast.child(0)), typeOf(cast.child(0)), to, otherwise));
    }
    m_out.appendTo(otherwise);
    SetForScope isCarryingOut(m_isCarryingOutSunkCoercion, true);
    SetForScope isInArm(m_isInArm, true);
    {
        // It changes the object where it is: what it yields is what it was given.
        SetForScope currentIndex(m_currentIndexInBlock, m_currentIndexInBlock - 1);
        lowerCoercion(sunk.cast, m_function.at(sunk.cast));
    }
    join.add(convert(lowerCast(result, cast), to, rep));
    return join.finish();
}

std::optional<Operand> Lowering::castByBody(const Value& cast, Operand operand, const Type& from, const Type& to)
{
    if (!isCompact() || (operand.rep != Rep::JSValue && operand.rep != Rep::Cell) || m_program.isHeldAlready(from, to, cast.castReason()))
        return std::nullopt;
    if (m_environmentAwaitingBarrier || m_isFillingArrayAtBirth || !m_fieldsAwaitingFirstStore.isEmpty())
        return std::nullopt;
    FunctionID body = m_program.bodyOfCast(cast);
    if (!body)
        return std::nullopt;
    auto& callee = m_program.at(body);
    if (callee.writes > m_function.writes || (callee.writesShapeWords && !m_function.writesShapeWords))
        return std::nullopt;
    return callCompiled(body, { Argument { operand, &from } }, to);
}

Operand Lowering::lowerCast(ValueID result, const Value& value)
{
    if (value.castReason() == CastReason::Upcast) {
        if (auto found = m_pinnedReads.find(value.child(0).index()); found != m_pinnedReads.end()) {
            PinnedRead read = found->value;
            m_pinnedReads.set(result.index(), read);
            return placeholder(repOf(typeOf(result)));
        }
    }
    if (handlerEnteredByBranch()) {
        auto& written = value.inlineFrame ? m_program.at(m_function.inlineFrames[value.inlineFrame].callee) : m_function;
        bool isOpenedForLog = m_program.logsTypeErrors && (written.isGenericEntrypoint() || written.has(Format::FunctionIsHiddenInStackTraces) || written.has(FunctionLogsMisfits)) && typeOf(result).admitsForeignObjects(m_program);
        SetForScope asIfClosed(m_foreignObjectsDoNotFit, m_foreignObjectsDoNotFit || isOpenedForLog);
        SetForScope isHolding(m_isHoldingToTypeOpenedForLog, isOpenedForLog);
        return ensure(get(value.child(0)), typeOf(value.child(0)), typeOf(result));
    }
    if (m_cloneGroup && m_cloneGroup->cast == m_currentIndexInBlock)
        return lowerCloneGroup(*std::exchange(m_cloneGroup, std::nullopt));
    if (m_mergeGroup && m_mergeGroup->first + 2 == m_currentIndexInBlock)
        return lowerMergeGroup(*std::exchange(m_mergeGroup, std::nullopt));
    if (auto fused = lowerFusedCast(m_currentIndexInBlock))
        return *fused;
    // But for what stands for the access of a property, which throws as the access does.
    if (Options::auditAOTTypedAssertions() && value.castReason() != CastReason::Upcast && !value.accessedName()) [[unlikely]]
        return lowerAuditedCast(result, value);
    if (!m_isCarryingOutSunkCoercion) {
        if (auto sunk = sunkCoercionAt(m_currentIndexInBlock))
            return lowerCastBehindCoercion(result, value, *sunk);
    }
    Operand operand = get(value.child(0));
    auto& from = typeOf(value.child(0));
    auto& to = typeOf(result);
    switch (value.castReason()) {
    case CastReason::Upcast:
        return convert(operand, from, repOf(to));
    case CastReason::As:
        if (auto called = castByBody(value, operand, from, to))
            return *called;
        return coerce(operand, from, value);
    case CastReason::Coerce:
        return lowerCoercion(result, value);
    case CastReason::CoerceFreshObject:
    case CastReason::CoerceFreshGraph:
    case CastReason::AsFreshObject:
        RELEASE_ASSERT_NOT_REACHED();
    case CastReason::NonNull:
        if (m_program.isOpenedForLog(to)) [[unlikely]]
            return coerce(operand, from, value);
        return lowerNonNull(result, value);
    case CastReason::Flow:
        if (!value.string() && (narrowingMayRetag(m_program, to) || m_program.isOpenedForLog(to)))
            return coerce(operand, from, value);
        break;
    case CastReason::Store:
    case CastReason::Claim:
        if (!value.string() && m_program.isOpenedForLog(to)) [[unlikely]]
            return coerce(operand, from, value);
        break;
    }
    // The receiver of a call that was resolved, with the name of what is called: what is left of the read of the property. See resolveCalls() and resolveBuiltins().
    if (StringID name = value.string(); name && !(from.primitives & ~(cellPrimitives | Primitive::UndefinedOrNull)) && from.withoutPrimitives(Primitive::UndefinedOrNull).isWithin(to, m_program))
        return convert(nonNil(operand, from, name), to, repOf(to));
    if (StringID name = value.string(); name && !from.hasObjects() && from.mayBeUndefinedOrNull() && from.primitives & (Primitive::Number | Primitive::Boolean)) {
        LBasicBlock isNil = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(lowerIsUndefinedOrNull(value.child(0)).value(), rarely(isNil), usually(continuation));
        m_out.appendTo(isNil);
        throwNullish(in<Boxed>(value.child(0)), name, { });
        m_out.appendTo(continuation);
        return ensure(operand, from, to);
    }
    if (auto* engineClass = to.hasOnlyPrimitives(Primitive::String) ? &m_builtins.stringClass() : to.onlyEngineClass(); engineClass && !value.string() && answerOfGuardAhead(value.child(0), *engineClass, AskedFor::Class).value_or(false))
        return convert(operand, to, repOf(to));
    // What was made here is an object, whatever its type says: in a JavaScript function that is anything.
    if (Opcode madeBy = m_function.at(value.child(0)).opcode; to.anyObject && !value.string() && (madeBy == Opcode::NewObject || madeBy == Opcode::NewObjectWithShape || madeBy == Opcode::NewArray))
        return convert(operand, to, repOf(to));
    if (auto called = castByBody(value, operand, from, to))
        return *called;
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
        return logicalNot(isCell(value, type));
    switch (type.primitives & Primitive::UndefinedOrNull) {
    case Primitive::Undefined:
        return haveEqualBits(value, undefined());
    case Primitive::Null:
        return haveEqualBits(value, boxedConstant(jsNull()));
    default:
        return isUndefinedOrNull(value, type);
    }
}

// What may be a number and a bigint may be about anything.
bool Lowering::toBooleanTakesStub(const Type& type)
{
    return repOf(type) == Rep::JSValue && type.primitives & Primitive::Number && type.primitives & Primitive::BigInt;
}

Word32 Lowering::isTruthy(CellPointer cell, const Type& type)
{
    // There is one empty string, and nothing else is it.
    Word32 isNotEmptyString = type.primitives & Primitive::String ? notEqual(cell, CellPointer(emptyString())) : boolean(true);
    if (!(type.primitives & Primitive::BigInt))
        return isNotEmptyString;
    return either<Word32>(equal(typeOfCell(cell), word32(HeapBigIntType)), [&] {
        return logicalNot(isZero(loadField<Word32>(cell, m_heaps.JSBigInt_length)));
    }, [&] {
        return isNotEmptyString;
    });
}

// By what the value can be.
Word32 Lowering::isTruthy(Boxed value, const Type& type)
{
    if (type.primitives & Primitive::Number) {
        Type rest = type.withoutPrimitives(Primitive::Number);
        Join join(m_out, Rep::Int32, Int32);
        // Goes on where the condition does not hold.
        auto when = [&](Word32 condition, const auto& answer) {
            LBasicBlock holds = m_out.newBlock();
            LBasicBlock next = m_out.newBlock();
            m_out.branch(condition.value(), unsure(holds), unsure(next));
            m_out.appendTo(holds);
            join.add(answer());
            m_out.appendTo(next);
        };
        when(isInt32(value), [&] {
            return logicalNot(isZero(low32(value)));
        });
        // The engine may keep an integral number as a double.
        auto ofDouble = [&] {
            return greaterThan(abs(unboxedDouble(value)), number(0));
        };
        if (rest.isNever())
            join.add(ofDouble());
        else {
            when(isNumber(value), ofDouble);
            join.add(isTruthy(value, rest));
        }
        return join.finish().as<Word32>();
    }
    Word32 isTrue = type.primitives & Primitive::True ? equal(value, boxedConstant(jsBoolean(true))) : boolean(false);
    if (!type.hasCells())
        return isTrue;
    if (!(type.primitives & ~(Primitive::String | Primitive::Symbol | Primitive::BigInt)))
        return isTruthy(asCell(value), type);
    if (!(type.primitives & (Primitive::String | Primitive::BigInt))) {
        if (!(type.primitives & Primitive::True))
            return isCell(value, type);
        // `true` has the lowest bit, a pointer has neither of the lowest two, and what is falsy has the second alone.
        static_assert(JSValue::ValueTrue & 1 && !(JSValue::ValueFalse & 1) && !(JSValue::ValueNull & 1) && !(JSValue::ValueUndefined & 1));
        return Word32(m_out.notEqual(m_out.bitAnd(value.value(), m_out.constInt64(3)), m_out.constInt64(JSValue::OtherTag)));
    }
    return either<Word32>(isCell(value, type), [&] {
        return isTruthy(asCell(value), type);
    }, [&] {
        return isTrue;
    });
}

// A block parameter that joins a boolean or a number with something else is kept boxed: each edge boxes what it passes, and whoever asks whether the parameter is truthy takes it
// apart again. Each edge knows more of what it passes. So it says that too.
// The fetch of a String or a BuiltinFunction is a load, a test and a call that may allocate. Whoever asks for an operand may do so behind an allocation of its own, with the fresh cell not yet written. So the fetch is left to
// get() only where every use is known to do without the cell, or to ask for it ahead of everything else and to store it to no cell it has made. Otherwise the leaf is fetched where it stands.
void Lowering::findLeavesFetchedLate()
{
    auto isLeaf = [&](ValueID id) {
        Opcode opcode = m_function.at(id).opcode;
        return opcode == Opcode::String || opcode == Opcode::BuiltinFunction;
    };
    BitVector hasOtherUse;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& user = m_function.at(id);
            ValueID late;
            switch (user.opcode) {
            case Opcode::GetByVal:
            case Opcode::PutByVal:
            case Opcode::InByVal:
            case Opcode::DeleteByVal:
                late = user.child(1);
                break;
            case Opcode::CallBuiltin:
                if (isDefineField(user) || isAccessorDefinition(user))
                    late = user.arguments[1];
                else if (looksForConstantWithoutItsCell(user) || isErrorThrownWhereMade(block, id))
                    late = user.arguments[0];
                break;
            default:
                break;
            }
            unsigned timesLate = 0;
            user.forEachChild([&](ValueID child) {
                if (!isLeaf(child) || user.opcode == Opcode::StrictEq)
                    return;
                // In one position only.
                if (child != late || timesLate++)
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
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            if (isLeaf(id) && !hasOtherUse.get(id.index()))
                m_isFetchedLate.set(id.index());
        }
    }
}

void Lowering::findTruthinessPhis()
{
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            if (m_function.at(id).opcode != Opcode::ToBoolean)
                continue;
            ValueID asked = m_function.at(id).child(0);
            auto& type = m_program.at(m_function.at(asked).type);
            if (m_function.at(asked).opcode == Opcode::Parameter && m_function.at(asked).rep == Rep::JSValue && type.primitives & (Primitive::True | Primitive::Number) && !toBooleanTakesStub(type))
                m_hasTruthinessPhi.set(asked.index());
        }
    }
    if (m_hasTruthinessPhi.isEmpty())
        return;
    for (ValueID id : m_function.blocks[0].parameters)
        m_hasTruthinessPhi.clear(id.index());
    // No edge calls the stub for an answer that may not be asked for.
    for (auto& block : m_function.blocks) {
        for (auto& target : block.terminator.targets) {
            auto& parameters = m_function.at(target.block).parameters;
            for (unsigned i = 0; i < target.arguments.size() && i < parameters.size(); ++i) {
                if (toBooleanTakesStub(m_program.at(m_function.at(target.arguments[i]).type)))
                    m_hasTruthinessPhi.clear(parameters[i].index());
            }
        }
    }
}

// A block parameter that is a cell, `undefined` or `null` by its type is joined in parts (see Parts) where some edge has parts to pass and no use needs the boxed value.
// `while (node) node = node.parent`.
// Has parts: a read of a field, cast or not, and a parameter that is itself joined in parts.
// Needs the boxed value: every use but a cast, a test for nil or truthiness, and the receiver of an access by name; and an edge to a parameter that is not joined in parts.
// A box behind the join is paid for on every path. Ahead of it, only on the paths with a read.
void Lowering::findParametersJoinedInParts()
{
    BitVector isNeverBoxed;
    for (unsigned i = 1; i < m_function.blocks.size(); ++i) {
        for (ValueID id : m_function.blocks[i].parameters) {
            auto& parameter = m_function.at(id);
            if (parameter.rep == Rep::JSValue && joinedAs(m_program.at(parameter.type)).isReference)
                isNeverBoxed.set(id.index());
        }
    }
    if (isNeverBoxed.isEmpty())
        return;
    auto originOf = [&](ValueID id) {
        while (m_function.at(id).opcode == Opcode::Cast)
            id = m_function.at(id).child(0);
        return id;
    };
    auto takeBoxed = [&](ValueID id) {
        if (id)
            isNeverBoxed.clear(originOf(id).index());
    };
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& user = m_function.at(id);
            if (user.opcode == Opcode::Cast || user.opcode == Opcode::ToBoolean || user.opcode == Opcode::IsUndefinedOrNull)
                continue;
            if (user.opcode == Opcode::StrictEq && (m_function.at(user.child(0)).opcode == Opcode::Null || m_function.at(user.child(1)).opcode == Opcode::Null))
                continue;
            bool hasReceiver = user.opcode == Opcode::GetById || user.opcode == Opcode::PutById;
            for (unsigned k = hasReceiver; k < user.children.size(); ++k)
                takeBoxed(user.children[k]);
            for (ValueID argument : user.arguments)
                takeBoxed(argument);
        }
        takeBoxed(block.terminator.value);
    }
    auto forEachEdge = [&](auto&& functor) {
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets) {
                auto& parameters = m_function.at(target.block).parameters;
                for (unsigned i = 0; i < target.arguments.size() && i < parameters.size(); ++i)
                    functor(originOf(target.arguments[i]), parameters[i]);
            }
        }
    };
    for (bool changed = true; changed;) {
        changed = false;
        forEachEdge([&](ValueID passed, ValueID parameter) {
            if (isNeverBoxed.get(passed.index()) && !isNeverBoxed.get(parameter.index())) {
                isNeverBoxed.clear(passed.index());
                changed = true;
            }
        });
    }
    forEachEdge([&](ValueID passed, ValueID parameter) {
        Opcode opcode = m_function.at(passed).opcode;
        if (isNeverBoxed.get(parameter.index()) && (opcode == Opcode::GetById || opcode == Opcode::GetField))
            m_isJoinedInParts.set(parameter.index());
    });
    for (bool changed = true; changed;) {
        changed = false;
        forEachEdge([&](ValueID passed, ValueID parameter) {
            if (m_isJoinedInParts.get(passed.index()) && !m_isJoinedInParts.get(parameter.index())) {
                RELEASE_ASSERT(isNeverBoxed.get(parameter.index()));
                m_isJoinedInParts.set(parameter.index());
                changed = true;
            }
        });
    }
}

Word32 Lowering::lowerToBoolean(ValueID id)
{
    auto& type = typeOf(id);
    Operand operand = get(id);
    // Unless a test has shown more of the value.
    if (auto known = m_truthinessPhis.get(id.index()); known.isTruthy && operand.value == known.value)
        return Word32(known.isTruthy);
    // A cast that converts nothing has boxed what was kept some other way: `a && b` passes `a` on as `false|number`. A read and a cast may have yielded a placeholder: see
    // lowerFusedCast().
    if (auto& value = m_function.at(id); operand.rep == Rep::JSValue && value.opcode == Opcode::Cast && value.castReason() != CastReason::Coerce) {
        auto& from = m_function.at(value.child(0));
        if (from.rep != Rep::JSValue && from.opcode != Opcode::Cast && from.opcode != Opcode::GetById)
            return lowerToBoolean(value.child(0));
    }
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
        return isTruthy(operand.as<CellPointer>(), type);
    case Rep::JSValue:
        if (toBooleanTakesStub(type))
            break;
        return isTruthy(operand.as<Boxed>(), type);
    case Rep::Void:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return toBooleanGenerally(id);
}

// Whether an object of the program that is of the type may inherit from anything, the prototype object of a class among that: see
// Format::ShapeMayHaveOtherPrototype. What has the layout of a class inherits from the prototype object of that class, whatever extends it.
static bool admitsRecordsWithOtherPrototype(const Program& program, const Type& type)
{
    auto isSuch = [](const Layout& layout) {
        return layout.kind == LayoutKind::Record && layout.mayHaveOtherPrototype;
    };
    if (type.anyObject)
        return std::ranges::any_of(program.layouts, isSuch);
    return std::ranges::any_of(type.views, [&](LayoutID view) {
        return std::ranges::any_of(program.at(view).members, [&](LayoutID member) {
            return isSuch(program.at(member));
        });
    });
}

bool mayWalkPrototypeChain(const Program& program, const Function& function, const Value& value)
{
    auto& type = program.typeOf(function, value.child(0));
    return type.anyObject || type.anyFunction || !type.engineClasses.isEmpty() || type.admitsForeignObjects(program) || program.at(value.layout()).instancesMayBecomeRecords || admitsRecordsWithOtherPrototype(program, type);
}

// An object of the program keeps its prototype, so its shape says what it is an instance of, but for those records. The chain of any other
// object is walked.
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
        Word32 isInstance = passes(shapeWord, m_program.at(value.layout()).instances);
        // Then a shape that is not of the class says nothing.
        if (m_program.at(value.layout()).instancesMayBecomeRecords || admitsRecordsWithOtherPrototype(m_program, type)) {
            return either<Word32>(isInstance, [&] {
                return boolean(true);
            }, [&] {
                return AOT_TYPED_OPERATION(InheritsFromClass, cell, word32(value.layout().index()));
            });
        }
        if (hasOnlyObjectsOfTheProgram)
            return isInstance;
        Word32 hasShape = passes(shapeWord, ShapeTest { 1, m_program.numberOfShapes });
        return either<Word32>(hasShape, [&] {
            return isInstance;
        }, [&] {
            return AOT_TYPED_OPERATION(InheritsFromClass, cell, word32(value.layout().index()));
        });
    };
    if (operand.rep == Rep::Cell)
        return test(operand.as<CellPointer>());
    Boxed boxed = operand.as<Boxed>();
    return either<Word32>(isCell(boxed, typeOf(value.child(0))), [&] {
        return test(asCell(boxed));
    }, [&] {
        return boolean(false);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
