/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

// For a program as read, before its values are typed. `Object.create(prototype)`, the callee being that built-in by binding, is `{ __proto__: prototype }` but for a prototype that is neither an object nor null, which it refuses.
// So it becomes that literal, with a layout of its own that has no field: the flow analysis gives the layout the names that it sees, as it does for `{}`.
// Where it gives none the literal makes an ordinary object: see computeLayoutsOfDictionaries().
void giveLayoutsToCreatedObjects(Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
