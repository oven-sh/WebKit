/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

std::optional<CoroutineKind> kindOfSuspendingBody(Walker&, Reference& driver);
Reference buildSuspensionInCoroutine(Walker&, Draft&& suspension);

bool isProvenToHaveNoThen(const Program&, const Facts&, TypeArena&, TypeID);
bool isProvenToHaveNoThen(Walker&, Reference);
Reference buildTestForNoThen(Walker&, Reference value, Origin);

void noteSuspensionOfCoroutine(Walker&, Draft& suspension);
bool replaceSuspensionOfCoroutine(Walker&, const BodyValue&);
Reference buildAwaitOfWhatHasNoThen(Walker&, Reference awaited, std::span<const Reference> held, bool takesExtraTurn, Origin);
void resolvePromiseThatNothingHasTaken(Walker&, Reference promise, Reference value, bool mayTest, Origin);
bool buildPromiseOperation(Walker&, const BodyValue&);
Reference valueOfPromiseFulfilledHere(Walker&, Reference promise);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
