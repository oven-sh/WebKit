/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

class Facts;
class Program;
struct PromotedEnvironments;
struct SlotPlanParts;

void leaveClosuresOfLiteralsUnmade(const Program&, const Facts&, const PromotedEnvironments&, bool useWideReferences, SlotPlanParts&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
