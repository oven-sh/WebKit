/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include <memory>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace B3 {

class Procedure;

// Making an estimate sets the classes of the edges. The frequencies of the blocks follow from what it found, the edges and their classes alone,
// so they may be set at any later time at which the CFG is still the one that was estimated.
class FrequencyEstimate {
    WTF_MAKE_NONCOPYABLE(FrequencyEstimate);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FrequencyEstimate);
public:
    static std::unique_ptr<FrequencyEstimate> create(Procedure&);
    virtual ~FrequencyEstimate() = default;

    virtual void setBlockFrequencies() = 0;

protected:
    FrequencyEstimate() = default;
};

JS_EXPORT_PRIVATE bool estimateFrequencies(Procedure&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
