/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include "CallFrame.h"
#include "LineColumn.h"
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

struct SiteDescription;

struct SourcePosition {
    friend bool operator==(const SourcePosition&, const SourcePosition&) = default;

    LineColumn lineColumn;
    ModuleID source;
};

static constexpr unsigned sitesBetweenPositionCheckpoints = 512;

void encodePositions(Vector<uint8_t>& stream, const SiteDescription&);
std::optional<SourcePosition> decodePosition(const Image&, uint32_t function, const uint8_t* positions, CallSiteIndex);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
