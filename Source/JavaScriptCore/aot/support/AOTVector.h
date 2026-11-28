/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <utility>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

using WTF::Vector;

template<typename T>
inline constexpr bool movesAsBytes = WTF::VectorTraits<T>::canMoveWithMemcpy;

template<typename List>
ALWAYS_INLINE void takeRoom(List& list, List& former)
{
    ASSERT(list.isEmpty());
    if (former.capacity() == list.capacity())
        return;
    former.shrink(0);
    list = WTF::move(former);
}

template<typename List>
class RoomOnly {
    WTF_MAKE_NONCOPYABLE(RoomOnly);
public:
    RoomOnly() = default;

    ALWAYS_INLINE List& take() LIFETIME_BOUND
    {
        if constexpr (requires { m_list.shrink(0); })
            m_list.shrink(0);
        else if constexpr (requires { m_list.clearAll(); })
            m_list.clearAll();
        else
            m_list.clear();
        return m_list;
    }

private:
    List m_list;
};

template<typename Signature, typename Functor>
ScopedLambda<Signature> scopedLambda(Functor&& functor LIFETIME_BOUND)
{
    return ScopedLambda<Signature>(std::forward<Functor>(functor));
}

} } // namespace JSC::AOT
