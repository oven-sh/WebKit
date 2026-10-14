/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/ScopedLambda.h>
#include <wtf/Threading.h>

namespace JSC { namespace AOT { namespace Typed {

// What is done for one index writes nothing that is read or written for another, so the outcome does not depend on the threads.
// A helper that has not woken by the time the indices are gone is not waited for.
void forEachIndexInParallel(unsigned count, NOESCAPE const ScopedLambda<void(unsigned)>&);

bool saysClocks();

template<typename T>
void destroyBeside(T&& value)
{
    static_assert(!std::is_lvalue_reference_v<T>);
    Thread::create("AOT destroyer"_s, [value = WTF::move(value)]() mutable {
        T gone = WTF::move(value);
    })->detach();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
