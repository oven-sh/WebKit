/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "B3ArgumentRegValue.h"
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

Word32 Lowering::loadCellType(CellPointer cell)
{
    return loadByte(cell, m_heaps.JSCell_typeInfoType, IsFixed | safeIfReaches(cell, sizeof(JSCell)));
}

bool Lowering::mayHaveCalledSince(LBasicBlock block, unsigned index, unsigned numberOfBlocks)
{
    auto mayCall = [](B3::Value* value) {
        if (value->opcode() == B3::CCall)
            return true;
        if (value->opcode() != B3::Patchpoint)
            return false;
        auto& effects = value->as<PatchpointValue>()->effects;
        return effects.terminal || effects.exitsSideways || effects.controlDependent || effects.writesLocalState || effects.readsLocalState || effects.writesPinned || effects.readsPinned || effects.fence || effects.writes || effects.reads;
    };
    for (unsigned i = index; i < block->size(); ++i) {
        if (mayCall(block->at(i)))
            return true;
    }
    for (unsigned i = numberOfBlocks; i < m_proc.size(); ++i) {
        if (!m_proc[i] || (m_proc[i]->size() && m_proc[i]->last()->opcode() == B3::Oops))
            continue;
        for (B3::Value* value : *m_proc[i]) {
            if (mayCall(value))
                return true;
        }
    }
    return false;
}

Word32 Lowering::shapeWordOf(CellPointer cell)
{
    static_assert(offsetOfShapeWord + sizeof(uint32_t) <= MarkedBlock::atomSize);
    unsigned flags = safeIfReaches(cell, offsetOfShapeWord + sizeof(uint32_t)) | (m_program.castsMayRetag ? 0 : IsFixed);
    Word32 word = loadField<Word32>(cell, m_heaps.AOTTypedObject_shapeWord, flags);
    m_shapeWordCells.add(word.value(), cell.value());
    return word;
}

Word32 Lowering::blockHalfwordOf(CellPointer cell)
{
    unsigned flags = safeIfReaches(cell, offsetOfShapeWord + sizeof(uint32_t)) | (m_program.castsMayRetag ? 0 : IsFixed);
#if CPU(X86_64)
    return Word32(marked(m_out.load16ZeroExt32(m_out.address(cell.value(), m_heaps.AOTTypedObject_shapeWord, 1)), flags));
#else
    return Word32(marked(m_out.load16SignExt32(m_out.address(cell.value(), m_heaps.AOTTypedObject_shapeWord, 1)), flags));
#endif
}

Word32 Lowering::orderWordOf(CellPointer cell)
{
    return loadField<Word32>(cell, m_heaps.AOTTypedObject_orderWord, safeIfReaches(cell, offsetOfOrderWord + sizeof(uint32_t)));
}

Word32 Lowering::passes(Word32 shapeWord, const ShapeTest& test, LBasicBlock otherwise)
{
    if (test.isRange() && test.count > 1 && !test.blockByte && test.first == 1)
        return aboveOrEqual(shapeWord, word32(0u - test.last()));
    return passes(shapeWord, MergedShapeTest { test, !test.isRange() && test.count <= 64 ? std::optional(bitsOf(test)) : std::nullopt }, otherwise);
}

uint64_t Lowering::bitsOf(const ShapeTest& test) const
{
    RELEASE_ASSERT(!test.isRange() && test.count <= 64);
    uint64_t bits = 0;
    for (uint32_t i = 0; i < test.count; ++i)
        bits |= static_cast<uint64_t>(m_program.shapeSetBits.get(test.set + i)) << i;
    return bits;
}

const Vector<uint32_t>& Lowering::possibleShapesOf(const Type& type)
{
    bool acceptsOtherCells = type.anyObject || type.anyFunction || !type.engineClasses.isEmpty() || type.primitives & cellPrimitives;
    if (!acceptsOtherCells && type.objectLayouts.size() > 1 && type.objectLayouts == m_knownTypeShapeLayouts)
        return m_knownTypeShapes;
    m_knownTypeShapes.shrink(0);
    m_knownTypeShapeLayouts.shrink(0);
    if (acceptsOtherCells)
        return m_knownTypeShapes;
    if (type.objectLayouts.size() == 1)
        return m_program.at(type.objectLayouts[0]).subtypeShapes;
    for (LayoutID supertype : type.objectLayouts) {
        auto& shapes = m_program.at(supertype).subtypeShapes;
        if (shapes.isEmpty()) {
            m_knownTypeShapes.shrink(0);
            return m_knownTypeShapes;
        }
        m_knownTypeShapes.appendVector(shapes);
    }
    std::ranges::sort(m_knownTypeShapes);
    m_knownTypeShapes.shrink(std::ranges::unique(m_knownTypeShapes).begin() - m_knownTypeShapes.begin());
    m_knownTypeShapeLayouts = type.objectLayouts;
    return m_knownTypeShapes;
}

std::optional<Word32> Lowering::resolveWithinKnownShapes(Word32 shapeWord, std::span<const MergedShapeTest> tests)
{
    if (!m_knownCellType || m_isValidatingFact)
        return std::nullopt;
    auto cell = m_shapeWordCells.find(shapeWord.value());
    if (cell == m_shapeWordCells.end() || cell->value != m_knownTypeCell)
        return std::nullopt;
    auto& shapes = possibleShapesOf(*m_knownCellType);
    auto passes = [&](uint32_t shape) -> bool {
        for (auto& [test, bits] : tests) {
            uint32_t place = test.last() - shape;
            if (bits ? place < test.count && (*bits >> place & 1) : m_program.passes(test, shape))
                return true;
        }
        return false;
    };
    uint32_t minShape = UINT32_MAX;
    uint32_t maxShape = 0;
    for (auto& merged : tests) {
        if (merged.test.neverPasses())
            continue;
        minShape = std::min(minShape, merged.test.first);
        maxShape = std::max(maxShape, merged.test.last());
    }
    size_t first = notFound;
    size_t last = 0;
    size_t count = 0;
    size_t end = std::ranges::upper_bound(shapes, maxShape) - shapes.begin();
    for (size_t i = std::ranges::lower_bound(shapes, minShape) - shapes.begin(); i < end; ++i) {
        if (!passes(shapes[i]))
            continue;
        if (first == notFound)
            first = i;
        last = i;
        ++count;
    }
    if (!count || count != last - first + 1)
        return std::nullopt;
    if (count == shapes.size())
        return boolean(true);
    bool isOneRange = tests.size() == 1 && tests[0].test.isRange() && !tests[0].bits;
    if (isOneRange && (tests[0].test.count == 1 || tests[0].test.blockByte || tests[0].test.first == 1))
        return std::nullopt;
    if (!first)
        return aboveOrEqual(shapeWord, word32(0u - shapes[last]));
    if (last + 1 == shapes.size())
        return belowOrEqual(shapeWord, word32(0u - shapes[first]));
    if (count == 1)
        return equal(shapeWord, word32(0u - shapes[first]));
    if (isOneRange)
        return std::nullopt;
    return below(add(shapeWord, word32(shapes[last])), word32(shapes[last] - shapes[first] + 1));
}

std::optional<Word32> Lowering::passesWithinKnownShapes(Word32 shapeWord, std::span<const MergedShapeTest> tests)
{
    auto answer = resolveWithinKnownShapes(shapeWord, tests);
    if (!answer || !Options::validateAOTTypedFacts()) [[likely]]
        return answer;
    CellPointer cell(m_knownTypeCell);
    SetForScope knownCellType(m_knownCellType, nullptr);
    Word32 asRequested;
    for (auto& test : tests)
        asRequested = logicalOr(asRequested, passes(shapeWord, test));
    LBasicBlock agrees = m_out.newBlock();
    LBasicBlock differs = newColdBlock();
    m_out.branch(equal(*answer, asRequested).value(), usually(agrees), rarely(differs));
    m_out.appendTo(differs);
    {
        SetForScope isReporting(m_isReportingFactViolation, true);
        AOT_TYPED_OPERATION(FactViolated, box(cell), word32(m_id.index()), word32(m_currentBlock->values.isEmpty() ? 0 : m_currentBlock->values[std::min<size_t>(m_currentIndexInBlock, m_currentBlock->values.size() - 1)].index()));
    }
    m_out.jump(agrees);
    m_out.appendTo(agrees);
    return answer;
}

Word32 Lowering::passes(Word32 shapeWord, const MergedShapeTest& merged, LBasicBlock otherwise)
{
    auto& test = merged.test;
    if (test.neverPasses())
        return boolean(false);
    if (auto answer = passesWithinKnownShapes(shapeWord, std::span { &merged, 1 }))
        return *answer;
    if (test.isRange() && !merged.bits && test.count == 1)
        return equal(shapeWord, word32(0u - test.last()));
    if (test.isRange() && !merged.bits && test.blockByte) {
        auto found = m_shapeWordCells.find(shapeWord.value());
        if (found != m_shapeWordCells.end()) {
            Word32 expected = word32((isX86_64() ? 0xff00u : 0xffffff00u) | test.blockByte);
            return test.passesEarlierBlocks ? aboveOrEqual(blockHalfwordOf(CellPointer(found->value)), expected) : equal(blockHalfwordOf(CellPointer(found->value)), expected);
        }
    }
    Word32 place = add(shapeWord, word32(test.last()));
    if (test.isRange() && !merged.bits)
        return below(place, word32(test.count));

    Word32 isWithinReach = below(place, word32(test.count));
    if (otherwise)
        orElse(isWithinReach, otherwise);
    Word32 hasBit;
    if (merged.bits) {
        uint64_t bits = *merged.bits;
        if (test.count <= 32)
            hasBit = hasBits(shiftRightUnsigned(word32(static_cast<uint32_t>(bits)), place), 1);
        else
            hasBit = hasBits(low32(Integer(m_out.lShr(integer(static_cast<int64_t>(bits)).value(), place.value()))), 1);
    } else {
        Word32 index = otherwise ? place : select(isWithinReach, place, word32(0));
        Pointer words(m_out.add(fixedPointer(Instance::offsetOfTypedShapeSets()), m_out.constIntPtr(test.set / 8)));
        Integer wordIndex = zeroExtended(shiftRightUnsigned(index, word32(5)));
        Word32 word(marked(m_out.load32(TypedPointer(m_heaps.AOTTyped_shapeSets, offsetBy(words, wordIndex, 2).value())), otherwise ? 0 : IsSafe));
        hasBit = hasBits(shiftRightUnsigned(word, index), 1);
    }
    return otherwise ? hasBit : logicalAnd(isWithinReach, hasBit);
}

static unsigned rangeCost(uint32_t count)
{
    return count == 1 ? 2 : 3;
}

static unsigned bitsCost(uint64_t bits)
{
    unsigned halfwords = 0;
    for (unsigned i = 0; i < 64; i += 16)
        halfwords += !!(bits >> i & 0xffff);
    return 5 + halfwords;
}

static unsigned costOf(const ShapeTest& test, const Program& program)
{
    if (test.isRange())
        return rangeCost(test.count);
    if (test.count > 64)
        return 9;
    uint64_t bits = 0;
    for (uint32_t i = 0; i < test.count; ++i)
        bits |= static_cast<uint64_t>(program.shapeSetBits.get(test.set + i)) << i;
    return bitsCost(bits);
}

Vector<Lowering::MergedShapeTest, 4> Lowering::merged(Vector<ShapeTest, 4> given) const
{
    given.removeAllMatching([](auto& test) {
        return test.neverPasses();
    });
    std::ranges::sort(given, { }, &ShapeTest::first);
    Vector<ShapeTest, 4> tests;
    for (auto& test : given) {
        if (!tests.isEmpty() && tests.last().isRange() && test.isRange() && (test.first <= tests.last().first + tests.last().count || !m_program.shapeCountInRange(tests.last().first + tests.last().count, test.first - 1))) {
            tests.last().count = std::max(tests.last().count, test.first + test.count - tests.last().first);
            tests.last().blockByte = 0;
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
            RELEASE_ASSERT(bits & 1 && bits >> (count - 1) == 1);
            bool isRange = !(bits & (bits + 1));
            if ((isRange ? rangeCost(count) : bitsCost(bits)) < oneByOne) {
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

Word32 Lowering::passesAnyOf(Word32 shapeWord, const Vector<ShapeTest, 4>& tests, LBasicBlock otherwise)
{
    auto all = merged(tests);
    if (all.size() == 1)
        return passes(shapeWord, all[0], otherwise);
    if (auto answer = passesWithinKnownShapes(shapeWord, all.span()))
        return *answer;
    Word32 result;
    for (auto& test : all)
        result = logicalOr(result, passes(shapeWord, test));
    return result ? result : boolean(false);
}

Word32 Lowering::passesAnyOf(Word32 shapeWord, const Vector<LayoutID, 1>& objectLayouts, LBasicBlock otherwise)
{
    Vector<ShapeTest, 4> tests;
    for (LayoutID supertype : objectLayouts)
        tests.append(m_useCastTest ? m_program.at(supertype).castTest : m_program.at(supertype).test);
    if (auto* test = objectLayouts.size() > 1 ? m_program.findMultiLayoutShapeTest(objectLayouts, m_useCastTest ? Program::ShapeTestKind::SubtypesKeptByCast : Program::ShapeTestKind::Members) : nullptr) {
        unsigned oneByOne = 0;
        for (auto& merged : merged(tests))
            oneByOne += merged.bits ? bitsCost(*merged.bits) : costOf(merged.test, m_program);
        if (costOf(*test, m_program) < oneByOne)
            return passes(shapeWord, *test, otherwise);
    }
    return passesAnyOf(shapeWord, tests, otherwise);
}

void Lowering::combineRetagSourceTests(Vector<ShapeTest, 4>& sources, const Vector<LayoutID, 1>& objectLayouts) const
{
    auto* test = sources.size() > 1 ? m_program.findMultiLayoutShapeTest(objectLayouts, Program::ShapeTestKind::RetagSources) : nullptr;
    if (!test)
        return;
    unsigned oneByOne = 0;
    for (auto& merged : merged(sources))
        oneByOne += merged.bits ? bitsCost(*merged.bits) : costOf(merged.test, m_program);
    if (costOf(*test, m_program) < oneByOne)
        sources = { *test };
}

void Lowering::branchOnAnyShapeTest(Word32 shapeWord, const Vector<ShapeTest, 4>& tests, FTL::WeightedTarget passing, FTL::WeightedTarget failing)
{
    auto all = merged(tests);
    if (all.isEmpty()) {
        m_out.jump(failing.target());
        return;
    }
    if (auto answer = all.size() > 1 ? passesWithinKnownShapes(shapeWord, all.span()) : std::nullopt) {
        m_out.branch(answer->value(), passing, failing);
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

Word32 Lowering::lowerIsClassObjectUnmodified(const Value& value)
{
    if (value.testsPinnedField()) {
        auto& field = m_program.at(value.layout()).fields[value.immediates[1]];
        if (!field.isPinnedToFunction || !field.slot || !m_program.at(field.slot).hasPinnedFields)
            return boolean(false);
        return isZero(Word32(m_out.load8ZeroExt32(pinnedFieldsGuard(field.slot))));
    }
    return isZero(Word32(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTInstance_mutableFields, fixedPointer(Instance::offsetOfTypedModifiedClassObjects()), value.layout().index()))));
}

void Lowering::accessStaticField(const Value& value, const Invocable<void(CellPointer classObject, TypedPointer slot)> auto& inlinePath, const Invocable<void(Word32 ofClass, Word32 field)> auto& genericPath)
{
    auto& layout = m_program.at(value.layout());
    uint32_t place = layout.staticFields[value.immediates[1]].place;
    auto word = [&](uint32_t number) {
        return inStatics(m_heaps.AOTInstance_mutableFields, m_program.staticFieldSlotsOffset + number * sizeof(int32_t));
    };
    LBasicBlock isReady = m_out.newBlock();
    LBasicBlock asks = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue distance = m_out.load32(word(place));
    m_out.branch(m_out.notZero32(distance), usually(isReady), rarely(asks));
    m_out.appendTo(isReady);
    CellPointer classObject(m_out.load64(word(layout.classObjectSlot)));
    LValue butterfly = m_out.loadPtr(classObject.value(), m_heaps.JSObject_butterfly);
    inlinePath(classObject, TypedPointer(m_heaps.properties.atAnyNumber(), m_out.add(butterfly, m_out.signExt32ToPtr(distance))));
    m_out.jump(continuation);
    m_out.appendTo(asks);
    genericPath(word32(value.layout().index()), word32(place - layout.classObjectSlot - 2));
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

Word32 Lowering::lowerHasShapeOf(const Value& value)
{
    auto& layout = m_program.at(value.layout());
    Operand operand = get(value.child(0));
    auto test = [&](CellPointer cell) {
        Word32 hasShape = passes(shapeWordOf(cell), value.immediates[1] == shapeTestIsExact ? ShapeTest { layout.shape, layout.shape ? 1u : 0u } : layout.test);
        if (value.immediates[1] != shapeTestIsForThisInstance)
            return hasShape;
        return either<Word32>(hasShape, [&] {
            return belongsToThisInstance(cell);
        }, [&] {
            return boolean(false);
        });
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

Word32 Lowering::isClosureOf(CellPointer cell, FunctionID function, bool mustBeUnmodified, bool allowsModified)
{
    auto entrypoint = m_program.genericEntrypoints.find(function.index());
    if (entrypoint == m_program.genericEntrypoints.end())
        return boolean(false);
    uint32_t general = entrypoint->value.index();
    ptrdiff_t structureID = closureStructureIDOffset(m_program.at(function));
    bool isSlim = m_program.usesSlimClosures(m_program.at(function));
    if (m_program.at(function).captureTypes.isEmpty() && !m_program.at(function).hasManyClosures) {
        Word32 isTheOne = isSingletonClosure(cell, general);
        if (!mustBeUnmodified)
            return isTheOne;
        return either<Word32>(isTheOne, [&] {
            return equal(loadField<Word32>(cell, m_heaps.JSCell_structureID), Word32(loadRuntimeState32(structureID)));
        }, [&] {
            return boolean(false);
        });
    }
    Word32 hasExpectedStructure = allowsModified && !mustBeUnmodified ? hasClosureStructure(cell, structureID) : equal(loadField<Word32>(cell, m_heaps.JSCell_structureID, safeIfReaches(cell, sizeof(JSCell))), Word32(loadRuntimeState32(structureID)));
    return either<Word32>(hasExpectedStructure, [&] {
        return equal(closureFunction(cell, isSlim), word32(general));
    }, [&] {
        if (mustBeUnmodified || (m_program.at(function).kind != FunctionKind::Constructor && !allowsModified))
            return boolean(false);
        Integer entry(callAllocationStub(Stub::TypedClosureEntry, { { cell.value(), stubTemporaryGPRs[0] } }, { Writes::Nothing, false }, false));
        if (m_program.at(function).kind != FunctionKind::Constructor) {
            Word32 entrypointTarget(m_out.bitAnd(m_out.castToInt32(m_out.lShr(entry.value(), m_out.constInt32(ClosureWord::functionShift))), m_out.constInt32(static_cast<int32_t>(ClosureWord::functionMask))));
            return logicalAnd(logicalNot(isZero(entry)), equal(entrypointTarget, word32(general)));
        }
        Integer functionEntry(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(Instance::offsetOfTypedFunctions()), static_cast<ptrdiff_t>(general) * sizeof(void*))));
        return logicalAnd(logicalNot(isZero(entry)), equal(entry, functionEntry));
    });
}

ptrdiff_t Lowering::closureStructureIDOffset(const Function& function)
{
    if (m_program.usesSlimClosures(function))
        return Instance::offsetOfTypedSlimClosureHeader(slimClosureKindOf(static_cast<uint8_t>(function.kind) | function.declaredAs));
    return Instance::offsetOfTypedClosureStructureID(function.kind == FunctionKind::Constructor ? 0 : closureStructureOf(function.declaredAs));
}

Word32 Lowering::isUnreifiedSlimClosure(CellPointer cell)
{
    static_assert(SlimClosureWithPropertiesType == SlimClosureType + 1);
    return equal(loadCellType(cell), word32(SlimClosureType));
}

Word32 Lowering::closureFunction(CellPointer closure, bool isSlim)
{
    LValue upperHalf = m_out.castToInt32(m_out.lShr(loadField<Integer>(closure, isSlim ? m_heaps.AOTTypedSlimClosure_word : m_heaps.AOTTypedClosure_entry).value(), m_out.constInt32(ClosureWord::functionShift)));
    return Word32(m_out.bitAnd(upperHalf, m_out.constInt32(static_cast<int32_t>(ClosureWord::functionMask))));
}

Word32 Lowering::hasClosureStructure(CellPointer cell, ptrdiff_t offsetOfStructureID)
{
    Word32 structureID = loadField<Word32>(cell, m_heaps.JSCell_structureID, safeIfReaches(cell, sizeof(JSCell)));
    Word32 isUnmodified = equal(structureID, Word32(loadRuntimeState32(offsetOfStructureID)));
    if (offsetOfStructureID != Instance::offsetOfTypedClosureStructureID())
        return isUnmodified;
    return logicalOr(isUnmodified, isKnownClosureStructureID(structureID));
}

Word32 Lowering::isClosureWithNoPropertyButPrototype(CellPointer cell)
{
    Word32 structureID = loadField<Word32>(cell, m_heaps.JSCell_structureID, safeIfReaches(cell, sizeof(JSCell)));
    return logicalOr(equal(structureID, Word32(loadRuntimeState32(Instance::offsetOfTypedClosureStructureID()))), equal(structureID, Word32(loadRuntimeState32(Instance::offsetOfTypedClosureWithPrototypeStructureID()))));
}

Word32 Lowering::isKnownClosureStructureID(Word32 structureID)
{
    LValue index = m_out.bitAnd(m_out.lShr(structureID.value(), m_out.constInt32(Instance::knownClosureStructureIDShift)), m_out.constInt32(Instance::numberOfKnownClosureStructureIDs - 1));
    LValue row = m_out.add(m_instance, m_out.shl(m_out.zeroExtPtr(index), m_out.constInt32(2)));
    return equal(structureID, Word32(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, row, Instance::offsetOfKnownClosureStructureIDs()))));
}

void Lowering::checkClosureOfOrElse(CellPointer cell, FunctionID function, bool mustBeUnmodified, LBasicBlock otherwise, bool allowsModified)
{
    auto entrypoint = m_program.genericEntrypoints.find(function.index());
    auto& callee = m_program.at(function);
    if (entrypoint == m_program.genericEntrypoints.end() || (callee.captureTypes.isEmpty() && !callee.hasManyClosures) || callee.kind == FunctionKind::Constructor) {
        orElse(isClosureOf(cell, function, mustBeUnmodified, allowsModified), otherwise);
        return;
    }
    if (allowsModified) {
        LBasicBlock isUnmodifiedStructure = m_out.newBlock();
        LBasicBlock isModified = newColdBlock();
        LBasicBlock isOne = m_out.newBlock();
        m_out.branch(hasClosureStructure(cell, closureStructureIDOffset(callee)).value(), usually(isUnmodifiedStructure), rarely(isModified));
        m_out.appendTo(isUnmodifiedStructure);
        m_out.branch(equal(closureFunction(cell, m_program.usesSlimClosures(callee)), word32(entrypoint->value.index())).value(), unsure(isOne), unsure(otherwise));
        m_out.appendTo(isModified);
        Integer entry(callAllocationStub(Stub::TypedClosureEntry, { { cell.value(), stubTemporaryGPRs[0] } }, { Writes::Nothing, false }, false));
        Word32 entrypointTarget(m_out.bitAnd(m_out.castToInt32(m_out.lShr(entry.value(), m_out.constInt32(ClosureWord::functionShift))), m_out.constInt32(static_cast<int32_t>(ClosureWord::functionMask))));
        m_out.branch(logicalAnd(logicalNot(isZero(entry)), equal(entrypointTarget, word32(entrypoint->value.index()))).value(), unsure(isOne), unsure(otherwise));
        m_out.appendTo(isOne);
        return;
    }
    orElse(equal(loadField<Word32>(cell, m_heaps.JSCell_structureID, safeIfReaches(cell, sizeof(JSCell))), Word32(loadRuntimeState32(closureStructureIDOffset(callee)))), otherwise);
    orElse(equal(closureFunction(cell, m_program.usesSlimClosures(callee)), word32(entrypoint->value.index())), otherwise);
}

void Lowering::branchOnClosureOf(const Value& value, LBasicBlock isOne, LBasicBlock isNone)
{
    Operand operand = get(value.child(0));
    if (operand.rep != Rep::Cell && operand.rep != Rep::JSValue) {
        m_out.jump(isNone);
        return;
    }
    if (isCalledWithoutAllocation(value.child(0))) {
        LBasicBlock isCreated = m_out.newBlock();
        m_out.branch(isUnmaterializedClosure(operand.as<Boxed>()).value(), unsure(isOne), unsure(isCreated));
        m_out.appendTo(isCreated);
    }
    auto& function = m_program.at(value.function());
    bool isComparedByAddress = !m_decoded.contains(operand.value) && (value.immediates[1] == closureIsConstructingCallee || (value.immediates[1] != closureIsUnmodified && m_program.genericEntrypoints.contains(value.function().index()) && function.captureTypes.isEmpty() && !function.hasManyClosures));
    if (operand.rep == Rep::JSValue && !isComparedByAddress)
        orElse(isCell(operand.as<Boxed>(), typeOf(value.child(0))), isNone);
    if (value.immediates[1] == closureIsConstructingCallee) {
        orElse(isSingletonClosure(operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>()), m_program.genericEntrypoints.get(value.function().index()).index()), isNone);
        m_out.jump(isOne);
        return;
    }
    checkClosureOfOrElse(operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>()), value.function(), value.immediates[1] == closureIsUnmodified, isNone, value.immediates[1] == closureMayBeModified);
    m_out.jump(isOne);
}

void Lowering::findClosureTestsFusedWithBranches()
{
    BitVector isInspectedElsewhere(m_function.values.size());
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            m_function.at(id).forEachChild([&](ValueID child) {
                isInspectedElsewhere.set(child.index());
            });
        }
        for (auto& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                isInspectedElsewhere.set(argument.index());
        }
        if (block.terminator.value && (block.terminator.kind != TerminatorKind::Branch || block.values.isEmpty() || block.values.last() != block.terminator.value))
            isInspectedElsewhere.set(block.terminator.value.index());
    }
    for (auto& block : m_function.blocks) {
        if (block.terminator.kind != TerminatorKind::Branch || isInspectedElsewhere.get(block.terminator.value.index()))
            continue;
        auto& condition = m_function.at(block.terminator.value);
        bool isRelational = condition.opcode == Opcode::Less || condition.opcode == Opcode::LessEq || condition.opcode == Opcode::Greater || condition.opcode == Opcode::GreaterEq;
        if (isRelational && ((isNumericOperand(typeOf(condition.child(0))) && isNumericOperand(typeOf(condition.child(1)))) || successorsOf(block.terminator).size() != 2))
            isRelational = false;
        if (isRelational || condition.opcode == Opcode::IsClosureOf || isAwaitMayContinue(condition) || isReceiverGuard(condition) || isForInStampCheck(condition) || isHostObjectCacheTest(condition) || isJSONParserTest(condition))
            m_isFusedWithBranch.set(block.terminator.value.index());
        if (condition.opcode == Opcode::ToBoolean && successorsOf(block.terminator).size() == 2 && !Options::auditAOTTypedAssertions() && !Options::validateAOTTypedFacts())
            m_isTruthinessOnlyBranchedOn.set(block.terminator.value.index());
    }
}

