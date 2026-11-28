/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWireEnums.h"
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

    uint64_t magic;
    uint32_t size;
    uint32_t numberOfFacts;
    uint64_t cpuFeatures;
    uint8_t architecture;
    uint8_t platform;
    uint8_t imageForm;
    uint8_t checks;
    uint32_t imageFormatVersion;
};
static_assert(sizeof(RuntimeDescription) == 32 && offsetof(RuntimeDescription, imageForm) == 26 && offsetof(RuntimeDescription, imageFormatVersion) == 28, "whoever reads an executable without this header finds the members at these places");

struct BuiltinTableDescription {
    static constexpr uint64_t expectedMagic = 0x4254544f414e5542ULL;

    uint64_t magic { expectedMagic };
    uint32_t size { sizeof(BuiltinTableDescription) };
    uint32_t hashOfBuiltinTokens;
    uint32_t hashOfTable;
    uint32_t hashOfHostBindings;
};
static_assert(sizeof(BuiltinTableDescription) == 24);

#if OS(DARWIN)
#define AOT_IN_RUNTIME_DESCRIPTION_SECTION __attribute__((section("__TEXT,__bun_aot")))
#define AOT_KEPT_BY_EVERY_LINKER __attribute__((used))
#elif OS(WINDOWS)
#define AOT_IN_RUNTIME_DESCRIPTION_SECTION __attribute__((section(".bunaot")))
#define AOT_KEPT_BY_EVERY_LINKER __attribute__((used))
#else
#define AOT_IN_RUNTIME_DESCRIPTION_SECTION __attribute__((section(".bun_aot")))
#define AOT_KEPT_BY_EVERY_LINKER __attribute__((used, retain))
#endif

extern "C" AOT_IN_RUNTIME_DESCRIPTION_SECTION const BuiltinTableDescription describedBuiltinTable;

struct RuntimeDifference {
    ASCIILiteral name;
    uint64_t here { 0 };
    uint64_t there { 0 };
};

JS_EXPORT_PRIVATE std::span<const uint8_t> descriptionOfThisRuntime();
JS_EXPORT_PRIVATE std::span<const ASCIILiteral> namesOfRuntimeFacts();
JS_EXPORT_PRIVATE std::optional<RuntimeDifference> findRuntimeDifference(std::span<const uint8_t> other);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
