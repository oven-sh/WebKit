/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = NewClosure function=f4
    v6 = NewClosure function=f5
    v7 = Call v1, -, v2, v3, v4, v5, v6
    Return
}
function f1 name="makePromise" role=Library results=[{type=Promise}] {
    v0 = NewPromise
    Return v0
}
function f2 name="waiter" flags=[IsAsync] parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Await v0
    v3 = String string="waiter"
    v4 = Call v1, -, v3
    Return
}
function f3 name="awaitsNext" role=Library flags=[IsAsync] parameters=[{type=Promise}, {type=anything}] results=[{type=anything}] {
    v2 = Null
    v3 = Await v2
    v4 = Integer integer=1
    ResolvePromise v0, v4
    v6 = Integer integer=2
    v7 = Await v6
    v8 = String string="resolver goes on"
    v9 = Call v1, -, v8
    Return
}
function f4 name="returnsNext" role=Library flags=[IsAsync] parameters=[{type=Promise}, {type=anything}] results=[{type=anything}] {
    v2 = Null
    v3 = Await v2
    v4 = Integer integer=1
    ResolvePromise v0, v4
    v6 = String string="resolver is over"
    v7 = Call v1, -, v6
    Return
}
function f5 name="drainsNext" role=Library flags=[IsAsync] parameters=[{type=Promise}, {type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = Null
    v4 = Await v3
    v5 = Integer integer=1
    ResolvePromise v0, v5
    v7 = Call v2
    v8 = String string="resolver goes on"
    v9 = Call v1, -, v8
    Return
}
)IR"_s;

#define WITH_RESOLVER(name) "globalThis.publish = (makePromise, waiter, awaitsNext, returnsNext, drainsNext) => { let promise = makePromise(); waiter(promise, print); " name "(promise, print, drainMicrotasks); };"_s

static void checkUnderEveryOption(ASCIILiteral statementsBeforeProgram, ASCIILiteral expectedOutput)
{
    checkOutputOfProgramInShell(programText, expectedOutput, { }, statementsBeforeProgram);
    for (ASCIILiteral option : { "validate"_s, "disable=flow-results"_s })
        checkOutputOfProgramInShell(programText, expectedOutput, { option }, statementsBeforeProgram);
}

static void testWaiterRunsBeforeNextAwait()
{
    checkUnderEveryOption(WITH_RESOLVER("awaitsNext"), "waiter\nresolver goes on\n"_s);
}

static void testWaiterRunsAfterResolverReturns()
{
    checkUnderEveryOption(WITH_RESOLVER("returnsNext"), "resolver is over\nwaiter\n"_s);
}

static void testWaiterRunsWhenQueueIsDrained()
{
    checkUnderEveryOption(WITH_RESOLVER("drainsNext"), "waiter\nresolver goes on\n"_s);
}

#undef WITH_RESOLVER

std::span<const NativeTest> waiterOrderProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "waiter-order/waiter-runs-before-next-await"_s, testWaiterRunsBeforeNextAwait },
        { "waiter-order/waiter-runs-after-resolver-returns"_s, testWaiterRunsAfterResolverReturns },
        { "waiter-order/waiter-runs-when-queue-is-drained"_s, testWaiterRunsWhenQueueIsDrained },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
