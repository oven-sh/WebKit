/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <wtf/Platform.h>

#if ENABLE(B3_JIT)

#include <cstddef>

namespace JSC { namespace B3 {

class InsertionSet;
class Procedure;
class RelocatableValue;
class Value;

class RelocationLowering {
public:
    virtual ~RelocationLowering() = default;

    virtual Value* lower(Procedure&, InsertionSet&, size_t index, const RelocatableValue&) = 0;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
