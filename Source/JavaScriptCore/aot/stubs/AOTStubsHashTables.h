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
    std::optional<StubRegister> owner;
};

inline constexpr HashTableRegisterPlan hashTableRegisterPlan =
#if CPU(X86_64)
    { StubRegister::R0, StubRegister::A2, StubRegister::A3, StubRegister::T13, StubRegister::T11, StubRegister::T9, StubRegister::T10 };
#else
    { StubRegister::T13, StubRegister::T11, StubRegister::T12, StubRegister::T15, StubRegister::T14, StubRegister::T9, std::nullopt };
#endif

inline constexpr GPRReg ownerOfHashTableGPR = hashTableRegisterPlan.owner ? gprOf(*hashTableRegisterPlan.owner) : thisGPR;

struct HashTableRegisters {
    GPRReg owner;
    GPRReg key;
    GPRReg hash;
    GPRReg table;
    GPRReg cursor;
    GPRReg keys;
    GPRReg entry;
    GPRReg candidate;

    static HashTableRegisters take(StubAssembler&);
};

struct HashTableLookup {
    KeyComparison comparison;
    bool findsInsertionPoint { false };
    bool keepsSlot { false };
    bool searchesTablesInImage { false };
    bool asksForPresenceAlone { false };
};

struct InsertionPointExits {
    JumpList amongTags;
    JumpList atSlot;
};

struct HashTableExits {
    JumpList foundInCompactTable;
    JumpList foundAmongNarrowKeys;
    JumpList foundAmongWideKeys;
    JumpList foundInImage;
    JumpList absent;
    JumpList hasNoStorage;
    InsertionPointExits insertionPoints;
    JumpList unknown;
};

ptrdiff_t offsetOfHashTableStorage();
void emitFindInHashTable(StubAssembler&, const HashTableRegisters&, HashTableLookup, HashTableExits&);
void emitFindLatin1CharactersInHashTable(StubAssembler&, const HashTableRegisters&, GPRReg length, HashTableExits&);
void emitLoadTagOfSlot(StubAssembler&, GPRReg hash);
void emitMarkThatKeyLeftHomeSlot(StubAssembler&, BaseIndex tagAtHome);
void emitHashOfBits(StubAssembler&, GPRReg key, GPRReg hash, GPRReg scratch, GPRReg scratch2);
void emitLoadValuesOfCompactTable(StubAssembler&, const HashTableRegisters&, GPRReg values, JumpList* keysAreNarrow = nullptr, JumpList* keysAreWide = nullptr, bool everyoneKnowsTheWidth = false);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
