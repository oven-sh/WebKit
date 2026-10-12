/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

// What is done for one index writes nothing that is read or written for another, so the outcome does not depend on the threads.
// A helper that has not woken by the time the indices are gone is not waited for.
void forEachIndexInParallel(unsigned count, NOESCAPE const ScopedLambda<void(unsigned)>&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
