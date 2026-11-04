/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <optional>
#include <wtf/HashSet.h>
#include <wtf/HashTraits.h>

namespace JSC { namespace AOT { namespace Typed {

// What AOTWalkLoopsOverGenerators.cpp keeps while one unit is walked. Walker::beginUnit() puts a new one in its place. It is no part of WalkState: nothing here is copied on an edge or met at a join.
struct LoopsOverGeneratorsOfUnit {
    // Reference::bits() of what the IteratorOpen of such a loop is bound to.
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> states;
    std::optional<bool> engineAnswers;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
