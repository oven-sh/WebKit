/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include <optional>

namespace JSC { namespace AOT {

enum class KeyComparison : uint8_t;

struct HashTableRegisterPlan {
    StubRegister hash;
    StubRegister table;
    StubRegister cursor;
    StubRegister keys;
    StubRegister entry;
    StubRegister candidate;
    std::optional<StubRegister> copyOfOwner;
};

inline constexpr HashTableRegisterPlan hashTableRegisterPlan =
#if CPU(X86_64)
    { StubRegister::R0, StubRegister::A2, StubRegister::A3, StubRegister::T13, StubRegister::T11, StubRegister::T9, StubRegister::T10 };
#else
    { StubRegister::T13, StubRegister::T11, StubRegister::T12, StubRegister::T15, StubRegister::T14, StubRegister::T9, std::nullopt };
#endif

struct HashTableRegisters {
    // The emitters preserve owner and key.
    GPRReg owner;
    GPRReg key;
    GPRReg hash;
    GPRReg table;
    GPRReg cursor;
    GPRReg keys;
    GPRReg entry;
    GPRReg candidate;

    // Operand 0 is the owner, operand 1 the key.
    static HashTableRegisters take(StubAssembler&);
};

struct HashTableLookup {
    KeyComparison comparison;
    bool searchesTablesInImage;
    bool findsInsertionPoint { false };
};

// Where the index of a heap table takes a key that the table lacks. table and keys are as at foundInCompactTable.
struct InsertionPointExits {
    // hash: the hash of the key.
    JumpList amongTags;
    // cursor: the number of the free slot. hash: its low byte is the tag of the key.
    JumpList atSlot;
};

// owner and key are intact at every exit.
struct HashTableExits {
    // table: the CompactTable, a heap table. keys: address of its first key. entry: the entry's number.
    JumpList foundInCompactTable;
    // The same, where the search has found out that the keys are narrow.
    JumpList foundAmongNarrowKeys;
    // The same, where it has found out that they are wide.
    JumpList foundAmongWideKeys;
    // Empty when findsInsertionPoint.
    JumpList absent;
    // The next two are empty unless findsInsertionPoint. hash: the hash of the key.
    JumpList hasNoStorage;
    InsertionPointExits insertionPoints;
    JumpList unknown;
};

ptrdiff_t offsetOfHashTableStorage();
// Never falls through.
void emitFindInHashTable(StubAssembler&, const HashTableRegisters&, HashTableLookup, HashTableExits&);
// hash = rapidHashMix64(key). On x86-64 hash is rax and scratch is rdx.
void emitHashOfBits(StubAssembler&, GPRReg key, GPRReg hash, GPRReg scratch, GPRReg scratch2);
// Only at a found exit of a compact table or a table in the image. values may be keys. Overwrites cursor. Whoever knows that the keys are narrow comes in behind the test, which is left out where nobody falls in from above.
void emitLoadValuesOfCompactTable(StubAssembler&, const HashTableRegisters&, GPRReg values, JumpList* keysAreNarrow = nullptr, JumpList* keysAreWide = nullptr, bool everyoneKnowsTheWidth = false);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
