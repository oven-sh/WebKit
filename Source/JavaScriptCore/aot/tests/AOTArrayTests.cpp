/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTArrayStorageForms.h"
#include "AOTBuiltinTable.h"
#include "AOTConvention.h"
#include "AOTNativeTest.h"
#include "AOTObjects.h"
#include "AOTRealmTestSupport.h"
#include "AOTTypeArena.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr TypeID int53Type = *tryMakeTypeOfPrimitives(Primitive::Integer, Range::exact());
static constexpr TypeID fractionType = typeOfPrimitives(Primitive::FractionalNumber);
static constexpr TypeID wholeNumberType = typeOfPrimitives(Primitive::WholeNumber);
static constexpr TypeID numberWithoutNaNType = typeOfPrimitives(Primitive::Integer | Primitive::FractionalNumber);

struct TableOfElementTypes : TypeTable {
    TableOfElementTypes()
        : TypeTable(BuiltinTable::singleton())
    {
        setObjects(makeUniqueWithoutFastMallocCheck<ObjectTable>(BuiltinTable::singleton(), std::span<const FunctionInOrderOfObjects>()));
    }
};

static void testFormsTable()
{
    AOT_TEST_CHECK(arrayStorageForms.size() == 3);
    for (size_t index = 0; index < arrayStorageForms.size(); ++index) {
        const ArrayStorageForm& form = arrayStorageForms[index];
        AOT_TEST_CHECK(!index || arrayStorageForms[index - 1].shape < form.shape);
        AOT_TEST_CHECK((form.indexingType & IndexingShapeMask) == form.shape && (form.indexingType & IsArray));
        AOT_TEST_CHECK(arrayStorageOf(form.indexingType) == form.kind);
        AOT_TEST_CHECK(arrayStorageOf(form.indexingType | CopyOnWrite) == form.kind);
        AOT_TEST_CHECK(arrayStorageOf(form.shape) == form.kind);
        AOT_TEST_CHECK(findArrayStorageForm(form.kind) == &form && &arrayStorageFormOf(form.kind) == &form && hasArrayStorageForm(form.kind));
        AOT_TEST_CHECK(elementRepresentationOf(form.kind) == form.element);
        AOT_TEST_CHECK(form.kind != ArrayStorage::Other);
    }

    AOT_TEST_CHECK(arrayStorageForms[0].kind == ArrayStorage::Int32 && arrayStorageForms[1].kind == ArrayStorage::Double && arrayStorageForms[2].kind == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageFormOf(ArrayStorage::Int32).element == Representation::Int32 && !arrayStorageFormOf(ArrayStorage::Int32).hole && !arrayStorageFormOf(ArrayStorage::Int32).holdsCells);
    AOT_TEST_CHECK(arrayStorageFormOf(ArrayStorage::Double).element == Representation::Double && arrayStorageFormOf(ArrayStorage::Double).hole == PNaNAsBits && !arrayStorageFormOf(ArrayStorage::Double).holdsCells);
    AOT_TEST_CHECK(arrayStorageFormOf(ArrayStorage::Contiguous).element == Representation::JSValue && !arrayStorageFormOf(ArrayStorage::Contiguous).hole && arrayStorageFormOf(ArrayStorage::Contiguous).holdsCells);

    AOT_TEST_CHECK(!findArrayStorageForm(ArrayStorage::Other) && !hasArrayStorageForm(ArrayStorage::Other));
    AOT_TEST_CHECK(elementRepresentationOf(ArrayStorage::Other) == Representation::JSValue);
    for (IndexingType indexingType : { NonArray, ArrayClass, ArrayWithUndecided, ArrayWithArrayStorage, ArrayWithSlowPutArrayStorage })
        AOT_TEST_CHECK(arrayStorageOf(indexingType) == ArrayStorage::Other);
    AOT_TEST_CHECK(arrayStorageTestMask == IndexingShapeMask);
}

