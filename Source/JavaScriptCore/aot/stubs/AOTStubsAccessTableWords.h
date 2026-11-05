/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include <array>
#include <cstring>
#include <type_traits>
#include <utility>
#include <wtf/MathExtras.h>

namespace JSC {

class JSCell;

namespace AOT {

struct AccessTable {
    static constexpr unsigned numberOfPlacesShift = 12;
    static constexpr uint32_t numberOfPlaces = 1u << numberOfPlacesShift;
    static constexpr uint32_t indexMask = numberOfPlaces - 1;
    static constexpr unsigned structureIDShift = 6;
    static constexpr unsigned nameShiftInKey = 32;
    static constexpr uint32_t lowBitsOfStructureID = 15;
    static constexpr uint32_t keyBitOfFieldLeftOut = 1;
    static constexpr uint32_t indexBitOfFieldLeftOut = numberOfPlaces / 2;

    static constexpr uint64_t keyFor(uint32_t structureID, uint32_t name) { return structureID | static_cast<uint64_t>(name) << nameShiftInKey; }
    static constexpr uint32_t indexFor(uint32_t structureID, uint32_t name) { return ((structureID >> structureIDShift) + name) & indexMask; }

    static constexpr unsigned answerShift = 3;
    static constexpr uint32_t answerHasForm = 1;
    static constexpr uint32_t valueIsInOwnStorage = 2;
    static constexpr uint32_t valueIsInStorageOfHolder = 4;
    static constexpr uint32_t getterIsInOwnStorage = valueIsInOwnStorage | valueIsInStorageOfHolder;
    static constexpr unsigned wordIndexShiftBehindHolder = answerShift + 32;

    static constexpr uint64_t answerOfFieldTable(uint32_t formAndOperand) { return static_cast<uint64_t>(formAndOperand) << answerShift | answerHasForm; }
    static constexpr uint64_t answerInOwnStorage(int32_t wordIndex) { return static_cast<uint64_t>(static_cast<int64_t>(wordIndex)) << answerShift | valueIsInOwnStorage | answerHasForm; }
    static constexpr uint64_t answerOfGetterInOwnStorage(int32_t wordIndex) { return answerInOwnStorage(wordIndex) | getterIsInOwnStorage; }
    static constexpr uint64_t answerInStorageOfHolder(uint32_t holder, int8_t wordIndex) { return static_cast<uint64_t>(static_cast<int64_t>(wordIndex)) << wordIndexShiftBehindHolder | static_cast<uint64_t>(holder) << answerShift | valueIsInStorageOfHolder | answerHasForm; }

    struct Read {
        static constexpr unsigned sizeShift = 5;

        uint64_t keyOfOwnValue;
        uint64_t offsetInCell;
        uint64_t keyOfOtherAnswer;
        uint64_t otherAnswer;
    };

    struct Store {
        static constexpr unsigned sizeShift = 4;

        uint64_t key;
        uint32_t answer;
        uint32_t orderWordBitsThatForbid;
    };

    struct Call {
        static constexpr unsigned sizeShift = 5;

        uint64_t key;
        const EncodedJSValue* slotAbove;
        JSCell* callee;
        uint64_t closureWord;
    };

    static constexpr size_t alignment = numberOfPlaces << Read::sizeShift;
    static constexpr uint32_t maxKeysListed = 512;

    std::array<Read, numberOfPlaces> reads;
    std::array<Call, numberOfPlaces> calls;
    std::array<Store, numberOfPlaces> stores;
    std::array<uint64_t*, maxKeysListed> keysThatDependOnChains;
};

struct AccessTableWords {
    AccessTable::Read* reads;
    AccessTable::Store* stores;
    AccessTable::Call* calls;
    uint64_t** keysThatDependOnChains;
    EncodedJSValue undefinedValue;
    uint32_t numberOfKeysThatDependOnChains;
    uint8_t holdsStoresToUnwatchedStructures;

    void publish(AccessTable& table)
    {
        RELEASE_ASSERT(!(reinterpret_cast<uintptr_t>(&table) % AccessTable::alignment));
        reads = table.reads.data();
        stores = table.stores.data();
        calls = table.calls.data();
        keysThatDependOnChains = table.keysThatDependOnChains.data();
        undefinedValue = JSValue::ValueUndefined;
    }

