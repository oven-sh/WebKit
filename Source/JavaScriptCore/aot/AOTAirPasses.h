/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC {

namespace B3 { namespace Air {
class Code;
} }

namespace AOT { namespace Typed {

void placeInvokeResults(B3::Air::Code&);
void resequenceMovesBeforePatches(B3::Air::Code&);
void mergeIdenticalBlocks(B3::Air::Code&);
void duplicateReturnsIntoJumps(B3::Air::Code&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
