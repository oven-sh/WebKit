/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

struct RuntimeDescription {
    static constexpr uint64_t expectedMagic = 0x5452544f414e5542ULL;

    enum Check : uint8_t {
        Assertions = 1 << 0,
        AddressSanitizer = 1 << 1,
    };
    enum class ImageForm : uint8_t {
        Absolute,
        PositionIndependent,
    };
    enum class OperatingSystem : uint8_t {
        Linux,
        Darwin,
        Windows,
        FreeBSD,
        Other,
    };

    uint64_t magic;
    uint32_t size;
    uint32_t numberOfFacts;
    uint64_t cpuFeatures;
    uint8_t architecture;
    OperatingSystem operatingSystem;
    ImageForm imageForm;
    uint8_t checks;
    uint32_t reserved;
};
static_assert(sizeof(RuntimeDescription) == 32 && offsetof(RuntimeDescription, imageForm) == 26);

struct RuntimeDifference {
    ASCIILiteral name;
    uint64_t here { 0 };
    uint64_t there { 0 };
};

JS_EXPORT_PRIVATE std::span<const uint8_t> runtimeDescription();
JS_EXPORT_PRIVATE std::optional<RuntimeDifference> findDifferenceFromThisRuntime(std::span<const uint8_t> other);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
