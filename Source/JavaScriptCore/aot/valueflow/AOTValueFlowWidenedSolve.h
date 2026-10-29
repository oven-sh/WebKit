/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowAnalysis.h"

namespace JSC { namespace AOT { namespace Typed {

// More to begin with never proves more: whatever a solve finds, a solve that was given more finds too. The solution holds both solves. Compared are the functions that are reached, the uses of built-ins,
// the records of unseen code, the marks of objects that neither escape nor are lost, and what every node of the bodies stands for. Each sort of which the wider solve lacks something is an internal error
// whose detail is three numbers: the sort (0 functions, 1 built-ins, 2 records, 3 objects, 4 nodes), how many lack something, the first of them.
// The test program reports in words: it names the rules.
void reportWhatIsFoundOnlyFromLess(ValueFlowSolution&, std::span<const UnseenCodeRecord> unseenCodeOfProgramAsItIs, BuildContext&);

using ReporterOfWhatIsFoundOnlyFromLess = void (*)(ValueFlowSolution&, std::span<const UnseenCodeRecord>, BuildContext&);
void reportWhatIsFoundOnlyFromLessBy(ReporterOfWhatIsFoundOnlyFromLess);

// Where a callee may be anything, the call makes an object nobody knows and not the object of its site. The object of such a site that no node of the solve holds is one of those that the unknown object stands for there.
ValueSetID objectsThatTheirOwnSitesNoLongerMake(const ValueFlowSolver&, const ValueFlowBodies&, ValueSetMaker&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
