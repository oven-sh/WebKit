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
// Nothing comes to such a cast. It has the type of its holder, so no type says so: it stays until the code behind it is cut.
bool narrowsHolderToNothing(const TypingContext&, const Value&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