Word32 Lowering::lowerIsClosureOf(const Value& value)
{
    Operand operand = get(value.child(0));
    bool mustBeUnmodified = value.immediates[1] == closureIsUnmodified;
    bool allowsModified = value.immediates[1] == closureMayBeModified;
    auto test = [&](CellPointer cell) {
        if (value.immediates[1] == closureIsConstructingCallee)
            return isSingletonClosure(cell, m_program.genericEntrypoints.get(value.function().index()).index());
        return isClosureOf(cell, value.function(), mustBeUnmodified, allowsModified);
    };
    if (operand.rep == Rep::Cell)
        return test(operand.as<CellPointer>());
    if (operand.rep != Rep::JSValue)
        return boolean(false);
    Boxed boxed = operand.as<Boxed>();
    if (isCalledWithoutAllocation(value.child(0))) {
        return either<Word32>(isUnmaterializedClosure(boxed), [&] {
            return boolean(true);
        }, [&] {
            return either<Word32>(isCell(boxed, typeOf(value.child(0))), [&] {
                return test(asCell(boxed));
            }, [&] {
                return boolean(false);
            });
        });
    }
    return either<Word32>(isCell(boxed, typeOf(value.child(0))), [&] {
        return test(asCell(boxed));
    }, [&] {
        return boolean(false);
    });
}

Word32 Lowering::isCallable(CellPointer cell)
{
    return either<Word32>(hasTypeOfFunctionFlag(cell), [&] {
        return boolean(true);
    }, [&] {
        return AOT_TYPED_OPERATION(IsCallable, cell);
    });
}

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
    if (Operand optional = get(value.child(0)); optional.rep == Rep::OptionalDouble || optional.rep == Rep::OptionalInt64) {
        bool mayBeNull = typeOf(value.child(0)).primitives & Primitive::Null;
        switch (wanted) {
        case TypeofType::Number:
            return logicalNot(isMarker(optional));
        case TypeofType::Undefined:
            return mayBeNull ? equal(bitsOf(optional), integer(markerBits(Primitive::Undefined, optional.rep))) : isMarker(optional);
        case TypeofType::Object:
            return mayBeNull ? equal(bitsOf(optional), integer(markerBits(Primitive::Null, optional.rep))) : boolean(false);
        default:
            return boolean(false);
        }
    }
    Boxed operand = in<Boxed>(value.child(0));
    if (wanted == TypeofType::Number)
        return isNumber(operand);
    if (wanted == TypeofType::Boolean)
        return isBoolean(operand);

    Join join(m_out, Rep::Int32, Int32);
    auto when = [&](Word32 condition, Word32 answer, bool usuallyHolds = false) {
        LBasicBlock holds = m_out.newBlock();
        LBasicBlock next = usuallyHolds ? newUnlikelyBlock() : m_out.newBlock();
        m_out.branch(condition.value(), unsure(holds), unsure(next));
        m_out.appendTo(holds);
        join.add(answer);
        m_out.appendTo(next);
    };
    Word32 nonCellResult = boolean(false);
    if (wanted == TypeofType::Undefined)
        nonCellResult = equal(operand, undefined());
    else if (wanted == TypeofType::Object)
        nonCellResult = equal(operand, boxedConstant(jsNull()));
    when(logicalNot(isCell(operand, typeOf(value.child(0)))), nonCellResult);

    CellPointer cell = asCell(operand);
    Word32 type = loadCellType(cell);
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
    when(hasBits(flags, TypeOfReturnsFunction), is(TypeofType::Function), wanted == TypeofType::Function);
    if (wanted != TypeofType::Function)
        when(below(type, word32(ObjectType)), boolean(false));
    when(hasNoBits(flags, MasqueradesAsUndefined | OverridesGetCallData), is(TypeofType::Object));
    join.add(equal(AOT_TYPED_OPERATION(TypeOfObject, cell), word32(static_cast<int32_t>(wanted))));
    return join.finish().as<Word32>();
}

Word32 Lowering::cellIsOfType(CellPointer cell, const Type& from, const Type& to, LBasicBlock otherwise)
{
    Word32 loadedType;
    auto type = [&] {
        if (!loadedType)
            loadedType = loadCellType(cell);
        return loadedType;
    };
    bool anyObjectMatches = to.anyObject || (!m_rejectsUntypedObjects && to.acceptsUncheckedObjects(m_program)) || (m_isValidatingFact && to.acceptsUntypedObjects(m_program));
    bool mayBeAnyObject = from.anyObject || from.acceptsUntypedObjects(m_program);
    bool mayBeModifiedArray = false;
    Word32 result;
    static constexpr std::pair<uint16_t, JSType> primitives[] = { { Primitive::String, StringType }, { Primitive::Symbol, SymbolType }, { Primitive::BigInt, HeapBigIntType } };
    for (auto [primitive, valueType] : primitives) {
        if (from.primitives & to.primitives & primitive)
            result = logicalOr(result, equal(type(), word32(valueType)));
    }
    if (!from.hasObjects() || !to.hasObjects())
        return result ? result : boolean(false);
    if (anyObjectMatches)
        return logicalOr(result, aboveOrEqual(type(), word32(ObjectType)));

    if (to.anyFunction && (from.anyFunction || mayBeAnyObject))
        result = logicalOr(result, isCallable(cell));
    for (uint16_t index : to.engineClasses) {
        JSType valueType = m_builtins.classes()[index].type;
        if (!mayBeAnyObject && !std::ranges::any_of(from.engineClasses, [&](uint16_t other) { return m_builtins.classes()[other].type == valueType; }))
            continue;
        result = logicalOr(result, equal(type(), word32(valueType)));
        if (valueType != ArrayType)
            continue;
        if (m_isValidatingFact)
            result = logicalOr(result, isModifiedArray(cell));
        else if (!mayBeAnyObject)
            result = logicalOr(result, equal(type(), word32(DerivedArrayType)));
        else
            mayBeModifiedArray = true;
    }
    if (!to.objectLayouts.isEmpty() && (from.anyObject || !from.objectLayouts.isEmpty())) {
        SetForScope knownTypeCell(m_knownTypeCell, cell.value());
        SetForScope knownCellType(m_knownCellType, &from);
        result = logicalOr(result, passesAnyOf(shapeWordOf(cell), to.objectLayouts, result || mayBeModifiedArray ? nullptr : otherwise));
    }
    if (!mayBeModifiedArray)
        return result ? result : boolean(false);
    LBasicBlock fits = m_out.newBlock();
    LBasicBlock doesNot = newColdBlock();
    m_out.branch(result.value(), usually(fits), rarely(doesNot));
    m_out.appendTo(doesNot);
    m_out.branch(isModifiedArray(cell).value(), unsure(fits), unsure(otherwise));
    m_out.appendTo(fits);
    return boolean(true);
}

Word32 Lowering::nonCellIsOfType(Boxed value, const Type& from, const Type& to)
{
    auto is = [&](JSValue constant) {
        return equal(value, boxedConstant(constant));
    };
    uint16_t both = from.primitives & to.primitives;
    Word32 result;
    switch (both & Primitive::UndefinedOrNull) {
    case Primitive::UndefinedOrNull:
        result = logicalOr(result, from.primitives & ~(cellPrimitives | Primitive::UndefinedOrNull) ? isUndefinedOrNull(value, from) : logicalNot(isCell(value, from)));
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
        result = logicalOr(result, either<Word32>(isInt32(value), [&] {
            if (Range::int32().isSubsetOf(to.integers))
                return boolean(true);
            return isSubsetOf(signExtended(low32(value)), to.integers);
        }, [&] {
            return either<Word32>(isNumber(value), [&] {
                Number number = unboxedDouble(value);
                Integer truncated = truncateToInteger(number);
                return logicalAnd(equal(bitsOf(convertToNumber(truncated)), bitsOf(number)), isSubsetOf(truncated, to.integers));
            }, [&] {
                return boolean(false);
            });
        }));
    }
    return result ? result : boolean(false);
}

Integer Lowering::integralOrElse(Number number, Range range, LBasicBlock otherwise)
{
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics() && range.isSubsetOf(Range::int32())) {
        LBasicBlock continuation = m_out.newBlock();
        PatchpointValue* patchpoint = m_out.patchpoint(Int32);
        patchpoint->effects = Effects::none();
        patchpoint->effects.terminal = true;
        patchpoint->append(ConstrainedValue(number.value(), ValueRep::SomeRegister));
        m_out.appendSuccessor(rarely(otherwise));
        m_out.appendSuccessor(usually(continuation));
        patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
            CCallHelpers::Jump notInt32 = jit.branchOnFlags(CCallHelpers::NotEqual);
            CCallHelpers::Jump isInt32;
            if (!params.fallsThroughToSuccessor(1))
                isInt32 = jit.jump();
            Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
            params.addLatePath([=](CCallHelpers& jit) {
                notInt32.linkTo(*labels[0], &jit);
                if (isInt32.isSet())
                    isInt32.linkTo(*labels[1], &jit);
            });
        });
        m_out.appendTo(continuation);
        Integer exact = signExtended(Word32(patchpoint));
        if (!Range::int32().isSubsetOf(range))
            orElse(isSubsetOf(exact, range), otherwise);
        return exact;
    }
#endif
    Integer truncated = truncateToInteger(number);
    orElse(equal(bitsOf(convertToNumber(truncated)), bitsOf(number)), otherwise);
    orElse(isSubsetOf(truncated, range), otherwise);
    return truncated;
}

Operand Lowering::narrowBoxedToIntegerOrElse(Boxed value, const Type& from, const Type& to, LBasicBlock otherwise)
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
        if (!Range::int32().isSubsetOf(to.integers))
            orElse(isSubsetOf(narrowed, to.integers), otherwise);
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
        results.append(m_out.anchor(integer(markerBits(primitive, rep)).value()));
        m_out.jump(continuation);
        m_out.appendTo(isNot);
    }
    if (mayBeNumber) {
        results.append(m_out.anchor(integralOrElse(unboxedDouble(value), to.integers, otherwise).value()));
        m_out.jump(continuation);
    } else
        m_out.jump(otherwise);

    m_out.appendTo(continuation);
    return Operand(rep, m_out.phi(Int64, results));
}

Operand Lowering::narrowBoxedOrElse(Boxed value, const Type& from, const Type& to, LBasicBlock otherwise)
{
    Rep rep = repOf(to);
    if (rep == Rep::Int64 || rep == Rep::OptionalInt64)
        return narrowBoxedToIntegerOrElse(value, from, to, otherwise);

    if (rep == Rep::OptionalDouble && from.primitives & Primitive::Number && to.primitives & Primitive::Double && from.primitives & to.primitives & Primitive::UndefinedOrNull) {
        LBasicBlock isNumberCase = m_out.newBlock();
        LBasicBlock isNotNumberCase = newUnlikelyBlock();
        LBasicBlock continuation = m_out.newBlock();
        Vector<ValueFromBlock, 3> results;
        m_out.branch(isNumber(value).value(), unsure(isNumberCase), unsure(isNotNumberCase));
        m_out.appendTo(isNumberCase);
        results.append(m_out.anchor(convert(value, to.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent), rep).value));
        m_out.jump(continuation);
        m_out.appendTo(isNotNumberCase);
        for (uint16_t primitive : { Primitive::Undefined, Primitive::Null }) {
            if (!(from.primitives & to.primitives & primitive))
                continue;
            Boxed constant = boxedConstant(primitive == Primitive::Undefined ? jsUndefined() : jsNull());
            LBasicBlock isIt = m_out.newBlock();
            LBasicBlock isNot = m_out.newBlock();
            m_out.branch(equal(value, constant).value(), unsure(isIt), unsure(isNot));
            m_out.appendTo(isIt);
            results.append(m_out.anchor(convert(constant, Type::fromPrimitives(primitive), rep).value));
            m_out.jump(continuation);
            m_out.appendTo(isNot);
        }
        m_out.jump(otherwise);
        m_out.appendTo(continuation);
        LValue joined = m_out.phi(b3TypeOf(rep), results);
        m_boxedBeforeUnboxing.add(joined, value.value());
        return Operand(rep, joined);
    }

    if (!from.hasCells() || !to.hasCells()) {
        orElse(nonCellIsOfType(value, from, to), otherwise);
        return convert(value, to, rep);
    }

    uint16_t others = from.primitives & ~cellPrimitives;
    bool othersMatch = withoutCells(from).isSubsetOf(withoutCells(to), m_program);
    CellPointer cell = asCell(value);
    if (onlyCellsOf(from).isSubsetOf(onlyCellsOf(to), m_program, !m_rejectsUntypedObjects)) {
        uint16_t gone = others & ~to.primitives;
        Word32 isOne;
        if (!(others & to.primitives))
            isOne = isCell(value, from);
        else if (!(gone & ~Primitive::UndefinedOrNull) && withoutCells(from).withoutPrimitives(Primitive::UndefinedOrNull).isSubsetOf(withoutCells(to), m_program)) {
            if (gone == Primitive::Undefined)
                isOne = notEqual(value, undefined());
            else if (gone == Primitive::Null)
                isOne = notEqual(value, boxedConstant(jsNull()));
            else
                isOne = logicalNot(isUndefinedOrNull(value, from));
        } else
            isOne = logicalOr(isCell(value, from), nonCellIsOfType(value, from, to));
        orElse(isOne, otherwise);
        return convert(value, to, rep);
    }

    if (!others) {
        orElse(cellIsOfType(cell, from, to, otherwise), otherwise);
        return convert(value, to, rep);
    }
    if (!(others & to.primitives)) {
        orElse(isCell(value, from), otherwise);
        orElse(cellIsOfType(cell, from, to, otherwise), otherwise);
        return convert(value, to, rep);
    }

    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    LBasicBlock otherCase = othersMatch ? continuation : newUnlikelyBlock();
    if (othersMatch)
        m_out.branch(isCell(value, from).value(), usually(cellCase), rarely(continuation));
    else
        m_out.branch(isCell(value, from).value(), unsure(cellCase), unsure(otherCase));
    m_out.appendTo(cellCase);
    m_out.branch(cellIsOfType(cell, from, to, otherwise).value(), usually(continuation), rarely(otherwise));
    if (!othersMatch) {
        m_out.appendTo(otherCase);
        m_out.branch(nonCellIsOfType(value, from, to).value(), usually(continuation), rarely(otherwise));
    }
    m_out.appendTo(continuation);
    return convert(value, to, rep);
}

static bool mayHaveValuesInCommon(const Type& a, const Type& b)
{
    uint16_t both = a.primitives & b.primitives;
    if (both & ~Primitive::Int || (both & Primitive::Int && !a.integers.intersectWith(b.integers).isEmpty()))
        return true;
    return a.hasObjects() && b.hasObjects();
}

Operand Lowering::narrowLiteralStringOrElse(Operand operand, const Type& from, const Type& to, LBasicBlock otherwise)
{
    if (!from.hasLiteralString()) {
        Type asString = to.withLiteralStringWidened();
        return literalStringOf(narrowOrElse(operand, from, asString, otherwise), asString);
    }
    if (!to.hasLiteralString()) {
        Type asString = from.withLiteralStringWidened();
        return narrowOrElse(convert(operand, from, repOf(asString)), asString, to, otherwise);
    }
    LValue word = rawWordOf(operand.as<LiteralStringWord>());
    uint16_t gone = from.primitives & ~to.primitives;
    if (!(to.primitives & (Primitive::UndefinedOrNull | Primitive::Absent)))
        orElse(Word32(m_out.above(word, m_out.constInt64(undefinedCell32))), otherwise);
    else {
        for (auto [primitive, bits] : { std::pair { Primitive::Undefined, undefinedCell32 }, std::pair { Primitive::Null, nullCell32 }, std::pair { Primitive::Absent, absentCell32 } }) {
            if (gone & primitive)
                orElse(Word32(m_out.notEqual(word, m_out.constInt64(bits))), otherwise);
        }
    }
    return operand;
}

Operand Lowering::narrowOrElse(Operand operand, const Type& from, const Type& to, LBasicBlock otherwise)
{
    Rep rep = repOf(to);
    if (from.isSubsetOf(to, m_program, !m_rejectsUntypedObjects))
        return convert(operand, from, rep);
    if (from.hasLiteralString() || to.hasLiteralString())
        return narrowLiteralStringOrElse(convert(operand, from, repOf(from)), from, to, otherwise);
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
        return narrowBoxedOrElse(operand.as<Boxed>(), from, to, otherwise);
    case Rep::Cell:
        if (!to.hasCells())
            return never();
        orElse(cellIsOfType(operand.as<CellPointer>(), from, to, otherwise), otherwise);
        return convert(operand, to, rep);
    case Rep::Double:
        if (!(to.primitives & Primitive::Int))
            return never();
        return convert(integralOrElse(operand.as<Number>(), to.integers, otherwise), to, rep);
    case Rep::Int64:
        if (!(to.primitives & Primitive::Int))
            return never();
        orElse(isSubsetOf(operand.as<Integer>(), to.integers), otherwise);
        return convert(operand, to, rep);
    case Rep::Int32:
        if (!(to.primitives & Primitive::Boolean))
            return never();
        orElse(equal(operand.as<Word32>(), boolean(to.primitives & Primitive::True)), otherwise);
        return convert(operand, to, rep);
    case Rep::OptionalInt64:
    case Rep::OptionalDouble:
        break;
    case Rep::LiteralString:
    case Rep::Void:
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool isInteger = operand.rep == Rep::OptionalInt64;
    Operand number(isInteger ? Rep::Int64 : Rep::Double, operand.value);
    Type numbers = from.withoutPrimitives(Primitive::UndefinedOrNull | Primitive::Absent);
    if (!(from.primitives & to.primitives & Primitive::UndefinedOrNull)) {
        bool rejectsFractions = !isInteger && !numbers.isSubsetOf(to, m_program);
        if (!rejectsFractions)
            orElse(logicalNot(isMarker(operand)), otherwise);
        return narrowOrElse(number, numbers, to, otherwise);
    }
    LBasicBlock numberCase = m_out.newBlock();
    LBasicBlock markerCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;
    m_out.branch(isMarker(operand).value(), unsure(markerCase), unsure(numberCase));
    m_out.appendTo(numberCase);
    results.append(m_out.anchor(narrowOrElse(number, numbers, to, otherwise).value));
    m_out.jump(continuation);
    m_out.appendTo(markerCase);
    Integer bits = bitsOf(operand);
    for (uint16_t primitive : { Primitive::Undefined, Primitive::Null }) {
        if (!(from.primitives & to.primitives & primitive))
            continue;
        LBasicBlock isIt = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        m_out.branch(equal(bits, integer(markerBits(primitive, operand.rep))).value(), unsure(isIt), unsure(isNot));
        m_out.appendTo(isIt);
        results.append(m_out.anchor(convert(boxedConstant(primitive == Primitive::Undefined ? jsUndefined() : jsNull()), Type::fromPrimitives(primitive), rep).value));
        m_out.jump(continuation);
        m_out.appendTo(isNot);
    }
    m_out.jump(otherwise);
    m_out.appendTo(continuation);
    return Operand(rep, m_out.phi(b3TypeOf(rep), results));
}

Word32 Lowering::fits(Operand operand, const Type& from, const Type& to)
{
    if (from.isSubsetOf(to, m_program))
        return boolean(true);
    LBasicBlock doesNot = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    narrowOrElse(operand, from, to, doesNot);
    ValueFromBlock yes = m_out.anchor(m_out.booleanTrue);
    m_out.jump(continuation);
    m_out.appendTo(doesNot);
    ValueFromBlock no = m_out.anchor(m_out.booleanFalse);
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return Word32(m_out.phi(Int32, yes, no));
}