static void testFormsHolding()
{
    TableOfElementTypes table;
    TypeArena types(table, ArenaKeeps::InProgram);

    AOT_TEST_CHECK(arrayStorageHolding(types, TypeTable::never) == ArrayStorage::Other);
    AOT_TEST_CHECK(arrayStorageHolding(types, TypeTable::absent) == ArrayStorage::Other);
    AOT_TEST_CHECK(arrayStorageHolding(types, TypeTable::int32) == ArrayStorage::Int32);
    AOT_TEST_CHECK(arrayStorageHolding(types, types.integer(Range::one(7))) == ArrayStorage::Int32);
    AOT_TEST_CHECK(arrayStorageHolding(types, types.withPrimitives(TypeTable::int32, Primitive::Absent)) == ArrayStorage::Int32);
    AOT_TEST_CHECK(arrayStorageHolding(types, int53Type) == ArrayStorage::Double);
    AOT_TEST_CHECK(arrayStorageHolding(types, types.integer({ 0, static_cast<int64_t>(INT32_MAX) + 1 })) == ArrayStorage::Double);
    AOT_TEST_CHECK(arrayStorageHolding(types, fractionType) == ArrayStorage::Double);
    AOT_TEST_CHECK(arrayStorageHolding(types, numberWithoutNaNType) == ArrayStorage::Double);
    AOT_TEST_CHECK(arrayStorageHolding(types, types.withPrimitives(fractionType, Primitive::Absent)) == ArrayStorage::Double);
    AOT_TEST_CHECK(arrayStorageHolding(types, TypeTable::string) == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageHolding(types, types.withPrimitives(TypeTable::int32, Primitive::String)) == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageHolding(types, types.withPrimitives(TypeTable::int32, Primitive::Boolean)) == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageHolding(types, TypeTable::object) == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageHolding(types, TypeTable::anything) == ArrayStorage::Contiguous);
}

static void testFormsAtAllocation()
{
    TableOfElementTypes table;
    TypeArena types(table, ArenaKeeps::InProgram);

    AOT_TEST_CHECK(arrayStorageAtAllocation(types, types.withPrimitives(numberWithoutNaNType, Primitive::Undefined)) == ArrayStorage::Double);
    AOT_TEST_CHECK(arrayStorageAtAllocation(types, types.withPrimitives(fractionType, Primitive::Null)) == ArrayStorage::Double);
    AOT_TEST_CHECK(arrayStorageAtAllocation(types, types.withPrimitives(TypeTable::int32, Primitive::UndefinedOrNull | Primitive::Absent)) == ArrayStorage::Int32);
    AOT_TEST_CHECK(arrayStorageAtAllocation(types, TypeTable::int32) == ArrayStorage::Int32);
    AOT_TEST_CHECK(arrayStorageAtAllocation(types, types.withPrimitives(TypeTable::string, Primitive::Undefined)) == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageAtAllocation(types, TypeTable::anything) == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageAtAllocation(types, TypeTable::never) == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageAtAllocation(types, TypeTable::undefined) == ArrayStorage::Contiguous);
}

