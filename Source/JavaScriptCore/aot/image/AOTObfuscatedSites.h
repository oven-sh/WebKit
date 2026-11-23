/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include "AOTImageFormat.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class LinkedFunctions;

struct ObfuscatedSites {
    Vector<Wire::ObfuscatedSiteRecord> records;
    Vector<uint8_t> names;
};

ObfuscatedSites collectObfuscatedSites(const LinkedFunctions&, const Typed::Program&, BuildContext&);
Vector<ModuleOfRuntimeFunctions> listModulesOfRuntimeFunctions(const Typed::Program&, const Typed::CallingConventions&, const BitVector& functionsWithFrames);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
