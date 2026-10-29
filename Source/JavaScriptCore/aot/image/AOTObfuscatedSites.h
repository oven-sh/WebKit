/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyFactsForTables.h"
#include "AOTImageForward.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include <span>

namespace JSC { namespace AOT {

class LinkedFunctions;

struct ObfuscatedSites {
    Vector<Wire::ObfuscatedSiteRecord> records;
    Vector<uint8_t> names;
};

// A place of a tail call is listed as the one site of a function of its own, behind the functions that have code: ProgramTables::placesOfTailCalls.
ObfuscatedSites collectObfuscatedSites(const LinkedFunctions&, const Typed::Program&, BuildContext&, std::span<const Typed::WrittenTailCall> = { });

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