    void forgetEverything()
    {
        if (!reads)
            return;
        memset(static_cast<void*>(reads), 0, sizeof(AccessTable::reads));
        memset(static_cast<void*>(stores), 0, sizeof(AccessTable::stores));
        memset(static_cast<void*>(calls), 0, sizeof(AccessTable::calls));
        numberOfKeysThatDependOnChains = 0;
        holdsStoresToUnwatchedStructures = 0;
    }

    void didChangeObjectOnSomeChain()
    {
        uint32_t numberOfKeys = std::exchange(numberOfKeysThatDependOnChains, 0);
        if (numberOfKeys <= AccessTable::maxKeysListed) {
            for (uint32_t index = 0; index < numberOfKeys; ++index)
                *keysThatDependOnChains[index] = 0;
            return;
        }
        for (uint32_t index = 0; index < AccessTable::numberOfPlaces; ++index)
            reads[index].keyOfOtherAnswer = 0;
        memset(static_cast<void*>(calls), 0, sizeof(AccessTable::calls));
    }

    void didStartWatchingReplacements()
    {
        if (std::exchange(holdsStoresToUnwatchedStructures, 0))
            memset(static_cast<void*>(stores), 0, sizeof(AccessTable::stores));
    }
};

static_assert(sizeof(AccessTable::Read) == 1u << AccessTable::Read::sizeShift && sizeof(AccessTable::Store) == 1u << AccessTable::Store::sizeShift && sizeof(AccessTable::Call) == 1u << AccessTable::Call::sizeShift);
static_assert(OBJECT_OFFSETOF(AccessTable::Read, offsetInCell) == OBJECT_OFFSETOF(AccessTable::Read, keyOfOwnValue) + sizeof(uint64_t) && OBJECT_OFFSETOF(AccessTable::Read, otherAnswer) == OBJECT_OFFSETOF(AccessTable::Read, keyOfOtherAnswer) + sizeof(uint64_t));
static_assert(OBJECT_OFFSETOF(AccessTable::Store, orderWordBitsThatForbid) == OBJECT_OFFSETOF(AccessTable::Store, answer) + sizeof(uint32_t));
static_assert(OBJECT_OFFSETOF(AccessTable::Call, slotAbove) == OBJECT_OFFSETOF(AccessTable::Call, key) + sizeof(uint64_t) && OBJECT_OFFSETOF(AccessTable::Call, closureWord) == OBJECT_OFFSETOF(AccessTable::Call, callee) + sizeof(void*));
static_assert(std::is_trivially_default_constructible_v<AccessTableWords> && std::is_trivially_copyable_v<AccessTable>);
static_assert(!OBJECT_OFFSETOF(AccessTable::Call, key) && sizeof(uint64_t*) == 1u << 3);
static_assert(!OBJECT_OFFSETOF(AccessTable, reads) && !(OBJECT_OFFSETOF(AccessTable, calls) % sizeof(AccessTable::calls)) && !(OBJECT_OFFSETOF(AccessTable, stores) % sizeof(AccessTable::stores)) && sizeof(AccessTable::reads) == AccessTable::alignment);
static_assert(AccessTable::keyFor(0x40, 1) == 0x100000040ull && AccessTable::indexFor(0x40, 1) == 2);
static_assert(!(OBJECT_OFFSETOF(AccessTableWords, undefinedValue) % (1u << AccessTable::answerShift)) && (AccessTable::answerHasForm | AccessTable::valueIsInOwnStorage | AccessTable::valueIsInStorageOfHolder) < 1u << AccessTable::answerShift);
static_assert(AccessTable::answerOfGetterInOwnStorage(-2) == 0xfffffffffffffff7ull);
static_assert(AccessTable::answerOfFieldTable(0x1234) == 0x91a1 && AccessTable::answerInOwnStorage(-2) == 0xfffffffffffffff3ull && AccessTable::answerInStorageOfHolder(0x80000002u, -3) == 0xffffffec00000015ull);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
