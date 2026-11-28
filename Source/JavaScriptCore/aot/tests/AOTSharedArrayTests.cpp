/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTNativeTest.h"
#include "AOTSharedArray.h"
#include <array>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

using Numbers = SharedArray<uint32_t, 0>;

static Numbers numbersCountedFrom(uint32_t first, uint32_t size)
{
    Numbers numbers;
    numbers.fill(0, size);
    for (uint32_t index = 0; index < size; ++index)
        numbers.mutableAt(index) = first + index;
    return numbers;
}

static void sharedArrayOfZerosIsWrittenWithoutTouchingItsCopies()
{
    for (uint32_t size : { 1u, 31u, 32u, 33u, 1024u, 1025u, 40000u }) {
        Numbers zeros;
        zeros.fill(0, size);
        Numbers written = zeros;
        written.mutableAt(size - 1) = 7;
        written.mutableAt(size / 2) = 5;
        written.append(9);
        Numbers others;
        others.fill(0, size);
        AOT_TEST_CHECK(zeros.size() == size && others.size() == size && written.size() == size + 1 && written[size] == 9);
        for (uint32_t index = 0; index < size; ++index) {
            AOT_TEST_CHECK(!zeros[index] && !others[index]);
            AOT_TEST_CHECK(written[index] == (index == size / 2 ? 5u : index == size - 1 ? 7u : 0u));
        }
    }
}

static void sharedArrayTakesStretchesOfAnother()
{
    uint32_t state = 12345;
    auto next = [&](uint32_t bound) {
        state = state * 1664525 + 1013904223;
        return (state >> 8) % bound;
    };
    for (auto [size, sizeOfOther] : { std::pair { 20u, 20u }, std::pair { 40u, 33u }, std::pair { 1100u, 1100u }, std::pair { 40000u, 900u }, std::pair { 40000u, 40000u }, std::pair { 1100u, 40000u } }) {
        Numbers other = numbersCountedFrom(1000000, sizeOfOther);
        Numbers taker = numbersCountedFrom(1, size);
        Vector<uint32_t> expected;
        for (uint32_t index = 0; index < size; ++index)
            expected.append(1 + index);
        for (unsigned round = 0; round < 60; ++round) {
            uint32_t most = std::min(size, sizeOfOther);
            uint32_t begin = round ? next(most + 1) : 0;
            uint32_t end = round ? begin + next(most - begin + 1) : most;
            Numbers before = taker;
            taker.takeStretchOf(other, begin, end);
            for (uint32_t index = 0; index < size; ++index)
                AOT_TEST_CHECK(before[index] == expected[index]);
            for (uint32_t index = begin; index < end; ++index)
                expected[index] = 1000000 + index;
            uint32_t written = next(size);
            taker.mutableAt(written) = expected[written] = 5000000 + round;
            for (uint32_t index = 0; index < size; ++index)
                AOT_TEST_CHECK(taker[index] == expected[index]);
            AOT_TEST_CHECK(written >= sizeOfOther || other[written] == 1000000 + written);
        }
    }
}

static void sharedArraySaysWhereTwoHoldTheSame()
{
    for (uint32_t size : { 1u, 32u, 33u, 1100u, 40000u }) {
        Numbers first = numbersCountedFrom(1, size);
        Numbers madeApart = numbersCountedFrom(1, size);
        Numbers shorter = numbersCountedFrom(1, (size + 1) / 2);
        AOT_TEST_CHECK(first.isSameIn(first, 0, size) && first.isSameIn(madeApart, 0, size) && first.isSameIn(shorter, 0, shorter.size()) && shorter.isSameIn(first, 0, shorter.size()));
        for (uint32_t changed : { 0u, size / 3, size / 2, size - 1 }) {
            for (const Numbers* from : { &first, &madeApart }) {
                Numbers second = *from;
                second.mutableAt(changed) = 0;
                AOT_TEST_CHECK(first.isSameIn(second, 0, changed) && first.isSameIn(second, changed + 1, size) && first.isSameIn(second, changed, changed));
                AOT_TEST_CHECK(!first.isSameIn(second, changed, changed + 1) && !first.isSameIn(second, 0, size) && !second.isSameIn(first, 0, changed + 1));
            }
        }
    }
}

