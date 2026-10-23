/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWire.h"
#include "CPUFeatureSet.h"
#include "JSExportMacros.h"
#include <array>
#include <cstdint>

namespace JSC { namespace AOT {

JS_EXPORT_PRIVATE uint64_t imageStamp(CPUFeatureSet, const std::array<uint8_t, 16>& identity);
JS_EXPORT_PRIVATE uint64_t imageStamp(CPUFeatureSet, const std::array<uint8_t, 16>& identity, Wire::ImageForm);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
