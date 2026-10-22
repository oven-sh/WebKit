/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
class Heat;
class Program;
class ProgramEditor;
struct FieldReference;

// `{ b: 1, c: { d: 2, e: 3 } }` makes two objects. Where every object of the outer layout is born with a fresh object in `c`, nothing ever stores to `c` again, and all that anybody does
// with what a read of `c` gives is to read and store its fields, nobody can tell whether the inner object exists. So it does not: its fields are fields of the outer layout, under private names,
// `a.c.d` is one read from `a`, and the outer object is the only allocation. No pointer into a cell comes of it, so the collector and the run time see an ordinary object.
// Where that is not proven the field stays, with a slot that says whether the inner object was made. It is born in the outer object all the same. An access tests the slot.
// Whoever wants the inner object as a value, in compiled code or in the run time, makes it first, out of the fields in the outer object, and from then on the slot holds it.
// A store of another object is a store like any other. So nothing has to be known about who reads, stores or holds.
// Whatever else there is stays two objects, which AllocateInnerObjects may still give one cell.
void flattenInnerObjects(ProgramEditor&, const FlowResults&, const Heat&, BuildContext&);

// Whether the layouts allow it: for whoever would part the two allocations before the pass has seen them.
bool mayLieInOuterObject(const Program&, FieldReference fieldOfOuterLayout, LayoutID innerLayout);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