static void testFormsStoreWithoutConversion()
{
    TableOfElementTypes table;
    TypeArena types(table, ArenaKeeps::InProgram);

    AOT_TEST_CHECK(canStoreWithoutConversion(types, ArrayStorage::Int32, TypeTable::int32));
    AOT_TEST_CHECK(!canStoreWithoutConversion(types, ArrayStorage::Int32, int53Type));
    AOT_TEST_CHECK(!canStoreWithoutConversion(types, ArrayStorage::Int32, fractionType));
    AOT_TEST_CHECK(!canStoreWithoutConversion(types, ArrayStorage::Int32, types.withPrimitives(TypeTable::int32, Primitive::Undefined)));

    AOT_TEST_CHECK(canStoreWithoutConversion(types, ArrayStorage::Double, TypeTable::int32));
    AOT_TEST_CHECK(canStoreWithoutConversion(types, ArrayStorage::Double, int53Type));
    AOT_TEST_CHECK(canStoreWithoutConversion(types, ArrayStorage::Double, fractionType));
    AOT_TEST_CHECK(!canStoreWithoutConversion(types, ArrayStorage::Double, wholeNumberType));
    AOT_TEST_CHECK(!canStoreWithoutConversion(types, ArrayStorage::Double, TypeTable::number));
    AOT_TEST_CHECK(!canStoreWithoutConversion(types, ArrayStorage::Double, types.withPrimitives(fractionType, Primitive::Undefined)));
    AOT_TEST_CHECK(!canStoreWithoutConversion(types, ArrayStorage::Double, TypeTable::string));

    AOT_TEST_CHECK(canStoreWithoutConversion(types, ArrayStorage::Contiguous, TypeTable::int32));
    AOT_TEST_CHECK(canStoreWithoutConversion(types, ArrayStorage::Contiguous, wholeNumberType));
    AOT_TEST_CHECK(canStoreWithoutConversion(types, ArrayStorage::Contiguous, TypeTable::anything));

    AOT_TEST_CHECK(!canStoreWithoutConversion(types, ArrayStorage::Other, TypeTable::int32));
    AOT_TEST_CHECK(!canStoreWithoutConversion(types, ArrayStorage::Other, TypeTable::anything));
}

static void testFormsElementReadTypes()
{
    AOT_TEST_CHECK(typeOfElementRead(ArrayStorage::Int32) == *tryMakeTypeOfPrimitives(Primitive::Integer | Primitive::Undefined, Range::int32()));
    AOT_TEST_CHECK(typeOfElementRead(ArrayStorage::Double) == typeOfPrimitives(Primitive::Number | Primitive::Undefined));
    AOT_TEST_CHECK(typeOfElementRead(ArrayStorage::Contiguous) == TypeTable::anything);
    AOT_TEST_CHECK(typeOfElementRead(ArrayStorage::Other) == TypeTable::anything);
}

static JSArray* createArray(Realms& realms, IndexingType indexingType, unsigned length)
{
    JSArray* array = JSArray::tryCreate(realms.vm(), realms.plain()->arrayStructureForIndexingTypeDuringAllocation(indexingType), length);
    AOT_TEST_CHECK(array && array->indexingType() == indexingType && array->length() == length);
    return array;
}

static void review(Realms& realms, ArrayAllocationProfile& profile, IndexingType indexingType, unsigned length)
{
    reviewArrayAllocationProfile(profile, createArray(realms, indexingType, length));
}

static void review(Realms& realms, ArrayAllocationProfile& profile, IndexingType indexingType, unsigned length, unsigned times)
{
    for (unsigned count = 0; count < times; ++count)
        review(realms, profile, indexingType, length);
}

static_assert(ArrayAllocationProfile::lengthsKept == 3);

static bool hasWindow(const ArrayAllocationProfile& profile, unsigned reviews, unsigned largest, unsigned second, unsigned third)
{
    return profile.reviews == reviews && profile.largestLengths[0] == largest && profile.largestLengths[1] == second && profile.largestLengths[2] == third;
}

static bool hasNewWindow(const ArrayAllocationProfile& profile)
{
    return hasWindow(profile, 0, 0, 0, 0);
}

static constexpr unsigned defaultCapacity = arrayLiteralVectorLength(0);
static constexpr unsigned window = ArrayAllocationProfile::reviewsInWindow;
static_assert(window > 5);

