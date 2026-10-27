/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3HeapRange.h"
#include "B3Opcode.h"
#include "B3Origin.h"
#include "B3Type.h"
#include "GPRInfo.h"
#include <limits>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/SharedTask.h>
#include <wtf/Vector.h>

namespace JSC {

class CCallHelpers;

namespace B3 {

class BasicBlock;
class PatchpointValue;
class Procedure;
class Value;

struct WordChange {
    uint64_t before;
    uint64_t after;
};

// What is left of a load once a phase has moved it or put another value in its place.
struct MovedLoad {
    // Only where the policy asks for the log.
    static std::optional<MovedLoad> of(const Procedure&, const Value*);
    JS_EXPORT_PRIVATE static std::optional<MovedLoad> describe(const Value*);

    bool comparesBitByBit() const { return ~bitsThatCount || bitsThatMayGetSet || bitsThatMayGetCleared; }

    Opcode opcode;
    Type type;
    int32_t offset;
    Origin origin;
    HeapRange range;
    // A difference counts only in these bits.
    uint64_t bitsThatCount { std::numeric_limits<uint64_t>::max() };
    // If set: a difference counts only where one of the two words has one of these bits.
    uint64_t bitsOfWordsThatCount { 0 };
    // That one of these bits was set, or cleared, since the users' value was read is no difference.
    uint64_t bitsThatMayGetSet { 0 };
    uint64_t bitsThatMayGetCleared { 0 };
    // Nor is it one that the word went from `before` to `after`.
    std::span<const WordChange> changesThatDoNotCount { };
    // Nor that it was zero when the users' value was read.
    bool mayGetFilled { false };
};

// Reads the location again and reports unless it holds what the users of the load are given. No load or store waits for it, and no function gets a frame for it: the report does not come back.
using DifferenceReport = SharedTask<void(CCallHelpers&, const MovedLoad&, GPRReg found, GPRReg given)>;
JS_EXPORT_PRIVATE PatchpointValue* createMovedLoadCheck(Procedure&, const MovedLoad&, Value* address, Value* givenToUsers, RefPtr<DifferenceReport>);

// Where loads stood. The procedure holds nothing of it, so every phase does what it does without the log.
class MovedLoadLog {
public:
    bool isEmpty() const { return m_entries.isEmpty(); }

    // `index`: where in the block the load stands or stood. `store`: the store that the users' value comes from, if it comes from one.
    void add(const MovedLoad&, BasicBlock*, unsigned index, Value* address, Value* givenToUsers, Value* store = nullptr);
    // For a load that has left a loop: none of what stays in the loop writes what it reads.
    void addBehindEachValueThatStaysIn(BasicBlock*, const MovedLoad&, Value* address, Value* givenToUsers);

    // A copy of the value in front of a load's place has such a place behind it, if the address and what the users are given are there too.
    void didClone(Value* original, Value* clone);
    void willDelete(Value*);
    // What stood in front of the split is in `head` now.
    void didSplitForward(BasicBlock*, BasicBlock* head);
    void willDelete(BasicBlock*);
    // For a phase that takes a write from its place. The log cannot tell on which side of a load it ends up.
    JS_EXPORT_PRIVATE void willMove(Value* write);

    // For behind the last phase that moves a value. An entry whose place or values are no longer there is left out.
    JS_EXPORT_PRIVATE bool insertChecks(Procedure&, RefPtr<DifferenceReport>);

private:
    struct Entry {
        bool names(const Value* value) const { return address == value || givenToUsers == value || store == value || before == value; }

        MovedLoad load;
        Value* address;
        Value* givenToUsers;
        Value* store;
        // The last value in front of the load that stays where it is. Without one the load's block begins with values that may move.
        Value* before;
        BasicBlock* block;
    };

    void name(Value*, unsigned entry);
    void drop(Entry& entry) { entry.address = nullptr; }
    void dropEntriesWrittenBy(const Value*);

    Vector<Entry> m_entries;
    UncheckedKeyHashMap<Value*, Vector<unsigned, 2>> m_entriesNaming;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
