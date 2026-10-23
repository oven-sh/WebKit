/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowApplication.h"
#include "AOTVector.h"

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

FlowAccessSiteNumbering numberAccessSites(const Program&, BuildContext&);
Vector<AccessSiteID> accessSitesByValue(const Function&, const FlowAccessSiteNumbering&, FunctionID functionAsRead);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
