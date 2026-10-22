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

// `{ b: 1, c: { d: 2, e: 3 } }` makes two objects.
void flattenInnerObjects(ProgramEditor&, const FlowResults&, const Heat&, BuildContext&);

// Whether the layouts allow it: for whoever would part the two allocations before the pass has seen them.
bool mayLieInOuterObject(const Program&, FieldReference fieldOfOuterLayout, LayoutID innerLayout);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
