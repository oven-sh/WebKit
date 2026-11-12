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

static void testProfileIgnoresLongMinority()
{
    Realms realms;
    ArrayAllocationProfile fewLong { };
    for (unsigned count = 0; count < 10 * window; ++count)
        review(realms, fewLong, ArrayWithContiguous, count % 3 ? 3 : 20);
    AOT_TEST_CHECK(fewLong.capacity() <= defaultCapacity);

    ArrayAllocationProfile manyLong { };
    for (unsigned count = 0; count < 10 * window; ++count)
        review(realms, manyLong, ArrayWithContiguous, count % 3 ? 20 : 3);
    AOT_TEST_CHECK(manyLong.capacity() == 21);
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

static void testProfileSamplesWalkThroughTheResiduesOfTheInterval()
{
    ArrayAllocationProfile profile { };
    profile.set(3, ContiguousShape);
    for (unsigned reviews = 0; reviews < window; ++reviews) {
        profile.reviews = reviews;
        profile.startInterval();
        uint32_t allocations = 0;
        do {
            profile.word -= ArrayAllocationProfile::oneAllocation;
            ++allocations;
        } while (!profile.isDueForReview());
        AOT_TEST_CHECK(allocations == ArrayAllocationProfile::shortestInterval - reviews);
    }
}

static void testProfileLearnsNothingFromListsThatAreStillEmpty()
{
    Realms realms;
    ArrayAllocationProfile allEmpty { };
    review(realms, allEmpty, ArrayWithContiguous, 0, window);
    AOT_TEST_CHECK(!allEmpty.capacity() && hasNewWindow(allEmpty));

    ArrayAllocationProfile oneFilled { };
    review(realms, oneFilled, ArrayWithContiguous, 4);
    review(realms, oneFilled, ArrayWithContiguous, 0, window - 1);
    AOT_TEST_CHECK(!oneFilled.capacity() && hasNewWindow(oneFilled));

    ArrayAllocationProfile someShort { };
    review(realms, someShort, ArrayWithContiguous, 2);
    review(realms, someShort, ArrayWithContiguous, 0, window - 1);
    static_assert(defaultCapacity == 3);
    AOT_TEST_CHECK(!someShort.capacity() && hasNewWindow(someShort));

    ArrayAllocationProfile allShort { };
    review(realms, allShort, ArrayWithContiguous, 1, window);
    AOT_TEST_CHECK(allShort.capacity() == 1 && hasNewWindow(allShort));
}

static void testProfileReviewIntervalGrowsWhileNothingChanges()
{
    Realms realms;
    constexpr uint32_t shortest = ArrayAllocationProfile::shortestInterval;
    constexpr uint32_t longest = shortest << ArrayAllocationProfile::maxIntervalDoublings;
    ArrayAllocationProfile profile { };
    review(realms, profile, ArrayWithDouble, 1, window);
    AOT_TEST_CHECK(profile.capacity() == 1 && profile.interval() == shortest);
    for (uint32_t expected = 2 * shortest; expected <= longest; expected *= 2) {
        review(realms, profile, ArrayWithDouble, 1, window - 1);
        AOT_TEST_CHECK(profile.interval() == expected / 2);
        review(realms, profile, ArrayWithDouble, 1);
        AOT_TEST_CHECK(profile.interval() == expected && hasNewWindow(profile));
    }
    review(realms, profile, ArrayWithDouble, 1, 2 * window);
    AOT_TEST_CHECK(profile.interval() == longest && profile.capacity() == 1);

    for (uint32_t expected = longest / 2; expected >= shortest; expected /= 2) {
        reviewArrayAllocationProfile(profile, nullptr);
        AOT_TEST_CHECK(profile.interval() == expected && profile.capacity() == 1);
    }
    reviewArrayAllocationProfile(profile, nullptr);
    AOT_TEST_CHECK(profile.interval() == shortest);
    review(realms, profile, ArrayWithDouble, 1, ArrayAllocationProfile::maxIntervalDoublings * window);
    AOT_TEST_CHECK(profile.interval() == longest);

    review(realms, profile, ArrayWithDouble, 2);
    AOT_TEST_CHECK(profile.interval() == shortest && profile.capacity() == 1);
    review(realms, profile, ArrayWithDouble, 1, 2 * window);
    AOT_TEST_CHECK(profile.interval() > shortest);
    review(realms, profile, ArrayWithContiguous, 1);
    AOT_TEST_CHECK(profile.interval() == shortest && profile.shape() == ContiguousShape);
    review(realms, profile, ArrayWithContiguous, 1, 2 * window);
    AOT_TEST_CHECK(profile.interval() > shortest);
    review(realms, profile, ArrayWithContiguous, 1, 2 * window);
    AOT_TEST_CHECK(profile.capacity() == 1);
    reviewArrayAllocationProfile(profile, nullptr);
    AOT_TEST_CHECK(profile.capacity() == 1);
}

static JSArray* createListForAnyValue(Realms& realms, std::initializer_list<JSValue> values)
{
    JSArray* array = createArray(realms, ArrayWithContiguous, values.size());
    unsigned position = 0;
    for (JSValue value : values)
        array->butterfly()->contiguous().at(array, position++).setWithoutWriteBarrier(value);
    return array;
}

static void testProfileReadsContentsOfFirstListThatTakesAnyValue()
{
    Realms realms;
    auto learnedFrom = [&](std::initializer_list<JSValue> values) {
        ArrayAllocationProfile profile { };
        reviewArrayAllocationProfile(profile, createListForAnyValue(realms, values), true);
        return profile.shape();
    };
    AOT_TEST_CHECK(learnedFrom({ }) == Int32Shape);
    AOT_TEST_CHECK(learnedFrom({ jsNumber(1), jsNumber(-2) }) == Int32Shape);
    AOT_TEST_CHECK(learnedFrom({ jsNumber(1), JSValue(), jsNumber(2) }) == Int32Shape);
    AOT_TEST_CHECK(learnedFrom({ jsNumber(1), jsNumber(2.5) }) == DoubleShape);
    AOT_TEST_CHECK(learnedFrom({ jsNumber(-0.0) }) == DoubleShape);
    AOT_TEST_CHECK(learnedFrom({ jsNumber(1.5), jsNaN() }) == ContiguousShape);
    AOT_TEST_CHECK(learnedFrom({ jsNumber(1), jsUndefined() }) == ContiguousShape);
    AOT_TEST_CHECK(learnedFrom({ jsNull() }) == ContiguousShape);

    ArrayAllocationProfile notAsked { };
    reviewArrayAllocationProfile(notAsked, createListForAnyValue(realms, { jsNumber(1) }));
    AOT_TEST_CHECK(notAsked.shape() == ContiguousShape);

    ArrayAllocationProfile hasLearned { };
    review(realms, hasLearned, ArrayWithInt32, 2);
    reviewArrayAllocationProfile(hasLearned, createListForAnyValue(realms, { jsNumber(1) }), true);
    AOT_TEST_CHECK(hasLearned.shape() == ContiguousShape);
}

std::span<const NativeTest> arrayTests()
{
    static constexpr NativeTest tests[] = {
        { "arrays/forms/table"_s, testFormsTable },
        { "arrays/forms/store-without-conversion"_s, testFormsStoreWithoutConversion },
        { "arrays/profile/grows-with-the-third-longer-array"_s, testProfileGrowsWithTheThirdLongerArray },
        { "arrays/profile/ignores-long-minority"_s, testProfileIgnoresLongMinority },
        { "arrays/profile/review-interval"_s, testProfileReviewInterval },
        { "arrays/profile/samples-walk-through-the-residues-of-the-interval"_s, testProfileSamplesWalkThroughTheResiduesOfTheInterval },
        { "arrays/profile/learns-nothing-from-lists-that-are-still-empty"_s, testProfileLearnsNothingFromListsThatAreStillEmpty },
        { "arrays/profile/review-interval-grows-while-nothing-changes"_s, testProfileReviewIntervalGrowsWhileNothingChanges },
        { "arrays/profile/reads-the-contents-of-the-first-list-that-takes-any-value"_s, testProfileReadsContentsOfFirstListThatTakesAnyValue },
    };
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
