/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionDescription.h"

namespace JSC::AOT {

class ShownFrameFilter {
public:
    explicit ShownFrameFilter(bool hasShownFrame = false)
        : m_hasShownFrame(hasShownFrame)
    {
    }

    bool shows(TraceVisibility visibility, bool isTailDeleted)
    {
        bool isShown = !isTailDeleted && (visibility == TraceVisibility::Visible || (visibility == TraceVisibility::VisibleOnlyOnTop && !m_hasShownFrame));
        m_hasShownFrame |= isShown;
        return isShown;
    }

    bool hasShownFrame() const { return m_hasShownFrame; }

private:
    bool m_hasShownFrame;
};

} // namespace JSC::AOT

#endif // ENABLE(AOT)