Operand Lowering::thrown(Operand operand, const Type& type)
{
    if (operand.rep == Rep::JSValue) {
        auto found = m_decoded.find(operand.value);
        if (found == m_decoded.end())
            return operand;
        LValue word = m_out.opaque(found->value.loaded);
        return Boxed(m_out.select(m_out.below(word, m_out.constInt64(firstCellWord)), word, m_out.opaque(found->value.cell)));
    }
    if (operand.rep == Rep::Int32 && !(type.primitives & Primitive::Boolean))
        return convert(operand, type, Rep::Int64);
    if (operand.rep == Rep::LiteralString)
        return convert(operand, type, Rep::JSValue);
    return operand;
}

Boxed Lowering::thrownNullish(Boxed value)
{
    auto found = m_decoded.find(value.value());
    return found == m_decoded.end() ? value : Boxed(found->value.loaded);
}

void Lowering::throwTypeMismatch(Operand operand, const Type& from, const Type& to)
{
    operand = thrown(operand, from);
    auto& current = m_function.at(m_currentBlock->values[m_currentIndexInBlock]);
    if (&m_program.at(current.type) == &to && m_program.isNeededAtRuntime(current.type)) {
        throwAt(ThrowKind::TypeMismatch, operand, current.type.index());
        m_out.unreachable();
        return;
    }
    uint32_t kinds = to.primitives & Primitive::All;
    if (to.anyObject || !to.objectLayouts.isEmpty() || !to.engineClasses.isEmpty())
        kinds |= typeAllowsObjects;
    if (to.anyFunction)
        kinds |= typeAllowsFunctions;
    StringID name;
    if (!to.objectLayouts.isEmpty())
        name = m_program.at(to.objectLayouts[0]).name;
    else if (!to.engineClasses.isEmpty())
        name = m_program.stringOf(m_builtins.classes()[to.engineClasses[0]].name);
    throwAt(ThrowKind::KindMismatch, operand, kinds, name.index());
    m_out.unreachable();
}

bool Lowering::resultMayInstantiateType(const Type& guarantee, const Type& type) const
{
    auto& value = m_function.at(m_currentBlock->values[m_currentIndexInBlock]);
    bool isReadOfCoercibleValue = value.opcode == Opcode::GetById && (readMayInstantiate(m_program, type) || fieldReadMayAllocate(m_program, m_function, value));
    bool isClaimOfCoercibleValue = value.type && value.opcode == Opcode::Cast && claimMayAllocate(m_program, m_function, value);
    if (!value.type || &m_program.at(value.type) != &type || !(guarantee.anyObject || guarantee.acceptsUntypedObjects(m_program) || isReadOfCoercibleValue || isClaimOfCoercibleValue) || writesOfElementRead(m_program, type) == Writes::Nothing)
        return false;
    if (Options::auditAOTTypedAssertions() || m_useCastTest || m_rejectsUntypedObjects || handlerEnteredByBranch() || m_environmentAwaitingBarrier || m_isFillingArrayAtCreation || m_isFillingReusedEnvironment || !m_fieldsAwaitingFirstStore.isEmpty())
        return false;
    if (value.opcode == Opcode::Cast)
        return claimMayAllocate(m_program, m_function, value);
    if (m_isElementOfEntries && (value.opcode == Opcode::GetByVal || value.opcode == Opcode::IteratorValue) && keyedAccessOf(m_program, m_function, value) == KeyedAccess::Array)
        return true;
    if (value.opcode == Opcode::CallDirect)
        return m_program.at(value.function()).writes == Writes::Heap;
    return value.opcode == Opcode::CallBuiltin || fieldReadMayAllocate(m_program, m_function, value) || writesOf(m_program, m_function, *m_currentBlock, m_currentIndexInBlock) == Writes::Heap;
}

std::optional<ArgumentMoves> Lowering::typeCheckThunkTests(const Type& type) const
{
    constexpr GPRReg value = GPRInfo::returnValueGPR;
    constexpr unsigned maxRanges = 2;
    uint64_t kinds = 0;
    unsigned numberOfTypes = 0;
    bool hasRoom = true;
    auto addType = [&](JSType jsType) {
        for (unsigned i = 0; i < numberOfTypes; ++i) {
            if ((kinds >> (KindCheck::typesShift + 8 * i) & 0xff) == static_cast<uint64_t>(jsType))
                return;
        }
        if (numberOfTypes == KindCheck::maxTypes) {
            hasRoom = false;
            return;
        }
        kinds |= static_cast<uint64_t>(jsType) << (KindCheck::typesShift + 8 * numberOfTypes++);
    };
    if (type.primitives & Primitive::Undefined)
        kinds |= KindCheck::Undefined;
    if (type.primitives & Primitive::Null)
        kinds |= KindCheck::Null;
    if ((type.primitives & Primitive::Boolean) == Primitive::Boolean)
        kinds |= KindCheck::Boolean;
    else if (type.primitives & Primitive::Boolean)
        return std::nullopt;
    if (type.primitives & Primitive::Double)
        kinds |= KindCheck::Number;
    else if (type.primitives & Primitive::Int)
        return std::nullopt;
    if (type.primitives & Primitive::String)
        addType(StringType);
    if (type.primitives & Primitive::Symbol)
        addType(SymbolType);
    if (type.primitives & Primitive::BigInt)
        addType(HeapBigIntType);
    Vector<uint64_t, maxRanges> ranges;
    if (type.anyObject || type.acceptsUncheckedObjects(m_program))
        kinds |= KindCheck::Object;
    else if (type.hasObjects()) {
        if (type.anyFunction)
            kinds |= KindCheck::Function;
        for (uint16_t index : type.engineClasses) {
            addType(m_builtins.classes()[index].type);
            if (m_builtins.classes()[index].type == ArrayType)
                kinds |= KindCheck::ChangedArray;
        }
        Vector<ShapeTest, 4> tests;
        for (LayoutID supertype : type.objectLayouts)
            tests.append(m_program.at(supertype).test);
        for (auto& merged : merged(tests)) {
            if (merged.test.neverPasses())
                continue;
            if (merged.bits || !merged.test.isRange() || ranges.size() == maxRanges)
                return std::nullopt;
            ranges.append(merged.test.last() | static_cast<uint64_t>(merged.test.count) << 32);
        }
    }
    if (!hasRoom || (!kinds && ranges.isEmpty()))
        return std::nullopt;
    ArgumentMoves tests;
    appendArgumentMove(tests, value, ArgumentSource::KindCheck, kinds);
    for (uint64_t range : ranges)
        appendArgumentMove(tests, value, ArgumentSource::ShapeCheck, range);
    return tests;
}

Boxed Lowering::checkTypeWithThunk(Boxed given, TypeID type, ArgumentMoves&& moves)
{
    bool isSlowPathCall = !m_out.m_block->frequency();
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    applyEffects(patchpoint, { Writes::Nothing, true });
    patchpoint->append(ConstrainedValue(given.value(), ValueRep::SomeRegister));
    RegisterSet changed = *registersClobberedBy(Stub::CoerceToType);
    changed.add(GPRInfo::argumentGPR1, IgnoreVectors);
    changed.add(GPRInfo::argumentGPR2, IgnoreVectors);
    changed.add(GPRInfo::returnValueGPR, IgnoreVectors);
    if (!hasFramelessFunctionStubs) {
        if (!isSlowPathCall || LoweringContext::coldSitesForceFrame() || m_activeHandler) {
            m_context.emitsCalls = true;
            patchpoint->clobberLate(RegisterSet { callMarkerGPR });
        } else
            m_context.hasColdSites = true;
    }
    patchpoint->clobberLate(changed);
    patchpoint->clobberEarly(*registersClobberedBy(Stub::CoerceToType));
    CallSite site { nextSite() };
    patchpoint->setGenerator([context = &m_context, site, type, tests = WTF::move(moves)](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg held = params[0].gpr();
        ArgumentMoves moves;
        for (size_t i = 0; i < tests.size(); i += 2)
            appendArgumentMove(moves, held, static_cast<ArgumentSource>(tests[i] & 0xff), tests[i + 1]);
        if (held != GPRInfo::argumentGPR1)
            appendArgumentMove(moves, GPRInfo::argumentGPR1, ArgumentSource::Register, held);
        appendArgumentMove(moves, GPRInfo::argumentGPR2, ArgumentSource::Constant, type.index());
        bool hasNoFrame = context->hasNoFrame(params.proc().code());
        bool isLeaf = hasFramelessFunctionStubs && hasNoFrame;
        bool createsFrame = !hasFramelessFunctionStubs && hasNoFrame;
#if CPU(ARM64)
        if (isLeaf)
            jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
        if (createsFrame)
            jit.emitFunctionPrologue();
        context->stubCalls.call(jit, isLeaf ? Stub::LeafCoerceToType : Stub::CoerceToType, site, moves);
        if (createsFrame)
            jit.emitFunctionEpilogueWithEmptyFrame();
    });
    unwindToLandingPad(patchpoint, site);
    return given;
}

bool Lowering::mayCastInRegister(const Type& from, const Type& to) const
{
    return m_currentBlock && !m_function.isSharedOutlinedBody && !m_checksExtraPropertyResult && !handlerEnteredByBranch()
        && !m_environmentAwaitingBarrier && !m_isFillingArrayAtCreation && m_fieldsAwaitingFirstStore.isEmpty()
        && from.isAnything() && !(from.primitives & Primitive::Absent) && holdsCells(repOf(to)) && to.hasCells()
        && &m_program.at(m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type) == &to
        && !Options::auditAOTTypedAssertions() && !Options::validateAOTTypedFacts();
}

std::optional<ArgumentMoves> Lowering::castInRegisterTests(const Type& type, bool usesCastTest, bool inspectsUntypedObjects) const
{
    constexpr unsigned maxRanges = 2;
    uint64_t kinds = 0;
    unsigned numberOfTypes = 0;
    bool hasRoom = true;
    auto addType = [&](JSType jsType) {
        for (unsigned i = 0; i < numberOfTypes; ++i) {
            if ((kinds >> (KindCheck::typesShift + 8 * i) & 0xff) == static_cast<uint64_t>(jsType))
                return;
        }
        if (numberOfTypes == KindCheck::maxTypes) {
            hasRoom = false;
            return;
        }
        kinds |= static_cast<uint64_t>(jsType) << (KindCheck::typesShift + 8 * numberOfTypes++);
    };
    if (type.primitives & Primitive::Undefined)
        kinds |= KindCheck::Undefined;
    if (type.primitives & Primitive::Null)
        kinds |= KindCheck::Null;
    if ((type.primitives & Primitive::Boolean) == Primitive::Boolean)
        kinds |= KindCheck::Boolean;
    else if (type.primitives & Primitive::Boolean)
        return std::nullopt;
    if (type.primitives & Primitive::Double)
        kinds |= KindCheck::Number;
    else if (type.primitives & Primitive::Int)
        return std::nullopt;
    if (type.primitives & Primitive::String)
        addType(StringType);
    if (type.primitives & Primitive::Symbol)
        addType(SymbolType);
    if (type.primitives & Primitive::BigInt)
        addType(HeapBigIntType);
    Vector<uint64_t, maxRanges> ranges;
    if (type.anyObject || (!inspectsUntypedObjects && type.acceptsUncheckedObjects(m_program)))
        kinds |= KindCheck::Object;
    else if (type.hasObjects()) {
        if (type.anyFunction)
            kinds |= KindCheck::Function;
        for (uint16_t index : type.engineClasses)
            addType(m_builtins.classes()[index].type);
        Vector<ShapeTest, 4> tests;
        for (LayoutID supertype : type.objectLayouts)
            tests.append(usesCastTest ? m_program.at(supertype).castTest : m_program.at(supertype).test);
        for (auto& merged : merged(tests)) {
            if (merged.test.neverPasses())
                continue;
            if (merged.bits || !merged.test.isRange() || ranges.size() == maxRanges)
                return std::nullopt;
            RELEASE_ASSERT(merged.test.count <= ShapeCheck::maxCount);
            ranges.append(merged.test.last() | static_cast<uint64_t>(merged.test.count) << ShapeCheck::countShift | static_cast<uint64_t>(merged.test.passesEarlierBlocks) << ShapeCheck::passesEarlierBlocksShift | static_cast<uint64_t>(merged.test.blockByte) << ShapeCheck::blockByteShift);
        }
    }
    if (!hasRoom || (!(kinds & ~(KindCheck::Undefined | KindCheck::Null | KindCheck::Boolean | KindCheck::Number)) && ranges.isEmpty()))
        return std::nullopt;
    ArgumentMoves tests;
    appendArgumentMove(tests, GPRInfo::returnValueGPR, ArgumentSource::KindCheck, kinds);
    for (uint64_t range : ranges)
        appendArgumentMove(tests, GPRInfo::returnValueGPR, ArgumentSource::ShapeCheck, range);
    return tests;
}

Boxed Lowering::castInRegister(Boxed given, Entry operation, bool operationReturnsValue, uint32_t argument, OperationEffects effects, ArgumentMoves&& tests, bool mayBeModifiedArray)
{
    PatchpointValue* patchpoint = m_out.patchpoint(Int64);
    applyEffects(patchpoint, effects);
    patchpoint->append(ConstrainedValue(given.value(), ValueRep::ColdAny));
    patchpoint->resultConstraints = { ValueRep::SomeRegisterWithClobber };
    patchpoint->numGPScratchRegisters = cellChecksNeedScratch(tests);
    patchpoint->clobber(*registersClobberedBy(Stub::CastInRegister));
    if (LoweringContext::coldSitesForceFrame()) {
        m_context.emitsCalls = true;
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    } else if (!hasFramelessFunctionStubs)
        m_context.hasColdSites = true;
    CallSite site { nextSite() };
    patchpoint->setGenerator([context = &m_context, site, operation, operationReturnsValue, argument, mayBeModifiedArray, slowPathIsInline = m_function.isGenericEntrypoint() || (!m_isInLoop && m_proc.code().placesRareBlocksBehindGuards()), tests = WTF::move(tests)](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg held = params[0].gpr();
        CCallHelpers::JumpList doesNotFit;
        CCallHelpers::JumpList fits;
        jumpUnlessCellPassesChecks(jit, held, cellChecksNeedScratch(tests) ? params.gpScratch(0) : InvalidGPRReg, tests, doesNotFit, slowPathIsInline ? &fits : nullptr);
        CCallHelpers::Label done = jit.label();
        ArgumentMoves moves;
        appendArgumentMove(moves, held, ArgumentSource::KindCheck, (tests[1] & (KindCheck::Undefined | KindCheck::Null | KindCheck::Boolean | KindCheck::Number)) | (mayBeModifiedArray ? KindCheck::ChangedArray : 0));
        appendArgumentMove(moves, castInRegisterWordGPR(), ArgumentSource::Constant, CastInRegister::word(argument, static_cast<unsigned>(operation) * sizeof(void*), operationReturnsValue, held));
        bool isLeaf = context->hasNoFrame(params.proc().code());
        if (slowPathIsInline) {
            doesNotFit.link(&jit);
#if CPU(ARM64)
            if (isLeaf)
                jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
            context->stubCalls.call(jit, isLeaf ? Stub::LeafCastInRegister : Stub::CastInRegister, site, moves);
            fits.link(&jit);
            return;
        }
        params.addNearbyLatePath(jit, [context, site, isLeaf, doesNotFit, done, moves = WTF::move(moves)](CCallHelpers& jit) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            doesNotFit.link(&jit);
#if CPU(ARM64)
            if (isLeaf)
                jit.move(CCallHelpers::linkRegister, stubTemporaryGPRs[1]);
#endif
            context->stubCalls.call(jit, isLeaf ? Stub::LeafCastInRegister : Stub::CastInRegister, site, moves);
            jit.jump().linkTo(done, &jit);
        });
    });
    unwindToLandingPad(patchpoint, site, RegistersAtLandingPad::PreservedFromSite);
    return Boxed(patchpoint);
}

bool Lowering::checksRuntimeAllocatedValue(const Value& current) const
{
    auto& created = current.opcode == Opcode::Cast ? m_function.at(withoutUpcasts(current.child(0))) : current;
    if (created.opcode == Opcode::CatchValue)
        return true;
    if (created.opcode != Opcode::CallBuiltin)
        return false;
    auto kind = m_builtins.at(created.builtin()).implementation.kind;
    return kind == BuiltinImplementation::Generic || kind == BuiltinImplementation::Call || kind == BuiltinImplementation::Operation;
}

Operand Lowering::ensure(Operand operand, const Type& guarantee, const Type& type)
{
    if (m_fusedCasts && &type == &m_fusedCasts->type && !m_fusedCasts->areBeingLowered)
        return lowerFusedCasts(operand, guarantee);
    if (&guarantee == &type || type.isNever() || guarantee.isSubsetOf(type, m_program, !m_isCheckingTypeRelaxedForLogging))
        return convert(operand, guarantee, repOf(type));
    if (resultMayInstantiateType(guarantee, type)) {
        TypeID given = m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type;
        OperationEffects effects { Writes::RuntimeState, true, true };
        if (m_program.isColdInCallGraph(m_id) && !m_isInLoop && repOf(guarantee) == Rep::JSValue)
            return convert(AOT_TYPED_OPERATION_WITH_EFFECTS(EnforceTypeOrConvert, effects, rematerializedValue(convert(operand, guarantee, Rep::JSValue).as<Boxed>()), word32(given.index())), type, repOf(type));
        Rep rep = repOf(type);
        LBasicBlock typeMismatch = newColdBlock();
        Join join(m_out, rep, b3TypeOf(rep));
        join.add(narrowOrElse(operand, guarantee, type, typeMismatch));
        m_out.appendTo(typeMismatch);
        join.add(convert(AOT_TYPED_OPERATION_WITH_EFFECTS(EnforceTypeOrConvert, effects, rematerializedValue(convert(operand, guarantee, Rep::JSValue).as<Boxed>()), word32(given.index())), type, rep));
        return join.finish();
    }
    bool isOncePerAsyncActivation = !m_isInLoop && (m_function.has(FunctionIsPartOfAsyncFunction) || ((m_function.resumeFunction || m_function.isGenericEntrypoint()) && (m_function.declaredAs & (functionIsAsync | functionIsGenerator)) == functionIsAsync));
    if (m_currentBlock && (!m_out.m_block->frequency() || (m_program.isColdInCallGraph(m_id) && !m_isInLoop) || isOncePerAsyncActivation || m_function.at(m_currentBlock->values[m_currentIndexInBlock]).checksAwaitResult() || checksRuntimeAllocatedValue(m_function.at(m_currentBlock->values[m_currentIndexInBlock]))) && !m_function.isSharedOutlinedBody && !m_checksExtraPropertyResult
        && !m_environmentAwaitingBarrier && !m_isFillingArrayAtCreation && m_fieldsAwaitingFirstStore.isEmpty() && repOf(guarantee) == Rep::JSValue && !(guarantee.primitives & Primitive::Absent) && holdsCells(repOf(type)) && &m_program.at(m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type) == &type
        && !m_useCastTest && !m_rejectsUntypedObjects && !handlerEnteredByBranch() && !Options::auditAOTTypedAssertions() && !Options::validateAOTTypedFacts()) {
        if (auto tests = typeCheckThunkTests(type))
            return convert(checkTypeWithThunk(rematerializedValue(convert(operand, guarantee, Rep::JSValue).as<Boxed>()), m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type, WTF::move(*tests)), type, repOf(type));
    }
    if (mayCastInRegister(guarantee, type) && !m_useCastTest && !m_rejectsUntypedObjects) {
        if (auto tests = castInRegisterTests(type, false, false)) {
            bool mayBeModifiedArray = !type.anyObject && !type.acceptsUncheckedObjects(m_program) && std::ranges::any_of(type.engineClasses, [&](uint16_t index) { return m_builtins.classes()[index].type == ArrayType; });
            return convert(castInRegister(rematerializedValue(convert(operand, guarantee, Rep::JSValue).as<Boxed>()), Entry::operationAOTTypedCoerceToType, true, m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type.index(), { Writes::Nothing, true }, WTF::move(*tests), mayBeModifiedArray), type, repOf(type));
        }
    }
    bool isInColdBlockOfColdFunction = !m_out.m_block->frequency() && m_currentBlock && (m_program.isColdInCallGraph(m_id) || (prefersGenericPathInColdCode() && !m_checksUntypedObjectResult));
    if ((m_program.isColdInCallGraph(m_id) || isInColdBlockOfColdFunction) && !m_isInLoop && !m_function.isSharedOutlinedBody
        && !m_environmentAwaitingBarrier && !m_isFillingArrayAtCreation && m_fieldsAwaitingFirstStore.isEmpty() && repOf(guarantee) == Rep::JSValue && holdsCells(repOf(type)) && &m_program.at(m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type) == &type
        && !m_useCastTest && !m_rejectsUntypedObjects && !handlerEnteredByBranch() && !Options::auditAOTTypedAssertions() && !Options::validateAOTTypedFacts()) {
        Boxed given = rematerializedValue(convert(operand, guarantee, Rep::JSValue).as<Boxed>());
        LValue number = m_out.constInt64(m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type.index());
        LValue held = callAllocationStub(Stub::CoerceToType, { { given.value(), GPRInfo::argumentGPR1 }, { number, GPRInfo::argumentGPR2 } }, { Writes::Nothing, true }, true, !m_out.m_block->frequency());
        return convert(Boxed(held), type, repOf(type));
    }
    if (isInColdBlockOfColdFunction && repOf(guarantee) == Rep::JSValue && holdsCells(repOf(type)) && &m_program.at(m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type) == &type
        && !m_useCastTest && !m_rejectsUntypedObjects && !handlerEnteredByBranch() && !Options::auditAOTTypedAssertions() && !Options::validateAOTTypedFacts()) {
        Boxed given = convert(operand, guarantee, Rep::JSValue).as<Boxed>();
        return convert(AOT_TYPED_OPERATION(EnforceType, rematerializedValue(given), word32(m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type.index())), type, repOf(type));
    }
    LBasicBlock mismatch = newColdBlock();
    Operand result = narrowOrElse(operand, guarantee, type, mismatch);
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
        if (auto& value = m_function.at(m_currentBlock->values[m_currentIndexInBlock]); value.reportsMismatchAtCaller() && &m_program.at(value.type) == &type)
            AOT_TYPED_OPERATION(RecordTypeMismatch, rematerializedValue(convert(operand, guarantee, Rep::JSValue).as<Boxed>()), word32(value.type.index()), word32(value.mismatchedArgumentIndex()), word32(2));
        else if (bool isEntryWithUntypedClone = m_program.logsTypeErrors && !m_function.has(FunctionLogsTypeMismatches) && m_function.has(Format::FunctionIsHiddenInStackTraces) && !m_function.has(Format::FunctionIsBuiltin) && value.opcode == Opcode::Cast && m_function.at(value.child(0)).opcode == Opcode::Parameter; (m_function.has(FunctionLogsTypeMismatches) || isEntryWithUntypedClone) && &m_program.at(value.type) == &type) {
            auto& held = m_function.at(value.child(0));
            uint32_t first = m_function.isGenericEntrypoint() ? 2 : m_function.captureTypes.size() + m_function.has(Format::FunctionHasThis);
            uint32_t parameter = UINT32_MAX;
            if (held.opcode == Opcode::Parameter && held.immediates[1] >= first)
                parameter = held.immediates[1] - first;
            else if (held.opcode == Opcode::GetArgument)
                parameter = held.immediates[0];
            AOT_TYPED_OPERATION(RecordTypeMismatch, rematerializedValue(convert(operand, guarantee, Rep::JSValue).as<Boxed>()), word32(value.type.index()), word32(parameter), word32(isEntryWithUntypedClone ? 3 : !m_function.isGenericEntrypoint() && !m_function.has(FunctionIsEnteredByValue)));
        }
        m_out.jump(handler);
    } else
        throwTypeMismatch(operand, guarantee, type);
    m_out.appendTo(continuation);
    return result;
}

