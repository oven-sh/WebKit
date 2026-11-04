/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <memory>

namespace JSC { namespace AOT { namespace Typed {

struct KeptForUnitOfCoroutines {
    virtual ~KeptForUnitOfCoroutines() = default;
};

// What the files AOTWalkCoroutine*.cpp keep while one unit is walked. Walker::beginUnit() puts a new one in its place. It is no part of WalkState: nothing here is copied on an edge or met at a join.
struct CoroutineStateOfUnit {
    // A suspension was built for the unit. It only ever turns true. Writer: noteSuspensionOfCoroutine().
    bool hasBuiltSuspension { false };
    // Each is made, cast and read by its package alone. Null: the package has kept nothing yet.
    std::unique_ptr<KeptForUnitOfCoroutines> keptByPromises; // coroutines-2
    std::unique_ptr<KeptForUnitOfCoroutines> keptByCalls; // coroutines-3
    std::unique_ptr<KeptForUnitOfCoroutines> keptByGenerators; // coroutines-4
    std::unique_ptr<KeptForUnitOfCoroutines> keptByFrames; // coroutines-5
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
