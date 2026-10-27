/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;
class BuiltinTable;

namespace Typed {

bool namesObjectCreate(const BuiltinTable&, const Value&);
bool createsObjectBelowItsArgument(const BuiltinTable&, const Function&, const Value&);

// `const once = run => { let result; return () => result ??= run(); }`   `const a = once(loadA), b = once(loadB);`
// Flow keeps one answer per function, so a() and b() would both answer what either load answers. Each call of such a function in the body of a
// module gets a copy of the function and of the functions it creates closures of. A closure of a copy does what a closure of the original does.
// `const Timed = Base => class extends Base { .. };`   `class A extends Timed(B) { .. }`   The copy evaluates a copy of the class, which is then a class that is evaluated once, below the one base it is handed.
// The function may be declared, in another module too. The class gets a name, or a class below it does: one that is only handed on is left as it is.
// `for (const name of ["a", "b"]) target[name] = function () { .. name .. };`   A turn of such a loop has a version for each written name, with copies of the functions it creates closures of.
// For a program as read, before its values are typed. Returns which functions are copies.
BitVector copyClosureFactories(ProgramEditor&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