Operand Lowering::ensureElement(CellPointer array, Integer index, Boxed element, const Type& type)
{
    TypeID given = m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type;
    Writes writes = writesOfElementRead(m_program, type);
    if (writes == Writes::Nothing || &m_program.at(given) != &type || Options::auditAOTTypedAssertions())
        return ensure(element, type);
    Rep rep = repOf(type);
    LBasicBlock typeMismatch = newColdBlock();
    LBasicBlock cellCase = newColdBlock();
    LBasicBlock plainObject = newColdBlock();
    LBasicBlock mismatch = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    join.add(narrowOrElse(element, m_program.at(m_program.types.anything), type, typeMismatch));
    m_out.appendTo(typeMismatch);
    m_out.branch(isCell(element).value(), usually(cellCase), rarely(mismatch));
    m_out.appendTo(cellCase);
    m_out.branch(logicalOr(equal(loadCellType(asCell(element)), word32(FinalObjectType)), equal(loadCellType(asCell(element)), word32(ObjectType))).value(), usually(plainObject), rarely(mismatch));
    m_out.appendTo(plainObject);
    OperationEffects effects { writes, true, true };
    join.add(convert(AOT_TYPED_OPERATION_WITH_EFFECTS(CoerceElement, effects, array, low32(index), word32(given.index())), type, rep));
    m_out.appendTo(mismatch);
    throwTypeMismatch(element, m_program.at(m_program.types.anything), type);
    return join.finish();
}

Operand Lowering::ensureProperty(CellPointer holder, StringID name, Boxed returnedValue, const Type& type)
{
    TypeID given = m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type;
    Writes writes = writesOfElementRead(m_program, type);
    SetForScope checksUntypedObjectResult(m_checksUntypedObjectResult, true);
    if (writes == Writes::Nothing || &m_program.at(given) != &type || Options::auditAOTTypedAssertions())
        return ensure(returnedValue, type);
    Rep rep = repOf(type);
    if (auto tests = !m_out.m_block->frequency() && holdsCells(rep) && !m_activeHandler && !handlerEnteredByBranch() && !m_program.logsTypeErrors && !Options::validateAOTTypedFacts() ? typeCheckThunkTests(type) : std::nullopt) {
        SetForScope checkedValue(m_valueCheckedByColdOperation, returnedValue.value());
        SetForScope checkedValueTests(m_coldOperationTypeTests, &*tests);
        return convert(AOT_TYPED_OPERATION_WITH_EFFECTS(CoerceProperty, (OperationEffects { writes, true, true }), holder, word32(name.index()), returnedValue, word32(given.index())), type, rep);
    }
    LBasicBlock typeMismatch = newColdBlock();
    LBasicBlock cellCase = newColdBlock();
    LBasicBlock plainObject = newColdBlock();
    LBasicBlock mismatch = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    join.add(narrowOrElse(returnedValue, m_program.at(m_program.types.anything), type, typeMismatch));
    m_out.appendTo(typeMismatch);
    m_out.branch(isCell(returnedValue).value(), usually(cellCase), rarely(mismatch));
    m_out.appendTo(cellCase);
    Word32 mayBeChecked = type.acceptsUntypedObjects(m_program) ? aboveOrEqual(loadCellType(asCell(returnedValue)), word32(ObjectType)) : logicalOr(equal(loadCellType(asCell(returnedValue)), word32(FinalObjectType)), equal(loadCellType(asCell(returnedValue)), word32(ObjectType)));
    m_out.branch(mayBeChecked.value(), usually(plainObject), rarely(mismatch));
    m_out.appendTo(plainObject);
    OperationEffects effects { writes, true, true };
    join.add(convert(AOT_TYPED_OPERATION_WITH_EFFECTS(CoerceProperty, effects, holder, word32(name.index()), returnedValue, word32(given.index())), type, rep));
    m_out.appendTo(mismatch);
    throwTypeMismatch(returnedValue, m_program.at(m_program.types.anything), type);
    return join.finish();
}

Word32 Lowering::auditLineAndColumn()
{
    LineColumn where = m_program.lineColumnFor(m_function.module, m_function.at(m_currentBlock->values[m_currentIndexInBlock]).position);
    return word32(AuditSite::pack(where.line, where.column));
}

ValueID Lowering::nextStoreReceiver(ValueID stored) const
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
    bool isStoreOfValue = (store.opcode == Opcode::PutById && store.child(1) == stored) || (store.opcode == Opcode::PutByVal && store.child(2) == stored);
    if (!isStoreOfValue || (block != m_currentBlock && block->parameters.contains(store.child(0))))
        return { };
    return store.child(0);
}

Boxed Lowering::assertionFailed(Operand operand, const Type& from, TypeID wanted, uint32_t what, ValueID storedTo)
{
    if (storedTo)
        return AOT_TYPED_OPERATION(StoreAssertionFailed, convertTo<Boxed>(operand, from), in<Boxed>(storedTo), word32(wanted.index()), word32(m_id.index()), auditLineAndColumn());
    return AOT_TYPED_OPERATION(AssertionFailed, convertTo<Boxed>(operand, from), word32(wanted.index()), word32(what), word32(m_id.index()), auditLineAndColumn());
}

