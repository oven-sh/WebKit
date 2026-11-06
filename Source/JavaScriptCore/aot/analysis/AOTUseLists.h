/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTChangeLog.h"
#include "AOTVector.h"
#include <compare>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

struct Use {
    enum class Kind : uint8_t { Operand, TerminatorOperand, EdgeArgument, HandlerArgument, BlockSubject };

    Kind kind { Kind::Operand };
    BlockID block;
    ValueID user;
    uint32_t operandIndex { 0 };
    uint32_t targetIndex { 0 };

    bool isEdgeUse() const { return kind == Kind::EdgeArgument || kind == Kind::HandlerArgument; }
    Edge edge() const
    {
        RELEASE_ASSERT(isEdgeUse());
        return kind == Kind::HandlerArgument ? Edge::toHandlerOf(block) : Edge { block, targetIndex };
    }

    friend bool operator==(const Use&, const Use&) = default;
};

ValueID usedValueOf(const Function&, const Use&);

enum class ValuePlacement : uint8_t { Detached, Parameter, InBlock };

struct ValueLocation {
    ValuePlacement placement { ValuePlacement::Detached };
    BlockID block;
    uint32_t index { 0 };

    friend bool operator==(const ValueLocation&, const ValueLocation&) = default;
};

struct UseOrder {
    uint32_t blockOrder { 0 };
    uint32_t index { 0 };
    uint32_t rank { 0 };
    uint32_t targetIndex { 0 };
    uint32_t operandIndex { 0 };

    friend auto operator<=>(const UseOrder&, const UseOrder&) = default;
};

class UseLists {
    WTF_MAKE_NONCOPYABLE(UseLists);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(UseLists);
public:
    // With the log of the function's changes, lists of a function that changes little at a time read only the blocks that have changed.
    UseLists(const Function&, const CFG&, const ChangeLog* = nullptr);

    std::span<const Use> usesOf(ValueID id) const
    {
        UseRange range = rangeOf(id);
        if (range.end & isLinked)
            return layOut(id);
        return { uses() + range.first, range.end - range.first };
    }
    unsigned useCount(ValueID id) const
    {
        UseRange range = rangeOf(id);
        return range.end & isLinked ? range.end & ~marksOfLinks : range.end - range.first;
    }
    std::optional<Use> singleUse(ValueID id) const { return useCount(id) == 1 ? std::optional(usesOf(id)[0]) : std::nullopt; }
    ValueLocation locationOf(ValueID id) const
    {
        RELEASE_ASSERT(id.index() < m_locations.size());
        return m_locations[id.index()];
    }
    BlockID blockOf(ValueID id) const { return locationOf(id).block; }
    ProgramPoint pointOf(const Use&) const;
    ProgramPoint pointOf(ValueID valueInBlock) const;
    ProgramPoint pointAfter(ValueID) const;
    UseOrder orderOf(const Use&) const;
    bool isBefore(const Use& a, const Use& b) const { return orderOf(a) < orderOf(b); }
    unsigned numberOfValues() const { return m_locations.size(); }

    void didChangeBlock(BlockID block)
    {
        m_isUpToDate = false;
        m_namedBlocks.append(block);
    }
    void didChangeOperandsOf(ValueID user);
    void didChangeWhatTheLogSays()
    {
        m_isUpToDate = false;
        m_mustBeMadeAnew |= !m_changes;
    }

    bool isUpToDate() const { return m_isUpToDate; }
    bool mustBeMadeAnew() const { return m_mustBeMadeAnew; }

    enum class ReasonToMakeAnew : uint8_t { None, NoLog, NothingKeptForUpdates, ForgottenChanges, MuchHasChanged, MuchIsGone, UnknownChange };
    static constexpr unsigned numberOfReasonsToMakeAnew = 7;

    struct Update {
        ReasonToMakeAnew madeAnewFor { ReasonToMakeAnew::None };
        size_t numberOfValuesRead { 0 };
    };
    Update bringUpToDate(const CFG&, bool isGraphThatWasRead = false);

private:
    struct UseRange {
        uint32_t first { 0 };
        uint32_t end { 0 };
    };
    static constexpr uint32_t isLinked = 1u << 31;
    static constexpr uint32_t mayBeOutOfOrder = 1u << 30;
    static constexpr uint32_t marksOfLinks = isLinked | mayBeOutOfOrder;
    static constexpr uint32_t useIsGone = std::numeric_limits<uint32_t>::max();

    struct StoredUse {
        uint32_t words[5];
    };
    static_assert(sizeof(StoredUse) == sizeof(Use) && alignof(StoredUse) == alignof(Use) && std::is_trivially_copyable_v<Use>);

    const Use* uses() const { return reinterpret_cast<const Use*>(m_uses.span().data()); }
    UseRange rangeOf(ValueID id) const
    {
        RELEASE_ASSERT(id.index() < m_ranges.size());
        return m_ranges.span().data()[id.index()];
    }

    // Where the uses in a block and the values that it lists stand. An update takes them out by it.
    struct PartOfBlock {
        static constexpr uint32_t notListed = std::numeric_limits<uint32_t>::max();

        bool isListed() const { return firstUse != notListed; }

        uint32_t firstUse { notListed };
        uint32_t endOfUses { 0 };
        uint32_t firstListedValue { 0 };
        uint32_t endOfListedValues { 0 };
    };

    void makeAnew();
    template<bool keepsWhatUpdatesRead> void makeAnewFromBlocks();
    struct RoomForUses {
        StoredUse* uses;
        uint32_t* placeOfNextUse;
        uint32_t* usedValues;
        size_t size;
    };
    NEVER_INLINE RoomForUses makeRoomForUses(size_t size, size_t capacity);
    std::span<const Use> layOut(ValueID) const;
    NEVER_INLINE std::span<const Use> layOutBehindUpdates(ValueID) const;
    ReasonToMakeAnew findReasonToMakeAnew() const;
    struct ChangedBlocks {
        size_t numberOfValuesRead { 0 };
        bool keepTheirOrder { true };
    };
    ChangedBlocks findChangedBlocks(bool isGraphThatWasRead, size_t mostValuesRead);
    void forgetChangedBlocks();
    bool hasChangedLittle();
    bool readChangedBlocks();
    void linkAgain(UseRange&, ValueID);
    void didBecomeUpToDate();

    const Function& m_function;
    const CFG* m_cfg;
    const ChangeLog* m_changes;
    ChangeLog::Cursor m_firstUnreadChange;
    mutable Vector<UseRange> m_ranges;
    mutable Vector<StoredUse> m_uses;
    mutable Vector<uint32_t> m_placeOfNextUse;
    mutable Vector<uint32_t> m_firstPlacesOfRows;
    Vector<ValueLocation> m_locations;
    Vector<uint32_t> m_usedValues;
    Vector<ValueID> m_listedValues;
    Vector<PartOfBlock> m_partsOfBlocks;
    Vector<BlockID> m_order;
    Vector<BlockID> m_namedBlocks;
    Vector<BlockID> m_changedBlocks;
    BitVector m_isChangedBlock;
    size_t m_numberOfUses { 0 };
    bool m_isUpToDate { false };
    bool m_mustBeMadeAnew { false };
    bool m_keepsWhatUpdatesRead { false };
    bool m_wasUpdated { false };
};

// A branch on a test passes the tested operand on as a parameter of the block it enters.
Vector<ValueID, 2> findNarrowedParameters(const Function&, const UseLists&, const Use& useByTest);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
