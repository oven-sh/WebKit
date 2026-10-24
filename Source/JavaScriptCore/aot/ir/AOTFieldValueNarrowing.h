/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTResultRules.h"

namespace JSC { namespace AOT { namespace Typed {

// The type of `holder` once a read of `key` from it has returned a value of type `read`.
Type narrowHolderByFieldValue(const TypingContext&, const Type& holder, PropertyKeyID, const Type& read);
// True where nothing that `holder` may be can have returned such a value, so the narrowing is never reached. narrowHolderByFieldValue() gives the type of the holder there.
bool narrowsHolderToNothing(const TypingContext&, const Type& holder, PropertyKeyID, const Type& read);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
