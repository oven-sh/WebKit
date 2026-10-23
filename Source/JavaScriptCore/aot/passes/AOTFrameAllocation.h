/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTVector.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Forward.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
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

    bool mayBeInFrame(LayoutID layout) const { RELEASE_ASSERT(m_isFinished); return m_layoutsInFrames.get(layout.index()); }
    bool isOnlyInFrames(LayoutID layout) const { return mayBeInFrame(layout) && !m_layoutsOnHeap.get(layout.index()); }
    bool mayBeInFrame(FunctionID closureFunction) const { RELEASE_ASSERT(m_isFinished); return m_closuresInFrames.get(closureFunction.index()); }
    bool isOnlyInFrames(FunctionID closureFunction) const { return mayBeInFrame(closureFunction) && !m_closuresOnHeap.get(closureFunction.index()); }

    bool mayHoldFramePointer(FieldReference) const;
    bool mayHoldFramePointer(CaptureReference) const;
    std::span<const FieldReference> fieldsHoldingFramePointers() const LIFETIME_BOUND { return m_fieldsHoldingFramePointers.span(); }
    std::span<const CaptureReference> capturesHoldingFramePointers() const LIFETIME_BOUND { return m_capturesHoldingFramePointers.span(); }

    void addCell(LayoutID, AllocationStorage);
    void addClosure(FunctionID closureFunction, AllocationStorage);
    void addHolder(FieldReference);
    void addHolder(CaptureReference);
    void merge(const FrameAllocation&);
    void finish();

    void dump(PrintStream&, const Program&) const;

private:
    template<typename IDType>
    struct PendingStorage {
        IDType id;
        AllocationStorage storage { AllocationStorage::Heap };
    };

    Vector<PendingStorage<LayoutID>> m_pendingCells;
    Vector<PendingStorage<FunctionID>> m_pendingClosures;
    BitVector m_layoutsInFrames;
    BitVector m_layoutsOnHeap;
    BitVector m_closuresInFrames;
    BitVector m_closuresOnHeap;
    Vector<FieldReference> m_fieldsHoldingFramePointers;
    Vector<CaptureReference> m_capturesHoldingFramePointers;
    bool m_isFinished { true };
};

// The flow results are those of the inputs. Their table of borrowed parameters grows by what the plans put into frames.
FrameAllocation allocateInFrames(ProgramEditor&, const OptimizationInputs&, FlowResults&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
