/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>
#include "AOTBitVector.h"
#include <wtf/FastMalloc.h>
#include <wtf/Forward.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
struct FramePlan;
struct OptimizationInputs;

struct CaptureReference {
    FunctionID function;
    uint32_t capture { 0 };

    friend bool operator==(const CaptureReference&, const CaptureReference&) = default;
    friend auto operator<=>(const CaptureReference&, const CaptureReference&) = default;
};

class FrameAllocation {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FrameAllocation);
public:
    FrameAllocation() = default;
    FrameAllocation(const Program&, std::span<const FramePlan>);

    bool mayBeInFrame(LayoutID layout) const { return m_layoutsInFrames.get(layout.index()); }
    bool isOnlyInFrames(LayoutID layout) const { return mayBeInFrame(layout) && !m_layoutsOnHeap.get(layout.index()); }
    bool mayBeInFrame(FunctionID closureFunction) const { return m_closuresInFrames.get(closureFunction.index()); }
    bool isOnlyInFrames(FunctionID closureFunction) const { return mayBeInFrame(closureFunction) && !m_closuresOnHeap.get(closureFunction.index()); }
    bool mayHoldFramePointer(FieldReference field) const { return std::ranges::binary_search(m_fieldsHoldingFramePointers, field); }
    bool mayHoldFramePointer(CaptureReference capture) const { return std::ranges::binary_search(m_capturesHoldingFramePointers, capture); }

    void dump(PrintStream&, const Program&) const;

private:
    BitVector m_layoutsInFrames;
    BitVector m_layoutsOnHeap;
    BitVector m_closuresInFrames;
    BitVector m_closuresOnHeap;
    Vector<FieldReference> m_fieldsHoldingFramePointers;
    Vector<CaptureReference> m_capturesHoldingFramePointers;
};

// The flow results are those of the inputs. Their table of borrowed parameters grows by what the plans put into frames.
FrameAllocation allocateInFrames(ProgramEditor&, const OptimizationInputs&, FlowResults&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