static void testProfileGrowsWithTheThirdLongerArray()
{
    Realms realms;
    ArrayAllocationProfile profile { };

    review(realms, profile, ArrayWithInt32, defaultCapacity);
    AOT_TEST_CHECK(!profile.capacity() && hasWindow(profile, 1, defaultCapacity, 0, 0));

    review(realms, profile, ArrayWithInt32, 12);
    AOT_TEST_CHECK(!profile.capacity() && hasWindow(profile, 2, 12, defaultCapacity, 0));
    review(realms, profile, ArrayWithInt32, 1000);
    AOT_TEST_CHECK(!profile.capacity() && hasWindow(profile, 3, BASE_CONTIGUOUS_VECTOR_LEN_MAX, 12, defaultCapacity));
    review(realms, profile, ArrayWithInt32, 1);
    AOT_TEST_CHECK(!profile.capacity() && hasWindow(profile, 4, BASE_CONTIGUOUS_VECTOR_LEN_MAX, 12, defaultCapacity));

    review(realms, profile, ArrayWithInt32, defaultCapacity + 1);
    AOT_TEST_CHECK(profile.capacity() == ((defaultCapacity + 1) | 1) && profile.shape() == Int32Shape && hasNewWindow(profile));

    review(realms, profile, ArrayWithInt32, 12, 2);
    AOT_TEST_CHECK(profile.capacity() == ((defaultCapacity + 1) | 1) && hasWindow(profile, 2, 12, 12, 0));
    review(realms, profile, ArrayWithInt32, 20);
    AOT_TEST_CHECK(profile.capacity() == 13 && hasNewWindow(profile));

    review(realms, profile, ArrayWithInt32, 13);
    AOT_TEST_CHECK(profile.capacity() == 13 && hasWindow(profile, 1, 13, 0, 0));

    review(realms, profile, ArrayWithInt32, 1000, 3);
    AOT_TEST_CHECK(profile.capacity() == BASE_CONTIGUOUS_VECTOR_LEN_MAX && hasNewWindow(profile));

    review(realms, profile, ArrayWithInt32, 1000);
    AOT_TEST_CHECK(profile.capacity() == BASE_CONTIGUOUS_VECTOR_LEN_MAX && hasWindow(profile, 1, BASE_CONTIGUOUS_VECTOR_LEN_MAX, 0, 0));
}

static void testProfileShrinksAtTheEndOfAWindow()
{
    Realms realms;
    ArrayAllocationProfile profile { };

    review(realms, profile, ArrayWithInt32, 20, 3);
    AOT_TEST_CHECK(profile.capacity() == 21 && hasNewWindow(profile));
    unsigned reviews = 0;
    for (unsigned length : { 4u, 10u, 2u, 6u }) {
        review(realms, profile, ArrayWithInt32, length);
        AOT_TEST_CHECK(profile.capacity() == 21 && profile.reviews == ++reviews);
    }
    AOT_TEST_CHECK(hasWindow(profile, 4, 10, 6, 4));
    review(realms, profile, ArrayWithInt32, 1, window - 5);
    AOT_TEST_CHECK(profile.capacity() == 21 && hasWindow(profile, window - 1, 10, 6, 4));
    review(realms, profile, ArrayWithInt32, 1);
    AOT_TEST_CHECK(profile.capacity() == 5 && profile.shape() == Int32Shape && hasNewWindow(profile));

    // Two longer arrays in a window change nothing.
    review(realms, profile, ArrayWithInt32, 16);
    review(realms, profile, ArrayWithInt32, 9);
    review(realms, profile, ArrayWithInt32, 5, window - 3);
    AOT_TEST_CHECK(profile.capacity() == 5 && hasWindow(profile, window - 1, 16, 9, 5));
    review(realms, profile, ArrayWithInt32, 1);
    AOT_TEST_CHECK(profile.capacity() == 5 && hasNewWindow(profile));

    review(realms, profile, ArrayWithInt32, 16);
    review(realms, profile, ArrayWithInt32, 9);
    review(realms, profile, ArrayWithInt32, 0, window - 2);
    AOT_TEST_CHECK(profile.capacity() == 1 && hasNewWindow(profile));

    ArrayAllocationProfile unlearned { };
    review(realms, unlearned, ArrayWithInt32, 2, window);
    AOT_TEST_CHECK(unlearned.capacity() == 3 && hasNewWindow(unlearned));
}

