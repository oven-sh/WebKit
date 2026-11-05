/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkFlow.h"

namespace JSC { namespace AOT { namespace Typed {

void seedCellsSharedWithUnseenCode(LinkFlow&);

void lookAtSummariesForLayoutsAndCells(const LinkFlow&, LinkTables&);
void writeLayoutsAndCellsAheadOfTypes(const LinkFlow&, BitVector&& functionsWhoseCallersAreAllKnown);
void writeLayoutsAndCells(const LinkFlow&, TypeArena&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
