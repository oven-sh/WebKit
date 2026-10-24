/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <type_traits>

namespace JSC { namespace AOT {

class StringBuilderStorage;

struct StringBuilderWords {
    StringBuilderStorage* freeStorage;
    bool isPoolRegistered;
};

static_assert(std::is_trivially_default_constructible_v<StringBuilderWords>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
