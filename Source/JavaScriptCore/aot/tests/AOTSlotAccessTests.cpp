/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTMachineTestSupport.h"
#include "AOTShapeTest.h"
#include "AOTSlotAccess.h"
#include "AOTSlotAccessEmitter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PureNaN.h"
#include <bit>
#include <limits>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC::AOT {

std::span<const NativeTest> slotAccessTests();

static constexpr ASCIILiteral programText =
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"_s;

static constexpr uint64_t untouchedBits = 0xa5a5a5a5a5a5a5a5ULL;

static constexpr int32_t int32Samples[] = {
    0, 1, -1, 7, INT8_MIN, INT8_MAX, UINT8_MAX, INT16_MIN, INT16_MAX, UINT16_MAX, 1 << 24, (1 << 24) + 1,
    INT32_MAX, INT32_MIN, absentInt32, undefinedInt32, nullInt32, minInlineInt32 - 1, minInlineInt32,
};

static constexpr double doubleSamples[] = {
    0.0, -0.0, 1.5, 7.0, 0.1, 4294967295.0, 4294967296.0, -2147483649.0, 9007199254740992.0, 0x1p62, -0x1p62, 1e300,
    std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), PNaN,
    std::bit_cast<double>(absentDouble), std::bit_cast<double>(undefinedDouble), std::bit_cast<double>(nullDouble), std::bit_cast<double>(0xfff8000000000001ULL),
};

namespace {

} // anonymous namespace

static uint64_t bitsOutsideSlot(SlotRepresentation representation, uint64_t word)
{
    unsigned width = widthOf(representation);
    return width == sizeof(uint64_t) ? 0 : word >> (8 * width);
}

static void checkUnboxedWrite(const Instance& instance, SlotRepresentation representation, JSValue boxed, const Invocable<bool(void*)> auto& write)
{
    uint64_t slotWrittenFromBoxedValue = untouchedBits;
    bool shouldWrite = tryWriteSlot(instance, representation, &slotWrittenFromBoxedValue, boxed);

    uint64_t slot = untouchedBits;
    AOT_TEST_CHECK(write(&slot) == shouldWrite);
    AOT_TEST_CHECK(slot == slotWrittenFromBoxedValue);
    if (!shouldWrite) {
        AOT_TEST_CHECK(slot == untouchedBits);
        return;
    }
    AOT_TEST_CHECK(bitsOutsideSlot(representation, slot) == bitsOutsideSlot(representation, untouchedBits));
    AOT_TEST_CHECK(readSlot(representation, &slot, instance) == boxed);
}

static void testUnboxedWritesMatchBoxedWrites(LoadedProgram& program)
{
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        for (int32_t number : int32Samples) {
            checkUnboxedWrite(program.instance(), representation, jsNumber(number), [&](void* slot) {
                return tryWriteInt32(representation, slot, number);
            });
        }
        for (double number : doubleSamples) {
            checkUnboxedWrite(program.instance(), representation, jsNumber(withoutPayload(number)), [&](void* slot) {
                return tryWriteDouble(representation, slot, number);
            });
        }
        for (bool value : { false, true }) {
            checkUnboxedWrite(program.instance(), representation, jsBoolean(value), [&](void* slot) {
                return tryWriteBoolean(representation, slot, value);
            });
        }
    }
}

static void testCellsRoundTrip(LoadedProgram& program)
{
    uintptr_t cageBase = program.instance().cellCageBase;
    JSObject* object = constructEmptyObject(program.realm());
    AOT_TEST_CHECK(program.vm().heap.cage().contains(object));
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        SlotTraits traits = traitsOf(representation);
        uint64_t slot = untouchedBits;
        if (!traits.holdsCell) {
            AOT_TEST_CHECK(!tryWriteSlot(program.instance(), representation, &slot, object));
            AOT_TEST_CHECK(slot == untouchedBits);
            continue;
        }
        AOT_TEST_CHECK(tryWriteSlot(program.instance(), representation, &slot, object));
        AOT_TEST_CHECK(bitsOutsideSlot(representation, slot) == bitsOutsideSlot(representation, untouchedBits));
        AOT_TEST_CHECK(cellInSlot(representation, &slot, cageBase) == object);
        AOT_TEST_CHECK(readSlot(representation, &slot, program.instance()) == JSValue(object));
    }
}

