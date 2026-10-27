/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <wtf/PrintStream.h>

namespace JSC {

namespace B3 {
class Procedure;
namespace Air {
class Code;
}
}

namespace AOT { namespace Typed {

enum class B3DumpScope : uint8_t { WholeProcedure, LoopsOnly, SizeOnly, SizeOfFunctionAndLoops };

void printOriginsAsOperations(B3::Procedure&, const Function&);
void dumpB3(PrintStream&, B3::Procedure&, B3DumpScope);
void dumpAir(PrintStream&, B3::Air::Code&, B3DumpScope);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
