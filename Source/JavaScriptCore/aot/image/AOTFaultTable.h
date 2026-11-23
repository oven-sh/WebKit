/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC::AOT {

class BuildContext;
class LinkedFunctions;

enum class FaultSiteKind : uint8_t {
    NullCheck,
    LoopPoll,
};

struct FaultSite {
    // How far from null, or from the start of the cell cage, an access may lie and stand for a null check. Every target leaves one page unmapped at address 0, and no
    // more than that can be counted on: WTF::Config::initialize() never puts lowestAccessibleAddress() below one page, and where /proc/sys/vm/mmap_min_addr says
    // 4096 (RHEL) a process may map the second page. A page has 4 KB or more.
    static constexpr uintptr_t reach = 4 * KB;
    static constexpr size_t maxThrowSiteWords = 3;

    FaultSiteKind kind;
    bool hasFrame;
    std::array<uint32_t, maxThrowSiteWords> throwSite;
};

Vector<uint8_t> encodeFaultSites(const LinkedFunctions&, std::span<const uint32_t> bodyStarts, BuildContext&);

} // namespace JSC::AOT

#endif // ENABLE(AOT)