static void testMarkersHoldNoCell(LoadedProgram& program)
{
    uintptr_t cageBase = program.instance().cellCageBase;
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        SlotTraits traits = traitsOf(representation);
        auto checkMarker = [&](bool hasMarker, uint64_t bits, JSValue value) {
            if (!hasMarker)
                return;
            uint64_t slot = untouchedBits;
            writeSlotBits(representation, &slot, bits);
            AOT_TEST_CHECK(readSlotBits(representation, &slot) == bits);
            AOT_TEST_CHECK(!cellInSlot(representation, &slot, cageBase));
            AOT_TEST_CHECK(readSlot(representation, &slot, program.instance()) == value);
        };
        checkMarker(traits.hasAbsent, traits.absentBits, JSValue());
        checkMarker(traits.hasUndefined, traits.undefinedBits, jsUndefined());
        checkMarker(traits.hasNull, traits.nullBits, jsNull());

        uint64_t slot = untouchedBits;
        clearSlot(representation, &slot);
        AOT_TEST_CHECK(slotHoldsAbsent(representation, &slot) == traits.hasAbsent);
        AOT_TEST_CHECK(readSlotBits(representation, &slot) == (traits.hasAbsent ? traits.absentBits : 0));
        AOT_TEST_CHECK(bitsOutsideSlot(representation, slot) == bitsOutsideSlot(representation, untouchedBits));
        AOT_TEST_CHECK(!cellInSlot(representation, &slot, cageBase));
    }
}

static Typed::DistancesByComparison checkedDistancesByComparison(std::span<const Typed::DistanceOfShapes> members)
{
    Typed::DistancesByComparison plan = Typed::distancesByComparison(members);
    size_t numberOfMembersWithDistance = 0;
    for (const Typed::DistanceOfShapes& member : members) {
        numberOfMembersWithDistance += !!member.distance;
        for (uint32_t shape = member.shapes.first; shape < member.shapes.end(); ++shape)
            AOT_TEST_CHECK(plan.distanceOf(shape) == member.distance);
    }
    AOT_TEST_CHECK(plan.numberOfComparisons() <= numberOfMembersWithDistance);
    return plan;
}

static Typed::DistancesByShape checkedDistancesIn(Typed::DistanceForm form, std::span<const Typed::DistanceOfShapes> members)
{
    Typed::DistancesByShape distances = Typed::encodeDistancesByShape(members, form);
    AOT_TEST_CHECK(distances.form == form || (form == Typed::DistanceForm::HashedTable && distances.form == Typed::DistanceForm::PagedTable));
    AOT_TEST_CHECK(distances.lastShape + 1 - distances.numberOfShapes == std::ranges::min(members, { }, [](const Typed::DistanceOfShapes& member) {
        return member.shapes.first;
    }).shapes.first);
    for (const Typed::DistanceOfShapes& member : members) {
        for (uint32_t shape = member.shapes.first; shape < member.shapes.end(); ++shape)
            AOT_TEST_CHECK(distances.distanceOf(shape) == member.distance);
    }
    return distances;
}

namespace {

struct OperationsThatFindDistance {
    unsigned loads { 0 };
    unsigned selects { 0 };
};

} // anonymous namespace

