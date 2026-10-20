/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkedFunctions.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTThreads.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

void LinkedFunctions::swap(LinkedFunctions& other)
{
    std::swap(m_first, other.m_first);
    std::swap(m_size, other.m_size);
    std::swap(m_capacity, other.m_capacity);
}

void LinkedFunctions::clear()
{
    std::destroy_n(m_first, m_size);
    fastFree(m_first);
    m_first = nullptr;
    m_size = 0;
    m_capacity = 0;
}

void LinkedFunctions::clearOnAllThreads(BuildContext& context)
{
    forEachIndexInParallel(context.parallelLoops(), m_size, [&](unsigned index) {
        CompiledCode code = WTF::move(m_first[index]);
    });
    clear();
}

void LinkedFunctions::reserveCapacity(size_t capacity)
{
    if (capacity <= m_capacity)
        return;
    RELEASE_ASSERT(capacity <= std::numeric_limits<size_t>::max() / sizeof(CompiledCode));
    auto* first = static_cast<CompiledCode*>(fastMalloc(capacity * sizeof(CompiledCode)));
    std::uninitialized_move_n(m_first, m_size, first);
    std::destroy_n(m_first, m_size);
    fastFree(m_first);
    m_first = first;
    m_capacity = capacity;
}

void LinkedFunctions::grow(size_t newSize, BuildContext& context)
{
    RELEASE_ASSERT(newSize >= m_size);
    reserveCapacity(newSize);
    constexpr size_t functionsInChunk = 1024;
    size_t number = newSize - m_size;
    forEachIndexInParallel(context.parallelLoops(), static_cast<unsigned>((number + functionsInChunk - 1) / functionsInChunk), [&](unsigned chunk) {
        size_t start = static_cast<size_t>(chunk) * functionsInChunk;
        std::uninitialized_value_construct_n(m_first + m_size + start, std::min(functionsInChunk, number - start));
    });
    m_size = newSize;
}

void LinkedFunctions::reorder(std::span<const uint32_t> order, BuildContext& context)
{
    LinkedFunctions reordered;
    reordered.reserveCapacity(order.size());
    forEachIndexInParallel(context.parallelLoops(), static_cast<unsigned>(order.size()), [&](unsigned index) {
        new (NotNull, reordered.m_first + index) CompiledCode(WTF::move((*this)[order[index]]));
    });
    reordered.m_size = order.size();
    context.destroyLater(std::exchange(*this, WTF::move(reordered)));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
