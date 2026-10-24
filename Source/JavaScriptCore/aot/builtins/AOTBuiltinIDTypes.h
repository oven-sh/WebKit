/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <compare>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <wtf/Assertions.h>
#include <wtf/HashFunctions.h>
#include <wtf/HashTraits.h>
#include <wtf/Hasher.h>

namespace JSC {

enum class RemovableFeature : uint16_t;

namespace AOT {

template<typename Tag, typename Storage>
class BuiltinTableID {
public:
    static constexpr Storage absentIndex = std::numeric_limits<Storage>::max();
    static constexpr Storage hashTableDeletedIndex = absentIndex - 1;
    static constexpr bool safeToCompareToHashTableEmptyOrDeletedValue = true;

    constexpr BuiltinTableID() = default;
    constexpr explicit BuiltinTableID(uint32_t index)
        : m_index(index >= absentIndex ? absentIndex : static_cast<Storage>(index))
    {
    }

    constexpr explicit operator bool() const { return m_index != absentIndex; }
    constexpr uint32_t index() const { return m_index; }
    unsigned hash() const { return intHash(static_cast<uint32_t>(m_index)); }
    friend constexpr bool operator==(BuiltinTableID, BuiltinTableID) = default;
    friend constexpr auto operator<=>(BuiltinTableID, BuiltinTableID) = default;

private:
    Storage m_index { absentIndex };
};

template<typename Tag, typename Storage>
inline void add(Hasher& hasher, BuiltinTableID<Tag, Storage> id)
{
    add(hasher, id.index());
}

template<typename ID>
class BuiltinTableIDRange {
public:
    class Iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = ID;

        constexpr Iterator() = default;
        constexpr explicit Iterator(uint32_t index)
            : m_index(index)
        {
        }

        constexpr ID operator*() const { return ID(m_index); }
        constexpr Iterator& operator++()
        {
            ++m_index;
            return *this;
        }
        constexpr Iterator operator++(int)
        {
            Iterator result = *this;
            ++*this;
            return result;
        }
        friend constexpr bool operator==(Iterator, Iterator) = default;

    private:
        uint32_t m_index { 0 };
    };

    constexpr BuiltinTableIDRange() = default;
    constexpr BuiltinTableIDRange(ID first, uint32_t count)
        : m_first(count ? first.index() : 0)
        , m_count(count)
    {
    }

    constexpr Iterator begin() const { return Iterator(m_first); }
    constexpr Iterator end() const { return Iterator(m_first + m_count); }
    constexpr uint32_t size() const { return m_count; }
    constexpr bool isEmpty() const { return !m_count; }
    constexpr ID at(uint32_t position) const
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(position < m_count);
        return ID(m_first + position);
    }

private:
    uint32_t m_first { 0 };
    uint32_t m_count { 0 };
};

using BuiltinID = BuiltinTableID<struct BuiltinIDTag, uint16_t>;
using BuiltinName = BuiltinTableID<struct BuiltinNameTag, uint16_t>;
using BuiltinClassID = BuiltinTableID<struct BuiltinClassIDTag, uint16_t>;
using BuiltinSlotID = BuiltinTableID<struct BuiltinSlotIDTag, uint16_t>;
using BuiltinLayoutID = BuiltinTableID<struct BuiltinLayoutIDTag, uint16_t>;
using BuiltinOverloadID = BuiltinTableID<struct BuiltinOverloadIDTag, uint16_t>;
using BuiltinTypeID = BuiltinTableID<struct BuiltinTypeIDTag, uint16_t>;
using BuiltinDispatchNodeID = BuiltinTableID<struct BuiltinDispatchNodeIDTag, uint16_t>;
using RuntimeModuleID = BuiltinTableID<struct RuntimeModuleIDTag, uint16_t>;
using RuntimeFunctionID = BuiltinTableID<struct RuntimeFunctionIDTag, uint32_t>;

static_assert(std::forward_iterator<BuiltinTableIDRange<BuiltinID>::Iterator>);

template<typename ID> struct NamedIDsOf;

// What code of the engine names, as in Builtins::ArrayPrototypePush: a position in the engine's own list of names. The table says which of its entries that is.
// So the numbers of the table may change without the code that names an entry being compiled again.
// A list at namespace scope holds these, which are constant expressions, and converts each where it is read: the ids are data of another object file.
template<typename ID>
class NamedTableID {
public:
    constexpr explicit NamedTableID(uint16_t position)
        : m_position(position)
    {
    }

