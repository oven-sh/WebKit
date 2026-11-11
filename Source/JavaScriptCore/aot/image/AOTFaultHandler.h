/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"
#include <cstdint>
#include <wtf/PlatformRegisters.h>

namespace JSC { namespace AOT {

void installFaultHandler();

void takeFaultsOfThisThreadOnAnotherStack();

JS_EXPORT_PRIVATE bool handleAccessFault(void* faultingAddress, PlatformRegisters&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