static void testProfileShapeOnlyRises()
{
    Realms realms;
    ArrayAllocationProfile profile { };
    AOT_TEST_CHECK(!profile.shape());

    struct Step {
        IndexingType indexingType;
        IndexingType learnedShape;
    };
    for (Step step : { Step { ArrayWithInt32, Int32Shape }, Step { ArrayWithDouble, DoubleShape }, Step { ArrayWithInt32, DoubleShape }, Step { ArrayWithContiguous, ContiguousShape }, Step { ArrayWithDouble, ContiguousShape }, Step { ArrayWithInt32, ContiguousShape } }) {
        review(realms, profile, step.indexingType, 1);
        AOT_TEST_CHECK(profile.shape() == step.learnedShape);
    }

    ArrayAllocationProfile growing { };
    review(realms, growing, ArrayWithContiguous, 1);
    review(realms, growing, ArrayWithInt32, 10, 3);
    AOT_TEST_CHECK(growing.capacity() == 11 && growing.shape() == ContiguousShape);
    review(realms, growing, ArrayWithDouble, 2, window);
    AOT_TEST_CHECK(growing.capacity() == 3 && growing.shape() == ContiguousShape);
}

static bool isSameProfile(const ArrayAllocationProfile& first, const ArrayAllocationProfile& second)
{
    return first.word == second.word && hasWindow(first, second.reviews, second.largestLengths[0], second.largestLengths[1], second.largestLengths[2]);
}

static void testProfileIgnoresArrayStorage()
{
    Realms realms;
    ArrayAllocationProfile profile { };
    review(realms, profile, ArrayWithInt32, 8, 3);
    review(realms, profile, ArrayWithInt32, 3);
    AOT_TEST_CHECK(profile.capacity() == 9 && profile.shape() == Int32Shape && hasWindow(profile, 1, 3, 0, 0));
    ArrayAllocationProfile before = profile;

    review(realms, profile, ArrayWithArrayStorage, 20);
    AOT_TEST_CHECK(isSameProfile(profile, before));
    review(realms, profile, ArrayWithArrayStorage, 1);
    AOT_TEST_CHECK(isSameProfile(profile, before));
    review(realms, profile, ArrayWithUndecided, 20);
    AOT_TEST_CHECK(isSameProfile(profile, before));
    reviewArrayAllocationProfile(profile, nullptr);
    AOT_TEST_CHECK(isSameProfile(profile, before));

    review(realms, profile, ArrayWithContiguous, 20, 3);
    AOT_TEST_CHECK(profile.capacity() == 21 && profile.shape() == ContiguousShape);
}

static void testProfileKeepsCount()
{
    Realms realms;
    constexpr uint32_t count = 0xabcdu << ArrayAllocationProfile::allocationCountShift;
    constexpr uint32_t countMask = ArrayAllocationProfile::countMask;
    ArrayAllocationProfile profile { };
    profile.word = count;

    review(realms, profile, ArrayWithInt32, 20, 3);
    AOT_TEST_CHECK(profile.capacity() == 21 && (profile.word & countMask) == count);
    review(realms, profile, ArrayWithDouble, 2);
    AOT_TEST_CHECK(profile.shape() == DoubleShape && (profile.word & countMask) == count);
    review(realms, profile, ArrayWithDouble, 2, window - 1);
    AOT_TEST_CHECK(profile.capacity() == 3 && (profile.word & countMask) == count);
    AOT_TEST_CHECK(profile.word == (count | static_cast<uint32_t>(DoubleShape) << ArrayAllocationProfile::shapeShift | 3));
}

