/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModuleData.h"
#include "JSExportMacros.h"
#include <span>

namespace JSC { namespace AOT {

JS_EXPORT_PRIVATE void computeModuleFlags(std::span<ModuleData> modulesAndEndRecord, std::span<const ModuleRequestData> requests);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
