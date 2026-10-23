/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeTemplates.h"
#include "AOTRelocation.h"
#include "AOTVector.h"
#include <expected>
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

enum class UnitSharing : uint8_t {
    Unique,
    Copyable,
};

struct LinkUnit {
    std::span<const uint8_t> bytes;
    std::span<const Relocation> relocations;
    uint32_t alignment { 1 };
    UnitSharing sharing { UnitSharing::Unique };
};

struct UnitTarget {
    uint32_t unit;
    uint32_t offset { 0 };
};

struct PlacedUnit {
    uint32_t unit;
    uint32_t offset;
};

// uniqueUnitOffsets is indexed like SectionLinkInputs::units. The entry of a Copyable unit is 0: copies has its offsets.
struct LinkedSection {
    Vector<uint8_t> bytes;
    Vector<uint32_t> uniqueUnitOffsets;
    Vector<PlacedUnit> copies;
    uint32_t numberOfVeneers { 0 };
};

enum class SectionLinkError : uint8_t {
    OutOfPCRelativeReach,
    SymbolWithoutUnit,
    SectionTooLarge,
};

struct SectionLinkFailure {
    SectionLinkError error;
    uint32_t unit;
    uint32_t relocation;
};

using UnitResolver = ScopedLambda<std::optional<UnitTarget>(uint32_t unit, const Relocation&)>;

struct SectionLinkInputs {
    std::span<const LinkUnit> units;
    const UnitResolver& resolve;
    const CodeTemplates& templates;
    uint64_t maxSize;
};

JS_EXPORT_PRIVATE std::expected<LinkedSection, SectionLinkFailure> linkSection(const SectionLinkInputs&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
