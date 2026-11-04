/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Facts;
class Program;
struct SlotPlanParts;

// PlannedLayout::isEscapedOffset, and the size with it, of each layout of the kind Environment whose cells may be taken again: walk/AOTWalkEnvironmentCells.cpp.
// Nothing is proven: the byte says at run time whether a cell of the heap, a saved frame or a caller holds the environment.
void planEnvironmentsHandedBack(const Program&, const Facts&, BuildContext&, SlotPlanParts&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
