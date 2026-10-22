/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class BorrowedParameters;
class FlowResults;
class FrameAllocation;

class EnvironmentReuse {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(EnvironmentReuse);
public:
    EnvironmentReuse() = default;

    bool isReused(LayoutID layout) const { return m_reusedLayouts.get(layout.index()); }

    void dump(PrintStream&, const Program&) const;

private:
    friend EnvironmentReuse reuseEnvironments(ProgramEditor&, const FlowResults&, const FrameAllocation&, BuildContext&, NOESCAPE const ScopedLambda<BorrowedParameters()>& computeBorrowedParameters);

    BitVector m_reusedLayouts;
};

EnvironmentReuse reuseEnvironments(ProgramEditor&, const FlowResults&, const FrameAllocation&, BuildContext&, NOESCAPE const ScopedLambda<BorrowedParameters()>& computeBorrowedParameters);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
