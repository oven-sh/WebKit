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
    uint8_t imageForm;
    uint8_t checks;
    // The one ImageHeader::formatVersion that this run time takes. A run time from before this member has 0 here.
    uint32_t imageFormatVersion;
};
static_assert(sizeof(RuntimeDescription) == 32 && offsetof(RuntimeDescription, imageForm) == 26 && offsetof(RuntimeDescription, imageFormatVersion) == 28, "whoever reads an executable without this header finds the members at these places");

// The table of built-ins is compiled with the program that embeds the engine. It describes itself in a record of its own in the same section.
struct BuiltinTableDescription {
    static constexpr uint64_t expectedMagic = 0x4254544f414e5542ULL;

    uint64_t magic { expectedMagic };
    uint32_t size { sizeof(BuiltinTableDescription) };
    uint32_t hashOfBuiltinTokens;
    uint32_t numberOfBuiltins;
    uint32_t numberOfOverloads;
    uint32_t numberOfTypes;
    uint32_t numberOfClasses;
    uint32_t numberOfSlots;
    uint32_t numberOfLayouts;
    uint32_t numberOfModules;
    uint32_t numberOfFunctions;
};
static_assert(sizeof(BuiltinTableDescription) == 48);

// A declaration says the section and the definition says that no linker leaves it out.
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
// One per fact, in the order of the facts after the header of the description.
JS_EXPORT_PRIVATE std::span<const ASCIILiteral> namesOfRuntimeFacts();
// Takes the record of the engine or the record of the table.
JS_EXPORT_PRIVATE std::optional<RuntimeDifference> findDifferenceFromThisRuntime(std::span<const uint8_t> other);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
