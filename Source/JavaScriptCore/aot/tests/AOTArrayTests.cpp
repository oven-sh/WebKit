/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTArrayChains.h"
#include "AOTArrayStorageForms.h"
#include "AOTBuiltinTable.h"
#include "AOTConvention.h"
#include "AOTIRTestSupport.h"
#include "AOTRealmTestSupport.h"
#include "AOTRuntimeCounters.h"
#include "AOTVector.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

static Type int32Type() { return Type::integer(Range::int32()); }
static Type int53Type() { return Type::integer(Range::exact()); }
static Type fractionType() { return Type::fromPrimitives(Primitive::FractionalNumber); }
static Type wholeNumberType() { return Type::fromPrimitives(Primitive::WholeNumber); }
static Type numberWithoutNaNType() { return Type::fromPrimitives(Primitive::Integer | Primitive::FractionalNumber); }
static Type stringType() { return Type::fromPrimitives(Primitive::String); }
static Type undefinedType() { return Type::fromPrimitives(Primitive::Undefined); }

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
    AOT_TEST_CHECK(arrayStorageHolding(Type::never()) == ArrayStorage::Other);
    AOT_TEST_CHECK(arrayStorageHolding(Type::fromPrimitives(Primitive::Absent)) == ArrayStorage::Other);
    AOT_TEST_CHECK(arrayStorageHolding(int32Type()) == ArrayStorage::Int32);
    AOT_TEST_CHECK(arrayStorageHolding(Type::integer(Range::one(7))) == ArrayStorage::Int32);
    AOT_TEST_CHECK(arrayStorageHolding(int32Type().withPrimitives(Primitive::Absent)) == ArrayStorage::Int32);
    AOT_TEST_CHECK(arrayStorageHolding(int53Type()) == ArrayStorage::Double);
    AOT_TEST_CHECK(arrayStorageHolding(Type::integer({ 0, static_cast<int64_t>(INT32_MAX) + 1 })) == ArrayStorage::Double);
    AOT_TEST_CHECK(arrayStorageHolding(fractionType()) == ArrayStorage::Double);
    AOT_TEST_CHECK(arrayStorageHolding(numberWithoutNaNType()) == ArrayStorage::Double);
    AOT_TEST_CHECK(arrayStorageHolding(fractionType().withPrimitives(Primitive::Absent)) == ArrayStorage::Double);
    AOT_TEST_CHECK(arrayStorageHolding(stringType()) == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageHolding(int32Type().withPrimitives(Primitive::String)) == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageHolding(int32Type().withPrimitives(Primitive::Boolean)) == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageHolding(Type::object()) == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageHolding(Type::anything()) == ArrayStorage::Contiguous);
}

static void testFormsAtAllocation()
{
    AOT_TEST_CHECK(arrayStorageAtAllocation(numberWithoutNaNType().withPrimitives(Primitive::Undefined)) == ArrayStorage::Double);
    AOT_TEST_CHECK(arrayStorageAtAllocation(fractionType().withPrimitives(Primitive::Null)) == ArrayStorage::Double);
    AOT_TEST_CHECK(arrayStorageAtAllocation(int32Type().withPrimitives(Primitive::UndefinedOrNull | Primitive::Absent)) == ArrayStorage::Int32);
    AOT_TEST_CHECK(arrayStorageAtAllocation(int32Type()) == ArrayStorage::Int32);
    AOT_TEST_CHECK(arrayStorageAtAllocation(stringType().withPrimitives(Primitive::Undefined)) == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageAtAllocation(Type::anything()) == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageAtAllocation(Type::never()) == ArrayStorage::Contiguous);
    AOT_TEST_CHECK(arrayStorageAtAllocation(undefinedType()) == ArrayStorage::Contiguous);
}

