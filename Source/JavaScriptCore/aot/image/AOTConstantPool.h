/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkedFunctions.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

struct ConstantPool {
    Vector<uint8_t> bytes;
    Vector<Vector<uint32_t, 0>> constantOffsetsByBody;
    Vector<Vector<uint32_t, 0>> jumpTableOffsetsByBody;
};

ConstantPool buildConstantPool(const LinkedFunctions&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
