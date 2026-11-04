/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayout.h"

namespace JSC { namespace AOT { namespace Typed {

class LinkFlow;
class Program;
struct LinkTables;

// `function Counter() { }   Counter.prototype.n = 0;   c.n++`: a default lies above the objects, and the first store under the name gives an object a property of its own in front of it.

// Behind run(), beside WriterOfLinkLayouts::canNameAppendedField(), which refuses every name that lies above. All that code in sight stores above under the name are primitives, so the field hides no method,
// and no descriptor defines the name there, or a key that nobody knows, so no accessor takes the store. A LIST CLAIMS NOTHING: whoever finds the slot empty goes on upward.
bool namesFieldBelowPrimitivesLaidAbove(const LinkFlow&, LayoutID, PropertyKeyID);

// LinkTables::namesOnPrototypes has the name for a constructor function at or above the layout. For the plan, which lays such a field in the cell, and for the walk, which takes its empty slot for no answer.
bool isLaidOnPrototypeAbove(const Program&, const LinkTables&, LayoutID, PropertyKeyID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
