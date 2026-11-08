/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include <span>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Facts;
class Program;
struct SlotPlanParts;

struct EnvironmentsHandedBack {
    Vector<LayoutID> all;
    Vector<LayoutID> takenByClosuresMadeForCallsAlone;
};

EnvironmentsHandedBack findEnvironmentsHandedBack(const Program&, const Facts&, BuildContext&);
void layEnvironmentsHandedBack(BuildContext&, std::span<const LayoutID>, SlotPlanParts&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
