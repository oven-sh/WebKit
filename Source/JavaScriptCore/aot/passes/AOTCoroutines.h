/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTPass.h"

namespace JSC { namespace AOT { namespace Typed {

class PassRunner;

void splitCoroutines(ProgramEditor&, const OptimizationInputs&, const PassRunner&);

inline const CreatorRecord::Coroutine* coroutineOf(const Function& rampOrBody)
{
    return rampOrBody.creator.tryAs<CreatorRecord::Coroutine>();
}

inline const CreatorRecord::Coroutine& coroutineOfBody(const Function& body)
{
    RELEASE_ASSERT(body.role == FunctionRole::CoroutineBody);
    return body.creator.as<CreatorRecord::Coroutine>();
}

// The promise of an async function (undefined when the caller awaits the call itself), or the generator object.
inline ValueID driverOf(const Function& body)
{
    return body.at(BlockID(0u)).parameters[*coroutineOfBody(body).promiseParameter];
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
