/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CallGraph;
class Program;
class ProgramEditor;

void removeUnreachableFunctions(ProgramEditor&, const CallGraph&, BuildContext&);

// Bodies that no call in code that is generated names. Closures and the host enter entrypoints, a resumption a coroutine body, a stub its body, and only a call anything else. They stay for their effects alone.
BitVector findBodiesWithoutCode(const Program&, const BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