void Lowering::auditResult(Operand operand, const Value& value)
{
    auto& type = m_program.at(value.type);
    auto& asserted = m_program.at(value.assertedType());
    if (type.isSubsetOf(asserted, m_program))
        return;
    LBasicBlock mismatch = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    narrowOrElse(operand, type, asserted, mismatch);
    m_out.jump(continuation);
    m_out.appendTo(mismatch);
    assertionFailed(operand, type, value.assertedType(), assertionIsOnResult | static_cast<uint32_t>(value.opcode));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::validateSingleFunction(Operand stored, ValueID id, FunctionID function, bool mayBeNullish, FunctionID other)
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
    if (mayBeNullish)
        m_out.branch(isUndefinedOrNull(boxed).value(), unsure(continuation), rarely(doesNotHold));
    else
        m_out.jump(doesNotHold);
    m_out.appendTo(cellCase);
    auto entrypoint = m_program.genericEntrypoints.find(function.index());
    if (other) {
        SetForScope isReporting(m_isReportingFactViolation, true);
        m_out.branch(logicalOr(isClosureOf(asCell(boxed), function, false, true), isClosureOf(asCell(boxed), other, false, true)).value(), unsure(continuation), rarely(doesNotHold));
    } else if (entrypoint == m_program.genericEntrypoints.end())
        m_out.jump(doesNotHold);
    else {
        SetForScope isReporting(m_isReportingFactViolation, true);
        Integer word(callAllocationStub(Stub::TypedClosureEntry, { { boxed.value(), stubTemporaryGPRs[0] } }, { Writes::Nothing, false }, false));
        Word32 wordFunction(m_out.bitAnd(m_out.castToInt32(m_out.lShr(word.value(), m_out.constInt32(ClosureWord::functionShift))), m_out.constInt32(static_cast<int32_t>(ClosureWord::functionMask))));
        m_out.branch(logicalAnd(logicalNot(isZero(word)), equal(wordFunction, word32(entrypoint->value.index()))).value(), unsure(continuation), rarely(doesNotHold));
    }
    m_out.appendTo(doesNotHold);
    AOT_TYPED_OPERATION(FactViolated, boxed, word32(m_id.index()), word32(id.index()));
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
    if (m_entriesCreatedAsRecords.get(source.index()) || m_entryPairs.contains(source.index()))
        return;
    for (LayoutID supertype : type.objectLayouts) {
        if (!isObjectLayout(m_program.at(supertype).kind))
            return;
    }
    Type held;
    switch (operand.rep) {
    case Rep::Void:
    case Rep::LiteralString:
        return;
    case Rep::Int32:
        held = Type::fromPrimitives(Primitive::Boolean);
        break;
    case Rep::Int64:
    case Rep::OptionalInt64:
        held = Type::integer({ -maxTrackedInteger, maxTrackedInteger });
        break;
    case Rep::Double:
    case Rep::OptionalDouble:
        held = Type::fromPrimitives(Primitive::Number);
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
    if (held.isSubsetOf(type, m_program))
        return;
    LBasicBlock doesNotHold = newColdBlock();
    {
        SetForScope isValidatingFact(m_isValidatingFact, true);
        if (operand.rep == Rep::JSValue)
            Emitter::orElse(m_out.notZero64(operand.value), doesNotHold);
        narrowOrElse(operand, held, type, doesNotHold);
    }
    LBasicBlock continuation = m_out.newBlock();
    m_out.jump(continuation);
    m_out.appendTo(doesNotHold);
    AOT_TYPED_OPERATION(FactViolated, convertTo<Boxed>(operand, held), word32(m_id.index()), word32(id.index()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

bool Lowering::markNullishTest(CellPointer accessed)
{
    if (!accessed || m_activeHandler)
        return false;
    LValue number = m_out.constInt32(m_context.nullishTests.size());
    PatchpointValue* marker = m_out.patchpoint(Void);
    marker->effects = Effects::none();
    marker->effects.writesLocalState = true;
    marker->append(ConstrainedValue(accessed.value(), ValueRep::ColdAny));
    marker->append(ConstrainedValue(number, ValueRep::ColdAny));
    marker->setGenerator(m_context.nullishTestMarker);
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
        SetForScope isReporting(m_isReportingFactViolation, true);
        AOT_TYPED_OPERATION(FactViolated, Boxed(m_out.int64Zero), word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
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
    AOT_TYPED_OPERATION(FactViolated, value, word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
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
    bool mayFault = markNullishTest(accessed);
    uint32_t callSite = throwAt(kind, Operand(thrownNullish(value)), name.index());
    if (mayFault)
        m_context.nullishTests.append({ kind, name ? name.index() : ThrowSite::none, std::nullopt, callSite });
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
    return !memory.hasFence() && offset >= 0 && offset + static_cast<int64_t>(memory.accessByteSize()) <= maxFaultingAccessOffset && memory.isLegalOffset(offset);
}

template<typename AddressType>
static void emitAccess(CCallHelpers& jit, B3::Opcode opcode, B3::Type type, bool isSignExtended, const ValueRep& value, AddressType address)
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

#if CPU(X86_64) && OS(LINUX)
static PatchpointValue* loopPollHandler(LoweringContext& context, BasicBlock* slowPath, BasicBlock* continuation)
{
    if (slowPath->numPredecessors() != 1 || slowPath->last()->opcode() != B3::Jump || slowPath->successorBlock(0) != continuation)
        return nullptr;
    PatchpointValue* handler = nullptr;
    for (B3::Value* value : *slowPath) {
        if (value == slowPath->last() || value->opcode() == B3::Nop || value->isConstant())
            continue;
        if (handler || !value->as<PatchpointValue>())
            return nullptr;
        handler = value->as<PatchpointValue>();
    }
    if (!handler || handler->numChildren() || handler->type() != B3::Void || !context.loopPollSites.contains(handler->generator()))
        return nullptr;
    return handler;
}
#endif

void useMemoryOperandsAtLoopPolls(Procedure& proc, LoweringContext& context)
{
#if CPU(X86_64)
    if (!proc.loopPollGenerator())
        return;
    bool didRemoveBlocks = false;
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
#if OS(LINUX)
        BasicBlock* slowPath = block->successorBlock(0);
        BasicBlock* continuation = block->successorBlock(1);
        if (PatchpointValue* handler = loopPollHandler(context, slowPath, continuation)) {
            for (B3::Value*& value : *slowPath) {
                if (value == handler)
                    value = proc.add<B3::Value>(B3::Nop, B3::Void, handler->origin());
            }
            handler->setGenerator([context = &context, site = context.loopPollSites.get(handler->generator())](CCallHelpers& jit, const StackmapGenerationParams& params) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                context->loopPollTraps.append({ jit.labelIgnoringWatchpoints(), site });
                size_t start = jit.m_assembler.codeSize();
                jit.load32(CCallHelpers::Address(instanceGPR, Instance::offsetOfLoopPollPage() + (context->hasNoFrame(params.proc().code()) ? offsetOfLeafPollInLoopPollPage : 0)), CCallHelpers::s_scratchRegister);
                RELEASE_ASSERT(jit.m_assembler.codeSize() - start == sizeOfLoopPoll);
            });
            block->appendNonTerminal(handler);
            block->replaceLast(proc, proc.add<B3::Value>(B3::Jump, branch->origin()));
            block->setSuccessors(B3::FrequentedBlock(continuation));
            comparison->replaceWithNopIgnoringType();
            poll->replaceWithNopIgnoringType();
            didRemoveBlocks = true;
            continue;
        }
#endif
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
    if (didRemoveBlocks) {
        proc.resetReachability();
        proc.invalidateCFG();
    }
    UNUSED_PARAM(context);
#else
    UNUSED_PARAM(proc);
    UNUSED_PARAM(context);
#endif
}

void narrowZeroExtendedComparisons(Procedure& proc)
{
    if (!proc.hasEverAdded(B3::ZExt32) && !proc.hasEverAdded(B3::ZShr))
        return;
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

void storeAdjacentConstantsTogether(Procedure& proc)
{
    if (!proc.hasEverAdded(B3::Store))
        return;
    struct ConstantStore {
        unsigned index;
        MemoryValue* store;
    };
    struct Pair {
        ConstantStore low;
        ConstantStore high;
        uint64_t constant;
        unsigned run;
    };
    auto isPlainStore = [](B3::Value* value) {
        return (value->opcode() == B3::Store || value->opcode() == B3::Store8 || value->opcode() == B3::Store16) && !value->kind().traps() && !value->as<MemoryValue>()->hasFence();
    };
    InsertionSet insertionSet(proc);
    Vector<ConstantStore, 32> stores;
    Vector<Pair, 16> pairs;
    for (BasicBlock* block : proc) {
        B3::Value* base = nullptr;
        unsigned run = 0;
        pairs.shrink(0);
        auto endRun = [&] {
            std::ranges::sort(stores, { }, [](const ConstantStore& one) {
                return one.store->offset();
            });
            for (unsigned i = 0; i + 1 < stores.size(); ++i) {
                auto& low = stores[i];
                auto& high = stores[i + 1];
                if (low.store->offset() % 8 || high.store->offset() != low.store->offset() + 4)
                    continue;
                pairs.append({ low, high, static_cast<uint32_t>(low.store->child(0)->asInt32()) | static_cast<uint64_t>(static_cast<uint32_t>(high.store->child(0)->asInt32())) << 32, run });
            }
            stores.shrink(0);
            base = nullptr;
            ++run;
        };
        for (unsigned index = 0; index < block->size(); ++index) {
            B3::Value* value = block->at(index);
            if (isPlainStore(value)) {
                auto* store = value->as<MemoryValue>();
                int64_t begin = store->offset();
                int64_t end = begin + static_cast<int64_t>(store->accessByteSize());
                bool overlaps = std::ranges::any_of(stores, [&](const ConstantStore& other) {
                    return begin < static_cast<int64_t>(other.store->offset()) + 4 && other.store->offset() < end;
                });
                if (store->lastChild() != base || overlaps) {
                    endRun();
                    base = store->lastChild();
                }
                if (store->opcode() == B3::Store && store->child(0)->hasInt32())
                    stores.append({ index, store });
                continue;
            }
            if (stores.isEmpty())
                continue;
            Effects effects = value->effects();
            if (effects.mustExecute() || effects.reads || effects.readsLocalState || effects.readsPinned || effects.controlDependent)
                endRun();
        }
        endRun();
        std::ranges::sort(pairs, [](const Pair& a, const Pair& b) {
            return std::tuple(a.constant, a.run, a.low.store->offset()) < std::tuple(b.constant, b.run, b.low.store->offset());
        });
        for (unsigned first = 0, end = 0; first < pairs.size(); first = end) {
            unsigned adjacent = 0;
            for (end = first + 1; end < pairs.size() && pairs[end].constant == pairs[first].constant; ++end);
            for (unsigned i = first; i + 1 < end; ++i) {
                if (pairs[i + 1].run != pairs[i].run || pairs[i + 1].low.store->offset() != pairs[i].low.store->offset() + 8)
                    continue;
                ++adjacent;
                ++i;
            }
            uint64_t constant = pairs[first].constant;
            bool isSmaller = isARM64()
                ? adjacent > (constant && !ARM64LogicalImmediate::create64(constant).isValid() ? 1u : 0u)
                : end - first >= 2 || isRepresentableAs<int32_t>(static_cast<int64_t>(constant));
            if (!isSmaller) {
                for (unsigned i = first; i < end; ++i)
                    pairs[i].low.store = nullptr;
            }
        }
        for (auto& pair : pairs) {
            if (!pair.low.store)
                continue;
            unsigned index = std::max(pair.low.index, pair.high.index);
            Origin origin = block->at(index)->origin();
            B3::Value* constant = insertionSet.insert<Const64Value>(index, origin, static_cast<int64_t>(pair.constant));
            insertionSet.insert<MemoryValue>(index, B3::Store, origin, constant, pair.low.store->lastChild(), pair.low.store->offset(), pair.low.store->range() | pair.high.store->range());
            pair.low.store->replaceWithNop();
            pair.high.store->replaceWithNop();
        }
        insertionSet.execute(block);
    }
}

void narrowSignExtendedComparisons(Procedure& proc)
{
    if (!proc.hasEverAdded(B3::SExt32))
        return;
    auto isSignExtended = [](B3::Value* value) {
        return value->opcode() == B3::SExt32;
    };
    auto isSubsetOf = [](B3::Value* value, int64_t minimum, int64_t greatest) {
        return value->hasInt64() && value->asInt64() >= minimum && value->asInt64() <= greatest;
    };
    InsertionSet insertionSet(proc);
    for (BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            B3::Value* value = block->at(index);
            auto narrowConstant = [&](B3::Value* constant) {
                return insertionSet.insert<Const32Value>(index, value->origin(), static_cast<int32_t>(constant->asInt64()));
            };
            auto narrowMask = [&](B3::Value* masked) -> B3::Value* {
                if (masked->opcode() != B3::BitAnd || masked->type() != Int64 || !isSignExtended(masked->child(0)) || !isSubsetOf(masked->child(1), 0, std::numeric_limits<int32_t>::max()))
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
                } else if (isSubsetOf(value->child(1), std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max())) {
                    value->child(1) = narrowConstant(value->child(1));
                    value->child(0) = value->child(0)->child(0);
                }
                break;
            case B3::Below:
            case B3::BelowEqual:
            case B3::Above:
            case B3::AboveEqual:
                if (isSignExtended(value->child(0)) && isSubsetOf(value->child(1), 0, std::numeric_limits<int32_t>::max())) {
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

void loadDoubleConstantsFromInstance(Procedure& proc)
{
#if CPU(X86_64)
    if (!proc.hasEverAdded(B3::ConstDouble))
        return;
    B3::Value* instance = nullptr;
    for (B3::Value* value : *proc[0]) {
        if (auto* argument = value->as<ArgumentRegValue>(); argument && argument->argumentReg() == Reg(instanceGPR))
            instance = argument;
    }
    bool allocatedInstance = false;
    InsertionSet insertionSet(proc);
    for (BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            B3::Value* value = block->at(index);
            auto* stackmap = value->as<StackmapValue>();
            for (unsigned i = 0; i < value->numChildren(); ++i) {
                B3::Value*& child = value->child(i);
                if (!child->hasDouble() || (stackmap && stackmap->constrainedChild(i).rep().isAny()))
                    continue;
                auto offset = Instance::offsetOfDouble(child->asDouble());
                if (!offset)
                    continue;
                if (!instance) {
                    instance = proc.add<ArgumentRegValue>(B3::Origin(), Reg(instanceGPR));
                    allocatedInstance = true;
                }
                auto* load = insertionSet.insert<MemoryValue>(index, B3::Load, B3::Double, value->origin(), instance, static_cast<int32_t>(*offset), HeapRange());
                load->setControlDependent(false);
                load->setReadsMutability(B3::Mutability::Immutable);
                child = load;
            }
        }
        insertionSet.execute(block);
    }
    if (allocatedInstance)
        proc[0]->values().insert(0, instance);
#else
    UNUSED_PARAM(proc);
#endif
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

        UncheckedKeyHashMap<B3::Value*, unsigned> sinkableUses;
        sinkableUses.add(second, 1);
        Vector<B3::Value*, 8> sunk;
        bool crossesWrite = false;
        auto& values = block->values();
        for (unsigned i = values.size() - 1; i--;) {
            B3::Value* value = values[i];
            if (value == condition)
                continue;
            B3::Effects effects = value->effects();
            auto found = sinkableUses.find(value);
            bool reads = effects.reads || effects.readsPinned;
            if (found != sinkableUses.end() && found->value == useCounts.numUses(value) && !effects.mustExecute() && !effects.readsLocalState && !(reads && crossesWrite)) {
                sunk.append(value);
                for (B3::Value* child : value->children())
                    sinkableUses.add(child, 0).iterator->value++;
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

#if CPU(X86_64)
static bool fuseCompareWithMemoryAtBlockEnd(Procedure& proc, LoweringContext& context, B3::UseCountsWithoutUsingInstructions& useCounts, B3::BasicBlock* block, unsigned index, B3::Value* pointer, uint32_t nullishTest)
{
    auto* memory = block->at(index)->as<MemoryValue>();
    if ((memory->opcode() != Load && memory->opcode() != Load8Z && memory->opcode() != Load16Z) || memory->type() != Int32 || useCounts.numUses(memory) != 1)
        return false;
    B3::Value* branch = block->last();
    if (branch->opcode() != Branch)
        return false;
    B3::Value* comparison = branch->child(0);
    if ((comparison->opcode() != B3::Equal && comparison->opcode() != B3::NotEqual && comparison->opcode() != B3::AboveEqual && comparison->opcode() != B3::Below) || comparison->owner != block || useCounts.numUses(comparison) != 1 || comparison->child(0) != memory || !comparison->child(1)->hasInt32())
        return false;
    int32_t constant = comparison->child(1)->asInt32();
    if (memory->opcode() == Load8Z && static_cast<uint32_t>(constant) > 0xff)
        return false;
    if (memory->opcode() == Load16Z && static_cast<uint32_t>(constant) > 0xffff)
        return false;
    for (unsigned i = index + 1; i + 1 < block->size(); ++i) {
        B3::Value* value = block->at(i);
        if (value == comparison)
            continue;
        if (value->as<MemoryValue>() || value->effects().mustExecute() || value->effects().reads || value->effects().readsPinned)
            return false;
    }
    PatchpointValue* test = proc.add<PatchpointValue>(Void, memory->origin());
    test->append(ConstrainedValue(pointer, ValueRep::SomeRegister));
    test->effects = memory->effects();
    test->effects.exitsSideways = true;
    test->effects.reads = HeapRange::top();
    test->effects.terminal = true;
    test->setGenerator([context = &context, isByte = memory->opcode() == Load8Z, isHalfword = memory->opcode() == Load16Z, opcode = comparison->opcode(), offset = memory->offset(), constant, nullishTest](CCallHelpers& jit, const StackmapGenerationParams& params) {
        GPRReg base = params[0].gpr();
        bool targetsSecond = params.fallsThroughToSuccessor(0);
        CCallHelpers::RelationalCondition condition = opcode == B3::Equal ? CCallHelpers::Equal : opcode == B3::NotEqual ? CCallHelpers::NotEqual : opcode == B3::AboveEqual ? CCallHelpers::AboveOrEqual : CCallHelpers::Below;
        if (targetsSecond)
            condition = CCallHelpers::invert(condition);
        CCallHelpers::Label instruction = jit.labelIgnoringWatchpoints();
        CCallHelpers::Jump leaves;
        if (params.size() == 2) {
            CCallHelpers::BaseIndex address(base, params[1].gpr(), static_cast<CCallHelpers::Scale>(cell32Shift), offset);
            leaves = isByte ? jit.branch8(condition, address, CCallHelpers::TrustedImm32(constant)) : isHalfword ? jit.branch16(condition, address, CCallHelpers::TrustedImm32(constant)) : jit.branch32(condition, address, CCallHelpers::TrustedImm32(constant));
            base = params[1].gpr();
        } else {
            CCallHelpers::Address address(base, offset);
            leaves = isByte ? jit.branch8(condition, address, CCallHelpers::TrustedImm32(constant)) : isHalfword ? jit.branch16(condition, address, CCallHelpers::TrustedImm32(constant)) : jit.branch32(condition, address, CCallHelpers::TrustedImm32(constant));
        }
        context->faultingAccesses.append({ instruction, base, nullishTest });
        CCallHelpers::Jump goesOn;
        if (!targetsSecond && !params.fallsThroughToSuccessor(1))
            goesOn = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            leaves.linkTo(*labels[targetsSecond], &jit);
            if (goesOn.isSet())
                goesOn.linkTo(*labels[1], &jit);
        });
    });
    context.accessesThroughPointer.add(test, 0);
    block->replaceLast(proc, test);
    comparison->replaceWithNopIgnoringType();
    memory->replaceWithNopIgnoringType();
    return true;
}
#endif

#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
void checkShadowBeforeAccesses(Procedure& proc)
{
    constexpr int32_t shadowOffset = 0x7fff8000;
    InsertionSet insertionSet(proc);
    for (B3::BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            auto* memory = block->at(index)->as<MemoryValue>();
            if (!memory || memory->traps())
                continue;
            B3::Value* pointer = memory->lastChild();
            if (pointer->opcode() == SlotBase || pointer->opcode() == FramePointer)
                continue;
            PatchpointValue* test = insertionSet.insert<PatchpointValue>(index, Void, memory->origin());
            test->append(ConstrainedValue(pointer, ValueRep::SomeRegister));
            test->numGPScratchRegisters = 2;
            test->effects = Effects::none();
            test->effects.exitsSideways = true;
            test->effects.reads = HeapRange::top();
            test->setGenerator([offset = memory->offset(), size = static_cast<int32_t>(memory->accessByteSize()), isStore = memory->isStore()](CCallHelpers& jit, const StackmapGenerationParams& params) {
                GPRReg base = params[0].gpr();
                GPRReg shadow = params.gpScratch(0);
                GPRReg last = params.gpScratch(1);
                jit.addPtr(CCallHelpers::TrustedImm32(offset), base, shadow);
                jit.urshiftPtr(CCallHelpers::TrustedImm32(3), shadow);
                CCallHelpers::Jump mayBePoisoned = jit.branchTest8(CCallHelpers::NonZero, CCallHelpers::Address(shadow, shadowOffset));
                CCallHelpers::Label done = jit.label();
                params.addLatePath([=](CCallHelpers& jit) {
                    AllowMacroScratchRegisterUsage allowScratch(jit);
                    mayBePoisoned.link(&jit);
                    if (size < 8) {
                        jit.load8SignedExtendTo32(CCallHelpers::Address(shadow, shadowOffset), shadow);
                        jit.addPtr(CCallHelpers::TrustedImm32(offset), base, last);
                        jit.and32(CCallHelpers::TrustedImm32(7), last);
                        jit.add32(CCallHelpers::TrustedImm32(size - 1), last);
                        jit.branch32(CCallHelpers::LessThan, last, shadow).linkTo(done, &jit);
                    }
                    jit.addPtr(CCallHelpers::TrustedImm32(offset), base, GPRInfo::argumentGPR0);
                    jit.move(CCallHelpers::TrustedImm32(size | isStore << 8), GPRInfo::argumentGPR1);
                    jit.andPtr(CCallHelpers::TrustedImm32(-16), CCallHelpers::stackPointerRegister);
                    jit.move(instanceGPR, GPRInfo::argumentGPR2);
                    jit.loadPtr(CCallHelpers::Address(instanceGPR, Instance::offsetOfRuntimeTable()), GPRInfo::argumentGPR3);
                    jit.call(CCallHelpers::Address(GPRInfo::argumentGPR3, static_cast<unsigned>(Entry::operationAOTReportPoisonedAccess) * sizeof(void*)), OperationPtrTag);
                    jit.breakpoint();
                });
            });
        }
        insertionSet.execute(block);
    }
}
#endif

void useFaultsForNullishTests(Procedure& proc, LoweringContext& context)
{
    if (context.nullishTests.isEmpty())
        return;
    InsertionSet insertionSet(proc);
    bool didChange = false;
    UncheckedKeyHashMap<B3::Value*, Vector<B3::Value*, 1>> extensionsOf;
    for (B3::Value* value : proc.values()) {
        if ((value->opcode() == ZExt32 || value->opcode() == SExt32) && value->child(0)->as<MemoryValue>())
            extensionsOf.add(value->child(0), Vector<B3::Value*, 1> { }).iterator->value.append(value);
    }
#if CPU(X86_64)
    B3::UseCountsWithoutUsingInstructions useCounts(proc);
#endif
    for (B3::BasicBlock* arm : proc) {
        PatchpointValue* marker = nullptr;
        for (B3::Value* value : *arm) {
            if (auto* patchpoint = value->as<PatchpointValue>(); patchpoint && patchpoint->generator() == context.nullishTestMarker.get()) {
                marker = patchpoint;
                break;
            }
        }
        if (!marker)
            continue;
        B3::Value* pointer = marker->child(0);
        uint32_t nullishTest = marker->child(1)->asInt32();
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
            if (auto* latePathCall = value->as<PatchpointValue>()) {
                if (auto found = context.latePathCallAccesses.find(latePathCall->generator()); found != context.latePathCallAccesses.end() && latePathCall->child(found->value->child) == pointer) {
                    found->value->nullishTest = nullishTest;
                    block->replaceLastWithNew<B3::Value>(proc, Jump, block->last()->origin());
                    block->setSuccessors(FrequentedBlock(next));
                    arm->removePredecessor(block);
                    didChange = true;
                    break;
                }
            }
            auto* memory = value->as<MemoryValue>();
            if (!memory || memory->lastChild() != pointer || !isPlainAccess(*memory)) {
                Effects effects = value->effects();
                if (effects.terminal || effects.exitsSideways || effects.writesLocalState || effects.writesPinned || effects.fence || effects.writes)
                    break;
                continue;
            }
            bool isStore = memory->isStore();
#if CPU(X86_64)
            if (fuseCompareWithMemoryAtBlockEnd(proc, context, useCounts, next, index, pointer, nullishTest)) {
                block->replaceLastWithNew<B3::Value>(proc, Jump, block->last()->origin());
                block->setSuccessors(FrequentedBlock(next));
                arm->removePredecessor(block);
                didChange = true;
                break;
            }
#endif
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
            access->setGenerator([context = &context, opcode = memory->opcode(), type, isSignExtended, offset = memory->offset(), nullishTest](CCallHelpers& jit, const StackmapGenerationParams& params) {
                CCallHelpers::Label instruction = jit.labelIgnoringWatchpoints();
#if CPU(X86_64)
                if (params.size() == 3) {
                    emitAccess(jit, opcode, type, isSignExtended, params[0], CCallHelpers::BaseIndex(params[1].gpr(), params[2].gpr(), static_cast<CCallHelpers::Scale>(cell32Shift), offset));
                    context->faultingAccesses.append({ instruction, params[2].gpr(), nullishTest });
                    return;
                }
#endif
                emitAccess(jit, opcode, type, isSignExtended, params[0], CCallHelpers::Address(params[1].gpr(), offset));
                context->faultingAccesses.append({ instruction, params[1].gpr(), nullishTest });
            });
            context.accessesThroughPointer.add(access, isStore ? 1 : 0);
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

static B3::Value* wordSource(B3::Value* value)
{
    for (;;) {
        switch (value->opcode()) {
        case B3::Identity:
        case B3::ZExt32:
        case B3::Trunc:
            value = value->child(0);
            continue;
        case B3::Select:
            if (value->child(1)->isConstant()) {
                value = value->child(2);
                continue;
            }
            if (value->child(2)->isConstant()) {
                value = value->child(1);
                continue;
            }
            return value;
        default:
            return value;
        }
    }
}

static bool isCellCageBase(B3::Value* value)
{
#if CPU(ARM64) || CPU(X86_64)
    return value->opcode() == B3::ArgumentReg && value->as<ArgumentRegValue>()->argumentReg() == cellCageBaseGPR;
#else
    auto* memory = value->as<MemoryValue>();
    if (!memory || memory->opcode() != B3::Load || memory->offset() != Instance::offsetOfCellCageBase())
        return false;
    B3::Value* instance = memory->lastChild();
    return instance->opcode() == B3::ArgumentReg && instance->as<ArgumentRegValue>()->argumentReg() == instanceGPR;
#endif
}

struct WordDecoding {
    B3::Value* base { nullptr };
    B3::Value* word { nullptr };
};

static WordDecoding wordDecodingOf(B3::Value* pointer)
{
    if (pointer->opcode() != B3::Opaque || pointer->child(0)->opcode() != B3::Add)
        return { };
    B3::Value* sum = pointer->child(0);
    for (unsigned i = 0; i < 2; ++i) {
        B3::Value* shifted = sum->child(i);
        if (shifted->opcode() == B3::Shl && shifted->child(1)->isInt32(cell32Shift) && isCellCageBase(sum->child(!i)))
            return { sum->child(!i), shifted->child(0) };
    }
    return { };
}

void foldDecodingsIntoAccesses(Procedure& proc, LoweringContext& context)
{
    if (!context.keepsCellsAliveForWords)
        return;
    struct Candidate {
        Vector<B3::Value*, 4> accesses;
        B3::Value* boxing { nullptr };
        bool zeroReadsUndefined { false };
        unsigned numberOfUses { 0 };
        unsigned removedUseCount { 0 };
        bool isRefused { false };
    };
    UncheckedKeyHashMap<B3::Value*, Candidate> candidates;
    UncheckedKeyHashMap<B3::Value*, std::pair<B3::Value*, unsigned>> loadsAwaitingUser;
    IndexMap<B3::Value*, unsigned> wordEpoch(proc.values().size(), 0u);
    IndexMap<B3::BasicBlock*, unsigned> endEpoch(proc.size(), 0u);
    Vector<B3::Value*, 16> needless;
    unsigned lastEpoch = 0;
    unsigned epoch = 0;
    auto isFresh = [&](B3::Value* word) {
        return wordEpoch[wordSource(word)] == epoch;
    };
    auto recordAccess = [&](B3::Value* pointer, B3::Value* access) {
        if (pointer->opcode() != B3::Opaque)
            return false;
        Candidate& candidate = candidates.add(pointer, Candidate { }).iterator->value;
        WordDecoding decoding = wordDecodingOf(pointer);
        if (!decoding.word || !isFresh(decoding.word)) {
            candidate.isRefused = true;
            return false;
        }
        candidate.accesses.append(access);
        return true;
    };
#if CPU(X86_64) || CPU(ARM64)
    auto recordBoxing = [&](B3::Value* select) {
        bool pointerIsLast = select->child(2)->opcode() == B3::Opaque;
        B3::Value* pointer = select->child(pointerIsLast ? 2 : 1);
        if (pointer->opcode() != B3::Opaque)
            return;
        Candidate& candidate = candidates.find(pointer)->value;
        WordDecoding decoding = wordDecodingOf(pointer);
        B3::Value* condition = select->child(0);
        B3::Value* ifNullish = select->child(pointerIsLast ? 1 : 2);
        if (candidate.boxing || !decoding.word || !isFresh(decoding.word) || select->type() != Int64
            || condition->opcode() != (pointerIsLast ? B3::Below : B3::AboveEqual) || !condition->child(1)->hasInt() || condition->child(1)->asInt() != undefinedCell32 + 1
            || wordSource(condition->child(0)) != wordSource(decoding.word) || wordSource(ifNullish) != wordSource(decoding.word)) {
            candidate.isRefused = true;
            return;
        }
        while (ifNullish->opcode() == B3::Identity)
            ifNullish = ifNullish->child(0);
        B3::Value* ifZero = nullptr;
        B3::Value* otherwise = nullptr;
        if (ifNullish->opcode() == B3::Select) {
            B3::Value* test = ifNullish->child(0);
            bool comparesWithZero = (test->opcode() == B3::Equal || test->opcode() == B3::NotEqual) && test->child(1)->isInt(0) && wordSource(test->child(0)) == wordSource(decoding.word);
            if (comparesWithZero || wordSource(test) == wordSource(decoding.word)) {
                bool firstIsForZero = comparesWithZero && test->opcode() == B3::Equal;
                ifZero = ifNullish->child(firstIsForZero ? 1 : 2);
                otherwise = ifNullish->child(firstIsForZero ? 2 : 1);
            }
        }
        if (!ifZero || !ifZero->isInt64(undefinedCell32) || otherwise->opcode() == B3::Select) {
            candidate.isRefused = true;
            return;
        }
        candidate.zeroReadsUndefined = true;
        candidate.boxing = select;
    };
#endif
    for (B3::BasicBlock* block : proc.blocksInPreOrder()) {
        epoch = block->numPredecessors() == 1 ? endEpoch[block->predecessor(0)] : 0;
        if (!epoch)
            epoch = ++lastEpoch;
        loadsAwaitingUser.clear();
        for (B3::Value* value : *block) {
            for (B3::Value* child : value->children()) {
                if (child->opcode() == B3::Opaque)
                    candidates.add(child, Candidate { }).iterator->value.numberOfUses++;
                if (loadsAwaitingUser.isEmpty())
                    continue;
                auto found = loadsAwaitingUser.find(child);
                if (found == loadsAwaitingUser.end())
                    continue;
                if (found->value.second != epoch)
                    candidates.find(found->value.first)->value.isRefused = true;
                loadsAwaitingUser.remove(found);
            }
            if (auto* memory = value->as<MemoryValue>()) {
                if (isPlainAccess(*memory) && recordAccess(memory->lastChild(), memory) && !memory->isStore())
                    loadsAwaitingUser.add(memory, std::pair { memory->lastChild(), epoch });
                if (memory->opcode() == B3::Load && memory->type() == Int32)
                    wordEpoch[memory] = epoch;
                continue;
            }
            auto* patchpoint = value->as<PatchpointValue>();
            if (!patchpoint) {
                if (value->opcode() == B3::CCall)
                    epoch = ++lastEpoch;
#if CPU(X86_64) || CPU(ARM64)
                if (value->opcode() == B3::Select)
                    recordBoxing(value);
#endif
                continue;
            }
            if (patchpoint->generator() == context.emptyGenerator.get()) {
                if (patchpoint->numChildren() != 2)
                    continue;
                if (!isFresh(patchpoint->child(1))) {
                    wordEpoch[wordSource(patchpoint->child(1))] = epoch;
                    continue;
                }
                needless.append(patchpoint);
                if (patchpoint->child(0)->opcode() == B3::Opaque)
                    candidates.find(patchpoint->child(0))->value.removedUseCount++;
                continue;
            }
            if (auto found = context.accessesThroughPointer.find(patchpoint); found != context.accessesThroughPointer.end()) {
                recordAccess(patchpoint->child(found->value), patchpoint);
                if (patchpoint->type() == Int64)
                    wordEpoch[patchpoint] = epoch;
                continue;
            }
            if (patchpoint->generator() == context.numberBoxingGenerator.get() || patchpoint->generator() == context.slotBlockFetchGenerator.get())
                continue;
            epoch = ++lastEpoch;
        }
        endEpoch[block] = epoch;
    }
    for (B3::Value* patchpoint : needless)
        patchpoint->replaceWithNop();
#if CPU(X86_64) || CPU(ARM64)
    UncheckedKeyHashMap<B3::Value*, std::pair<B3::Value*, bool>> boxings;
    for (auto& entry : candidates) {
        B3::Value* pointer = entry.key;
        Candidate& candidate = entry.value;
        if (!candidate.isRefused && candidate.boxing && candidate.accesses.isEmpty() && 1 + candidate.removedUseCount == candidate.numberOfUses) {
            boxings.add(candidate.boxing, std::pair { pointer, candidate.zeroReadsUndefined });
            continue;
        }
#if CPU(X86_64)
        if (candidate.isRefused || candidate.boxing || candidate.accesses.isEmpty() || candidate.accesses.size() > 3 || candidate.accesses.size() + candidate.removedUseCount != candidate.numberOfUses)
            continue;
        WordDecoding decoding = wordDecodingOf(pointer);
        for (B3::Value* access : candidate.accesses) {
            if (auto* memory = access->as<MemoryValue>()) {
                memory->lastChild() = pointer->child(0);
                continue;
            }
            auto* patchpoint = access->as<PatchpointValue>();
            patchpoint->child(context.accessesThroughPointer.get(patchpoint)) = decoding.base;
            patchpoint->append(ConstrainedValue(decoding.word, ValueRep::SomeRegister));
        }
#endif
    }
    if (boxings.isEmpty())
        return;
    RefPtr<StackmapGenerator> generator = createSharedTask<StackmapGeneratorFunction>([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        GPRReg result = params[0].gpr();
        GPRReg word = params[1].gpr();
        GPRReg base = params[2].gpr();
        if (result != word) {
            jit.getEffectiveAddress(CCallHelpers::BaseIndex(base, word, static_cast<CCallHelpers::Scale>(cell32Shift)), result);
            CCallHelpers::Jump isCell = jit.branch32(CCallHelpers::AboveOrEqual, word, CCallHelpers::TrustedImm32(undefinedCell32 + 1));
            jit.move(CCallHelpers::TrustedImm32(undefinedCell32), result);
            jit.moveConditionallyTest32(CCallHelpers::NonZero, word, word, word, result);
            isCell.link(&jit);
            return;
        }
        CCallHelpers::Jump isCell = jit.branch32(CCallHelpers::AboveOrEqual, word, CCallHelpers::TrustedImm32(undefinedCell32 + 1));
        CCallHelpers::JumpList done;
        done.append(jit.branchTest32(CCallHelpers::NonZero, word));
        jit.move(CCallHelpers::TrustedImm32(undefinedCell32), result);
        done.append(jit.jump());
        isCell.link(&jit);
        jit.getEffectiveAddress(CCallHelpers::BaseIndex(base, word, static_cast<CCallHelpers::Scale>(cell32Shift)), result);
        done.link(&jit);
    });
    InsertionSet insertionSet(proc);
    for (B3::BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            B3::Value* select = block->at(index);
            if (select->opcode() != B3::Select)
                continue;
            auto found = boxings.find(select);
            if (found == boxings.end())
                continue;
            WordDecoding decoding = wordDecodingOf(found->value.first);
            PatchpointValue* boxed = insertionSet.insert<PatchpointValue>(index, Int64, select->origin());
            boxed->effects = Effects::none();
            boxed->effects.writesLocalState = true;
            boxed->resultConstraints = { ValueRep::SomeRegister };
            boxed->append(ConstrainedValue(decoding.word, ValueRep::SomeRegister));
            boxed->append(ConstrainedValue(decoding.base, ValueRep::SomeRegister));
            boxed->setGenerator(generator);
            select->replaceWithIdentity(boxed);
        }
        insertionSet.execute(block);
    }
#endif
}

void branchOverUndefinedForAbsentCells(Procedure& proc)
{
#if CPU(X86_64)
    if (!cell32IsPointer)
        return;
    auto wordOf = [](B3::Value* value) {
        while (value->opcode() == B3::Identity || value->opcode() == B3::Opaque || value->opcode() == B3::ZExt32)
            value = value->child(0);
        return value;
    };
    RefPtr<StackmapGenerator> generator;
    InsertionSet insertionSet(proc);
    for (B3::BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            B3::Value* select = block->at(index);
            if (select->opcode() != B3::Select || select->type() != Int64)
                continue;
            B3::Value* test = select->child(0);
            bool comparesWithZero = (test->opcode() == B3::Equal || test->opcode() == B3::NotEqual) && test->child(1)->isInt(0);
            bool firstIsForZero = comparesWithZero && test->opcode() == B3::Equal;
            B3::Value* ifZero = select->child(firstIsForZero ? 1 : 2);
            B3::Value* otherwise = select->child(firstIsForZero ? 2 : 1);
            if (!ifZero->isInt64(undefinedCell32) || wordOf(otherwise)->type() != Int32 || wordOf(comparesWithZero ? test->child(0) : test) != wordOf(otherwise))
                continue;
            if (!generator) {
                generator = createSharedTask<StackmapGeneratorFunction>([](CCallHelpers& jit, const StackmapGenerationParams& params) {
                    GPRReg result = params[0].gpr();
                    GPRReg cell = params[1].gpr();
                    if (result != cell) {
                        jit.move(CCallHelpers::TrustedImm32(undefinedCell32), result);
                        jit.moveConditionallyTest32(CCallHelpers::NonZero, cell, cell, cell, result);
                        return;
                    }
                    CCallHelpers::Jump isPresent = jit.branchTest32(CCallHelpers::NonZero, cell);
                    jit.move(CCallHelpers::TrustedImm32(undefinedCell32), result);
                    isPresent.link(&jit);
                });
            }
            PatchpointValue* boxed = insertionSet.insert<PatchpointValue>(index, Int64, select->origin());
            boxed->effects = Effects::none();
            boxed->resultConstraints = { ValueRep::SomeRegister };
            boxed->append(ConstrainedValue(otherwise, ValueRep::SomeRegister));
            boxed->setGenerator(generator);
            select->replaceWithIdentity(boxed);
        }
        insertionSet.execute(block);
    }
#else
    UNUSED_PARAM(proc);
#endif
}

CellPointer Lowering::nonNullish(Operand operand, const Type& type, StringID name)
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

static CastReason runtimeAnnotationCastReason(const Value& cast)
{
    if (cast.castReason() == CastReason::Flow && cast.keepsUnconvertibleObjects())
        return CastReason::FlowIfConvertible;
    if (cast.castReason() != CastReason::As)
        return cast.castReason();
    if (cast.keepsUnconvertibleObjects())
        return cast.coercesFreshData() ? CastReason::AsFreshObjectIfConvertible : CastReason::AsIfConvertible;
    return cast.coercesFreshData() ? CastReason::AsFreshObject : CastReason::As;
}

Operand Lowering::coerce(Operand operand, const Type& from, const Value& cast, std::optional<OperationEffects> castEffects)
{
    auto& to = m_program.at(cast.type);
    Rep rep = repOf(to);
    bool isNarrowing = cast.castReason() == CastReason::Flow;
    if (!from.hasObjects() || to.objectLayouts.isEmpty() || m_program.castIsRedundant(from, to, cast))
        return ensure(operand, from, to);

    if (mayCastInRegister(from, to) && operand.rep == Rep::JSValue) {
        bool inspectsUntypedObjects = (!isNarrowing && (cast.coercesFreshData() || m_program.annotationCoercesUntypedObjects(to))) || m_program.isRelaxedForLogging(to);
        bool usesCastTest = cast.castReason() == CastReason::As || isNarrowing;
        if (auto tests = castInRegisterTests(to, usesCastTest, inspectsUntypedObjects)) {
            OperationEffects effects = castEffects ? *castEffects : OperationEffects { writesOfCast(m_program, m_function, cast), true, castWritesShapeWords(m_program, m_function, cast) };
            CastReason reason = runtimeAnnotationCastReason(cast);
            RELEASE_ASSERT(cast.type.index() < 1u << 28 && static_cast<uint32_t>(reason) < 16);
            return convert(castInRegister(rematerializedValue(operand.as<Boxed>()), Entry::operationAOTTypedCoerce, true, cast.type.index() << 4 | static_cast<uint32_t>(reason), effects, WTF::move(*tests), false), to, rep);
        }
    }

    LBasicBlock slowPath = newColdBlock();
    OperandJoin join(*this, joinRepFor(to));
    Operand fitted;
    {
        SetForScope isCoerced(m_rejectsUntypedObjects, (!isNarrowing && (cast.coercesFreshData() || m_program.annotationCoercesUntypedObjects(to))) || m_program.isRelaxedForLogging(to));
        SetForScope useCastTest(m_useCastTest, cast.castReason() == CastReason::As || isNarrowing);
        fitted = narrowOrElse(operand, from, to, slowPath);
    }
    join.add(convert(fitted, to, rep));

    m_out.appendTo(slowPath);
    OperationEffects effects = castEffects ? *castEffects : OperationEffects { writesOfCast(m_program, m_function, cast), true, castWritesShapeWords(m_program, m_function, cast) };
    bool onlyCellsReachHere = operand.rep == Rep::JSValue && withoutCells(from).isSubsetOf(withoutCells(to), m_program);
    Boxed passed = onlyCellsReachHere ? Boxed(partsOf(operand.as<Boxed>()).cell) : operand.rep == Rep::JSValue ? rematerializedValue(operand.as<Boxed>()) : convertTo<Boxed>(operand, from);
    if (!from.anyObject && !from.acceptsUntypedObjects(m_program) && !Options::auditAOTTypedAssertions()) {
        AOT_TYPED_OPERATION_WITH_EFFECTS(CoerceInPlace, effects, passed, typeAndReason(cast.type, cast.castReason()));
        join.add(convert(convert(operand, from, repOf(from)), to, rep));
        return join.finish();
    }
    RELEASE_ASSERT(isNarrowing || !m_program.castsMayRetag || effects.writesShapeWords || cast.castReason() != CastReason::As);
    Boxed coerced = AOT_TYPED_OPERATION_WITH_EFFECTS(Coerce, effects, passed, typeAndReason(cast.type, runtimeAnnotationCastReason(cast)));
    join.add(convert(coerced, to, rep));
    return join.finish();
}

static CastReason runtimeCastReason(const Value& cast)
{
    if (cast.keepsUnconvertibleObjects())
        return cast.castReason() == CastReason::Flow ? CastReason::FlowIfConvertible : CastReason::AsIfConvertible;
    if (cast.castReason() != CastReason::Coerce || !cast.coercesFreshData())
        return cast.castReason();
    return cast.freshness() == Format::Freshness::WholeGraph ? CastReason::CoerceFreshGraph : CastReason::CoerceFreshObject;
}

Word32 Lowering::typeAndReason(TypeID type, CastReason reason)
{
    static_assert(static_cast<uint32_t>(CastReason::FlowIfConvertible) < 16);
    RELEASE_ASSERT(type.index() < 1u << 28 && static_cast<uint32_t>(reason) < 16);
    return word32(type.index() << 4 | static_cast<uint32_t>(reason));
}

bool Lowering::mayPass(const Type& type, const ShapeTest& test) const
{
    if (test.neverPasses())
        return false;
    if (type.anyObject || type.acceptsUntypedObjects(m_program))
        return true;
    for (LayoutID supertype : type.objectLayouts) {
        for (LayoutID subtype : m_program.at(supertype).subtypes) {
            if (m_program.passes(test, m_program.at(subtype).shape))
                return true;
        }
    }
    return false;
}

Operand Lowering::lowerCoercion(ValueID result, const Value& cast)
{
    Operand operand = get(cast.child(0));
    auto& from = typeOf(cast.child(0));
    auto& type = typeOf(result);
    if (!m_isLoweringSunkCoercion && (coercionSinks(m_currentIndexInBlock) || coercionIsOnlyRead(m_currentIndexInBlock)))
        return convert(operand, from, repOf(type));
    return lowerCoercion(operand, from, type, cast);
}

Operand Lowering::lowerCoercion(Operand operand, const Type& from, const Type& type, const Value& cast, std::optional<OperationEffects> castEffects)
{
    auto& to = m_program.at(cast.coercedType());
    Rep rep = repOf(type);
    bool mayChangeObjects = holdsCells(rep) && coercionMayMutateObjects(m_program, from, to, cast.coercesFreshData());
    bool mayBeAnyObject = from.anyObject || from.acceptsUntypedObjects(m_program);
    bool mayCopy = mayChangeObjects && mayBeAnyObject && !to.objectLayouts.isEmpty() && cast.coercesFreshData();
    Vector<ShapeTest, 4> sources;
    for (LayoutID supertype : to.objectLayouts) {
        if (mayChangeObjects && mayPass(from, m_program.at(supertype).retagSources))
            sources.append(m_program.at(supertype).retagSources);
    }
    combineRetagSourceTests(sources, to.objectLayouts);
    bool inspectsObjects = !sources.isEmpty() || mayCopy;
    if (!inspectsObjects && from.isSubsetOf(type, m_program))
        return convert(operand, from, rep);

    LBasicBlock slowPath = newColdBlock();
    LBasicBlock unchanged = m_out.newBlock();
    OperandJoin join(*this, joinRepFor(type));
    Operand fitted = narrowOrElse(operand, from, type, slowPath);
    if (!inspectsObjects)
        m_out.jump(unchanged);
    else {
        CellPointer cell;
        if (rep == Rep::Cell)
            cell = fitted.as<CellPointer>();
        else {
            LBasicBlock cellCase = m_out.newBlock();
            m_out.branch(isCell(fitted.as<Boxed>(), type).value(), usually(cellCase), rarely(unchanged));
            m_out.appendTo(cellCase);
            cell = asCell(fitted.as<Boxed>());
        }
        if (!sources.isEmpty()) {
            LBasicBlock isNotSource = mayCopy ? m_out.newBlock() : unchanged;
            LBasicBlock isSource = type.primitives & Primitive::BigInt ? newColdBlock() : slowPath;
            SetForScope knownTypeCell(m_knownTypeCell, cell.value());
            SetForScope knownCellType(m_knownCellType, &from);
            branchOnAnyShapeTest(shapeWordOf(cell), sources, rarely(isSource), usually(isNotSource));
            if (isSource != slowPath) {
                m_out.appendTo(isSource);
                m_out.branch(equal(loadCellType(cell), word32(HeapBigIntType)).value(), unsure(isNotSource), unsure(slowPath));
            }
            if (mayCopy)
                m_out.appendTo(isNotSource);
        }
        if (mayCopy)
            m_out.branch(equal(loadCellType(cell), word32(FinalObjectType)).value(), rarely(slowPath), usually(unchanged));
    }

    m_out.appendTo(unchanged);
    join.add(Operand(rep, fitted.value));

    m_out.appendTo(slowPath);
    OperationEffects effects = castEffects ? *castEffects : OperationEffects { writesOfCast(m_program, m_function, cast), true, castWritesShapeWords(m_program, m_function, cast) };
    bool onlyCellsReachHere = operand.rep == Rep::JSValue && withoutCells(from).isSubsetOf(withoutCells(type), m_program);
    Boxed passed = onlyCellsReachHere ? Boxed(partsOf(operand.as<Boxed>()).cell) : operand.rep == Rep::JSValue ? rematerializedValue(operand.as<Boxed>()) : convertTo<Boxed>(operand, from);
    if (!mayBeAnyObject && from.isSubsetOf(type, m_program) && !Options::auditAOTTypedAssertions()) {
        AOT_TYPED_OPERATION_WITH_EFFECTS(CoerceInPlace, effects, passed, typeAndReason(cast.coercedType(), CastReason::Coerce));
        join.add(Operand(rep, fitted.value));
        return join.finish();
    }
    RELEASE_ASSERT(!mayCopy || !m_program.castsMayRetag || effects.writesShapeWords);
    Boxed coerced = AOT_TYPED_OPERATION_WITH_EFFECTS(Coerce, effects, passed, typeAndReason(cast.coercedType(), runtimeCastReason(cast)));
    join.add(convert(coerced, type, rep));
    return join.finish();
}

Operand Lowering::lowerNonNull(ValueID result, const Value& cast)
{
    Operand operand = get(cast.child(0));
    auto& from = typeOf(cast.child(0));
    auto& to = typeOf(result);
    if (from.isSubsetOf(to, m_program))
        return convert(operand, from, repOf(to));
    LBasicBlock mismatch = newColdBlock();
    Operand fitted = narrowOrElse(operand, from, to, mismatch);
    LBasicBlock continuation = m_out.m_block;
    m_out.appendTo(mismatch);
    bool isNil = operand.rep == Rep::JSValue && from.withoutPrimitives(Primitive::UndefinedOrNull).isSubsetOf(to, m_program);
    throwAt(ThrowKind::NonNullFailed, isNil ? Operand(thrownNullish(operand.as<Boxed>())) : thrown(operand, from), cast.type.index(), cast.operandSource().index());
    m_out.unreachable();
    m_out.appendTo(continuation);
    return fitted;
}

Operand Lowering::lowerAuditedCast(ValueID result, const Value& cast)
{
    Operand operand = get(cast.child(0));
    auto& from = typeOf(cast.child(0));
    CastReason reason = cast.castReason();
    TypeID wanted = reason == CastReason::Coerce ? cast.coercedType() : cast.assertedType() ? cast.assertedType() : cast.type;
    bool looksIntoObjects = reason == CastReason::As || reason == CastReason::Coerce;
    bool callsByResult = reason == CastReason::Coerce && !m_program.at(wanted).hasObjects();
    auto& passing = m_program.at(callsByResult ? (cast.assertedType() ? cast.assertedType() : cast.type) : wanted);
    if (from.isSubsetOf(passing, m_program, !looksIntoObjects))
        return convert(operand, from, cast.rep);

    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Operand fitted;
    {
        SetForScope isStrict(m_rejectsUntypedObjects, looksIntoObjects);
        SetForScope useCastTest(m_useCastTest, looksIntoObjects || reason == CastReason::Flow);
        fitted = narrowOrElse(operand, from, passing, slowPath);
    }
    ValueFromBlock fast = m_out.anchor(convert(fitted, passing, cast.rep).value);
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    Boxed goesOn;
    if (looksIntoObjects || reason == CastReason::Flow || m_program.at(wanted).isUndefinedOrNull()) {
        OperationEffects effects { writesOfCast(m_program, m_function, cast), true, castWritesShapeWords(m_program, m_function, cast) };
        goesOn = AOT_TYPED_OPERATION_WITH_EFFECTS(AuditCoerce, effects, convertTo<Boxed>(operand, from), word32(wanted.index()), word32(static_cast<uint32_t>(runtimeCastReason(cast))), word32(m_id.index()), auditLineAndColumn());
    } else
        goesOn = assertionFailed(operand, from, wanted, static_cast<uint32_t>(reason), reason == CastReason::Store ? nextStoreReceiver(result) : ValueID());
    ValueFromBlock slow = m_out.anchor(convert(goesOn, m_program.at(cast.type), cast.rep).value);
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return Operand(cast.rep, m_out.phi(b3TypeOf(cast.rep), fast, slow));
}

unsigned Lowering::useCountOf(ValueID id)
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
    m_valueLocations.fill({ UINT32_MAX, UINT32_MAX }, count);
    m_isUsedByTerminator.ensureSize(count);
    for (uint32_t b = 0; b < m_function.blocks.size(); ++b) {
        auto& block = m_function.blocks[b];
        for (uint32_t i = 0; i < block.values.size(); ++i) {
            m_valueLocations[block.values[i].index()] = { b, i };
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
    return m_valueLocations[id.index()];
}

bool Lowering::mayFuseWithRead(size_t readIndex)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || m_currentBlock->terminator.kind == TerminatorKind::Invoke)
        return false;
    auto& read = m_function.at(m_currentBlock->values[readIndex]);
    return read.opcode == Opcode::GetById && !read.methodGetByIdBuiltin() && fieldFor(typeOf(read.child(0)), read.string());
}

bool Lowering::readsLengthOfPredictedArrayOrString(size_t readIndex)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || m_currentBlock->terminator.kind == TerminatorKind::Invoke)
        return false;
    auto& read = m_function.at(m_currentBlock->values[readIndex]);
    if (read.opcode != Opcode::GetById || read.string() != m_length || read.isAtKnownSlot() || read.methodGetByIdBuiltin() || prefersGenericPathInColdCode())
        return false;
    ValueID object = read.child(0);
    auto& receiver = typeOf(object);
    if (receiver.singleLayout() || receiver == Type::function() || fieldFor(receiver, m_length) || !predictionOf(object))
        return false;
    if (auto expectedObject = predictionOf(object, Primitive::All); expectedObject && predictedFieldFor(*expectedObject, m_length))
        return false;
    auto builtin = predictedBuiltin(BuiltinKind::Getter, object, m_length, { });
    if (!builtin)
        return false;
    auto& implementation = m_builtins.at(builtin->call.builtin()).implementation;
    return implementation.kind == BuiltinImplementation::Inline && (implementation.inlined == InlineBuiltin::ArrayLength || implementation.inlined == InlineBuiltin::StringLength);
}

static bool isConstant(Opcode opcode)
{
    return opcode == Opcode::Undefined || opcode == Opcode::Null || opcode == Opcode::Boolean || opcode == Opcode::Integer || opcode == Opcode::Double;
}

std::optional<ValueID> Lowering::elementReadAfterLengthCheck(ValueID array)
{
    if (!m_fusedCasts || !m_fusedCasts->useIndex || m_fusedCasts->count || m_fusedCasts->countAfterUse || m_sharedGenericPath || m_activeHandler)
        return std::nullopt;
    auto& values = m_currentBlock->values;
    ValueID length = values[m_fusedCasts->readIndex];
    ValueID comparison = values[*m_fusedCasts->useIndex];
    auto& use = m_function.at(comparison);
    ValueID index;
    if (use.opcode == Opcode::Less && use.child(1) == length)
        index = use.child(0);
    else if (use.opcode == Opcode::Greater && use.child(0) == length)
        index = use.child(1);
    else
        return std::nullopt;
    auto& terminator = m_currentBlock->terminator;
    if (terminator.kind != TerminatorKind::Branch || terminator.value != comparison || terminator.targets.size() != 2)
        return std::nullopt;
    if (!typeOf(index).isInteger() || typeOf(index).integers.minimum < 0 || get(index).rep != Rep::Int64)
        return std::nullopt;
    BlockID body = terminator.targets[0].block;
    unsigned predecessorCount = 0;
    for (auto& block : m_function.blocks) {
        for (auto& target : block.terminator.targets)
            predecessorCount += target.block == body;
    }
    if (predecessorCount != 1 || m_function.at(body).terminator.kind == TerminatorKind::Invoke)
        return std::nullopt;
    for (ValueID id : m_function.at(body).values) {
        auto& value = m_function.at(id);
        if (isConstant(value.opcode) || value.opcode == Opcode::LoopHint)
            continue;
        if (value.opcode == Opcode::GetByVal && value.child(1) == index && originOf(value.child(0)) == originOf(array))
            return id;
        break;
    }
    return std::nullopt;
}

bool Lowering::isFollowedByInBoundsElementRead(size_t index)
{
    auto& values = m_currentBlock->values;
    for (++index; index < values.size(); ++index) {
        if (isConstant(m_function.at(values[index]).opcode))
            continue;
        return m_elementsKnownPresent.contains(values[index].index());
    }
    return false;
}

std::optional<size_t> Lowering::fusedUseIndex(size_t readIndex, unsigned numberOfCasts)
{
    bool readsLength = !numberOfCasts && readsLengthOfPredictedArrayOrString(readIndex);
    if (!readsLength && !mayFuseWithRead(readIndex))
        return std::nullopt;
    auto& values = m_currentBlock->values;
    ValueID operand = values[readIndex + numberOfCasts];
    if (m_function.at(operand).rep != Rep::JSValue || useCountOf(operand) != 1)
        return std::nullopt;
    auto& read = m_function.at(values[readIndex]);
    auto* field = readsLength ? nullptr : fieldFor(typeOf(read.child(0)), read.string());
    bool keepsMoreThanNumbers = field && (m_program.at(field->stored).hasObjects() || m_program.at(field->stored).primitives & ~(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent));
    for (size_t index = readIndex + numberOfCasts + 1; index < values.size(); ++index) {
        auto& value = m_function.at(values[index]);
        if (isConstant(value.opcode))
            continue;
        if (field && value.opcode == Opcode::ToBoolean && value.child(0) == operand && !(m_program.at(field->stored).primitives & ~(Primitive::UndefinedOrNull | Primitive::Absent)) && !typeOf(read.child(0)).acceptsUntypedObjects(m_program))
            return index;
        if (keepsMoreThanNumbers)
            return std::nullopt;
        if (readsLength && (value.opcode == Opcode::Add || value.opcode == Opcode::Sub || value.opcode == Opcode::Mul))
            return std::nullopt;
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
            if (!isNumericOperand(m_program.at(m_function.at(value.child(value.child(0) == operand)).type)))
                return std::nullopt;
            return index;
        default:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

unsigned Lowering::fusedCastCount(size_t readIndex)
{
    return mayFuseWithRead(readIndex) ? numberOfFollowingCasts(readIndex) : 0;
}

unsigned Lowering::numberOfFollowingCasts(size_t index)
{
    auto& values = m_currentBlock->values;
    unsigned count = 0;
    for (size_t next = index + 1; next < values.size(); ++next, ++count) {
        auto& cast = m_function.at(values[next]);
        if (cast.opcode != Opcode::Cast || cast.child(0) != values[next - 1] || useCountOf(values[next - 1]) != 1 || m_function.at(values[next - 1]).rep == Rep::Cell)
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

std::optional<Operand> Lowering::lowerFusedCast(size_t index)
{
    if (m_fusedCasts)
        return std::nullopt;
    auto& values = m_currentBlock->values;
    size_t readIndex = index;
    while (readIndex && m_function.at(values[readIndex]).opcode == Opcode::Cast && m_function.at(values[readIndex]).child(0) == values[readIndex - 1])
        --readIndex;
    if (auto read = readFusedWithUse(readIndex)) {
        size_t useIndex = readIndex;
        unsigned behind = numberOfCastsFusedAfterUse(useIndex);
        if (index > useIndex + behind)
            return std::nullopt;
        if (index < useIndex + behind)
            return placeholder(m_function.at(values[index]).rep);
        return lowerReadFusedWithUse(*read, useIndex, behind);
    }
    if (unsigned behind = numberOfCastsFusedAfterArithmetic(readIndex); behind && index <= readIndex + behind) {
        if (index < readIndex + behind)
            return placeholder(m_function.at(values[index]).rep);
        size_t numberOfAssumptions = m_assumptions.size();
        ValueID id = values[readIndex];
        auto& operation = m_function.at(id);
        auto& wanted = m_program.at(m_function.at(values[index]).type);
        m_assumptions.append(Assumption { id, placeholder(operation.rep), &wanted });
        Operand result = lowerBinary(id, operation);
        m_assumptions.shrink(numberOfAssumptions);
        return convert(result, wanted, repOf(wanted));
    }
    unsigned count = fusedCastCount(readIndex);
    if (!count || index > readIndex + count)
        return std::nullopt;
    if (index < readIndex + count || fusedUseIndex(readIndex, count))
        return placeholder(m_function.at(values[index]).rep);
    m_fusedCasts = FusedCasts { readIndex, count, m_program.at(m_function.at(values[index]).type) };
    SetForScope currentIndex(m_currentIndexInBlock, readIndex);
    Operand result = lowerGetById(values[readIndex], m_function.at(values[readIndex]));
    m_fusedCasts = std::nullopt;
    return result;
}

std::optional<Lowering::ReadFusedWithUse> Lowering::readFusedWithUse(size_t useIndex)
{
    auto& values = m_currentBlock->values;
    size_t operandIndex = useIndex;
    while (operandIndex && isConstant(m_function.at(values[operandIndex - 1]).opcode))
        --operandIndex;
    if (!operandIndex--)
        return std::nullopt;
    size_t readIndex = operandIndex;
    while (readIndex && m_function.at(values[readIndex]).opcode == Opcode::Cast && m_function.at(values[readIndex]).child(0) == values[readIndex - 1])
        --readIndex;
    unsigned count = fusedCastCount(readIndex);
    if (readIndex + count != operandIndex || fusedUseIndex(readIndex, count) != useIndex)
        return std::nullopt;
    return ReadFusedWithUse { readIndex, count };
}

unsigned Lowering::numberOfCastsFusedAfterUse(size_t useIndex)
{
    auto& values = m_currentBlock->values;
    auto& use = m_function.at(values[useIndex]);
    if (use.rep != Rep::JSValue)
        return 0;
    unsigned count = numberOfFollowingCasts(useIndex);
    for (unsigned i = 1; i <= count; ++i) {
        auto& cast = m_function.at(values[useIndex + i]);
        auto& type = m_program.at(cast.type);
        bool isCheckOnlyCast = cast.castReason() == CastReason::As || cast.castReason() == CastReason::Store || cast.castReason() == CastReason::Claim || cast.castReason() == CastReason::Flow || cast.castReason() == CastReason::Upcast;
        if (!isCheckOnlyCast || cast.string() || type.hasObjects() || !type.isSubsetOf(m_program.at(m_function.at(values[useIndex + i - 1]).type), m_program))
            return i - 1;
    }
    return count;
}

unsigned Lowering::numberOfCastsFusedAfterArithmetic(size_t index)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || m_currentBlock->terminator.kind == TerminatorKind::Invoke)
        return 0;
    switch (m_function.at(m_currentBlock->values[index]).opcode) {
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Mod:
        return numberOfCastsFusedAfterUse(index);
    default:
        return 0;
    }
}

Operand Lowering::lowerReadFusedWithUse(ReadFusedWithUse read, size_t useIndex, unsigned numberOfCastsAfterUse)
{
    auto& values = m_currentBlock->values;
    size_t numberOfAssumptions = m_assumptions.size();
    auto& use = m_function.at(values[useIndex]);
    ValueID other = use.child(use.child(0) == values[read.readIndex + read.numberOfCasts]);
    bool isOnBits = use.opcode == Opcode::BitAnd || use.opcode == Opcode::BitOr || use.opcode == Opcode::BitXor || use.opcode == Opcode::LShift || use.opcode == Opcode::RShift || use.opcode == Opcode::URShift;
    if (isOnBits && !typeOf(other).isInteger())
        m_assumptions.append(Assumption { other, signExtended(toInt32(other)), &m_program.at(m_program.types.int32) });
    m_fusedCasts = FusedCasts { read.readIndex, read.numberOfCasts, m_program.at(m_function.at(values[useIndex + numberOfCastsAfterUse]).type), false, useIndex, numberOfCastsAfterUse };
    SetForScope currentIndex(m_currentIndexInBlock, read.readIndex);
    Operand result = lowerGetById(values[read.readIndex], m_function.at(values[read.readIndex]));
    m_fusedCasts = std::nullopt;
    m_assumptions.shrink(numberOfAssumptions);
    return result;
}

std::optional<Operand> Lowering::lowerFusedUse(size_t index)
{
    if (m_fusedCasts)
        return std::nullopt;
    auto read = readFusedWithUse(index);
    if (!read) {
        if (numberOfCastsFusedAfterArithmetic(index))
            return placeholder(m_function.at(m_currentBlock->values[index]).rep);
        return std::nullopt;
    }
    if (numberOfCastsFusedAfterUse(index))
        return placeholder(m_function.at(m_currentBlock->values[index]).rep);
    return lowerReadFusedWithUse(*read, index, 0);
}

Operand Lowering::lowerFusedCasts(Operand operand, const Type& from)
{
    SetForScope areBeingLowered(m_fusedCasts->areBeingLowered, true);
    SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
    SetForScope frequency(m_out.m_frequency, m_out.m_block->frequency() ? m_out.m_frequency : 0);
    auto& values = m_currentBlock->values;
    size_t readIndex = m_fusedCasts->readIndex;
    size_t numberOfAssumptions = m_assumptions.size();
    const Type* known = &from;
    if (auto& readType = m_program.at(m_function.at(values[readIndex]).type); !from.isSubsetOf(readType, m_program)) {
        operand = ensure(operand, from, readType);
        known = &readType;
    }
    m_assumptions.append(Assumption { values[readIndex], operand, known });
    for (unsigned i = 1; i <= m_fusedCasts->count; ++i) {
        ValueID id = values[readIndex + i];
        auto& cast = m_function.at(id);
        auto& to = m_program.at(cast.type);
        if (i < m_fusedCasts->count && m_out.m_frequency && operand.rep == Rep::JSValue && cast.castReason() == CastReason::Claim && !cast.accessedName() && !cast.string()) {
            ValueID nextValueID = values[readIndex + i + 1];
            auto& next = m_function.at(nextValueID);
            auto& wanted = m_program.at(next.type);
            if (next.castReason() == CastReason::Coerce && !wanted.hasObjects() && !wanted.mayBeUndefinedOrNull() && wanted.isSubsetOf(to, m_program) && !known->isSubsetOf(wanted, m_program) && mayHaveValuesInCommon(*known, wanted)) {
                LBasicBlock otherwise = newColdBlock();
                Join join(m_out, repOf(wanted), b3TypeOf(repOf(wanted)));
                join.add(narrowOrElse(operand, *known, wanted, otherwise));
                m_out.appendTo(otherwise);
                {
                    SetForScope currentIndex(m_currentIndexInBlock, readIndex + i);
                    m_assumptions.append(Assumption { id, lowerCast(id, cast), &to });
                }
                {
                    SetForScope currentIndex(m_currentIndexInBlock, readIndex + i + 1);
                    join.add(convert(lowerCast(nextValueID, next), wanted, repOf(wanted)));
                }
                m_assumptions.removeLast();
                operand = join.finish();
                known = &wanted;
                m_assumptions.append(Assumption { id, operand, known });
                m_assumptions.append(Assumption { nextValueID, operand, known });
                ++i;
                continue;
            }
        }
        bool rejectsNullish = (cast.accessedName() || cast.castReason() == CastReason::NonNull) && known->mayBeUndefinedOrNull();
        if (!known->isSubsetOf(to, m_program) || rejectsNullish || (known->hasObjects() && cast.castReason() != CastReason::Upcast)) {
            SetForScope currentIndex(m_currentIndexInBlock, readIndex + i);
            operand = lowerCast(id, cast);
            known = &to;
        }
        m_assumptions.append(Assumption { id, operand, known });
    }
    if (m_fusedCasts->useIndex) {
        size_t lastIndex = *m_fusedCasts->useIndex + m_fusedCasts->countAfterUse;
        SetForScope currentIndex(m_currentIndexInBlock, lastIndex);
        ValueID id = values[*m_fusedCasts->useIndex];
        auto& use = m_function.at(id);
        m_assumptions.append(Assumption { values[readIndex + m_fusedCasts->count], convert(operand, *known, repOf(*known)), known });
        known = &m_program.at(m_function.at(values[lastIndex]).type);
        if (m_fusedCasts->countAfterUse)
            m_assumptions.append(Assumption { id, placeholder(use.rep), known });
        bool isRelational = use.opcode == Opcode::Less || use.opcode == Opcode::LessEq || use.opcode == Opcode::Greater || use.opcode == Opcode::GreaterEq;
        operand = use.opcode == Opcode::ToBoolean ? Operand(lowerToBoolean(use.child(0))) : isRelational ? Operand(lowerRelational(use)) : lowerBinary(id, use);
    }
    m_assumptions.shrink(numberOfAssumptions);
    return convert(operand, *known, repOf(m_fusedCasts->type));
}

bool Lowering::usesCastTest(const Value& cast) const
{
    return cast.castReason() == CastReason::As || (cast.castReason() == CastReason::Flow && !cast.string() && (cast.keepsUnconvertibleObjects() || narrowingMayRetag(m_program, m_program.at(cast.type))));
}

std::optional<Lowering::SunkCoercion> Lowering::sunkCoercionAt(size_t useIndex)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || m_currentBlock->terminator.kind == TerminatorKind::Invoke)
        return std::nullopt;
    auto& values = m_currentBlock->values;
    auto& use = m_function.at(values[useIndex]);
    auto staticTypeOf = [&](ValueID id) -> const Type& {
        return m_program.at(m_function.at(id).type);
    };
    bool isRead = use.opcode == Opcode::GetById && !use.methodGetByIdBuiltin();
    bool isCast = use.opcode == Opcode::Cast && !use.string()
        && (use.castReason() == CastReason::As || use.castReason() == CastReason::Flow || use.castReason() == CastReason::Store || use.castReason() == CastReason::Claim);
    if (!isRead && !isCast)
        return std::nullopt;
    size_t index = useIndex;
    ValueID operand = use.child(0);
    for (;; operand = m_function.at(operand).child(0)) {
        if (!index || values[index - 1] != operand || useCountOf(operand) != 1 || m_function.at(operand).opcode != Opcode::Cast)
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
    if (!holdsCells(repOf(type)) || from.anyObject || from.acceptsUntypedObjects(m_program) || !from.isSubsetOf(type, m_program) || type.primitives & Primitive::BigInt
        || !coercionMayMutateObjects(m_program, from, coercedTo, cast.coercesFreshData()))
        return std::nullopt;
    SunkCoercion sunk { operand, { } };
    for (LayoutID supertype : coercedTo.objectLayouts) {
        if (mayPass(from, m_program.at(supertype).retagSources))
            sunk.sources.append(&m_program.at(supertype).retagSources);
    }
    if (sunk.sources.isEmpty())
        return std::nullopt;

    Vector<const ShapeTest*, 4> tests;
    if (isRead) {
        const Field* field = fieldFor(staticTypeOf(use.child(0)), use.string());
        if (!field || !field->isMissingInSomeSubtype || field->subtypesWithField.neverPasses())
            return std::nullopt;
        tests.append(&field->subtypesWithField);
    } else {
        auto& to = m_program.at(use.type);
        if (to.objectLayouts.isEmpty() || to.anyObject || to.acceptsUntypedObjects(m_program) || m_program.castIsRedundant(type, to, use.castReason()))
            return std::nullopt;
        for (LayoutID supertype : to.objectLayouts)
            tests.append(usesCastTest(use) ? &m_program.at(supertype).castTest : &m_program.at(supertype).test);
    }
    for (LayoutID supertype : isRead ? Vector<LayoutID, 1> { } : from.objectLayouts) {
        for (LayoutID subtype : m_program.at(supertype).subtypes) {
            uint32_t shape = m_program.at(subtype).shape;
            auto passesValue = [&](const ShapeTest* test) {
                return m_program.passes(*test, shape);
            };
            if (shape && std::ranges::any_of(tests, passesValue) && std::ranges::any_of(sunk.sources, passesValue))
                return std::nullopt;
        }
    }
    return sunk;
}

bool Lowering::coercionSinks(size_t castIndex)
{
    auto& values = m_currentBlock->values;
    size_t use = castIndex + 1;
    while (use < values.size() && m_function.at(values[use]).opcode == Opcode::Cast && m_function.at(values[use]).castReason() == CastReason::Upcast && m_function.at(values[use]).child(0) == values[use - 1])
        ++use;
    if (use >= values.size())
        return false;
    auto sunk = sunkCoercionAt(use);
    return sunk && sunk->cast == values[castIndex];
}

bool Lowering::coercionIsOnlyRead(size_t castIndex)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return false;
    auto& values = m_currentBlock->values;
    auto staticTypeOf = [&](ValueID id) -> const Type& {
        return m_program.at(m_function.at(id).type);
    };
    size_t use = castIndex + 1;
    for (;; ++use) {
        if (use >= values.size() || useCountOf(values[use - 1]) != 1 || m_function.at(values[use]).child(0) != values[use - 1])
            return false;
        if (m_function.at(values[use]).opcode != Opcode::Cast || m_function.at(values[use]).castReason() != CastReason::Upcast)
            break;
    }
    auto& read = m_function.at(values[use]);
    if (read.opcode != Opcode::GetById || read.methodGetByIdBuiltin())
        return false;
    auto& cast = m_function.at(values[castIndex]);
    auto& from = staticTypeOf(cast.child(0));
    auto& type = m_program.at(cast.type);
    if (!holdsCells(repOf(type)) || from.anyObject || from.acceptsUntypedObjects(m_program) || !from.isSubsetOf(type, m_program) || type.primitives & Primitive::BigInt)
        return false;
    auto& receiver = staticTypeOf(read.child(0));
    LayoutID supertype = receiver.singleLayout();
    const Field* field = fieldFor(receiver, read.string());
    return supertype && field && !field->isMissingInSomeSubtype && !m_program.at(supertype).acceptsUntypedObjects;
}

void Lowering::branchOnTransitionSource(CellPointer cell, const SunkCoercion& sunk, FTL::WeightedTarget isSource, FTL::WeightedTarget isNotSource)
{
    Vector<ShapeTest, 4> sources;
    for (const ShapeTest* test : sunk.sources)
        sources.append(*test);
    combineRetagSourceTests(sources, m_program.at(m_function.at(sunk.cast).coercedType()).objectLayouts);
    branchOnAnyShapeTest(shapeWordOf(cell), sources, isSource, isNotSource);
}

void Lowering::carryOut(CellPointer cell, const SunkCoercion& sunk)
{
    auto& cast = m_function.at(sunk.cast);
    OperationEffects effects { writesOfCast(m_program, m_function, cast), true, castWritesShapeWords(m_program, m_function, cast) };
    AOT_TYPED_OPERATION_WITH_EFFECTS(CoerceInPlace, effects, box(cell), typeAndReason(cast.coercedType(), CastReason::Coerce));
}

Operand Lowering::lowerCastAfterCoercion(ValueID result, const Value& cast, const SunkCoercion& sunk)
{
    auto& to = typeOf(result);
    Rep rep = repOf(to);
    LBasicBlock otherwise = newColdBlock();
    OperandJoin join(*this, joinRepFor(to));
    {
        SetForScope useCastTest(m_useCastTest, usesCastTest(cast));
        join.add(narrowOrElse(get(cast.child(0)), typeOf(cast.child(0)), to, otherwise));
    }
    m_out.appendTo(otherwise);
    SetForScope isLoweringGroup(m_isLoweringSunkCoercion, true);
    SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
    {
        SetForScope currentIndex(m_currentIndexInBlock, m_currentIndexInBlock - 1);
        lowerCoercion(sunk.cast, m_function.at(sunk.cast));
    }
    join.add(convert(lowerCast(result, cast), to, rep));
    return join.finish();
}

std::optional<Operand> Lowering::castInOutlinedBody(const Value& cast, Operand operand, const Type& from, const Type& to)
{
    if (!isCompact() || (operand.rep != Rep::JSValue && operand.rep != Rep::Cell) || m_program.castIsRedundant(from, to, cast))
        return std::nullopt;
    if (m_environmentAwaitingBarrier || m_isFillingArrayAtCreation || !m_fieldsAwaitingFirstStore.isEmpty())
        return std::nullopt;
    FunctionID body = m_program.castBody(cast);
    if (!body)
        return std::nullopt;
    auto& callee = m_program.at(body);
    if (callee.writes > m_function.writes || (callee.writesShapeWords && !m_function.writesShapeWords) || (callee.convertsArrayStorage && !m_function.convertsArrayStorage))
        return std::nullopt;
    return callCompiled(body, { Argument { operand, &from } }, to);
}

std::optional<Number> Lowering::unboxArgumentWithStub(ValueID result, const Value& cast)
{
    if (!isX86_64() || !m_function.isGenericEntrypoint() || m_function.has(FunctionUsesArgumentBuffer) || m_activeHandler || m_fusedCasts || m_program.logsTypeErrors || Options::validateAOTTypedFacts())
        return std::nullopt;
    if ((cast.castReason() != CastReason::As && cast.castReason() != CastReason::Store) || cast.assertedType() || cast.string() || typeOf(result) != m_program.at(m_program.types.number))
        return std::nullopt;
    constexpr unsigned firstArgumentIndex = 2;
    auto& parameter = m_function.at(cast.child(0));
    if (parameter.opcode != Opcode::Parameter || parameter.immediates[0] || parameter.immediates[1] < firstArgumentIndex || parameter.immediates[1] - firstArgumentIndex >= maxArgumentsUnboxedByStub || !typeOf(cast.child(0)).isAnything())
        return std::nullopt;
    auto& terminator = m_currentBlock->terminator;
    if (terminator.kind != TerminatorKind::Return || !terminator.value || m_function.at(terminator.value).opcode != Opcode::CallDirect || useCountOf(result) != 1)
        return std::nullopt;
    unsigned argument = parameter.immediates[1] - firstArgumentIndex;
    auto& call = m_function.at(terminator.value);
    auto& callee = m_program.at(call.function());
    std::optional<unsigned> fpr;
    unsigned numberOfFPRs = 0;
    for (unsigned i = callee.takesCapturesFromClosure ? callee.captureTypes.size() : 0; i < call.arguments.size(); ++i) {
        if (call.arguments[i] == result)
            fpr = numberOfFPRs;
        numberOfFPRs += b3TypeOf(repOf(declaredArgumentType(callee, i))) == Double;
    }
    if (!fpr || *fpr > argument)
        return std::nullopt;
    PatchpointValue* patchpoint = m_out.patchpoint(Double);
    applyEffects(patchpoint, OperationEffects { Writes::Nothing, true });
    patchpoint->append(ConstrainedValue(get(cast.child(0)).value, ValueRep::reg(argumentGPR(argument))));
    patchpoint->resultConstraints = { ValueRep::reg(FPRInfo::toArgumentRegister(*fpr)) };
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    CallSite site { nextSite() };
    patchpoint->setGenerator([context = &m_context, argument, fpr = *fpr, site](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        context->stubCalls.call(jit, unboxNumberArgumentStub(argument, fpr, context->hasNoFrame(params.proc().code())), site);
    });
    return Number(patchpoint);
}

void Lowering::convertPlainRowInPlace(ValueID row, TypeID type)
{
    if (get(row).rep != Rep::JSValue)
        return;
    Boxed boxed = in<Boxed>(row);
    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock plainObject = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(boxed).value(), usually(cellCase), rarely(continuation));
    m_out.appendTo(cellCase);
    m_out.branch(equal(loadCellType(asCell(boxed)), word32(FinalObjectType)).value(), rarely(plainObject), usually(continuation));
    m_out.appendTo(plainObject);
    AOT_TYPED_OPERATION(TryConvertRow, boxed, word32(type.index()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

Operand Lowering::lowerCast(ValueID result, const Value& value)
{
    if (m_castFusedIntoRead && m_castFusedIntoRead->block == m_currentBlock && m_castFusedIntoRead->index == m_currentIndexInBlock)
        return std::exchange(m_castFusedIntoRead, std::nullopt)->operand;
    if (value.castReason() == CastReason::Member)
        return convert(get(value.child(0)), typeOf(value.child(0)), repOf(typeOf(result)));
    if (value.castReason() == CastReason::Upcast) {
        if (auto found = m_pinnedReads.find(value.child(0).index()); found != m_pinnedReads.end()) {
            PinnedRead read = found->value;
            m_pinnedReads.set(result.index(), read);
            return placeholder(repOf(typeOf(result)));
        }
    }
    if (handlerEnteredByBranch()) {
        auto& written = value.inlineFrame ? m_program.at(m_function.inlineFrames[value.inlineFrame].callee) : m_function;
        bool isRelaxedForLogging = m_program.logsTypeErrors && (written.isGenericEntrypoint() || written.has(Format::FunctionIsHiddenInStackTraces) || written.has(FunctionLogsTypeMismatches)) && typeOf(result).acceptsUntypedObjects(m_program);
        SetForScope rejectUntypedScope(m_rejectsUntypedObjects, m_rejectsUntypedObjects || isRelaxedForLogging);
        SetForScope isHolding(m_isCheckingTypeRelaxedForLogging, isRelaxedForLogging);
        if (auto& layouts = typeOf(result).objectLayouts; value.castReason() == CastReason::Claim && layouts.size() == 1 && m_program.at(layouts[0]).isDecodedRow)
            convertPlainRowInPlace(value.child(0), value.type);
        return ensure(get(value.child(0)), typeOf(value.child(0)), typeOf(result));
    }
    if (m_cloneGroup && m_cloneGroup->cast == m_currentIndexInBlock)
        return lowerCloneGroup(*std::exchange(m_cloneGroup, std::nullopt));
    if (auto result = tryCastTypedClone())
        return *result;
    if (m_mergeGroup && m_mergeGroup->first + 2 == m_currentIndexInBlock)
        return lowerMergeGroup(*std::exchange(m_mergeGroup, std::nullopt));
    if (m_spreadGroup && m_spreadGroup->cast == m_currentIndexInBlock)
        return lowerSpreadGroup(*std::exchange(m_spreadGroup, std::nullopt));
    if (m_castOfCopiedRecord.block == m_currentBlock && m_castOfCopiedRecord.cast == m_currentIndexInBlock) {
        m_castOfCopiedRecord.block = nullptr;
        return m_castOfCopiedRecord.result;
    }
    if (auto fused = lowerFusedCast(m_currentIndexInBlock))
        return *fused;
    if (Options::auditAOTTypedAssertions() && value.castReason() != CastReason::Upcast && !value.accessedName()) [[unlikely]]
        return lowerAuditedCast(result, value);
    if (!m_isLoweringSunkCoercion) {
        if (auto sunk = sunkCoercionAt(m_currentIndexInBlock))
            return lowerCastAfterCoercion(result, value, *sunk);
    }
    if (auto number = unboxArgumentWithStub(result, value))
        return *number;
    Operand operand = get(value.child(0));
    auto& from = typeOf(value.child(0));
    auto& to = typeOf(result);
    switch (value.castReason()) {
    case CastReason::Upcast:
        return convert(operand, from, repOf(to));
    case CastReason::As:
        if (auto called = castInOutlinedBody(value, operand, from, to))
            return *called;
        return coerce(operand, from, value);
    case CastReason::Coerce:
        return lowerCoercion(result, value);
    case CastReason::CoerceFreshObject:
    case CastReason::CoerceFreshGraph:
    case CastReason::AsFreshObject:
    case CastReason::Member:
    case CastReason::AsIfConvertible:
    case CastReason::AsFreshObjectIfConvertible:
    case CastReason::FlowIfConvertible:
        RELEASE_ASSERT_NOT_REACHED();
    case CastReason::NonNull:
        if (m_program.isRelaxedForLogging(to)) [[unlikely]]
            return coerce(operand, from, value);
        return lowerNonNull(result, value);
    case CastReason::Flow:
        if (!value.string() && (value.keepsUnconvertibleObjects() || narrowingMayRetag(m_program, to) || m_program.isRelaxedForLogging(to)))
            return coerce(operand, from, value);
        break;
    case CastReason::Store:
    case CastReason::Claim:
        if (!value.string() && m_program.isRelaxedForLogging(to)) [[unlikely]]
            return coerce(operand, from, value);
        break;
    }
    if (StringID name = value.string(); name && !(from.primitives & ~(cellPrimitives | Primitive::UndefinedOrNull)) && from.withoutPrimitives(Primitive::UndefinedOrNull).isSubsetOf(to, m_program))
        return convert(nonNullish(operand, from, name), to, repOf(to));
    if (StringID name = value.string(); name && !from.hasObjects() && from.mayBeUndefinedOrNull() && from.primitives & (Primitive::Number | Primitive::Boolean)) {
        LBasicBlock isNil = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(lowerIsUndefinedOrNull(value.child(0)).value(), rarely(isNil), usually(continuation));
        m_out.appendTo(isNil);
        throwNullish(in<Boxed>(value.child(0)), name, { });
        m_out.appendTo(continuation);
        return ensure(operand, from, to);
    }
    if (auto* engineClass = to.hasOnlyPrimitives(Primitive::String) ? &m_builtins.stringClass() : to.singleEngineClass(); engineClass && !value.string() && (value.operandClassIsProven || resultOfDominatingGuard(value.child(0), *engineClass, GuardKind::Class).value_or(false)))
        return convert(operand, to, repOf(to));
    if (Opcode allocatedBy = m_function.at(value.child(0)).opcode; to.anyObject && !value.string() && (allocatedBy == Opcode::NewObject || allocatedBy == Opcode::NewObjectWithShape || allocatedBy == Opcode::NewArray))
        return convert(operand, to, repOf(to));
    if (auto called = castInOutlinedBody(value, operand, from, to))
        return *called;
    return ensure(operand, from, to);
}

Word32 Lowering::lowerIsUndefinedOrNull(ValueID id)
{
    if (encodesUndefinedAsZero(id))
        return Word32(m_out.belowOrEqual(m_values[id.index()].value, m_out.constInt64(JSValue::encode(jsNull()))));
    auto& type = typeOf(id);
    Operand operand = get(id);
    if (operand.rep == Rep::OptionalInt64 || operand.rep == Rep::OptionalDouble)
        return isMarker(operand);
    if (operand.rep == Rep::LiteralString)
        return type.mayBeUndefinedOrNull() ? Word32(m_out.belowOrEqual(rawWordOf(operand.as<LiteralStringWord>()), m_out.constInt64(undefinedCell32))) : boolean(false);
    if (operand.rep != Rep::JSValue)
        return boolean(false);
    Boxed value = operand.as<Boxed>();
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

bool Lowering::toBooleanNeedsStub(const Type& type)
{
    return repOf(type) == Rep::JSValue && type.primitives & Primitive::Number && type.primitives & Primitive::BigInt;
}

Word32 Lowering::isTruthy(CellPointer cell, const Type& type)
{
    Word32 isNotEmptyString = type.primitives & Primitive::String ? notEqual(cell, CellPointer(emptyString())) : boolean(true);
    if (!(type.primitives & Primitive::BigInt))
        return isNotEmptyString;
    return either<Word32>(equal(loadCellType(cell), word32(HeapBigIntType)), [&] {
        return logicalNot(isZero(loadField<Word32>(cell, m_heaps.JSBigInt_length)));
    }, [&] {
        return isNotEmptyString;
    });
}

Word32 Lowering::isTruthy(Boxed value, const Type& type)
{
    if (type.primitives & Primitive::Number) {
        Type rest = type.withoutPrimitives(Primitive::Number);
        Join join(m_out, Rep::Int32, Int32);
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
        auto doubleCase = [&] {
            return isNeitherZeroNorNaN(unboxedDouble(value));
        };
        if (rest.isNever())
            join.add(doubleCase());
        else {
            when(isNumber(value), doubleCase);
            join.add(isTruthy(value, rest));
        }
        return join.finish().as<Word32>();
    }
    if (!type.hasCells())
        return type.primitives & Primitive::True ? equal(beforeAbsentNormalization(value).as<Boxed>(), boxedConstant(jsBoolean(true))) : boolean(false);
    Word32 isTrue = type.primitives & Primitive::True ? equal(value, boxedConstant(jsBoolean(true))) : boolean(false);
    if (!(type.primitives & ~(Primitive::String | Primitive::Symbol | Primitive::BigInt)))
        return isTruthy(asCell(value), type);
    if (!(type.primitives & (Primitive::String | Primitive::BigInt))) {
        if (!(type.primitives & Primitive::True))
            return isCell(value, type);
        static_assert(JSValue::ValueTrue & 1 && !(JSValue::ValueFalse & 1) && !(JSValue::ValueNull & 1) && !(JSValue::ValueUndefined & 1));
        return Word32(m_out.notEqual(m_out.bitAnd(value.value(), m_out.constInt64(3)), m_out.constInt64(JSValue::OtherTag)));
    }
    return either<Word32>(isCell(value, type), [&] {
        return isTruthy(asCell(value), type);
    }, [&] {
        return isTrue;
    });
}

void Lowering::findLazilyFetchedLeaves()
{
    auto isLeaf = [&](ValueID id) {
        Opcode opcode = m_function.at(id).opcode;
        return (opcode == Opcode::String && m_function.at(id).rep != Rep::LiteralString) || opcode == Opcode::BuiltinFunction;
    };
    BitVector hasOtherUse;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& user = m_function.at(id);
            ValueID late;
            bool treatsNamesAsData = user.opcode == Opcode::CallBuiltin && m_builtins.at(user.builtin()).implementation.kind == BuiltinImplementation::Inline && m_builtins.at(user.builtin()).implementation.inlined == InlineBuiltin::StringAppend;
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
                else if (searchesForConstantWithoutCell(user) || isErrorThrownAtCreation(block, id) || (!user.arguments.isEmpty() && isErrorThrownToIgnoringHandler(block, id)))
                    late = user.arguments[0];
                else if (auto& implementation = m_builtins.at(user.builtin()).implementation; implementation.kind == BuiltinImplementation::Operation && (implementation.operation == LanguageOperation::MakeModuleNamespace || implementation.operation == LanguageOperation::CloneObjectWith || implementation.operation == LanguageOperation::CloneObjectAfter)) {
                    treatsNamesAsData = true;
                    for (size_t i = 2; implementation.operation == LanguageOperation::CloneObjectAfter && i < user.arguments.size(); i += 2) {
                        if (isLeaf(user.arguments[i]))
                            hasOtherUse.set(user.arguments[i].index());
                    }
                }
                break;
            default:
                break;
            }
            unsigned lateCount = 0;
            user.forEachChild([&](ValueID child) {
                if (!isLeaf(child) || user.opcode == Opcode::StrictEq || (user.opcode == Opcode::Eq && nonNumericStringConstant(child)))
                    return;
                if (treatsNamesAsData && m_function.at(child).opcode == Opcode::String)
                    return;
                if (child != late || lateCount++)
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
                m_isLazilyFetched.set(id.index());
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
            if (m_function.at(asked).opcode == Opcode::Parameter && m_function.at(asked).rep == Rep::JSValue && type.primitives & (Primitive::True | Primitive::Number) && !toBooleanNeedsStub(type))
                m_hasTruthinessPhi.set(asked.index());
        }
    }
    if (m_hasTruthinessPhi.isEmpty())
        return;
    for (ValueID id : m_function.blocks[0].parameters)
        m_hasTruthinessPhi.clear(id.index());
    for (auto& block : m_function.blocks) {
        for (auto& target : block.terminator.targets) {
            auto& parameters = m_function.at(target.block).parameters;
            for (unsigned i = 0; i < target.arguments.size() && i < parameters.size(); ++i) {
                if (toBooleanNeedsStub(m_program.at(m_function.at(target.arguments[i]).type)))
                    m_hasTruthinessPhi.clear(parameters[i].index());
            }
        }
    }
}

void Lowering::findSplitJoinParameters()
{
    BitVector isNeverBoxed;
    for (unsigned i = 1; i < m_function.blocks.size(); ++i) {
        for (ValueID id : m_function.blocks[i].parameters) {
            auto& parameter = m_function.at(id);
            if (parameter.rep == Rep::JSValue && joinRepFor(m_program.at(parameter.type)).isReference)
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
            m_isSplitJoin.set(parameter.index());
    });
    for (bool changed = true; changed;) {
        changed = false;
        forEachEdge([&](ValueID passed, ValueID parameter) {
            if (m_isSplitJoin.get(passed.index()) && !m_isSplitJoin.get(parameter.index())) {
                RELEASE_ASSERT(isNeverBoxed.get(parameter.index()));
                m_isSplitJoin.set(parameter.index());
                changed = true;
            }
        });
    }
}

Word32 Lowering::lowerToBoolean(ValueID id)
{
    auto& type = typeOf(id);
    Operand operand = get(id);
    if (auto known = m_truthinessPhis.get(id.index()); known.isTruthy && operand.value == known.value)
        return Word32(known.isTruthy);
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
        return isNeitherZeroNorNaN(operand.as<Number>());
    case Rep::OptionalDouble:
        return isNeitherZeroNorNaN(assumedNumber(beforeAbsentNormalization(operand).as<OptionalNumber>()));
    case Rep::OptionalInt64:
        if (type.integers.isSubsetOf({ -static_cast<int64_t>(UINT32_MAX), UINT32_MAX }))
            return Word32(m_out.notZero32(low32(assumedInteger(operand.as<OptionalInteger>())).value()));
        return logicalNot(logicalOr(isMarker(operand), isZero(assumedInteger(operand.as<OptionalInteger>()))));
    case Rep::Cell:
        return isTruthy(operand.as<CellPointer>(), type);
    case Rep::JSValue:
        if (toBooleanNeedsStub(type))
            break;
        return isTruthy(operand.as<Boxed>(), type);
    case Rep::LiteralString:
        static_assert(absentCell32 < emptyLiteralStringWord && nullCell32 < emptyLiteralStringWord && undefinedCell32 < emptyLiteralStringWord);
        return Word32(m_out.above(rawWordOf(operand.as<LiteralStringWord>()), m_out.constInt64(emptyLiteralStringWord)));
    case Rep::Void:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return genericToBoolean(id);
}

static bool acceptsObjectsWithOtherPrototype(const Program& program, const Type& type)
{
    if (type.anyObject)
        return true;
    return std::ranges::any_of(type.objectLayouts, [&](LayoutID supertype) {
        return std::ranges::any_of(program.at(supertype).subtypes, [&](LayoutID subtype) {
            return program.at(subtype).kind == LayoutKind::Record || program.at(subtype).mayHaveOtherPrototype;
        });
    });
}

bool mayWalkPrototypeChain(const Program& program, const Function& function, const Value& value)
{
    auto& type = program.typeOf(function, value.child(0));
    return type.anyObject || type.anyFunction || !type.engineClasses.isEmpty() || type.acceptsUntypedObjects(program) || program.at(value.layout()).instancesMayBecomeRecords || acceptsObjectsWithOtherPrototype(program, type);
}

bool Lowering::instanceOfUsesShapeTest(const Value& value)
{
    if (value.opcode != Opcode::InstanceOf || value.child(0) == value.child(1) || Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return false;
    auto& named = m_function.at(value.child(1));
    if (named.opcode != Opcode::FunctionReference)
        return false;
    LayoutID ofClass = m_program.at(named.function()).constructedClass;
    if (!ofClass || !m_program.at(ofClass).instances.isRange() || m_program.at(ofClass).instancesMayBecomeRecords)
        return false;
    for (LayoutID at = ofClass; at; at = m_program.at(at).base) {
        auto& layout = m_program.at(at);
        if (!layout.hasFixedLayout || !layout.isEvaluatedOnce || !layout.hasImmutableMembers || layout.hasComputedStaticMemberName || layout.hasUntypedBase)
            return false;
        for (auto& method : layout.staticMethods) {
            if (!method.name)
                return false;
        }
    }
    return true;
}

bool Lowering::isOnlyUsedByShapeInstanceOf(ValueID id)
{
    auto users = usersOf(id);
    if (users.empty() || users.size() != useCountOf(id) || isUsedByTerminator(id))
        return false;
    return std::ranges::all_of(users, [&](ValueID user) {
        return instanceOfUsesShapeTest(m_function.at(user));
    });
}

Word32 Lowering::lowerInstanceOfByShape(const Value& value)
{
    LayoutID ofClass = m_program.at(m_function.at(value.child(1)).function()).constructedClass;
    ShapeTest instances = m_program.at(ofClass).instances;
    bool instancesMayHaveOtherPrototype = m_program.at(ofClass).hasInstancesWithOtherPrototype;
    auto callStub = [&](bool walksChain) {
        LValue operand = in<Boxed>(value.child(0)).value();
        m_context.emitsCalls = true;
        PatchpointValue* patchpoint = m_out.patchpoint(Int32);
        applyEffects(patchpoint, OperationEffects { });
        patchpoint->append(ConstrainedValue(operand, ValueRep::reg(firstStubOperandGPR)));
        patchpoint->clobber(RegisterSet::macroClobberedGPRs());
        patchpoint->clobberLate(RegisterSet { callMarkerGPR });
        patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
#if CPU(X86_64)
        for (GPRReg reg : stubTemporaryGPRs)
            patchpoint->clobberLate(RegisterSet { reg });
#endif
        patchpoint->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
        CallSite site { nextSite() };
        patchpoint->setGenerator([stubCalls = &m_context.stubCalls, site, walksChain, instancesMayHaveOtherPrototype, numberOfClass = ofClass.index(), last = instances.count ? instances.last() : 0, count = instances.count](CCallHelpers& jit, const StackmapGenerationParams&) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            jit.move(CCallHelpers::TrustedImm32(numberOfClass), GPRInfo::argumentGPR1);
            if (walksChain) {
                stubCalls->call(jit, Stub::InstanceOfClassByChain, site);
                return;
            }
            if (count == 1) {
                jit.move(CCallHelpers::TrustedImm32(Typed::shapeWordOf(last)), GPRInfo::argumentGPR2);
                stubCalls->call(jit, instancesMayHaveOtherPrototype ? Stub::InstanceOfLeafClassCheckingPrototype : Stub::InstanceOfLeafClass, site);
                return;
            }
            jit.move(CCallHelpers::TrustedImm32(last), GPRInfo::argumentGPR2);
            jit.move(CCallHelpers::TrustedImm32(count), GPRInfo::argumentGPR3);
            stubCalls->call(jit, instancesMayHaveOtherPrototype ? Stub::InstanceOfClassCheckingPrototype : Stub::InstanceOfClass, site);
        });
        unwindToLandingPad(patchpoint, site);
        return Word32(patchpoint);
    };
    if (auto& type = typeOf(value.child(0)); m_isInLoop && m_out.m_block->frequency() >= 1 && holdsCells(get(value.child(0)).rep)) {
        LBasicBlock isOne = m_out.newBlock();
        LBasicBlock isNone = m_out.newBlock();
        LBasicBlock genericPath = newColdBlock();
        bool hasOnlyTypedObjects = !type.anyObject && !type.anyFunction && type.engineClasses.isEmpty() && !type.acceptsUntypedObjects(m_program);
        bool mayHaveOtherPrototype = acceptsObjectsWithOtherPrototype(m_program, type);
        LBasicBlock chainPath = hasOnlyTypedObjects && !mayHaveOtherPrototype && !instancesMayHaveOtherPrototype ? nullptr : newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        orElse(isZero(Word32(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTInstance_mutableFields, fixedPointer(Instance::offsetOfTypedModifiedClassObjects()), ofClass.index())))), genericPath);
        Operand given = get(value.child(0));
        CellPointer cell;
        if (given.rep == Rep::Cell && !type.mayBeUndefinedOrNull())
            cell = given.as<CellPointer>();
        else {
            Boxed boxed = in<Boxed>(value.child(0));
            orElse(isCell(boxed), isNone);
            cell = asCell(boxed);
        }
        Word32 shapeWord = shapeWordOf(cell);
        LBasicBlock hasOtherShape = m_out.newBlock();
        Word32 isInstance = passes(shapeWord, instances);
        if (m_program.mayRunInMultipleInstances)
            isInstance = Word32(m_out.bitAnd(isInstance.value(), belongsToThisInstance(cell).value()));
        if (instancesMayHaveOtherPrototype) {
            LBasicBlock hasShape = m_out.newBlock();
            m_out.branch(isInstance.value(), unsure(hasShape), unsure(hasOtherShape));
            m_out.appendTo(hasShape);
            m_out.branch(hasOtherPrototype(cell).value(), rarely(chainPath), usually(isOne));
        } else
            m_out.branch(isInstance.value(), unsure(isOne), unsure(hasOtherShape));
        m_out.appendTo(hasOtherShape);
        if (!hasOnlyTypedObjects) {
            LBasicBlock hasLayout = m_out.newBlock();
            m_out.branch(passes(shapeWord, ShapeTest { 1, m_program.numberOfShapes }).value(), usually(hasLayout), rarely(chainPath));
            m_out.appendTo(hasLayout);
        }
        if (mayHaveOtherPrototype)
            m_out.branch(hasOtherPrototype(cell).value(), rarely(chainPath), usually(isNone));
        else
            m_out.jump(isNone);
        m_out.appendTo(isOne);
        ValueFromBlock yes = m_out.anchor(boolean(true).value());
        m_out.jump(continuation);
        m_out.appendTo(isNone);
        ValueFromBlock no = m_out.anchor(boolean(false).value());
        m_out.jump(continuation);
        m_out.appendTo(genericPath);
        ValueFromBlock found = m_out.anchor(callStub(false).value());
        m_out.jump(continuation);
        if (!chainPath) {
            m_out.appendTo(continuation);
            return Word32(m_out.phi(Int32, yes, no, found));
        }
        m_out.appendTo(chainPath);
        ValueFromBlock walked = m_out.anchor(callStub(true).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return Word32(m_out.phi(Int32, yes, no, found, walked));
    }
    return callStub(false);
}

Word32 Lowering::lowerGenericInstanceOf(Boxed value, Boxed constructor)
{
    if (m_context.sites.size() < std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots)) {
        m_context.sites.append(Site { });
        Pointer slot = slotAddress(m_context.sites.size() - 1, m_out.m_block->frequency() >= 1 ? SlotAddressUse::FastPath : SlotAddressUse::ColdBlock);
        return Word32(callICStub(Stub::InstanceOfCached, Int32, { { value.value(), firstStubOperandGPR }, { constructor.value(), GPRInfo::argumentGPR1 }, { slot.value(), GPRInfo::argumentGPR2 } }));
    }
    return AOT_TYPED_OPERATION(InstanceOf, value, constructor);
}

static bool hasTwoLevelPrototypeChain(const EngineClass& engineClass)
{
    switch (engineClass.type) {
    case ArrayType:
    case JSMapType:
    case JSSetType:
    case JSWeakMapType:
    case JSWeakSetType:
    case JSDateType:
    case RegExpObjectType:
    case JSPromiseType:
        return engineClass.canCheckOriginalStructure();
    default:
        return false;
    }
}

const EngineClass* Lowering::instanceOfEngineClass(const Value& value)
{
    if (value.opcode != Opcode::InstanceOf || Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return nullptr;
    auto& named = m_function.at(originOf(value.child(1)));
    if (named.opcode != Opcode::GetGlobal)
        return nullptr;
    BuiltinName name = m_program.builtinName(named.string());
    if (!m_builtins.isGlobal(name) || m_builtins.isReplaceable(name))
        return nullptr;
    auto* engineClass = m_builtins.classNamed(name);
    return engineClass && engineClass->name == name && engineClass->index != m_builtins.objectClass().index && holdsCells(m_function.at(value.child(0)).rep) ? engineClass : nullptr;
}

bool Lowering::isUsedOnlyByInlineInstanceOf(ValueID id)
{
    auto users = usersOf(id);
    if (users.empty() || users.size() != useCountOf(id) || isUsedByTerminator(id))
        return false;
    return std::ranges::all_of(users, [&](ValueID user) {
        auto& use = m_function.at(user);
        return use.child(1) == id && use.child(0) != id && instanceOfEngineClass(use);
    });
}

void Lowering::findInstanceOfRuns(const Vector<BlockID>& order)
{
    struct Run {
        ValueID operand;
        ValueID first;
    };
    Vector<Run> runAtBlockStart(m_function.blocks.size());
    for (BlockID blockID : order) {
        auto& block = m_function.at(blockID);
        Run run = runAtBlockStart[blockID.index()];
        for (size_t index = 0; index < block.values.size(); ++index) {
            ValueID id = block.values[index];
            auto& value = m_function.at(id);
            if (instanceOfEngineClass(value)) {
                if (run.first && run.operand == value.child(0)) {
                    m_firstInstanceOfInRun.add(run.first.index(), run.first.index());
                    m_firstInstanceOfInRun.add(id.index(), run.first.index());
                } else
                    run = { value.child(0), id };
                continue;
            }
            if (!run.first)
                continue;
            if (value.opcode == Opcode::CallDirect || value.opcode == Opcode::Await || value.opcode == Opcode::Yield || writesOf(m_program, m_function, block, index) == Writes::Heap)
                run = { };
        }
        if (!run.first)
            continue;
        for (auto& target : block.terminator.targets) {
            if (target.block.index() && numberOfPredecessorsOf(target.block) == 1)
                runAtBlockStart[target.block.index()] = run;
        }
    }
}

Word32 Lowering::isNonObjectOrPlainObject(ValueID id)
{
    auto ofCell = [&](CellPointer cell) {
        Word32 hasLockedObjectPrototype(m_out.equal(m_out.load64(structureOf(cell.value()), m_heaps.Structure_prototype), fixedPointer(Instance::offsetOfLockedRealmObjectPrototype())));
        return logicalAnd(hasLockedObjectPrototype, hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), OverridesGetPrototype));
    };
    Operand operand = get(id);
    if (operand.rep == Rep::Cell && !typeOf(id).mayBeUndefinedOrNull())
        return ofCell(operand.as<CellPointer>());
    Boxed boxed = in<Boxed>(id);
    return either<Word32>(isCell(boxed), [&] {
        return ofCell(asCell(boxed));
    }, [&] {
        return boolean(true);
    });
}

Word32 Lowering::lowerInstanceOfEngineClass(ValueID id, const Value& value, const EngineClass& asked)
{
    auto& type = typeOf(value.child(0));
    Operand operand = get(value.child(0));
    Word32 matchesNoneAtOnce;
    auto first = m_out.m_block == m_unconditionalBlock ? m_firstInstanceOfInRun.find(id.index()) : m_firstInstanceOfInRun.end();
    LBasicBlock isOne = m_out.newBlock();
    LBasicBlock isNone = m_out.newBlock();
    bool isKnownToBeCell = operand.rep == Rep::Cell && !type.mayBeUndefinedOrNull();
    bool checksClassFirst = !m_firstInstanceOfInRun.contains(id.index()) && hasTwoLevelPrototypeChain(asked);
    auto checkOtherClassesOfType = [&](CellPointer cell) {
        for (uint16_t index : type.engineClasses) {
            auto& other = m_builtins.classes()[index];
            if (other.index == asked.index || !hasTwoLevelPrototypeChain(other))
                continue;
            LBasicBlock next = m_out.newBlock();
            m_out.branch(hasOriginalStructure(cell, other).value(), unsure(isNone), unsure(next));
            m_out.appendTo(next);
        }
    };
    if (checksClassFirst) {
        if (!isKnownToBeCell) {
            LBasicBlock isCellHere = m_out.newBlock();
            m_out.branch(isCell(in<Boxed>(value.child(0))).value(), unsure(isCellHere), unsure(isNone));
            m_out.appendTo(isCellHere);
        }
        LBasicBlock hasModifiedStructure = m_out.newBlock();
        m_out.branch(hasOriginalStructure(isKnownToBeCell ? operand.as<CellPointer>() : asCell(in<Boxed>(value.child(0))), asked).value(), unsure(isOne), unsure(hasModifiedStructure));
        m_out.appendTo(hasModifiedStructure);
        checkOtherClassesOfType(isKnownToBeCell ? operand.as<CellPointer>() : asCell(in<Boxed>(value.child(0))));
    }
    if (first != m_firstInstanceOfInRun.end() && first->value != id.index()) {
        if (auto found = m_instanceOfRunResults.find(first->value); found != m_instanceOfRunResults.end() && found->value.first == operand.value)
            matchesNoneAtOnce = Word32(found->value.second);
    }
    if (!matchesNoneAtOnce.value()) {
        matchesNoneAtOnce = isNonObjectOrPlainObject(value.child(0));
        if (first != m_firstInstanceOfInRun.end() && first->value == id.index())
            m_instanceOfRunResults.add(id.index(), std::pair { operand.value, matchesNoneAtOnce.value() });
    }
    LBasicBlock mayBeOne = m_out.newBlock();
    LBasicBlock genericPath = newUnlikelyBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(matchesNoneAtOnce.value(), unsure(isNone), unsure(mayBeOne));
    m_out.appendTo(mayBeOne);
    if (hasTwoLevelPrototypeChain(asked)) {
        CellPointer cell = isKnownToBeCell ? operand.as<CellPointer>() : asCell(in<Boxed>(value.child(0)));
        if (!checksClassFirst) {
            LBasicBlock isAnother = m_out.newBlock();
            m_out.branch(hasOriginalStructure(cell, asked).value(), unsure(isOne), unsure(isAnother));
            m_out.appendTo(isAnother);
            checkOtherClassesOfType(cell);
        }
    }
    m_out.jump(genericPath);
    m_out.appendTo(isOne);
    ValueFromBlock yes = m_out.anchor(boolean(true).value());
    m_out.jump(continuation);
    m_out.appendTo(isNone);
    ValueFromBlock no = m_out.anchor(boolean(false).value());
    m_out.jump(continuation);
    m_out.appendTo(genericPath);
    ValueFromBlock found = m_out.anchor(callICStub(Stub::InstanceOfEngineClass, Int32, { { in<Boxed>(value.child(0)).value(), firstStubOperandGPR } }, std::pair { GPRInfo::argumentGPR1, static_cast<uint32_t>(asked.index) }));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return Word32(m_out.phi(Int32, yes, no, found));
}

Word32 Lowering::lowerIsInstanceOfClass(const Value& value)
{
    ValueID object = value.child(0);
    auto& type = typeOf(object);
    Operand operand = get(object);
    if (!type.hasObjects() || !holdsCells(operand.rep))
        return boolean(false);
    bool hasOnlyTypedObjects = !type.anyObject && !type.anyFunction && type.engineClasses.isEmpty() && !type.acceptsUntypedObjects(m_program);
    auto test = [&](CellPointer cell) {
        Word32 shapeWord = shapeWordOf(cell);
        Word32 isInstance = passes(shapeWord, m_program.at(value.layout()).instances);
        if (m_program.mayRunInMultipleInstances)
            isInstance = Word32(m_out.bitAnd(isInstance.value(), belongsToThisInstance(cell).value()));
        auto walksChain = [&] {
            return AOT_TYPED_OPERATION(InheritsFromClass, cell, word32(value.layout().index()));
        };
        if (m_program.at(value.layout()).instancesMayBecomeRecords) {
            return either<Word32>(isInstance, [&] {
                return boolean(true);
            }, walksChain);
        }
        auto fromShape = [&] {
            if (!acceptsObjectsWithOtherPrototype(m_program, type))
                return isInstance;
            return either<Word32>(isInstance, [&] {
                return boolean(true);
            }, [&] {
                return either<Word32>(hasOtherPrototype(cell), walksChain, [&] {
                    return boolean(false);
                });
            });
        };
        if (hasOnlyTypedObjects)
            return fromShape();
        Word32 hasShape = passes(shapeWord, ShapeTest { 1, m_program.numberOfShapes });
        return either<Word32>(hasShape, fromShape, walksChain);
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
