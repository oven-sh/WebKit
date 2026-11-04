/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTLayout.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class Walker;

// `{ kind, pos, end, flags: 0 }` with a `pos` that nothing has tested. What the allocation lists in the place of a value that is not proven to fit its slot: a constant of the slot's type.
// The key is listed where it is written, the fields behind it stay listed, and the value is stored behind the allocation by an operation that tests it.
// A value of the type comes before a marker: with undefined or null in a raw slot a field of the birth is born marked.
// Nothing: no constant fits, or no store can fill the slot behind the allocation. Then the list of the allocation ends in front of the field.
std::optional<Draft> fillerThatFitsSlot(Walker&, FieldReference);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
