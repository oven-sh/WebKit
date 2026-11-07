/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTTypeIDs.h"

namespace JSC { namespace AOT { namespace Typed {

struct LinkedCoroutine {
    FunctionID function;
    TypeID fulfilledWith;
    TypeID returned;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
