/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <array>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC::AOT {

class BuildContext;
class LinkedFunctions;

enum class FaultSiteKind : uint8_t {
    NullCheck,
    LoopPoll,
};

struct FaultSite {
    static constexpr uintptr_t reach = 16 * KB;
    static constexpr size_t maxThrowSiteWords = 3;

    FaultSiteKind kind;
    bool hasFrame;
    std::array<uint32_t, maxThrowSiteWords> throwSite;
};

Vector<uint8_t> encodeFaultSites(const LinkedFunctions&, std::span<const uint32_t> bodyStarts, BuildContext&);

} // namespace JSC::AOT

#endif // ENABLE(AOT)