    operator ID() const { return NamedIDsOf<ID>::list()[m_position]; }
    uint32_t index() const { return static_cast<ID>(*this).index(); }
    constexpr uint16_t position() const { return m_position; }
    friend constexpr bool operator==(NamedTableID, NamedTableID) = default;

private:
    uint16_t m_position;
};

using NamedBuiltin = NamedTableID<BuiltinID>;
using NamedBuiltinName = NamedTableID<BuiltinName>;
using NamedBuiltinClass = NamedTableID<BuiltinClassID>;
using NamedBuiltinLayout = NamedTableID<BuiltinLayoutID>;

class BuiltinOperand {
public:
    enum class Kind : uint8_t {
        Receiver,
        Argument,
        ArgumentsFrom,
    };

    static constexpr unsigned maxArgumentIndex = UINT8_MAX;

    static constexpr BuiltinOperand receiver() { return BuiltinOperand(Kind::Receiver, 0); }
    static constexpr BuiltinOperand argument(unsigned index) { return BuiltinOperand(Kind::Argument, index); }
    static constexpr BuiltinOperand argumentsFrom(unsigned index) { return BuiltinOperand(Kind::ArgumentsFrom, index); }

    constexpr Kind kind() const { return m_kind; }
    constexpr bool isReceiver() const { return m_kind == Kind::Receiver; }
    constexpr unsigned argumentIndex() const
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(m_kind != Kind::Receiver);
        return m_index;
    }

    constexpr bool coversArgument(unsigned index) const
    {
        switch (m_kind) {
        case Kind::Receiver:
            return false;
        case Kind::Argument:
            return index == m_index;
        case Kind::ArgumentsFrom:
            return index >= m_index;
        }
        RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
    }

    friend constexpr bool operator==(BuiltinOperand, BuiltinOperand) = default;

private:
    constexpr BuiltinOperand(Kind kind, unsigned index)
        : m_kind(kind)
        , m_index(static_cast<uint8_t>(index))
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(index <= maxArgumentIndex);
    }

    Kind m_kind;
    uint8_t m_index;
};

} // namespace AOT
} // namespace JSC

namespace WTF {

template<typename Tag, typename Storage>
struct HashTraits<JSC::AOT::BuiltinTableID<Tag, Storage>> : GenericHashTraits<JSC::AOT::BuiltinTableID<Tag, Storage>> {
    using ID = JSC::AOT::BuiltinTableID<Tag, Storage>;
    static void constructDeletedValue(ID& slot) { slot = ID(static_cast<uint32_t>(ID::hashTableDeletedIndex)); }
    static bool isDeletedValue(ID id) { return id.index() == ID::hashTableDeletedIndex; }
};

} // namespace WTF

#include "AOTBuiltinIDs.h"

namespace JSC {
namespace AOT {

#define FOR_EACH_AOT_LIST_OF_NAMED_IDS(macro) \
    macro(BuiltinID, builtinTableNamedBuiltins, numberOfNamedBuiltins) \
    macro(BuiltinName, builtinTableNamedNames, numberOfNamedBuiltinNames) \
    macro(BuiltinClassID, builtinTableNamedClasses, numberOfNamedBuiltinClasses) \
    macro(BuiltinLayoutID, builtinTableNamedLayouts, numberOfNamedBuiltinLayouts)

// The name of the namespace says which list the engine was compiled with, so a table that was compiled for another list does not link.
#define AOT_DECLARE_LIST_OF_NAMED_IDS(ID, array, count) \
    inline namespace AOT_NAMESPACE_OF_ENGINE_LIST { extern const ID array[count]; } \
    template<> struct NamedIDsOf<ID> { \
        static const ID* list() { return array; } \
    };
FOR_EACH_AOT_LIST_OF_NAMED_IDS(AOT_DECLARE_LIST_OF_NAMED_IDS)
#undef AOT_DECLARE_LIST_OF_NAMED_IDS

// An instance keeps one byte for each built-in. How many the table has is known where the table is compiled, which checks it against this.
inline constexpr unsigned maxNumberOfBuiltins = 16384;

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
