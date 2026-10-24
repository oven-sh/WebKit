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
    v6 = Call v1, -, v2, v3, v4, v5
    Return
}
function f1 name="bindThis" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = PropGet v0 key="bind"
    v3 = Call v2, v0, v1 calleeKey="bind"
    Return v3
}
function f2 name="bindThree" parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    v5 = PropGet v0 key="bind"
    v6 = Call v5, v0, v1, v2, v3, v4 calleeKey="bind"
    Return v6
}
function f3 name="sum" flags=[HasThis] length=4 parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    v5 = Add v0, v1
    v6 = Add v5, v2
    v7 = Add v6, v3
    v8 = Add v7, v4
    Return v8
}
function f4 name="later" flags=[IsAsync] results=[{type=anything}] {
    Return
}
)IR"_s;

// The stub asks for the allocator of bound functions the first time it meets a closure it takes. The host function never does.
static constexpr ASCIILiteral statements = R"JS(
const preparations = () => Number(aotOperationCount("operationAOTPrepareBoundFunctionAllocation"));
const describe = bound => [bound.name, bound.length, typeof bound, Object.getOwnPropertyNames(bound).sort().join("+")].join(" ");
globalThis.publish = (bindThis, bindThree, sum, later) => {
    print("host function: " + describe(bindThis(Math.max, null)) + ", " + preparations());
    print("async function: " + describe(bindThis(later, null)) + ", " + preparations());
    print("object: " + (() => { try { return bindThis({ bind: Function.prototype.bind }, null); } catch (error) { return error.constructor.name; } })() + ", " + preparations());
    const changed = bindThis(sum, null);
    print("bound function: " + describe(bindThis(changed, null)) + ", " + preparations());
    for (let i = 0; i < 3; i++)
        print("closure: " + describe(bindThis(sum, "a")) + ", " + bindThis(sum, "a")("b", "c", "d", "e") + ", " + preparations());
    print("three arguments: " + describe(bindThree(sum, "a", "b", "c", "d")) + ", " + bindThree(sum, "a", "b", "c", "d")("e") + ", " + preparations());
    sum.extra = 1;
    print("closure with a property: " + describe(bindThis(sum, "a")) + ", " + bindThis(sum, "a")("b", "c", "d", "e"));
};
)JS"_s;

static void testBindFoundByName()
{
    checkOutputOfProgramInShell(programText,
        "host function: bound max 2 function length+name, 0\n"
        "async function: bound later 0 function length+name, 0\n"
        "object: TypeError, 0\n"
        "bound function: bound bound sum 4 function length+name, 1\n"
        "closure: bound sum 4 function length+name, abcde, 1\n"
        "closure: bound sum 4 function length+name, abcde, 1\n"
        "closure: bound sum 4 function length+name, abcde, 1\n"
        "three arguments: bound sum 1 function length+name, abcde, 1\n"
        "closure with a property: bound sum 4 function length+name, abcde\n"_s,
        { }, statements, { "--aotCountsPath=/dev/null"_s });
}

std::span<const NativeTest> boxedCallProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "boxed-calls/programs/bind-found-by-name"_s, testBindFoundByName },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