static void bitVectorAgreesWithWTF()
{
    uint32_t state = 4711;
    auto next = [&](uint32_t bound) {
        state = state * 1664525 + 1013904223;
        return (state >> 8) % bound;
    };
    constexpr uint32_t numberOfVectors = 3;
    std::array<BitVector, numberOfVectors> vectors;
    std::array<WTF::BitVector, numberOfVectors> expected;
    for (uint32_t step = 0; step < 4000; ++step) {
        uint32_t one = next(numberOfVectors);
        uint32_t other = next(numberOfVectors);
        size_t bit = next(1u << next(11));
        switch (next(14)) {
        case 0:
        case 1:
        case 2:
            AOT_TEST_CHECK(vectors[one].set(bit) == expected[one].set(bit));
            break;
        case 3:
            AOT_TEST_CHECK(vectors[one].add(bit) == expected[one].add(bit));
            break;
        case 4:
            AOT_TEST_CHECK(vectors[one].clear(bit) == expected[one].clear(bit));
            break;
        case 5:
            vectors[one].merge(vectors[other]);
            expected[one].merge(expected[other]);
            break;
        case 6:
            vectors[one].filter(vectors[other]);
            expected[one].filter(expected[other]);
            break;
        case 7:
            vectors[one].exclude(BitVector(vectors[other]));
            expected[one].exclude(WTF::BitVector(expected[other]));
            break;
        case 8:
            vectors[one] = vectors[other];
            expected[one] = expected[other];
            break;
        case 9:
            vectors[one] = std::exchange(vectors[other], BitVector());
            expected[one] = std::exchange(expected[other], WTF::BitVector());
            break;
        case 10:
            vectors[one].ensureSize(bit);
            expected[one].ensureSize(bit);
            break;
        case 11:
            vectors[one].clearAll();
            expected[one].clearAll();
            break;
        default:
            vectors[one] = BitVector(bit + 1);
            expected[one] = WTF::BitVector(bit + 1);
            AOT_TEST_CHECK(!vectors[one].quickSet(bit) && vectors[one].quickGet(bit));
            expected[one].quickSet(bit);
            break;
        }
        for (uint32_t index = 0; index < numberOfVectors; ++index) {
            AOT_TEST_CHECK(vectors[index].bitCount() == expected[index].bitCount() && vectors[index].isEmpty() == expected[index].isEmpty());
            AOT_TEST_CHECK(vectors[index].get(bit) == expected[index].get(bit) && vectors[index].contains(bit + 64) == expected[index].contains(bit + 64));
            AOT_TEST_CHECK((vectors[index] == vectors[other]) == (expected[index] == expected[other]));
            Vector<size_t> members;
            for (size_t member : vectors[index])
                members.append(member);
            size_t place = 0;
            for (size_t member : expected[index])
                AOT_TEST_CHECK(place < members.size() && members[place++] == member);
            AOT_TEST_CHECK(place == members.size());
            place = 0;
            vectors[index].forEachSetBit([&](size_t member) {
                AOT_TEST_CHECK(members[place++] == member);
            });
            AOT_TEST_CHECK(place == members.size());
        }
    }
}

#define FOR_EACH_AOT_SHARED_ARRAY_TEST(v) \
    v(sharedArrayOfZerosIsWrittenWithoutTouchingItsCopies) v(sharedArrayTakesStretchesOfAnother) v(sharedArraySaysWhereTwoHoldTheSame) \
    v(bitVectorAgreesWithWTF)

std::span<const NativeTest> sharedArrayTests()
{
#define AOT_SHARED_ARRAY_TEST_ENTRY(name) { #name ""_s, name },
    static constexpr NativeTest tests[] = { FOR_EACH_AOT_SHARED_ARRAY_TEST(AOT_SHARED_ARRAY_TEST_ENTRY) };
#undef AOT_SHARED_ARRAY_TEST_ENTRY
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
