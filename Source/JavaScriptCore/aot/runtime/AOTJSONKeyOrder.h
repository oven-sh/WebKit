/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include <cstdint>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class JSONKeyOrder {
public:
    enum class Kind : uint8_t {
        Numbered,
        Listed,
        Unknown,
    };

    constexpr JSONKeyOrder() = default;

    static constexpr JSONKeyOrder fromWord(uint64_t word) { return JSONKeyOrder(word); }
    static constexpr JSONKeyOrder numbered(uint32_t orderAndCount) { return JSONKeyOrder(orderAndCount); }
    static constexpr JSONKeyOrder listed(uint32_t list, uint32_t count) { return JSONKeyOrder(isListedBit | list | static_cast<uint64_t>(count) << countInListShift); }
    static constexpr JSONKeyOrder unknown() { return JSONKeyOrder(isUnknownBit); }

    constexpr uint64_t word() const { return m_word; }
    constexpr Kind kind() const
    {
        if (m_word & isUnknownBit)
            return Kind::Unknown;
        return m_word & isListedBit ? Kind::Listed : Kind::Numbered;
    }
    constexpr uint32_t orderAndCount() const { return static_cast<uint32_t>(m_word); }
    constexpr uint32_t list() const { return static_cast<uint32_t>(m_word); }
    constexpr uint32_t countInList() const { return static_cast<uint32_t>(m_word >> countInListShift); }

    static constexpr uint64_t isListedBit = 1ull << 32;
    static constexpr uint64_t isUnknownBit = 1ull << 33;
    static constexpr unsigned countInListShift = 34;
    static constexpr uint32_t maxCountInList = (1u << (64 - countInListShift)) - 1;

    friend constexpr bool operator==(JSONKeyOrder, JSONKeyOrder) = default;

private:
    explicit constexpr JSONKeyOrder(uint64_t word)
        : m_word(word)
    {
    }

    uint64_t m_word { unorderedOrderWord };
};

static_assert(JSONKeyOrder().kind() == JSONKeyOrder::Kind::Numbered && !JSONKeyOrder().word());
static_assert(JSONKeyOrder::numbered(orderAndCountMask).kind() == JSONKeyOrder::Kind::Numbered && JSONKeyOrder::numbered(orderAndCountMask).word() < JSONKeyOrder::isListedBit);
static_assert(JSONKeyOrder::listed(7, 3).kind() == JSONKeyOrder::Kind::Listed && JSONKeyOrder::listed(7, 3).list() == 7 && JSONKeyOrder::listed(7, 3).countInList() == 3);
static_assert(JSONKeyOrder::listed(UINT32_MAX, JSONKeyOrder::maxCountInList).kind() == JSONKeyOrder::Kind::Listed && JSONKeyOrder::listed(UINT32_MAX, JSONKeyOrder::maxCountInList).countInList() == JSONKeyOrder::maxCountInList);
static_assert(JSONKeyOrder::listed(0, 0).word() >= JSONKeyOrder::isListedBit);
static_assert(JSONKeyOrder::unknown().kind() == JSONKeyOrder::Kind::Unknown && JSONKeyOrder::unknown().word() >= JSONKeyOrder::isListedBit);

class JSONKeyOrderLists {
    WTF_MAKE_NONCOPYABLE(JSONKeyOrderLists);
public:
    using List = Vector<uint16_t, 16>;

    JSONKeyOrderLists() = default;

    uint32_t size() const { return m_lists.size(); }
    uint32_t add(std::span<const uint16_t> entries);
    void append(uint32_t list, uint16_t entry) { m_lists[list].append(entry); }
    std::span<const uint16_t> entries(uint32_t list) const LIFETIME_BOUND { return m_lists[list].span(); }
    void shrink(uint32_t size) { m_lists.shrink(size); }

private:
    Vector<List, 1> m_lists;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
