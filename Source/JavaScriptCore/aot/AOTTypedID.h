/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename Tag>
class ID {
public:
    constexpr ID() = default;
    constexpr explicit ID(uint32_t value)
        : m_value(value)
    {
    }
    constexpr explicit ID(size_t value)
        : m_value(static_cast<uint32_t>(value))
    {
    }

    constexpr explicit operator bool() const { return m_value != UINT32_MAX; }
    constexpr uint32_t index() const { return m_value; }
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
using FunctionID = ID<struct FunctionIDTag>;
using LayoutID = ID<struct LayoutIDTag>;
using SlotID = ID<struct SlotIDTag>;
using TypeID = ID<struct TypeIDTag>;
using StringID = ID<struct StringIDTag>;
using VariableID = ID<struct VariableIDTag>;
using ModuleID = ID<struct ModuleIDTag>;

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
