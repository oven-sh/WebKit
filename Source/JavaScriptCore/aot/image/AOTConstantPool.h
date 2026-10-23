/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkedFunctions.h"
#include "AOTVector.h"

namespace JSC { namespace AOT {

struct ConstantRange {
    uint32_t offset;
    uint32_t size;
};

// What the pool begins with, as it is. A constant of a body that equals one of the parts is that part. Each part is aligned as a constant of its size is.
struct SharedConstants {
    std::span<const uint8_t> bytes;
    std::span<const ConstantRange> parts;
};

struct ConstantPool {
    Vector<uint8_t> bytes;
    uint32_t sizeOfSharedConstants { 0 };
    Vector<Vector<uint32_t, 0>> constantOffsetsByBody;
    Vector<Vector<uint32_t, 0>> jumpTableOffsetsByBody;
};

ConstantPool buildConstantPool(const LinkedFunctions&, BuildContext&, const SharedConstants& = { });

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
