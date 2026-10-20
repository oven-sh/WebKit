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

// One line for each table that is reserved as zeroed pages and written as the program runs: its size, the pages the process has touched, the pages that hold data, and the 8-byte words that are not zero.
void dumpTableCensus(VM&, WTF::PrintStream&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
