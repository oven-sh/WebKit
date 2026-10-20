/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsObjectCreation.h"

#if ENABLE(AOT)

#include "AOTStubHelpers.h"

namespace JSC { namespace AOT {

void emitStoreValuesAndClear(StubAssembler& jit, unsigned firstValueArgument, unsigned count, unsigned end, GPRReg base, size_t offset)
{
    auto at = [&](unsigned i) { return static_cast<int32_t>(offset + i * sizeof(EncodedJSValue)); };
#if CPU(ARM64)
    auto word = [&](unsigned i) { return i < count ? GPRInfo::toArgumentRegister(firstValueArgument + i) : ARM64Registers::zr; };
    unsigned i = 0;
    for (; i + 1 < end; i += 2)
        jit.storePair64(word(i), word(i + 1), base, TrustedImm32(at(i)));
    if (i < end)
        jit.store64(word(i), Address(base, at(i)));
#else
    for (unsigned i = 0; i < count; ++i)
        jit.store64(GPRInfo::toArgumentRegister(firstValueArgument + i), Address(base, at(i)));
    for (unsigned i = count; i < end; ++i)
        jit.store64(TrustedImm32(0), Address(base, at(i)));
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
