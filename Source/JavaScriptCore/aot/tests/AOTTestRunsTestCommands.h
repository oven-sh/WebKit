/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTestHarness.h"

namespace JSC { namespace AOT { namespace Typed {

// test-run [oldest-first|scattered]
//     through-mock m1 "space"            each export of which mayComeThroughMock() holds, by module and place
//     any-other-name m1                  behind the exports of the module: it holds of a name that the module lacks
//     replaces ArrayPrototypeIncludes    Facts::builtinMembersThatTestsReplace(), ascending
//     adds GeneratorFunctionPrototype "stored"    Program::openedNames behind Facts: what the wire lists, then what Link has found
//     left-to-program 3120               how many members the table of the build leaves to the program. 0 in an executable
std::span<const TestCommand> testRunsTestCommands();

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
