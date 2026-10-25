/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTVector.h"
#include <compare>
#include <optional>
#include <span>
#include <type_traits>
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
    UseLists(const Function&, const CFG&);

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
        return range.end & isLinked ? range.end & ~isLinked : range.end - range.first;
    }
    std::optional<Use> singleUse(ValueID id) const { return useCount(id) == 1 ? std::optional(uses()[rangeOf(id).first]) : std::nullopt; }
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

    void didChangeBlock(BlockID) { m_isUpToDate = false; m_hasChangedBlocks = true; }
    void didChangeOperandsOf(ValueID user);
    void didReplaceUses(ValueID from, ValueID to);
    void didAddValues() { m_isUpToDate = false; }
    void didChangeControlFlow() { m_isUpToDate = false; m_graphMayHaveChanged = true; }
    void didChangeEverything() { m_isUpToDate = false; m_mustBeMadeAnew = true; }

    bool isUpToDate() const { return m_isUpToDate; }
    bool mustBeMadeAnew() const { return m_mustBeMadeAnew; }

    struct Update {
        bool hasMadeAnew { false };
        size_t numberOfValuesRead { 0 };
    };
    Update bringUpToDate(const CFG&);

private:
    struct UseRange {
        uint32_t first { 0 };
        uint32_t end { 0 };
    };
    static constexpr uint32_t isLinked = 1u << 31;

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

    void makeAnew();
    struct RoomForUses {
        StoredUse* uses;
        uint32_t* placeOfNextUse;
        size_t size;
    };
    NEVER_INLINE RoomForUses makeRoomForUses(size_t);
    std::span<const Use> layOut(ValueID) const;
    void didBecomeUpToDate();

    const Function& m_function;
    const CFG* m_cfg;
    mutable Vector<UseRange> m_ranges;
    mutable Vector<StoredUse> m_uses;
    Vector<uint32_t> m_placeOfNextUse;
    Vector<ValueLocation> m_locations;
    size_t m_numberOfUses { 0 };
    bool m_hasChangedBlocks { false };
    bool m_isUpToDate { false };
    bool m_graphMayHaveChanged { false };
    bool m_mustBeMadeAnew { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
