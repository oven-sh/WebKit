/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

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

class UseLists {
    WTF_MAKE_NONCOPYABLE(UseLists);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(UseLists);
public:
    UseLists(const Function&, const CFG&);

    std::span<const Use> usesOf(ValueID) const;
    unsigned useCount(ValueID id) const { return usesOf(id).size(); }
    std::optional<Use> singleUse(ValueID) const;
    ValueLocation locationOf(ValueID) const;
    BlockID blockOf(ValueID id) const { return locationOf(id).block; }
    ProgramPoint pointOf(const Use&) const;
    ProgramPoint pointOf(ValueID valueInBlock) const;
    ProgramPoint pointAfter(ValueID) const;
    bool isBefore(const Use&, const Use&) const;
    unsigned numberOfValues() const { return m_locations.size(); }

private:
    template<typename Functor> void forEachUse(NOESCAPE const Functor&) const;

    const Function& m_function;
    const CFG& m_cfg;
    Vector<uint32_t> m_firstUse;
    Vector<Use> m_uses;
    Vector<ValueLocation> m_locations;
};

inline std::span<const Use> UseLists::usesOf(ValueID id) const
{
    RELEASE_ASSERT(id.index() < m_locations.size());
    uint32_t first = m_firstUse[id.index()];
    return m_uses.span().subspan(first, m_firstUse[id.index() + 1] - first);
}

inline std::optional<Use> UseLists::singleUse(ValueID id) const
{
    auto uses = usesOf(id);
    if (uses.size() != 1)
        return std::nullopt;
    return uses.front();
}

inline ValueLocation UseLists::locationOf(ValueID id) const
{
    RELEASE_ASSERT(id.index() < m_locations.size());
    return m_locations[id.index()];
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
