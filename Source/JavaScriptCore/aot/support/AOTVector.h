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

template<typename Signature, typename Functor>
ScopedLambda<Signature> scopedLambda(Functor&& functor LIFETIME_BOUND)
{
    return ScopedLambda<Signature>(std::forward<Functor>(functor));
}

} } // namespace JSC::AOT
