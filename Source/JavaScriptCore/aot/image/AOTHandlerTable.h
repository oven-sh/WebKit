/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "CallFrame.h"
#include "HandlerInfo.h"
#include <optional>
#include <span>

namespace JSC::AOT {

class Image;
struct SiteHandler;

// Where a call goes on when its callee returns a value that is not of the type it promises (PromisedResults). No exception is caught there. Compiled code has no use for the type that the engine gives this number.
inline constexpr HandlerType landingOfMisfit = HandlerType::SynthesizedFinally;

struct HandlerRecord {
    friend bool operator==(const HandlerRecord&, const HandlerRecord&) = default;

    // No operation of the handler takes what was thrown: `catch {}`, or a binding that nothing reads.
    constexpr bool ignoresValue() const { return type == HandlerType::SynthesizedCatch; }
    constexpr bool catchesExceptions() const { return type != landingOfMisfit; }

    uint32_t codeOffset;
    HandlerType type;
};

void encodeHandlers(Vector<uint8_t>& bytes, std::span<const SiteHandler>);
std::optional<HandlerRecord> findHandler(std::span<const uint8_t> handlers, CallSiteIndex);
bool catchesExceptions(const Image&, uint32_t function);

} // namespace JSC::AOT

#endif // ENABLE(AOT)
