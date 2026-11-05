/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkFlow.h"

namespace JSC { namespace AOT { namespace Typed {

// In front of LinkFlow::run(), beside reachEntries(), which fills the variables and lets go what evaluated text sees and what is made at no node. What no summary shows and reachEntries() leaves out:
// the variables whose original may run, those of modules that no body fills, and the fields of layouts of which native code makes objects too. Idempotent.
void seedCellsSharedWithUnseenCode(LinkFlow&);

// Behind LinkFlow::run(). LinkTables::layouts, fields, variables, captures and members. The tables come sized as conservativeLinkTables() leaves them.
// LinkedLayout::possibleChanges is the whole answer. LinkFlow::possibleChanges lacks what is done through a lost value, a store under `__proto__` or under a computed key, and a name that Object.prototype may gain.
// IN THREE STEPS, in this order. Only the last makes types: whoever has to wait for his turn at the table of types takes the first two in front of it. What goes from step to step is left in LinkFlow::writerOfLayouts.
// A look at every summary that is reached. Of the tables it writes LinkedFunction::closuresAreChanged alone, and it reads none.
void lookAtSummariesForLayoutsAndCells(const LinkFlow&, LinkTables&);
// The fields that are appended, the runs of makers, and what is said of a field gathered at its declaration. WriterOfLink::functionsWhoseCallersAreAllKnown().
void writeLayoutsAndCellsAheadOfTypes(const LinkFlow&, BitVector&& functionsWhoseCallersAreAllKnown);
void writeLayoutsAndCells(const LinkFlow&, TypeArena&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
