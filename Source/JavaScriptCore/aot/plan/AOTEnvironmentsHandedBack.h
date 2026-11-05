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

void planEnvironmentsHandedBack(const Program&, const Facts&, BuildContext&, SlotPlanParts&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
