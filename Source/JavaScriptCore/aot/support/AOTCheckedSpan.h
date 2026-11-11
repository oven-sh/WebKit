/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <span>
#include <wtf/Assertions.h>
#include <wtf/Compiler.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

template<typename T, size_t extent>
ALWAYS_INLINE T& checkedAt(std::span<T, extent> list, size_t index)
{
    RELEASE_ASSERT(index < list.size());
    return list.data()[index];
}

template<typename T, size_t extent>
ALWAYS_INLINE std::span<T> checkedSubspan(std::span<T, extent> list, size_t first)
{
    RELEASE_ASSERT(first <= list.size());
    return { list.data() + first, list.size() - first };
}

template<typename T, size_t extent>
ALWAYS_INLINE std::span<T> checkedSubspan(std::span<T, extent> list, size_t first, size_t length)
{
    RELEASE_ASSERT(first <= list.size() && length <= list.size() - first);
    return { list.data() + first, length };
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
