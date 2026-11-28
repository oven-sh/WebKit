/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <wtf/Platform.h>

#if ENABLE(CODE_GENERATION)

#include "CCallHelpers.h"
#include <wtf/ForbidHeapAllocation.h>

namespace JSC { namespace AOT {

class ImageAssembler : public CCallHelpers {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    ImageAssembler()
    {
#if ENABLE(RETURN_ADDRESS_SIGNING)
        m_assembler.signLinkRegisterFromHereOn();
#endif
    }
};

} } // namespace JSC::AOT

#endif // ENABLE(CODE_GENERATION)
