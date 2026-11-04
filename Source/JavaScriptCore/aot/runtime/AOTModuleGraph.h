/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModuleData.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <span>

namespace JSC { namespace AOT {

JS_EXPORT_PRIVATE void computeModuleFlags(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests);

JS_EXPORT_PRIVATE Vector<LinkPlanStepData> planLinkOfEntryPoint(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests, ModuleID entryPoint);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
