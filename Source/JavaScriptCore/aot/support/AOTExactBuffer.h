/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/Vector.h>

namespace JSC { namespace AOT {

// Vector::shrinkToFit() reallocates plain data, and mimalloc's realloc returns the same block while it stays at least half full.
template<typename VectorType>
void moveToExactBuffer(VectorType& vector)
{
    VectorType exact;
    if (vector.size() == vector.capacity() || vector.capacity() == exact.capacity())
        return;
    exact.reserveInitialCapacity(vector.size());
    if constexpr (VectorTraits<typename VectorType::ValueType>::canCopyWithMemcpy)
        exact.append(vector.span());
    else {
        for (auto& element : vector)
            exact.append(WTF::move(element));
    }
    vector = WTF::move(exact);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
