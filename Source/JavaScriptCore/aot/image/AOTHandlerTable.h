/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "CallFrame.h"
#include "HandlerInfo.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC::AOT {

struct SiteHandler;

struct HandlerRecord {
    friend bool operator==(const HandlerRecord&, const HandlerRecord&) = default;

    uint32_t codeOffset;
    HandlerType type;
};

void encodeHandlers(Vector<uint8_t>& bytes, std::span<const SiteHandler>);
std::optional<HandlerRecord> findHandler(std::span<const uint8_t> handlers, CallSiteIndex);

} // namespace JSC::AOT

#endif // ENABLE(AOT)