static void testFormsStoreWithoutConversion()
{
    AOT_TEST_CHECK(canStoreWithoutConversion(ArrayStorage::Int32, int32Type()));
    AOT_TEST_CHECK(!canStoreWithoutConversion(ArrayStorage::Int32, int53Type()));
    AOT_TEST_CHECK(!canStoreWithoutConversion(ArrayStorage::Int32, fractionType()));
    AOT_TEST_CHECK(!canStoreWithoutConversion(ArrayStorage::Int32, int32Type().withPrimitives(Primitive::Undefined)));

    AOT_TEST_CHECK(canStoreWithoutConversion(ArrayStorage::Double, int32Type()));
    AOT_TEST_CHECK(canStoreWithoutConversion(ArrayStorage::Double, int53Type()));
    AOT_TEST_CHECK(canStoreWithoutConversion(ArrayStorage::Double, fractionType()));
    AOT_TEST_CHECK(!canStoreWithoutConversion(ArrayStorage::Double, wholeNumberType()));
    AOT_TEST_CHECK(!canStoreWithoutConversion(ArrayStorage::Double, Type::fromPrimitives(Primitive::Number)));
    AOT_TEST_CHECK(!canStoreWithoutConversion(ArrayStorage::Double, fractionType().withPrimitives(Primitive::Undefined)));
    AOT_TEST_CHECK(!canStoreWithoutConversion(ArrayStorage::Double, stringType()));

    AOT_TEST_CHECK(canStoreWithoutConversion(ArrayStorage::Contiguous, int32Type()));
    AOT_TEST_CHECK(canStoreWithoutConversion(ArrayStorage::Contiguous, wholeNumberType()));
    AOT_TEST_CHECK(canStoreWithoutConversion(ArrayStorage::Contiguous, Type::anything()));

    AOT_TEST_CHECK(!canStoreWithoutConversion(ArrayStorage::Other, int32Type()));
    AOT_TEST_CHECK(!canStoreWithoutConversion(ArrayStorage::Other, Type::anything()));
}

static void testFormsElementReadTypes()
{
    AOT_TEST_CHECK(typeOfElementRead(ArrayStorage::Int32) == int32Type().withPrimitives(Primitive::Undefined));
    AOT_TEST_CHECK(typeOfElementRead(ArrayStorage::Double) == Type::fromPrimitives(Primitive::Number | Primitive::Undefined));
    AOT_TEST_CHECK(typeOfElementRead(ArrayStorage::Contiguous) == Type::anything());
    AOT_TEST_CHECK(typeOfElementRead(ArrayStorage::Other) == Type::anything());
}

static constexpr ChainEndKind chainEnds[] = {
    ChainEndKind::Array, ChainEndKind::Length, ChainEndKind::ForEach, ChainEndKind::Reduce, ChainEndKind::Some,
    ChainEndKind::Every, ChainEndKind::Find, ChainEndKind::FindIndex, ChainEndKind::Join, ChainEndKind::FlatMap,
};

static Vector<std::array<ChainLinkKind, ChainShape::maxNumberOfLinks>> everyListOfLinks()
{
    Vector<std::array<ChainLinkKind, ChainShape::maxNumberOfLinks>> lists;
    for (unsigned count = 0; count <= ChainShape::maxNumberOfLinks; ++count) {
        for (unsigned choice = 0; choice < 1u << count; ++choice) {
            std::array<ChainLinkKind, ChainShape::maxNumberOfLinks> links { };
            for (unsigned index = 0; index < count; ++index)
                links[index] = choice >> index & 1 ? ChainLinkKind::Filter : ChainLinkKind::Map;
            lists.append(links);
        }
    }
    return lists;
}

static void testChainShapeRoundTrip()
{
    auto lists = everyListOfLinks();
    AOT_TEST_CHECK(lists.size() == 15);

    HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> words;
    for (const auto& links : lists) {
        for (ChainEndKind end : chainEnds) {
            for (bool hasInitialValue : { false, true }) {
                ChainShape shape { links, end, hasInitialValue };
                uint32_t word = shape.encode();
                AOT_TEST_CHECK(ChainShape::decode(word) == shape);
                AOT_TEST_CHECK(ChainShape::decode(word).numberOfLinks() == shape.numberOfLinks());
                AOT_TEST_CHECK(word < 1u << 11);
                AOT_TEST_CHECK((word & 0xf) == static_cast<uint32_t>(end));
                AOT_TEST_CHECK((word >> 4 & 3) == static_cast<uint32_t>(links[0]) && (word >> 6 & 3) == static_cast<uint32_t>(links[1]) && (word >> 8 & 3) == static_cast<uint32_t>(links[2]));
                AOT_TEST_CHECK((word >> 10 & 1) == hasInitialValue);
                AOT_TEST_CHECK(words.add(word).isNewEntry);
            }
        }
    }
    AOT_TEST_CHECK(words.size() == 15 * std::size(chainEnds) * 2);
    AOT_TEST_CHECK(!ChainShape { }.encode() && !ChainShape { }.numberOfLinks());
}