static void testProfileReviewInterval()
{
    ArrayAllocationProfile profile { };
    profile.set(BASE_CONTIGUOUS_VECTOR_LEN_MAX, ContiguousShape);
    AOT_TEST_CHECK(!profile.isDueForReview());
    for (uint32_t allocation = 1; allocation <= 3 * ArrayAllocationProfile::shortestInterval; ++allocation) {
        profile.word -= ArrayAllocationProfile::oneAllocation;
        AOT_TEST_CHECK(profile.isDueForReview() == (allocation % ArrayAllocationProfile::shortestInterval == 1));
        AOT_TEST_CHECK(profile.capacity() == BASE_CONTIGUOUS_VECTOR_LEN_MAX && profile.shape() == ContiguousShape);
        if (profile.isDueForReview())
            profile.startInterval();
    }
}

static void testProfileReviewIntervalGrowsWhileNothingChanges()
{
    Realms realms;
    constexpr uint32_t shortest = ArrayAllocationProfile::shortestInterval;
    constexpr uint32_t longest = shortest << ArrayAllocationProfile::maxIntervalDoublings;
    ArrayAllocationProfile profile { };
    review(realms, profile, ArrayWithDouble, 3, window);
    AOT_TEST_CHECK(profile.capacity() == 3 && profile.interval() == shortest);
    for (uint32_t expected = 2 * shortest; expected <= longest; expected *= 2) {
        review(realms, profile, ArrayWithDouble, 3, window - 1);
        AOT_TEST_CHECK(profile.interval() == expected / 2);
        review(realms, profile, ArrayWithDouble, 3);
        AOT_TEST_CHECK(profile.interval() == expected && hasNewWindow(profile));
    }
    review(realms, profile, ArrayWithDouble, 3, 2 * window);
    AOT_TEST_CHECK(profile.interval() == longest && profile.capacity() == 3);

    for (uint32_t expected = longest / 2; expected >= shortest; expected /= 2) {
        reviewArrayAllocationProfile(profile, nullptr);
        AOT_TEST_CHECK(profile.interval() == expected && profile.capacity() == 3);
    }
    reviewArrayAllocationProfile(profile, nullptr);
    AOT_TEST_CHECK(profile.interval() == shortest);
    review(realms, profile, ArrayWithDouble, 3, ArrayAllocationProfile::maxIntervalDoublings * window);
    AOT_TEST_CHECK(profile.interval() == longest);

    review(realms, profile, ArrayWithDouble, 4);
    AOT_TEST_CHECK(profile.interval() == shortest && profile.capacity() == 3);
    review(realms, profile, ArrayWithDouble, 3, 2 * window);
    AOT_TEST_CHECK(profile.interval() > shortest);
    review(realms, profile, ArrayWithContiguous, 3);
    AOT_TEST_CHECK(profile.interval() == shortest && profile.shape() == ContiguousShape);
    review(realms, profile, ArrayWithContiguous, 3, 2 * window);
    AOT_TEST_CHECK(profile.interval() > shortest);
    review(realms, profile, ArrayWithContiguous, 1, 2 * window);
    AOT_TEST_CHECK(profile.capacity() == 1);
    reviewArrayAllocationProfile(profile, nullptr);
    AOT_TEST_CHECK(profile.capacity() == 1);
}

static bool hasLengths(JSArray* array, unsigned publicLength, unsigned minVectorLength, unsigned maxVectorLength)
{
    unsigned vectorLength = array->butterfly()->vectorLength();
    return array->length() == publicLength && vectorLength >= minVectorLength && vectorLength <= maxVectorLength;
}

static void testReserveMakesRoomOnce()
{
    Realms realms;
    for (IndexingType indexingType : { ArrayWithInt32, ArrayWithDouble, ArrayWithContiguous }) {
        JSArray* array = createArray(realms, indexingType, 4);
        reserveArrayCapacity(realms.vm(), array, 100000);
        AOT_TEST_CHECK(array->indexingType() == indexingType && hasLengths(array, 4, 100004, 100004 + 16));

        Butterfly* butterfly = array->butterfly();
        reserveArrayCapacity(realms.vm(), array, 100000);
        AOT_TEST_CHECK(array->butterfly() == butterfly);
    }
}

