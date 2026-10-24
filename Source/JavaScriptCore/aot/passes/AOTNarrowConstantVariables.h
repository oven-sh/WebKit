/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
class ProgramEditor;
class Program;
struct ConstantLists;

// A variable of a module that is stored at most once, where every store in the program stores one primitive constant, holds that constant or is not initialized yet.
ConstantLists narrowConstantVariables(ProgramEditor&, const FlowResults&, BuildContext&);
// That answer alone, for the literals that the program writes out. The Prepare schedule reads it, so what it makes of them is there for whoever comes behind Prepare.
ConstantLists collectConstantLists(const Program&, const FlowResults&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
