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

// What the closed accesses by name found last. A place is found by the StructureID of the receiver and the number of the name, and holds both in one word, which is zero in an empty place.
// The stubs fill the places and look into them. The run time only clears them: nothing leads from a Structure or a name to its places.
struct AccessTable {
    static constexpr unsigned numberOfPlacesShift = 12;
    static constexpr uint32_t numberOfPlaces = 1u << numberOfPlacesShift;
    static constexpr uint32_t indexMask = numberOfPlaces - 1;
    // No two Structures begin within as many bytes: StructureRowWords::granuleShift.
    static constexpr unsigned structureIDShift = 6;
    static constexpr unsigned nameShiftInKey = 32;
    // A Structure is a cell, so these bits of its StructureID are zero. The key of a call has the number of arguments there.
    static constexpr uint32_t lowBitsOfStructureID = 15;
    // The key of what a read gives that has found the slot of a field empty has this one, and lies where the index differs in the other: the Structure and the name may have an answer of their own beside it.
    static constexpr uint32_t keyBitOfFieldLeftOut = 1;
    static constexpr uint32_t indexBitOfFieldLeftOut = numberOfPlaces / 2;

    static constexpr uint64_t keyFor(uint32_t structureID, uint32_t name) { return structureID | static_cast<uint64_t>(name) << nameShiftInKey; }
    static constexpr uint32_t indexFor(uint32_t structureID, uint32_t name) { return ((structureID >> structureIDShift) + name) & indexMask; }

    // An address and an offset are multiples of eight. An answer that is neither has the lowest bit, and the next two say which of the four below it is.
    static constexpr unsigned answerShift = 3;
    static constexpr uint32_t answerHasForm = 1;
    static constexpr uint32_t valueIsInOwnStorage = 2;
    static constexpr uint32_t valueIsInStorageOfHolder = 4;
    static constexpr uint32_t getterIsInOwnStorage = valueIsInOwnStorage | valueIsInStorageOfHolder;
    static constexpr unsigned wordIndexShiftBehindHolder = answerShift + 32;

    // The receiver is a compiled object, and the row of its shape has this form and operand for the name: a FieldTableEntry less the name, of any form but InlineValue.
    static constexpr uint64_t answerOfFieldTable(uint32_t formAndOperand) { return static_cast<uint64_t>(formAndOperand) << answerShift | answerHasForm; }
    // Every object of the Structure has the property as a plain value, so many words from where its butterfly points.
    static constexpr uint64_t answerInOwnStorage(int32_t wordIndex) { return static_cast<uint64_t>(static_cast<int64_t>(wordIndex)) << answerShift | valueIsInOwnStorage | answerHasForm; }
    // Every object of the Structure has an accessor of its own under the name, and its GetterSetter lies there. It is loaded at each read: a definition with another getter puts another one there and leaves the Structure.
    static constexpr uint64_t answerOfGetterInOwnStorage(int32_t wordIndex) { return answerInOwnStorage(wordIndex) | getterIsInOwnStorage; }
    // No object of the Structure has the property, and the first object above that has it, named by its narrow reference, has it like that.
    static constexpr uint64_t answerInStorageOfHolder(uint32_t holder, int8_t wordIndex) { return static_cast<uint64_t>(static_cast<int64_t>(wordIndex)) << wordIndexShiftBehindHolder | static_cast<uint64_t>(holder) << answerShift | valueIsInStorageOfHolder | answerHasForm; }

    // Two halves with a key each: a Structure and a name are in at most one of them.
    struct Read {
        static constexpr unsigned sizeShift = 5;

        // Every object of the Structure that has the property has it as a plain value this many bytes into its cell. A word of zero there says that this object does not.
        // It holds for as long as the Structure lives.
        uint64_t keyOfOwnValue;
        uint64_t offsetInCell;
        // An address: no object of the Structure has the property, and the word there holds what a read gives. It is a word of the cell of the first object above that has the property, or AccessTableWords::undefinedValue where none has.
        // Or one of the three forms. What speaks of the objects above holds until an object of some chain changes, the rest for as long as the Structure lives. They are ended together.
        // Under a key with keyBitOfFieldLeftOut: the objects of the Structure have a field of the name, and this is said of those that leave it out and have every field in its slot. An address or answerInStorageOfHolder().
        uint64_t keyOfOtherAnswer;
        uint64_t otherAnswer;
    };

    // A store to an object of the Structure that has the property, unless the word at offsetOfOrderWord has one of the bits.
    // It holds for as long as the Structure lives and nobody watches it for replacements.
    struct Store {
        static constexpr unsigned sizeShift = 4;

        uint64_t key;
        // How many bytes into the cell the value goes, where a word of zero says that this object lacks the property. Or the low half of answerInOwnStorage() or answerOfFieldTable(), the latter for a field that is stored without a test of its type.
        uint32_t answer;
        uint32_t orderWordBitsThatForbid;
    };

    // As an address in a Read. While the word there holds the callee, a call with the number of arguments in the key enters its code with nothing left to ask or to fill.
    struct Call {
        static constexpr unsigned sizeShift = 5;

        uint64_t key;
        const EncodedJSValue* slotAbove;
        JSCell* callee;
        // As the callee has it. These pages stay writable, so they hold no address of code: toCodeAddress() makes one of the low half, within the code of the image.
        uint64_t closureWord;
    };

    // Each part begins at a multiple of its size: on ARM64 a stub puts the index into the address of the part.
    static constexpr size_t alignment = numberOfPlaces << Read::sizeShift;
    static constexpr uint32_t maxKeysListed = 512;

    std::array<Read, numberOfPlaces> reads;
    std::array<Call, numberOfPlaces> calls;
    std::array<Store, numberOfPlaces> stores;
    // Read::keyOfOtherAnswer and Call::key of the places that were filled with what depends on chains, so that a change costs by what was filed since the last one and not by the size of the table.
    // `static count = 0` of a class that others extend is counted up again and again, and each time is a change on a chain.
    std::array<uint64_t*, maxKeysListed> keysThatDependOnChains;
};

// In PropertyCacheWords, as `accessTable`. The table is pages of the instance that are zero when they are handed out.
// The stubs do not ask whether there is a table: publish() comes before the first compiled code, in every instance that runs a stub.
struct AccessTableWords {
    AccessTable::Read* reads;
    AccessTable::Store* stores;
    AccessTable::Call* calls;
    uint64_t** keysThatDependOnChains;
    EncodedJSValue undefinedValue;
    // Counted up by the stub that fills such a place. Above AccessTable::maxKeysListed: some are not listed.
    uint32_t numberOfKeysThatDependOnChains;
    // Set by the stub that fills such a place, so that whoever begins to watch finds nothing to clear while nothing was filed since the last one.
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

    // At each collection: a StructureID, a holder and a callee are named here and not held. And wherever the tables in front are cleared as a whole.
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

    // Of every instance, wherever a slot that depends on a chain may be dropped or moved: VMPropertyCaches::didChangeObjectOnSomeChain(), and both arms of ClassPrototype::createMembers(), which tells the ChainDependents alone.
    // The latter takes the members out of the holders whose words the places name.
    // A place that is listed may hold something else by now, which is then found again.
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

    // VMPropertyCaches::didStartWatchingReplacements().
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