static OperationsThatFindDistance runCodeThatFindsDistances(std::span<const Typed::DistanceOfShapes> members, const Typed::DistancesByShape& distances)
{
    Machine::TestProcedure test;
    Output& out = test.out();
    uint32_t constant = distances.table.isEmpty() ? 0 : out.addConstantData(distances.table.span(), ConstantKind::OffsetTable);
    LValue shapeWord = out.castToInt32(test.argumentGPR(0));
    unsigned firstOperation = test.procedure().numberOfOperations();
    out.ret(out.zeroExtPtr(Typed::appendDistanceOfShape(out, test.heaps().AOTImage_tables, shapeWord, distances, constant)));

    OperationsThatFindDistance operations;
    for (unsigned index = firstOperation; index < test.procedure().numberOfOperations(); ++index) {
        Machine::Opcode opcode = test.procedure().operationAt(index)->opcode();
        operations.loads += Machine::isLoad(opcode);
        operations.selects += opcode == Machine::Select;
    }
    switch (distances.form) {
    case Typed::DistanceForm::Comparisons:
        AOT_TEST_CHECK(!operations.loads && operations.selects == distances.comparisons.numberOfComparisons());
        break;
    case Typed::DistanceForm::FlatTable:
    case Typed::DistanceForm::HashedTable:
        AOT_TEST_CHECK(operations.loads == 1 && !operations.selects);
        break;
    case Typed::DistanceForm::PagedTable:
        AOT_TEST_CHECK(operations.loads == 2 && !operations.selects);
        break;
    }

    Machine::compileOrFail(test);
    for (const Typed::DistanceOfShapes& member : members) {
        for (uint32_t shape = member.shapes.first; shape < member.shapes.end(); ++shape)
            AOT_TEST_CHECK(test.call<uint64_t>(static_cast<uint64_t>(shapeWordOf(shape))) == member.distance);
    }
    return operations;
}

static Vector<Typed::DistanceOfShapes> longRunsSideBySide(uint32_t numberOfRuns)
{
    constexpr uint32_t shapesPerRun = 3000;
    Vector<Typed::DistanceOfShapes> members;
    for (uint32_t run = 0; run < numberOfRuns; ++run)
        members.append({ { 1 + run * shapesPerRun, shapesPerRun }, 8 * run });
    return members;
}

static void testFormsWithEquallyManyInstructionsAreCompared()
{
    const Typed::DistanceOfShapes twoStretches[] { { { 10, 1 }, 0 }, { { 5000, 2 }, 8 }, { { 7000, 1 }, 8 } };
    Typed::DistancesByShape hashed = checkedDistancesIn(Typed::DistanceForm::HashedTable, twoStretches);
    AOT_TEST_CHECK(hashed.form == Typed::DistanceForm::HashedTable);
    Typed::DistancesByShape chosen = Typed::distancesByShape(twoStretches);
    AOT_TEST_CHECK(chosen.form == Typed::DistanceForm::Comparisons);
    AOT_TEST_CHECK(chosen.numberOfInstructions() == hashed.numberOfInstructions());
    AOT_TEST_CHECK(chosen.table.isEmpty() && !hashed.table.isEmpty());
    runCodeThatFindsDistances(twoStretches, chosen);

    Vector<Typed::DistanceOfShapes> threeRuns = longRunsSideBySide(3);
    Typed::DistancesByShape paged = checkedDistancesIn(Typed::DistanceForm::HashedTable, threeRuns.span());
    AOT_TEST_CHECK(paged.form == Typed::DistanceForm::PagedTable);
    chosen = Typed::distancesByShape(threeRuns.span());
    AOT_TEST_CHECK(chosen.form == Typed::DistanceForm::Comparisons);
    AOT_TEST_CHECK(chosen.numberOfInstructions() == paged.numberOfInstructions());
    runCodeThatFindsDistances(threeRuns.span(), chosen);
}

static Vector<Typed::DistanceOfShapes> membersInFourRuns(uint32_t scaleOfShapes, uint32_t scaleOfDistances)
{
    Vector<Typed::DistanceOfShapes> members;
    for (uint32_t run = 0; run < 4; ++run) {
        for (uint32_t place = 0; place < 9; ++place)
            members.append({ { 5 + run * 1750 * scaleOfShapes + place * 2, 1 + place % 2 }, 8 * ((run + place) % 5) * scaleOfDistances });
    }
    return members;
}

static void testPagedTablesHoldWideSpansAndWideDistances()
{
    for (uint32_t scaleOfDistances : { 1u, 100u }) {
        for (uint32_t scaleOfShapes : { 1u, 40u }) {
            Vector<Typed::DistanceOfShapes> members = membersInFourRuns(scaleOfShapes, scaleOfDistances);
            Typed::DistancesByShape distances = checkedDistancesIn(Typed::DistanceForm::PagedTable, members.span());
            AOT_TEST_CHECK(distances.logBytesPerDistance == (scaleOfDistances > 1));
            AOT_TEST_CHECK(distances.logBytesPerPageEntry == (scaleOfShapes > 1 ? 2 : 1));
            AOT_TEST_CHECK(!(distances.table.size() % (size_t { 1 } << distances.logBytesPerPageEntry)));
            runCodeThatFindsDistances(members.span(), distances);
        }
    }
}

