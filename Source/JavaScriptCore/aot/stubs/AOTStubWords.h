/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubFrames.h"
#include <bit>
#include <type_traits>
#include <wtf/Assertions.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

struct BoxedCallEntry {
    static constexpr unsigned minArgumentCount = 0;
    static constexpr unsigned maxArgumentCount = 4;
    static constexpr unsigned sizeShift = 6;

    static constexpr ptrdiff_t offsetOfNativeFunction() { return 0; }
    static constexpr ptrdiff_t offsetOfTarget(unsigned argumentCount)
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(argumentCount >= minArgumentCount && argumentCount <= maxArgumentCount);
        return static_cast<ptrdiff_t>((1 + argumentCount - minArgumentCount) * sizeof(void*));
    }

    const void* nativeFunction;
    const void* targets[maxArgumentCount - minArgumentCount + 1];
    const void* unused[2];
};
static_assert(sizeof(BoxedCallEntry) == 1u << BoxedCallEntry::sizeShift);
static_assert(OBJECT_OFFSETOF(BoxedCallEntry, nativeFunction) == BoxedCallEntry::offsetOfNativeFunction());
static_assert(OBJECT_OFFSETOF(BoxedCallEntry, targets) == BoxedCallEntry::offsetOfTarget(BoxedCallEntry::minArgumentCount));

struct StubWords {
    static constexpr unsigned numberOfBoxedCallEntries = 64;
    static constexpr unsigned boxedCallHashShift = 4;

    static constexpr unsigned indexOfBoxedCall(uintptr_t nativeFunctionBits) { return (nativeFunctionBits >> boxedCallHashShift) % numberOfBoxedCallEntries; }

    const void* returnAddress(StubFrameKind kind) const { return returnAddresses[static_cast<unsigned>(kind)]; }
    static constexpr ptrdiff_t offsetOfReturnAddress(StubFrameKind kind) { return static_cast<ptrdiff_t>(static_cast<unsigned>(kind) * sizeof(void*)); }

    const void* returnAddresses[numberOfStubFrameKinds];
#if !defined(BUN_HARDENED)
    BoxedCallEntry boxedCalls[numberOfBoxedCallEntries];
#endif
    const void* closureEntrypoint;
    const void* slimClosureEntrypoint;
};
static_assert(std::is_trivially_default_constructible_v<StubWords>);
static_assert(std::has_single_bit(StubWords::numberOfBoxedCallEntries));
static_assert(!OBJECT_OFFSETOF(StubWords, returnAddresses));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