static void testChainEndOperands()
{
    for (ChainEndKind end : chainEnds) {
        bool takesNoCallback = end == ChainEndKind::Array || end == ChainEndKind::Length || end == ChainEndKind::Join;
        AOT_TEST_CHECK(endTakesCallback(end) == !takesNoCallback);
        AOT_TEST_CHECK(endTakesOperand({ { }, end, false }) == (end == ChainEndKind::Join));
        AOT_TEST_CHECK(endTakesOperand({ { }, end, true }) == (end == ChainEndKind::Join || end == ChainEndKind::Reduce));
    }
}

static void testChainMethodRows()
{
    std::span<const ChainMethod> methods = chainMethods();
    AOT_TEST_CHECK(methods.size() == 12);

    unsigned numberOfRanges = 0;
    unsigned numberOfLinks = 0;
    for (const ChainMethod& method : methods) {
        AOT_TEST_CHECK(findChainMethod(method.builtin) == &method);
        switch (method.role) {
        case ChainMethodRole::Range:
            ++numberOfRanges;
            AOT_TEST_CHECK(method.builtin == Builtins::ArrayPrototypeSlice && method.link == ChainLinkKind::None && !method.takesCallback && !method.stopsEarly);
            break;
        case ChainMethodRole::Link:
            ++numberOfLinks;
            AOT_TEST_CHECK(method.link != ChainLinkKind::None && method.end == ChainEndKind::Array && method.takesCallback && !method.stopsEarly);
            break;
        case ChainMethodRole::End:
            AOT_TEST_CHECK(method.link == ChainLinkKind::None && method.end != ChainEndKind::Array && method.takesCallback == endTakesCallback(method.end));
            break;
        }
        bool leavesEarly = method.end == ChainEndKind::Some || method.end == ChainEndKind::Every || method.end == ChainEndKind::Find || method.end == ChainEndKind::FindIndex;
        AOT_TEST_CHECK(method.stopsEarly == leavesEarly);
    }
    AOT_TEST_CHECK(numberOfRanges == 1 && numberOfLinks == 2);

    for (BuiltinID other : std::initializer_list<BuiltinID> { Builtins::ArrayPrototypePush, Builtins::ArrayPrototypeReduceRight, Builtins::ArrayPrototype, Builtins::GlobalThis, BuiltinID() })
        AOT_TEST_CHECK(!findChainMethod(other));
}

static void testChainMethodsAgreeWithTable()
{
    const BuiltinTable& table = BuiltinTable::singleton();
    for (const ChainMethod& method : chainMethods()) {
        AOT_TEST_CHECK(table.contains(method.builtin) && table.canonicalID(method.builtin) == method.builtin);
        std::span<const BuiltinOverload> overloads = table.overloadsOf(method.builtin);
        AOT_TEST_CHECK(!overloads.empty());
        for (const BuiltinOverload& overload : overloads) {
            if (!method.takesCallback) {
                AOT_TEST_CHECK(overload.flow.callbacks.empty());
                continue;
            }
            AOT_TEST_CHECK(overload.flow.callbacks.size() == 1);
            AOT_TEST_CHECK(overload.flow.callbacks[0].times == BuiltinCallbackTimes::PerElement);
            AOT_TEST_CHECK(overload.flow.callbacks[0].callee == BuiltinOperand::argument(0));
        }
    }
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
    uint64_t limit = runtimeLimitValue(realms.vm(), LimitID::MaxReservedArrayCapacity);
    JSArray* array = createArray(realms, ArrayWithInt32, 4);
    Butterfly* butterfly = array->butterfly();
    for (uint64_t additional : { limit + 1, static_cast<uint64_t>(-1), static_cast<uint64_t>(MAX_STORAGE_VECTOR_LENGTH) })
        reserveArrayCapacity(realms.vm(), array, additional);
    AOT_TEST_CHECK(array->butterfly() == butterfly && array->length() == 4);

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
        { "arrays/chains/shape-round-trip"_s, testChainShapeRoundTrip },
        { "arrays/chains/end-operands"_s, testChainEndOperands },
        { "arrays/chains/method-rows"_s, testChainMethodRows },
        { "arrays/chains/methods-agree-with-table"_s, testChainMethodsAgreeWithTable },
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