static void testDistancesByComparisonOfManySets()
{
    uint32_t state = 1;
    auto next = [&](uint32_t bound) {
        state = state * 1664525 + 1013904223;
        return (state >> 8) % bound;
    };
    constexpr uint32_t distances[] { 0, 0, 8, 16, 16, 16, 40 };
    for (unsigned round = 0; round < 2000; ++round) {
        uint32_t scaleOfDistances = round % 5 == 4 ? 16 : 1;
        Vector<Typed::DistanceOfShapes> members;
        uint32_t shape = 1 + next(50);
        for (uint32_t count = 1 + next(40); count--;) {
            shape += next(400);
            uint32_t numberOfShapes = 1 + next(4);
            members.append({ { shape, numberOfShapes }, distances[next(std::size(distances))] * scaleOfDistances });
            shape += numberOfShapes;
        }
        for (size_t index = members.size(); index > 1; --index)
            std::swap(members[index - 1], members[next(index)]);
        Typed::DistancesByComparison byComparison = checkedDistancesByComparison(members.span());

        Typed::DistancesByShape chosen = Typed::distancesByShape(members.span());
        bool fitsFlatTable = byComparison.stretches.last().shapes.end() - byComparison.stretches.first().shapes.first <= Typed::maxShapesInFlatTable;
        AOT_TEST_CHECK(chosen.form == Typed::DistanceForm::Comparisons || chosen.numberOfInstructions() < byComparison.numberOfInstructions());
        AOT_TEST_CHECK(chosen.form == Typed::DistanceForm::Comparisons || (chosen.form == Typed::DistanceForm::FlatTable) == fitsFlatTable);

        Vector<Typed::DistanceOfShapes> inAnotherOrder = members;
        inAnotherOrder.reverse();
        for (Typed::DistanceForm form : { Typed::DistanceForm::Comparisons, Typed::DistanceForm::FlatTable, Typed::DistanceForm::HashedTable, Typed::DistanceForm::PagedTable }) {
            Typed::DistancesByShape inForm = checkedDistancesIn(form, members.span());
            Typed::DistancesByShape fromAnotherOrder = Typed::encodeDistancesByShape(inAnotherOrder.span(), form);
            AOT_TEST_CHECK(inForm.form == fromAnotherOrder.form && inForm.table == fromAnotherOrder.table);
            AOT_TEST_CHECK(inForm.hashMultiplier == fromAnotherOrder.hashMultiplier && inForm.hashShift == fromAnotherOrder.hashShift);
            AOT_TEST_CHECK(form != chosen.form || inForm.table == chosen.table);
            AOT_TEST_CHECK(chosen.form != Typed::DistanceForm::PagedTable || inForm.form != Typed::DistanceForm::HashedTable);
            if (!(round % 100))
                runCodeThatFindsDistances(members.span(), inForm);
        }
    }
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<slotAccessTests, programText, body>;

std::span<const NativeTest> slotAccessTests()
{
    static constexpr NativeTest tests[] = {
        { "slot-access/unboxed-writes-match-boxed-writes"_s, withProgram<testUnboxedWritesMatchBoxedWrites> },
        { "slot-access/cells-round-trip"_s, withProgram<testCellsRoundTrip> },
        { "slot-access/markers-hold-no-cell"_s, withProgram<testMarkersHoldNoCell> },
        { "slot-access/distances-by-comparison-of-many-sets"_s, testDistancesByComparisonOfManySets },
        { "slot-access/forms-with-equally-many-instructions-are-compared"_s, testFormsWithEquallyManyInstructionsAreCompared },
        { "slot-access/paged-tables-hold-wide-spans-and-wide-distances"_s, testPagedTablesHoldWideSpansAndWideDistances },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
