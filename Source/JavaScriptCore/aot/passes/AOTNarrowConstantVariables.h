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
// Its type says so, and a read in any function is the constant: flow sees a literal that is stored, and this pass what the bodies have folded to a constant since.
// Where the one store stores a list of primitive constants that nothing ever changes, the answer says what the list holds: whoever only looks into the list needs no list.
ConstantLists narrowConstantVariables(ProgramEditor&, const FlowResults&, BuildContext&);
// That answer alone, for the literals that the program writes out. The Prepare schedule reads it, so what it makes of them is there for whoever comes behind Prepare.
ConstantLists collectConstantLists(const Program&, const FlowResults&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
