/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTResultRules.h"

namespace JSC { namespace AOT { namespace Typed {

Type narrowHolderByFieldValue(const TypingContext&, const Type& holder, PropertyKeyID, const Type& read);
bool narrowsHolderToNothing(const TypingContext&, const Type& holder, PropertyKeyID, const Type& read);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
