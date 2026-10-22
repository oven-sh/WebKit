/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

void installFaultHandler();

#if CPU(X86_64)
// What the scratch register held at the last null check that faulted on this thread.
uint64_t scratchRegisterBeforeFault();
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
