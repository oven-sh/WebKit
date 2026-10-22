/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CallGraph;
class FlowResults;
class Heat;
class ProgramEditor;

// `{ b: 1, c: { d: 2, e: 3 } }` makes two objects. Where the objects of a layout are born with a fresh object in one of their fields, both get one cell: the inner object lies behind the
// fields of the outer one, with a header of its own, and the field holds a reference to it like any other field. So whoever holds the inner object holds an ordinary object, and a store
// to the field is an ordinary store. The inner object is made first, so its NewObject allocates the cell, and the NewObject of the outer object takes the cell from there.
void allocateInnerObjects(ProgramEditor&, const FlowResults&, const CallGraph&, const Heat&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
