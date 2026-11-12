/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstddef>
#include <cstdint>
#include <span>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

struct HostFunctionSources {
    void* owner { nullptr };
    size_t (*size)(void* owner, uint32_t module) { nullptr };
    void (*copy)(void* owner, uint32_t module, uint8_t* destination) { nullptr };
    size_t (*maxCompressedSize)(size_t sourceSize) { nullptr };
    size_t (*compress)(std::span<uint8_t> compressed, std::span<const uint8_t> text) { nullptr };
    const uint8_t* (*view)(void* owner, uint32_t module) { nullptr };
    void (*release)(void* owner) { nullptr };
};

struct HostStringEncoders {
    using AppendBytes = void (*)(void* sink, std::span<const uint8_t>);
    bool (*encodeForBuffer)(StringView string, StringView encoding, void* sink, AppendBytes) { nullptr };
};
using DecompressSourceFunction = bool (*)(std::span<uint8_t> text, std::span<const uint8_t> compressed);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
