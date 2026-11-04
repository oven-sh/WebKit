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

// Indexed by the number a unit gave a record. An entry is the program's number plus one, and zero where the program has no such record.
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

// One body for each function of the program's list, in its order. A function without a body has no words.
// A unit is the set of tables of one task of the checker. Which files share one, and the order of the units, follow the schedule of the front end: nothing orders or decides by a unit's index or by a number that a unit gave.
// unitCount is zero where code and records already hold the program's numbers. unitIndex is then zero everywhere.
// The engine reads code and tables in place until it calls release_program, which it does when the last function is compiled.
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
