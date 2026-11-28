/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/HashFunctions.h>
#include <wtf/HashTraits.h>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

struct ValueIDTag;

template<typename Tag>
class ID {
public:
    static constexpr uint32_t hashTableDeletedIndex = UINT32_MAX - 1;
    static constexpr bool safeToCompareToHashTableEmptyOrDeletedValue = true;
    static constexpr uint32_t largestIndex = hashTableDeletedIndex - 1;

    constexpr ID() = default;
    constexpr ID(std::nullptr_t) { }
    ID(int) = delete;
    constexpr explicit ID(uint32_t value)
        : m_value(value)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(value <= largestIndex || value >= hashTableDeletedIndex);
    }
    constexpr explicit ID(size_t value)
        : m_value(static_cast<uint32_t>(value))
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(value <= largestIndex);
    }

    static constexpr ID fromBits(uint32_t bits)
    {
        ID id;
        id.m_value = bits;
        return id;
    }
    constexpr uint32_t bits() const { return m_value; }

    constexpr explicit operator bool() const { return m_value != UINT32_MAX; }
    constexpr uint32_t index() const
    {
        return m_value;
    }
    unsigned hash() const { return intHash(index()); }
    friend constexpr bool operator==(ID a, ID b) { return a.index() == b.index(); }
    friend constexpr auto operator<=>(ID a, ID b) { return a.index() <=> b.index(); }

private:
    uint32_t m_value { UINT32_MAX };
};

template<typename Tag>
inline void add(Hasher& hasher, ID<Tag> id)
{
    add(hasher, id.index());
}

using ValueID = ID<ValueIDTag>;
using BlockID = ID<struct BlockIDTag>;

class RootThroughCasts {
public:
    RootThroughCasts() = default;
    explicit RootThroughCasts(ValueID root)
        : m_root(root)
    {
    }

    explicit operator bool() const { return !!m_root; }
    friend bool operator==(RootThroughCasts, RootThroughCasts) = default;
    friend bool operator==(RootThroughCasts root, ValueID value) { return root.m_root == value; }

private:
    ValueID m_root;
};

using StringID = ID<struct StringIDTag>;
using PropertyKeyID = ID<struct PropertyKeyIDTag>;
using FunctionID = ID<struct FunctionIDTag>;
using LayoutID = ID<struct LayoutIDTag>;
using VariableID = ID<struct VariableIDTag>;
using ModuleID = ID<struct ModuleIDTag>;
using ModuleFailureID = ID<struct ModuleFailureIDTag>;
using TypeID = ID<struct TypeIDTag>;
using TypeSiteID = ID<struct TypeSiteIDTag>;
using EvalScopeID = ID<struct EvalScopeIDTag>;
using DataID = ID<struct DataIDTag>;
using ExpectedResultID = ID<struct ExpectedResultIDTag>;

using LayoutSetID = ID<struct LayoutSetIDTag>;
using CalleeSetID = ID<struct CalleeSetIDTag>;
using RegExpRecordID = ID<struct RegExpRecordIDTag>;

} } } // namespace JSC::AOT::Typed

namespace WTF {

template<typename Tag>
struct HashTraits<JSC::AOT::Typed::ID<Tag>> : GenericHashTraits<JSC::AOT::Typed::ID<Tag>> {
    using ID = JSC::AOT::Typed::ID<Tag>;
    static void constructDeletedValue(ID& slot) { slot = ID(ID::hashTableDeletedIndex); }
    static bool isDeletedValue(ID id) { return id.index() == ID::hashTableDeletedIndex; }
};

} // namespace WTF

#endif // ENABLE(AOT)
