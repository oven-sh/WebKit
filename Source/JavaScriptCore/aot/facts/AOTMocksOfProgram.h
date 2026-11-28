/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTID.h"

namespace JSC { namespace AOT { namespace Typed {

class Program;

struct MocksOfProgram {
    BitVector modulesThatMayBeMocked;
    BitVector namespacesHandedOnByMockedModules;
    BitVector modulesReachingMocks;
    bool isAnyModuleMocked { false };
};
MocksOfProgram findMocksOfProgram(const Program&);

inline constexpr uint32_t nameThatModuleLacks = UINT32_MAX;
bool mayComeThroughMock(const Program&, const MocksOfProgram&, ModuleID exporter, uint32_t indexOfExport);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
