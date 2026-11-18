/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTLinkFlow.h"

namespace JSC { namespace AOT { namespace Typed {

void seedCellsSharedWithUnseenCode(LinkFlow&);

void lookAtSummariesForLayoutsAndCells(const LinkFlow&, LinkTables&);
enum class LateFills : bool {
    FoundHere,
    FoundByOtherReading,
};
void writeLayoutsAndCellsAheadOfTypes(const LinkFlow&, BitVector&& functionsWhoseCallersAreAllKnown, LateFills);
void writeLayoutsAndCells(const LinkFlow&, TypeArena&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
