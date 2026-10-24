/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace JSC { namespace AOT {

class BuildContext;

enum class RemappedIDKind : uint32_t {
    String,
    PropertyKey,
    Type,
    TypeSite,
    ExpectedResult,
    Layout,
    Variable,
    Module,
    ModuleFailure,
    EvalScope,
    Function,
    Data,
};
constexpr size_t numberOfRemappedIDKinds = 12;

// Indexed by unit-local id. An entry is the program id plus one, or zero if the linker dropped the item.
struct IDRemapTable {
    const uint32_t* entries;
    size_t size;
};

struct UnitIDRemap {
    std::array<IDRemapTable, numberOfRemappedIDKinds> tables;
};

struct ExternalFunctionBody {
    const uint32_t* code;
    uint32_t wordCount;
    uint32_t unitIndex;
};

// The code of each function lies outside the program's bytes. Code and record of a function hold the ids of its unit, but for the record's function types.
struct ExternalFunctionBodies {
    const ExternalFunctionBody* bodies;
    size_t bodyCount;
    const UnitIDRemap* units;
    size_t unitCount;
};

static_assert(sizeof(IDRemapTable) == 16 && sizeof(UnitIDRemap) == 16 * numberOfRemappedIDKinds && sizeof(ExternalFunctionBody) == 16 && sizeof(ExternalFunctionBodies) == 32);

namespace Typed {

class Program;

std::unique_ptr<Program> readProgramWithExternalBodies(std::span<const uint8_t> bytes, const ExternalFunctionBodies*, BuildContext&);

} // namespace Typed

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
