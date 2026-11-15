/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeLayout.h"
#include "AOTLinkedFunctions.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <cstdint>
#include <optional>
#include <span>

namespace JSC { namespace AOT {

class FunctionNumbers;

namespace Typed {
class Plan;
class Program;
}

struct OrderFile {
    static constexpr uint32_t noFile = UINT32_MAX - 1;
    static constexpr uint32_t wholeProgram = UINT32_MAX - 2;

    struct Unit {
        uint32_t file;
        uint32_t number;
        uint32_t rank;
        uint8_t kind;
        std::span<const uint8_t> hash;
        std::span<const uint8_t> name;
    };

    JS_EXPORT_PRIVATE static Vector<OrderFile> parse(std::span<const uint8_t>);

    Vector<std::span<const uint8_t>> files;
    Vector<Unit> units;
    std::optional<uint32_t> numberOfFunctions;
    uint32_t numberOfLines { 0 };
};

Vector<uint32_t> findListedUnits(const OrderFile&, const Typed::Program&, const Typed::Plan&, const FunctionNumbers&, const LinkedFunctions&);

struct OrderOfUnits {
    Vector<uint32_t> positions;
    uint32_t numberOfListed { 0 };
};

OrderOfUnits orderUnits(std::span<const uint32_t> listed, std::span<const FinalJump>, const LinkedFunctions&, size_t sizeOfPiece);
ResolvedCallees reorderCallees(const ResolvedCallees&, std::span<const uint32_t> order, std::span<const uint32_t> newPositions, const ParallelLoops&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
