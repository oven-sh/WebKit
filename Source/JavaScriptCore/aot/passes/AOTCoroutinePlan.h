/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTCoroutines.h"
#include "AOTIRBuilder.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

struct CoroutineTypes {
    TypeID promise;
    TypeID promiseOrUndefined;
    TypeID generator;
    TypeID asyncGenerator;

    TypeID allocatedBy(CoroutineKind) const;
    TypeID driverParameterOf(CoroutineKind) const;
};

struct CoroutinePlan {
    CreatorRecord::Coroutine record;
    Function* bodySlot { nullptr };
    bool didNarrowResult { false };
    // Only an async function: the slot for its body takes a worker.
    bool neverSuspends { false };
};

Function buildCoroutineRamp(const AnalysisContext&, TypeArena&, const CoroutineTypes&, const Function& unsplit, CoroutinePlan&);
Function buildCoroutineBody(const AnalysisContext&, TypeArena&, const CoroutineTypes&, Function&& unsplit, const CoroutinePlan&);

Function buildRampOfWorker(const AnalysisContext&, TypeArena&, const CoroutineTypes&, const Function& unsplit, CoroutinePlan&);
Function buildAsyncWorker(const AnalysisContext&, TypeArena&, Function&& unsplit, const CoroutinePlan&);

inline TypeID CoroutineTypes::allocatedBy(CoroutineKind kind) const
{
    switch (kind) {
    case CoroutineKind::Generator:
        return generator;
    case CoroutineKind::AsyncFunction:
        return promise;
    case CoroutineKind::AsyncGenerator:
        return asyncGenerator;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

inline TypeID CoroutineTypes::driverParameterOf(CoroutineKind kind) const
{
    switch (kind) {
    case CoroutineKind::Generator:
        return generator;
    case CoroutineKind::AsyncFunction:
        return promiseOrUndefined;
    case CoroutineKind::AsyncGenerator:
        return asyncGenerator;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