static void testReserveLeavesSmallStepsToUsualGrowth()
{
    Realms realms;
    JSArray* array = createArray(realms, ArrayWithContiguous, 1000);
    Butterfly* butterfly = array->butterfly();
    unsigned vectorLength = butterfly->vectorLength();
    for (uint64_t additional : { uint64_t { 0 }, uint64_t { 1 }, uint64_t { 400 } }) {
        reserveArrayCapacity(realms.vm(), array, additional);
        AOT_TEST_CHECK(array->butterfly() == butterfly && hasLengths(array, 1000, vectorLength, vectorLength));
    }
}

static void testReserveDeclines()
{
    Realms realms;
    // The constant of the same name in reserveArrayCapacity().
    constexpr unsigned maxElementsReservedInOneStep = 4194304;
    JSArray* array = createArray(realms, ArrayWithInt32, 4);
    Butterfly* butterfly = array->butterfly();
    // The longest storage has room for the first two counts, so only the bound on one reservation declines them.
    uint64_t largestCountThatFits = MAX_STORAGE_VECTOR_LENGTH - array->length();
    for (uint64_t additional : { uint64_t { maxElementsReservedInOneStep } + 1, largestCountThatFits, static_cast<uint64_t>(-1), static_cast<uint64_t>(MAX_STORAGE_VECTOR_LENGTH) })
        reserveArrayCapacity(realms.vm(), array, additional);
    AOT_TEST_CHECK(array->butterfly() == butterfly && array->length() == 4);

    reserveArrayCapacity(realms.vm(), array, maxElementsReservedInOneStep);
    AOT_TEST_CHECK(array->length() == 4 && array->butterfly()->vectorLength() >= maxElementsReservedInOneStep + 4);

    JSArray* withArrayStorage = createArray(realms, ArrayWithArrayStorage, 4);
    butterfly = withArrayStorage->butterfly();
    reserveArrayCapacity(realms.vm(), withArrayStorage, 100000);
    AOT_TEST_CHECK(withArrayStorage->butterfly() == butterfly && withArrayStorage->length() == 4);
}

std::span<const NativeTest> arrayTests()
{
    static constexpr NativeTest tests[] = {
        { "arrays/forms/table"_s, testFormsTable },
        { "arrays/forms/holding"_s, testFormsHolding },
        { "arrays/forms/at-allocation"_s, testFormsAtAllocation },
        { "arrays/forms/store-without-conversion"_s, testFormsStoreWithoutConversion },
        { "arrays/forms/element-read-types"_s, testFormsElementReadTypes },
        { "arrays/profile/grows-with-the-third-longer-array"_s, testProfileGrowsWithTheThirdLongerArray },
        { "arrays/profile/shrinks-at-the-end-of-a-window"_s, testProfileShrinksAtTheEndOfAWindow },
        { "arrays/profile/shape-only-rises"_s, testProfileShapeOnlyRises },
        { "arrays/profile/ignores-array-storage"_s, testProfileIgnoresArrayStorage },
        { "arrays/profile/keeps-count"_s, testProfileKeepsCount },
        { "arrays/profile/review-interval"_s, testProfileReviewInterval },
        { "arrays/profile/review-interval-grows-while-nothing-changes"_s, testProfileReviewIntervalGrowsWhileNothingChanges },
        { "arrays/reserve/makes-room-once"_s, testReserveMakesRoomOnce },
        { "arrays/reserve/leaves-small-steps-to-usual-growth"_s, testReserveLeavesSmallStepsToUsualGrowth },
        { "arrays/reserve/declines"_s, testReserveDeclines },
    };
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
