/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowAnalysis.h"

namespace JSC { namespace AOT { namespace Typed {

// What reportWhatIsFoundOnlyFromLess() counts, in words: each first place where the wider solve lacks something is an internal error that names the rules on the way there,
// what the place holds, and the nodes that the seed drew with the positions of their values in the text of the program.
void reportWhatIsFoundOnlyFromLessInWords(ValueFlowSolution&, std::span<const UnseenCodeRecord> unseenCodeOfProgramAsItIs, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
