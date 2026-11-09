/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstddef>
#include <cstdint>
#include <span>

namespace JSC { namespace AOT {

struct HostFunctionTexts {
    void* owner { nullptr };
    size_t (*size)(void* owner, uint32_t module) { nullptr };
    void (*copy)(void* owner, uint32_t module, uint8_t* destination) { nullptr };
    size_t (*mostBytesCompressed)(size_t sizeOfText) { nullptr };
    size_t (*compress)(std::span<uint8_t> compressed, std::span<const uint8_t> text) { nullptr };
};

using DecompressTextFunction = bool (*)(std::span<uint8_t> text, std::span<const uint8_t> compressed);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
