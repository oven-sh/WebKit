/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

class Facts;
class Program;
struct SlotPlanParts;

// planProgram(), behind the forms of closures and in front of SlotPlan(), the key orders and the shapes.
void leaveClosuresOfLiteralsUnmade(const Program&, const Facts&, bool useWideReferences, SlotPlanParts&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
