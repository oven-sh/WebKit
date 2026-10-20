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
    // cursor: address of the free slot. hash: the tag of the key, at its position in a slot.
    JumpList atShortSlot;
    JumpList atLongSlot;
};

// owner and key are intact at every exit.
struct HashTableExits {
    // table: the CompactTable, a heap table. keys: address of its first key. entry: the entry's number.
    JumpList foundInCompactTable;
    // table: the TableInImage. keys, entry: as above. Empty unless searchesTablesInImage.
    JumpList foundInTableInImage;
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
// Only at a found exit of a compact table or a table in the image. values may be keys. Overwrites cursor.
void emitLoadValuesOfCompactTable(StubAssembler&, const HashTableRegisters&, GPRReg values);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
