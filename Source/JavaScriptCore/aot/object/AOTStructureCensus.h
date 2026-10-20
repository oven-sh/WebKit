/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace WTF {
class PrintStream;
}

namespace JSC {

class VM;

namespace AOT {

// The live Structures by the class of their cells, the transition that made them, and what keeps them: cells of their own, a later Structure of their chain, or something else.
void dumpStructureCensus(VM&, WTF::PrintStream&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
