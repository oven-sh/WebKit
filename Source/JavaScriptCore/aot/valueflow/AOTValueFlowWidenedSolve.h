/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowAnalysis.h"

namespace JSC { namespace AOT { namespace Typed {

// More to begin with never proves more: whatever a solve finds, a solve that was given more finds too. The solution holds both solves. Compared are the functions that are reached, the uses of built-ins,
// the records of unseen code, the marks of objects that neither escape nor are lost, and what every node of the bodies stands for. Each first place where the wider solve lacks something is an internal error
// that names the rules on the way there.
void reportWhatIsFoundOnlyFromLess(ValueFlowSolution&, std::span<const UnseenCodeRecord> unseenCodeOfProgramAsItIs, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
