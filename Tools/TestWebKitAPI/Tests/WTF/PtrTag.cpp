/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include <wtf/PtrTag.h>

#include <wtf/Compiler.h>

namespace TestWebKitAPI {

static int s_variable;

TEST(WTF_PtrTag, RemoveReturnAddressSignatureKeepsAddressWithoutSignature)
{
    void* address = &s_variable;
    EXPECT_EQ(address, removeReturnAddressSignature(address));
    EXPECT_EQ(std::bit_cast<uintptr_t>(address), removeReturnAddressSignature<uintptr_t>(address));
    EXPECT_EQ(nullptr, removeReturnAddressSignature(static_cast<void*>(nullptr)));
}

#if CPU(ARM64) && OS(LINUX)
struct ReturnAddresses {
    void* saved;
    void* actual;
};

static NEVER_INLINE void clobberLinkRegister()
{
    asm volatile("" : : : "memory");
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

// Signs the return address that it saves, whatever this file is compiled with. A processor without pointer authentication saves it as it is.
__attribute__((target("branch-protection=pac-ret")))
static NEVER_INLINE SUPPRESS_ASAN ReturnAddresses returnAddressesOfThisFunction()
{
    clobberLinkRegister();
    return { static_cast<void**>(__builtin_frame_address(0))[1], __builtin_extract_return_addr(__builtin_return_address(0)) };
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

TEST(WTF_PtrTag, RemoveReturnAddressSignatureOfSavedReturnAddress)
{
    ReturnAddresses addresses = returnAddressesOfThisFunction();
    EXPECT_EQ(addresses.actual, removeReturnAddressSignature(addresses.saved));
}
#endif

} // namespace TestWebKitAPI
