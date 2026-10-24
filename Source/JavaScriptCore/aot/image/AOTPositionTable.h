/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include "AOTVector.h"
#include "CallFrame.h"
#include "LineColumn.h"
#include <optional>

namespace JSC { namespace AOT {

struct SiteDescription;

struct SourcePosition {
    friend bool operator==(const SourcePosition&, const SourcePosition&) = default;

    LineColumn lineColumn;
    ModuleID source;
};

static constexpr unsigned sitesBetweenPositionCheckpoints = 512;

struct OwnPosition {
    static OwnPosition of(const SiteDescription&);

    uint32_t line { 0 };
    uint32_t source { 0 };
};

void encodePositions(Vector<uint8_t>& stream, const SiteDescription&, OwnPosition ofFunctionInFront);
std::optional<SourcePosition> decodePosition(const Image&, uint32_t function, const uint8_t* entry, OwnPosition ofFunctionInFront, CallSiteIndex);
void forgetRememberedPositions();

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
