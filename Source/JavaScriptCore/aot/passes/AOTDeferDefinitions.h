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

// A binding that is stored once, with an allocation that does nothing but allocate, holds nothing until something needs the object. The code that held the definition does not make the object.
// - A closure that captures nothing: the variable is born as the function, and the first read that needs an object takes the closure from the closure table.
//   A field of an environment that is allocated at most once becomes such a variable.
// - A class whose record says all that its definition does: the variables that hold the class object are born as its constructor, and the run time defines the class with its class object.
// - A RegExp literal: each read finds the variable empty or filled, and the one that finds it empty makes the object.
// Where a read may come before the declaration and find the dead zone, the variable keeps its store and the tests behind its reads, and says only whether the declaration has run.
// A second variable stands for the object.
void deferDefinitions(ProgramEditor&, const FlowResults&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
