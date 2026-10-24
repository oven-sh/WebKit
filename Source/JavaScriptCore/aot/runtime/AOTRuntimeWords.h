/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTRuntimeRecords.h"
#include "JSCJSValue.h"
#include "PropertyOffset.h"
#include "StructureID.h"
#include <array>
#include <bit>
#include <tuple>
#include <type_traits>
#include <wtf/StdLibExtras.h>

namespace JSC {

class JSCell;
class PropertyRun;
class Structure;
template<typename> class RememberedSlot;

namespace AOT {

using RuntimeCounterID = Wire::RuntimeCounterID;
inline constexpr unsigned numberOfRuntimeCounters = numberOfEnumerators<RuntimeCounterID>;

struct ExceptionWords {
    EncodedJSValue thrownValue;
    // Of the frame of the handler that unwinding has found, from there to the jump to the handler.
    void* stackPointerOfHandler;
    // Not about exceptions: it fills what was padding, so that no other word moves.
    uint32_t predictionFailureState;
};

// What the last lookup of a bound name of the global scope found.
struct GlobalBinding {
    // Not zero: the value, which nothing has replaced since it was filed.
    EncodedJSValue constant;
    // Of the global object when the location was filed. Where script bindings are seen, compiled code compares the two as one 64-bit word.
    uint32_t structureID;
    uint32_t lexicalBindingEpoch;
    // Negative: the offset in bytes from the global object's butterfly. Positive: the address of a variable or of inline storage.
    int64_t location;
};
static_assert(sizeof(GlobalBinding) == 24);
static_assert(!OBJECT_OFFSETOF(GlobalBinding, constant) && OBJECT_OFFSETOF(GlobalBinding, structureID) == 8 && OBJECT_OFFSETOF(GlobalBinding, lexicalBindingEpoch) == 12 && OBJECT_OFFSETOF(GlobalBinding, location) == 16);

struct CounterWords {
    static constexpr ptrdiff_t offsetOfValue(RuntimeCounterID counter) { return static_cast<ptrdiff_t>(static_cast<size_t>(counter) * sizeof(uint64_t)); }

    uint64_t values[numberOfRuntimeCounters];
    uint64_t* branchCounts;
    // In a program that counts branches: what its stubs and its code allocate, every cell on every path. See ExactAllocationCounts.
    uint64_t* allocationCounts;
    // One record per GlobalBindingID, in zeroed pages. Null in a program that binds no name. Written once, before compiled code runs.
    GlobalBinding* globalBindings;
};
static_assert(!OBJECT_OFFSETOF(CounterWords, values));

// Of literalStrings and keyCells one table each exists: narrow references, or whole pointers in a program with wide references.
struct ConstantCellWords {
    // Where a stub makes the cell of a string constant itself: the allocator of such cells, and the cards of the string cells.
    void* stringAllocator;
    uint8_t* cardsOfStringCells;
    // Where the headers of constants are made whose text has none in the image: the place of the next one, the end, and the whole stretch.
    uint8_t* nextStringHeader;
    uint8_t* endOfStringHeaders;
    uint8_t* startOfStringHeaders;
    uint64_t sizeOfStringHeaders;
    RememberedSlot<JSCell*>* wideStringCells;
    uint32_t* literalStrings;
    JSCell** wideLiteralStrings;
    uint64_t* literalStringCache;
    uint32_t* keyCells;
    JSCell** wideKeyCells;
    // The strings that `name` of a function answered lately.
    static constexpr uint32_t numberOfCachedFunctionNames = 256;
    uint64_t* functionNameCache;
    // What `name` answers for a function that ProgramTables::functionNameGroups() gives no entry: its name is the string with the number 0.
    JSCell* nameOfFunctionsWithoutEntry;
    JSCell** bigIntCells;
};

struct ObjectLiteralWords {
    const PropertyRun** propertyRuns;
};

struct RuntimeWords {
    ObjectLiteralWords literals;
    ExceptionWords exceptions;
    CounterWords counters;
    ConstantCellWords constants;
};
static_assert(std::is_trivially_default_constructible_v<RuntimeWords>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
