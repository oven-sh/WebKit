/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPass.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet constructionAllocations { Opcode::NewArrayLike, Opcode::NewArrayWithCapacity };
inline constexpr OpcodeSet arrayCapacityOpcodes = constructionAllocations | OpcodeSet { Opcode::NewArray, Opcode::AppendElement, Opcode::CallBuiltin, Opcode::HasArrayStorage };

struct MeasuredStorage {
    ValueID owner;
    ValueID guard;
    AbstractHeap heap;
};

std::optional<MeasuredStorage> storageMeasuredBy(const Program&, const Value& lengthRead);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
