/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

void installFaultHandler();

#if CPU(X86_64)
// What the scratch register held at the last null check that faulted on this thread. The handler passes the return address in that register,
// and compiled code allocates it like any other, so the tested value may have been in it.
uint64_t scratchRegisterBeforeFault();
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
