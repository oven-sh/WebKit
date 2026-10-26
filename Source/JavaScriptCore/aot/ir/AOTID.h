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

template<typename Tag>
class ID {
public:
    static constexpr uint32_t hashTableDeletedIndex = UINT32_MAX - 1;
    static constexpr bool safeToCompareToHashTableEmptyOrDeletedValue = true;

    constexpr ID() = default;
    constexpr ID(std::nullptr_t) { }
    ID(int) = delete;
    constexpr explicit ID(uint32_t value)
        : m_value(value)
    {
    }
    constexpr explicit ID(size_t value)
        : m_value(static_cast<uint32_t>(value))
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(value < hashTableDeletedIndex);
    }

    constexpr explicit operator bool() const { return m_value != UINT32_MAX; }
    constexpr uint32_t index() const { return m_value; }
    unsigned hash() const { return intHash(m_value); }
    friend constexpr bool operator==(ID, ID) = default;
    friend constexpr auto operator<=>(ID, ID) = default;

private:
    uint32_t m_value { UINT32_MAX };
};

template<typename Tag>
inline void add(Hasher& hasher, ID<Tag> id)
{
    add(hasher, id.index());
}

using ValueID = ID<struct ValueIDTag>;
using BlockID = ID<struct BlockIDTag>;

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
using AccessSiteID = ID<struct AccessSiteIDTag>;
using AllocationSiteID = ID<struct AllocationSiteIDTag>;
using AllocationClassID = ID<struct AllocationClassIDTag>;
using FieldAccessID = ID<struct FieldAccessIDTag>;
using AllocationID = ID<struct AllocationIDTag>;
using RegExpRecordID = ID<struct RegExpRecordIDTag>;
using SiteCacheID = ID<struct SiteCacheIDTag>;
using GlobalBindingID = ID<struct GlobalBindingIDTag>;
using GuardBitID = ID<struct GuardBitIDTag>;

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
