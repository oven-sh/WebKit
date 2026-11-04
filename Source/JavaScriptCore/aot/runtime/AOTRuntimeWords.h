/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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
class Structure;
template<typename> class RememberedSlot;

namespace AOT {

struct ExceptionWords {
    EncodedJSValue thrownValue;
    // Of the frame of the handler that unwinding has found, from there to the jump to the handler.
    void* stackPointerOfHandler;
    // Set by a loop poll that was taken out of its handler, on its way to operationAOTPollSlow, which clears it.
    uint32_t pollLeavesOwedError;
};
static_assert(sizeof(ExceptionWords) == 24);

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

struct BindingWords {
    // One record for each word of ProgramTables::globalBindings(), in zeroed pages behind the string cells of the instance. Null in a program that binds no name. Written once, before compiled code runs.
    GlobalBinding* globalBindings;
};

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
    // The strings that `name` of a function answered lately, by the number of the closure: in the low half that number and one, in the high half the narrow reference. The handler of Slot::Kind::ClosureName reads it.
    static constexpr uint32_t numberOfCachedFunctionNames = 256;
    uint64_t* functionNameCache;
    JSCell** bigIntCells;
};

// Where a value that is not of the type its function promises goes, for a caller that was asked before: its landing, or, where it has none, the address that it is returned to anyway.
// Code does not move, so an entry holds for good.
struct LandingOfMisfit {
    const void* returnAddress;
    const void* landing;
};
static_assert(sizeof(LandingOfMisfit) == 16);

// An address has a place, and is listed there or in the first entry behind it that was empty. As many entries as there are places lie behind the places, and the last of them stays empty,
// so a search ends without turning round. The table grows, so two callers never take an entry from each other.
struct LandingsOfMisfits {
    static constexpr unsigned shiftOfAddress = 2;
    static constexpr size_t leastNumberOfPlaces = 64;
    size_t numberOfPlaces() const { return (maskOfAddress >> shiftOfAddress) + 1; }
    LandingOfMisfit* placeOf(const void* returnAddress) const { return entries + ((std::bit_cast<uintptr_t>(returnAddress) & maskOfAddress) >> shiftOfAddress); }

    LandingOfMisfit* entries;
    uintptr_t maskOfAddress;
    size_t numberOfEntriesFilled;
    // The table of a program that has returned no such value.
    LandingOfMisfit empty;
};

struct MisfitWords {
    // Not a root: the stub ReturnMisfit leaves the value here, and the landing takes it before anything else runs.
    uint64_t misfit;
    LandingsOfMisfits landings;
};

struct RuntimeWords {
    ExceptionWords exceptions;
    BindingWords bindings;
    ConstantCellWords constants;
};
static_assert(std::is_trivially_default_constructible_v<RuntimeWords>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
