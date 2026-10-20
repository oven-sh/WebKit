/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class ProgramEditor;

// A field defers its closure where every allocation of its layout gives it, before any other code can see the object, a closure of one function that was made for it alone
// and that captures nothing or only the object. The allocation then lists the closure, which is marked as made on demand and has no code, and the first read of the field makes it.
// The decision rests on the operations of the program alone: what unknown code does to the object later is left to the run time.
void deferClosuresInFields(ProgramEditor&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
