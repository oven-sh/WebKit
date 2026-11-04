/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTLoadedProgramTestSupport.h"
#include "AOTProgramStrings.h"
#include "AOTSlotAccess.h"
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

struct IntegerRange {
    SlotRepresentation representation;
    double minimum;
    double maximum;
};

} // anonymous namespace

static constexpr IntegerRange integerRanges[] = {
    { SlotRepresentation::Uint8, 0, UINT8_MAX },
    { SlotRepresentation::Int8, INT8_MIN, INT8_MAX },
    { SlotRepresentation::Uint16, 0, UINT16_MAX },
    { SlotRepresentation::Int16, INT16_MIN, INT16_MAX },
    { SlotRepresentation::Int32, INT32_MIN, INT32_MAX },
    { SlotRepresentation::Uint32, 0, UINT32_MAX },
    { SlotRepresentation::OptionalInt32, minInlineInt32, INT32_MAX },
};

static uint64_t bitsOutsideSlot(SlotRepresentation representation, uint64_t word)
{
    unsigned width = widthOf(representation);
    return width == sizeof(uint64_t) ? 0 : word >> (8 * width);
}

static void checkUnboxedWrite(ProgramStrings& strings, SlotRepresentation representation, JSValue boxed, const Invocable<bool(void*)> auto& write)
{
    uint64_t slotWrittenFromBoxedValue = untouchedBits;
    bool shouldWrite = tryWriteSlot(strings, representation, &slotWrittenFromBoxedValue, boxed);

    uint64_t slot = untouchedBits;
    AOT_TEST_CHECK(write(&slot) == shouldWrite);
    AOT_TEST_CHECK(slot == slotWrittenFromBoxedValue);
    if (!shouldWrite) {
        AOT_TEST_CHECK(slot == untouchedBits);
        return;
    }
    AOT_TEST_CHECK(bitsOutsideSlot(representation, slot) == bitsOutsideSlot(representation, untouchedBits));
    AOT_TEST_CHECK(readSlot(representation, &slot, strings) == boxed);
}

static void testUnboxedWritesMatchBoxedWrites(LoadedProgram& program)
{
    ProgramStrings& strings = *program.state().strings;
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        for (int32_t number : int32Samples) {
            checkUnboxedWrite(strings, representation, jsNumber(number), [&](void* slot) {
                return tryWriteInt32(representation, slot, number);
            });
        }
        for (double number : doubleSamples) {
            checkUnboxedWrite(strings, representation, jsNumber(withoutPayload(number)), [&](void* slot) {
                return tryWriteDouble(representation, slot, number);
            });
        }
        for (bool value : { false, true }) {
            checkUnboxedWrite(strings, representation, jsBoolean(value), [&](void* slot) {
                return tryWriteBoolean(representation, slot, value);
            });
        }
    }
}

static void testIntegersOutsideRangeAreRefused(LoadedProgram& program)
{
    ProgramStrings& strings = *program.state().strings;
    for (const IntegerRange& range : integerRanges) {
        for (double inside : { range.minimum, range.maximum }) {
            uint64_t slot = untouchedBits;
            AOT_TEST_CHECK(tryWriteSlot(strings, range.representation, &slot, jsNumber(inside)));
            AOT_TEST_CHECK(readSlot(range.representation, &slot, strings) == jsNumber(inside));
        }
        for (double outside : { range.minimum - 1, range.maximum + 1, range.maximum - 0.5 }) {
            uint64_t slot = untouchedBits;
            AOT_TEST_CHECK(!tryWriteSlot(strings, range.representation, &slot, jsNumber(outside)));
            AOT_TEST_CHECK(slot == untouchedBits);
        }
    }
    for (SlotRepresentation representation : { SlotRepresentation::Int64, SlotRepresentation::OptionalInt64 }) {
        for (double inside : { -0x1p61, 0x1p61 }) {
            uint64_t slot = untouchedBits;
            AOT_TEST_CHECK(tryWriteSlot(strings, representation, &slot, jsNumber(inside)));
            AOT_TEST_CHECK(readSlot(representation, &slot, strings) == jsNumber(inside));
        }
        for (double outside : { -0x1p62, 0x1p62, 0.5 }) {
            uint64_t slot = untouchedBits;
            AOT_TEST_CHECK(!tryWriteSlot(strings, representation, &slot, jsNumber(outside)));
            AOT_TEST_CHECK(slot == untouchedBits);
        }
    }
}

static void testCellsRoundTrip(LoadedProgram& program)
{
    ProgramStrings& strings = *program.state().strings;
    uintptr_t cageBase = program.instance().cellCageBase;
    JSObject* object = constructEmptyObject(program.realm());
    AOT_TEST_CHECK(program.vm().heap.cage().contains(object));
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        SlotTraits traits = traitsOf(representation);
        uint64_t slot = untouchedBits;
        if (!traits.holdsCell || traits.valueKind == SlotValueKind::LiteralString) {
            AOT_TEST_CHECK(!tryWriteSlot(strings, representation, &slot, object));
            AOT_TEST_CHECK(slot == untouchedBits);
            continue;
        }
        AOT_TEST_CHECK(tryWriteSlot(strings, representation, &slot, object));
        AOT_TEST_CHECK(bitsOutsideSlot(representation, slot) == bitsOutsideSlot(representation, untouchedBits));
        AOT_TEST_CHECK(cellInSlot(representation, &slot, cageBase) == object);
        AOT_TEST_CHECK(readSlot(representation, &slot, strings) == JSValue(object));
    }
}

static void testStringsOutsideLiteralTableRoundTrip(LoadedProgram& program)
{
    ProgramStrings& strings = *program.state().strings;
    uintptr_t cageBase = program.instance().cellCageBase;
    JSString* string = jsNontrivialString(program.vm(), "not a literal of the program"_s);
    for (SlotRepresentation representation : { SlotRepresentation::LiteralString, SlotRepresentation::LiteralString32 }) {
        uint64_t slot = untouchedBits;
        AOT_TEST_CHECK(tryWriteSlot(strings, representation, &slot, string));
        AOT_TEST_CHECK(bitsOutsideSlot(representation, slot) == bitsOutsideSlot(representation, untouchedBits));
        AOT_TEST_CHECK(cellInSlot(representation, &slot, cageBase) == string);
        AOT_TEST_CHECK(readSlot(representation, &slot, strings) == JSValue(string));
    }
}

static void testMarkersHoldNoCell(LoadedProgram& program)
{
    ProgramStrings& strings = *program.state().strings;
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
            AOT_TEST_CHECK(readSlot(representation, &slot, strings) == value);
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

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<slotAccessTests, programText, body>;

std::span<const NativeTest> slotAccessTests()
{
    static constexpr NativeTest tests[] = {
        { "slot-access/unboxed-writes-match-boxed-writes"_s, withProgram<testUnboxedWritesMatchBoxedWrites> },
        { "slot-access/integers-outside-range-are-refused"_s, withProgram<testIntegersOutsideRangeAreRefused> },
        { "slot-access/cells-round-trip"_s, withProgram<testCellsRoundTrip> },
        { "slot-access/strings-outside-literal-table-round-trip"_s, withProgram<testStringsOutsideLiteralTableRoundTrip> },
        { "slot-access/markers-hold-no-cell"_s, withProgram<testMarkersHoldNoCell> },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
