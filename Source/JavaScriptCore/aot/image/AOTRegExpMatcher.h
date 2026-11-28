/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTRegExpCompiler.h"
#include "AOTRegExpMatchPlan.h"
#include <memory>

namespace JSC::AOT {

inline constexpr uint32_t maxLengthSearchedByMatcher = 64;

std::unique_ptr<RegExpCode> compileRegExpMatcher(const RegExpMatchPlan&, bool recordsMatches, const AssemblerPolicy&, BuildContext*);

std::unique_ptr<RegExpCode> createRegExpMatcher(const RegExpCode&, uint32_t record);

} // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
