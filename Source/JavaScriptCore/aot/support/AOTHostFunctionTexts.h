/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstddef>
#include <cstdint>

namespace JSC { namespace AOT {

// The host keeps the text of a module that it has left out of the program, and the build asks for it where it lays out the text of functions.
struct HostFunctionTexts {
    void* owner { nullptr };
    size_t (*size)(void* owner, uint32_t module) { nullptr };
    void (*copy)(void* owner, uint32_t module, uint8_t* destination) { nullptr };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
