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
#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/IndexSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/SharedTask.h>
#include <wtf/Vector.h>

namespace JSC {

class CCallHelpers;

namespace B3 {

class BasicBlock;
class PatchpointValue;
class Procedure;
class Value;
class Variable;

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
// Compares the two integers and reports unless the answer is the one that a phase has put in the place of the comparison. The report is handed the two.
JS_EXPORT_PRIVATE PatchpointValue* createDecidedComparisonCheck(Procedure&, Opcode, Origin, Value* left, Value* right, bool answer, RefPtr<DifferenceReport>);

// Where loads stood. The procedure holds nothing of it, so every phase does what it does without the log.
class MovedLoadLog {
public:
    bool isEmpty() const { return m_entries.isEmpty(); }

    // `index`: where in the block the load stands or stood. `store`: the store that the users' value comes from, if it comes from one.
    void add(const MovedLoad&, BasicBlock*, unsigned index, Value* address, Value* givenToUsers, Value* store = nullptr);
    // For a phase that puts `answer` in the place of a comparison of two integers, or takes one way out of the branch on it. Only where the policy asks for the log. `index`: where in the block the comparison stands or stood.
    JS_EXPORT_PRIVATE void addDecidedComparison(const Procedure&, const Value* comparison, BasicBlock*, unsigned index, Value* left, Value* right, bool answer);
    // For a phase that takes two integers for equal and lets one stand for the other.
    JS_EXPORT_PRIVATE void addValuesTakenForEqual(const Procedure&, Origin, BasicBlock*, unsigned index, Value* left, Value* right);
    // With the comparison gone often nothing uses what it compared. These stay in the procedure for the check, so the code is not quite that of an ordinary build.
    // Such a block stays though it only jumps.
    bool holdsDecidedComparisonAtHeadOf(const BasicBlock* block) const
    {
        return std::ranges::any_of(m_entries, [&](const Entry& entry) {
            return entry.address && entry.answerOfComparison && !entry.before && entry.block == block;
        });
    }
    template<typename Functor>
    void forEachOperandOfDecidedComparison(const Functor& functor) const
    {
        for (const Entry& entry : m_entries) {
            if (!entry.address || !entry.answerOfComparison)
                continue;
            functor(entry.address);
            functor(entry.givenToUsers);
        }
    }
    // For a load that has left a loop: none of what stays in the loop writes what it reads.
    void addBehindEachValueThatStaysIn(BasicBlock*, const MovedLoad&, Value* address, Value* givenToUsers);

    // While blocks are copied with what is in them. At its end an entry whose place was copied has a copy that names the copies.
    class RegionCopy {
        WTF_MAKE_NONCOPYABLE(RegionCopy);
    public:
        explicit RegionCopy(MovedLoadLog& log)
            : m_log(log)
        {
            m_log.willCopyRegion();
        }
        ~RegionCopy() { m_log.didCopyRegion(); }

    private:
        MovedLoadLog& m_log;
    };
    void didCopy(BasicBlock* original, BasicBlock* copy);
    // While every value keeps its meaning at the places of the entries that name it, whatever is copied: no edge changes, or willCopy() was told which blocks get other ways in.
    class OriginalsKeepTheirMeaning {
        WTF_MAKE_NONCOPYABLE(OriginalsKeepTheirMeaning);
    public:
        explicit OriginalsKeepTheirMeaning(MovedLoadLog& log)
            : m_log(log)
        {
            ++m_log.m_depthOfCopiesBesideOriginals;
        }
        ~OriginalsKeepTheirMeaning() { --m_log.m_depthOfCopiesBesideOriginals; }

    private:
        MovedLoadLog& m_log;
    };
    // An entry that named one of these values names a Get of its variable at the place of the load from here on. fixSSA puts there what the users of the value are given at that place.
    void didDemote(Procedure&, const UncheckedKeyHashMap<Value*, Variable*>&);
    // For a phase that is about to copy these blocks and to lead other edges into them and their copies, and has demoted what is used outside its block. What nothing but an entry in another block names has no variable, and the entry goes.
    void willCopy(Procedure&, const IndexSet<BasicBlock*>&);
    // A copy that is made by itself may be put anywhere.
    void didClone(Value* original, Value* clone);
    void willDelete(Value*);
    // What stood in front of the split is in `head` now.
    void didSplitForward(BasicBlock*, BasicBlock* head);
    void willDelete(BasicBlock*);
    // What is in `block` goes to the end of `predecessor`, which has lost its terminal.
    void willAppendValuesOf(BasicBlock*, BasicBlock* predecessor);
    // While what nothing leads to is deleted. A write that does not run is not missed.
    class UnreachableCodeIsDeleted {
        WTF_MAKE_NONCOPYABLE(UnreachableCodeIsDeleted);
    public:
        explicit UnreachableCodeIsDeleted(MovedLoadLog& log)
            : m_log(log)
        {
            m_log.m_isDeletingUnreachableCode = true;
        }
        ~UnreachableCodeIsDeleted() { m_log.m_isDeletingUnreachableCode = false; }

    private:
        MovedLoadLog& m_log;
    };
    // For a phase that takes a write from its place. The log cannot tell on which side of a load it ends up.
    JS_EXPORT_PRIVATE void willMove(Value* write);

    // For behind the last phase that moves a value. An entry whose place or values are no longer there is left out.
    JS_EXPORT_PRIVATE bool insertChecks(Procedure&, RefPtr<DifferenceReport>, RefPtr<DifferenceReport> forDecidedComparisons = nullptr);

private:
    struct Entry {
        bool names(const Value* value) const { return address == value || givenToUsers == value || store == value || before == value; }

        // Of a comparison: its opcode, the type of what it compares, its origin.
        MovedLoad load;
        // Of a comparison: the left and the right operand.
        Value* address;
        Value* givenToUsers;
        Value* store;
        // The last value in front of the load that stays where it is. Without one the load's block begins with values that may move.
        Value* before;
        BasicBlock* block;
        std::optional<bool> answerOfComparison { };
    };

    void willCopyRegion();
    void didCopyRegion();
    static Value* lastValueThatStaysInFrontOf(BasicBlock*, unsigned index);
    void append(const Entry&);
    void name(Value*, unsigned entry);
    void rename(Value*& named, Value* replacement, unsigned entry);
    void drop(Entry& entry)
    {
        entry.address = nullptr;
    }
    void dropEntriesWrittenBy(const Value*);

    Vector<Entry> m_entries;
    UncheckedKeyHashMap<Value*, Vector<unsigned, 2>> m_entriesNaming;
    unsigned m_depthOfRegionCopies { 0 };
    unsigned m_depthOfCopiesBesideOriginals { 0 };
    bool m_isDeletingUnreachableCode { false };
    unsigned m_numberOfEntriesBeforeRegionCopy { 0 };
    UncheckedKeyHashMap<Value*, Value*> m_copiesInRegion;
    UncheckedKeyHashMap<BasicBlock*, BasicBlock*> m_blockCopiesInRegion;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
