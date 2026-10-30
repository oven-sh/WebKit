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
function f1 name="read" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="shared"
    Return v1
}
function f2 name="has" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropHas v0 key="shared"
    Return v1
}
function f3 name="hasOwn" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="Object" [isFreeName]
    v3 = PropGet v2 key="hasOwn"
    v4 = String string="shared"
    v5 = Call v3, v2, v0, v4 calleeKey="hasOwn"
    Return v5
}
function f4 name="readByKey" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = ElemGet v0, v1
    Return v2
}
function f5 name="isTruthy" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="shared"
    v2 = ToBoolean v1
    Branch v2 -> taken b1(), notTaken b2()
  b1():
    v3 = Integer integer=1
    Return v3
  b2():
    v4 = Integer
    Return v4
}
)IR"_s;

// A hundred accesses in a row call the run time at most three times where a cache answers: a site is filled at its second miss.
#define MEASURE_ACCESSES R"JS(
const operations = ["GetByIdAndCache", "GetById", "GetByValAndCache", "GetByVal", "InByIdAndCache", "HasOwnByIdAndCache"];
const calls = () => {
    let sum = 0;
    for (const name of operations) sum += Number(aotOperationCount("operationAOT" + name));
    return sum;
};
const measure = (what, access) => {
    const callsBefore = calls();
    let result;
    for (let i = 0; i < 100; i++) result = access();
    const callsMade = calls() - callsBefore;
    const served = callsMade <= 3 ? "cached" : callsMade >= 100 ? "not cached" : callsMade + " calls";
    print(what + ": " + result + (globalThis.printsCalls ? ", " + served : ""));
};
)JS"

#define STATEMENTS_CHANGING_THE_GLOBAL_OBJECT R"JS(
globalThis.publish = (read, has, hasOwn, readByKey, isTruthy) => {
    globalThis.shared = 7;
    measure("own value", () => read(globalThis));
    measure("own value tested", () => isTruthy(globalThis));
    measure("own value by key", () => readByKey(globalThis, "shared"));
    measure("own value, in", () => has(globalThis));
    measure("own value, hasOwn", () => hasOwn(globalThis));
    globalThis.shared = 8;
    measure("replaced", () => read(globalThis));
    delete globalThis.shared;
    measure("deleted", () => read(globalThis));
    measure("deleted, tested", () => isTruthy(globalThis));
    measure("deleted, by key", () => readByKey(globalThis, "shared"));
    measure("deleted, in", () => has(globalThis));
    measure("deleted, hasOwn", () => hasOwn(globalThis));
    globalThis.shared = 9;
    measure("added", () => read(globalThis));
    measure("added, by key", () => readByKey(globalThis, "shared"));
    measure("added, hasOwn", () => hasOwn(globalThis));
    let reads = 0;
    Object.defineProperty(globalThis, "shared", { get() { return ++reads; }, configurable: true });
    measure("getter", () => read(globalThis));
    measure("getter, hasOwn", () => hasOwn(globalThis));
    Object.defineProperty(globalThis, "shared", { get() { Object.defineProperty(globalThis, "shared", { value: 10, writable: true, configurable: true }); return -10; }, configurable: true });
    print("getter that replaces itself: " + read(globalThis));
    measure("value of that getter", () => read(globalThis));
    measure("inherited", () => readByKey(globalThis, "isPrototypeOf") === Object.prototype.isPrototypeOf);
    try { globalThis.isPrototypeOf = 5; } catch { }
    measure("inherited and stored to", () => readByKey(globalThis, "isPrototypeOf") === globalThis.isPrototypeOf);
    delete globalThis.shared;
    measure("deleted again", () => read(globalThis));
    loadString("var shared = 11;");
    measure("variable of a script", () => read(globalThis));
    measure("variable of a script, hasOwn", () => hasOwn(globalThis));
    loadString("shared = 12;");
    measure("variable stored to", () => read(globalThis));
};
)JS"

#define OUTPUT_LINE(what, served) what served "\n"

#define OUTPUT_WHILE_THE_GLOBAL_OBJECT_CHANGES(cached, notCached) \
    OUTPUT_LINE("own value: 7", cached) \
    OUTPUT_LINE("own value tested: 1", cached) \
    OUTPUT_LINE("own value by key: 7", cached) \
    OUTPUT_LINE("own value, in: true", cached) \
    OUTPUT_LINE("own value, hasOwn: true", notCached) \
    OUTPUT_LINE("replaced: 8", cached) \
    OUTPUT_LINE("deleted: undefined", cached) \
    OUTPUT_LINE("deleted, tested: 0", cached) \
    OUTPUT_LINE("deleted, by key: undefined", cached) \
    OUTPUT_LINE("deleted, in: false", cached) \
    OUTPUT_LINE("deleted, hasOwn: false", cached) \
    OUTPUT_LINE("added: 9", cached) \
    OUTPUT_LINE("added, by key: 9", cached) \
    OUTPUT_LINE("added, hasOwn: true", notCached) \
    OUTPUT_LINE("getter: 100", notCached) \
    OUTPUT_LINE("getter, hasOwn: true", notCached) \
    "getter that replaces itself: -10\n" \
    OUTPUT_LINE("value of that getter: 10", cached) \
    OUTPUT_LINE("inherited: true", cached) \
    OUTPUT_LINE("inherited and stored to: true", cached) \
    OUTPUT_LINE("deleted again: undefined", cached) \
    OUTPUT_LINE("variable of a script: 11", notCached) \
    OUTPUT_LINE("variable of a script, hasOwn: true", notCached) \
    OUTPUT_LINE("variable stored to: 12", notCached)

static void testReadsFollowTheGlobalObject()
{
    checkOutputOfProgramInShell(programText, OUTPUT_WHILE_THE_GLOBAL_OBJECT_CHANGES(", cached", ", not cached") ""_s,
        { }, "globalThis.printsCalls = true;" MEASURE_ACCESSES STATEMENTS_CHANGING_THE_GLOBAL_OBJECT ""_s, { "--aotCountsPath=/dev/null"_s });
    for (ASCIILiteral word : { "validate"_s, "heat=EverythingIsHot"_s, "heat=EverythingIsCold"_s })
        checkOutputOfProgramInShell(programText, OUTPUT_WHILE_THE_GLOBAL_OBJECT_CHANGES("", "") ""_s, { word }, MEASURE_ACCESSES STATEMENTS_CHANGING_THE_GLOBAL_OBJECT ""_s, { "--aotCountsPath=/dev/null"_s });
}

// The global object of another realm reports no change to this program, and an object below the proxy is no proxy.
static constexpr ASCIILiteral statementsWithOtherReceivers = MEASURE_ACCESSES R"JS(
globalThis.printsCalls = true;
globalThis.publish = (read, has, hasOwn) => {
    const other = createGlobalObject();
    other.shared = 3;
    measure("other realm", () => read(other));
    other.shared = 4;
    measure("other realm, replaced", () => read(other));
    delete other.shared;
    measure("other realm, deleted", () => read(other));
    measure("other realm, hasOwn", () => hasOwn(other));
    globalThis.shared = 5;
    const below = Object.create(globalThis);
    measure("below the proxy", () => read(below));
    delete globalThis.shared;
    measure("below the proxy, deleted", () => read(below));
    measure("below the proxy, in", () => has(below));
};
)JS"_s;

static void testOtherReceiversAreNotCached()
{
    checkOutputOfProgramInShell(programText,
        "other realm: 3, not cached\n"
        "other realm, replaced: 4, not cached\n"
        "other realm, deleted: undefined, not cached\n"
        "other realm, hasOwn: false, not cached\n"
        "below the proxy: 5, not cached\n"
        "below the proxy, deleted: undefined, not cached\n"
        "below the proxy, in: false, not cached\n"_s,
        { }, statementsWithOtherReceivers, { "--aotCountsPath=/dev/null"_s });
}

// `globalThis[key] = value` stores to the global object through its proxy: from the second store on, the same property takes no call of an operation that may run code.
static constexpr ASCIILiteral programWithStoresByKey = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = Call v1, -, v2
    Return
}
function f1 name="write" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    ElemSet v0, v1, v2 mode=Strict
    Return
}
)IR"_s;

#define STATEMENTS_WITH_STORES_BY_KEY R"JS(
const operations = ["PutByVal", "PutByValAndCache"];
const calls = () => operations.reduce((sum, name) => sum + Number(aotOperationCount("operationAOT" + name)), 0);
const measure = (what, write, key) => {
    const callsBefore = calls();
    let threw = false;
    for (let i = 0; i < 100; i++) {
        try { write(globalThis, key, i); } catch { threw = true; }
    }
    const callsMade = calls() - callsBefore;
    print(what + ": " + globalThis[key] + (threw ? ", threw" : "") + (globalThis.printsCalls ? ", " + (callsMade <= 3 ? "cached" : callsMade >= 100 ? "not cached" : callsMade + " calls") : ""));
};
globalThis.publish = (write) => {
    measure("first store adds it", write, "shared");
    measure("own value", write, "shared");
    const symbol = Symbol("shared");
    write(globalThis, symbol, 1);
    measure("own value under a symbol", write, symbol);
    let stored;
    Object.defineProperty(globalThis, "shared", { set(value) { stored = value; }, get() { return stored; }, configurable: true });
    measure("setter", write, "shared");
    Object.defineProperty(globalThis, "shared", { value: -1, writable: false, configurable: true });
    measure("read only", write, "shared");
    delete globalThis.shared;
    print("deleted: " + ("shared" in globalThis));
    measure("added again", write, "shared");
    measure("value again", write, "shared");
    globalThis.other = 5;
    Object.freeze(globalThis.other);
    measure("other", write, "other");
    loadString("var ofScript = 1;");
    measure("variable of a script", write, "ofScript");
    print("read by name: " + loadString("ofScript"));
    measure("inherited", write, "isPrototypeOf");
    const other = createGlobalObject();
    other.shared = 3;
    const callsBefore = calls();
    for (let i = 0; i < 100; i++) write(other, "shared", i);
    print("other realm: " + other.shared + (globalThis.printsCalls ? ", " + (calls() - callsBefore >= 100 ? "not cached" : "cached") : ""));
};
)JS"

#define OUTPUT_OF_STORES_BY_KEY(cached, notCached) \
    OUTPUT_LINE("first store adds it: 99", cached) \
    OUTPUT_LINE("own value: 99", cached) \
    OUTPUT_LINE("own value under a symbol: 99", cached) \
    OUTPUT_LINE("setter: 99", notCached) \
    OUTPUT_LINE("read only: -1, threw", notCached) \
    "deleted: false\n" \
    OUTPUT_LINE("added again: 99", cached) \
    OUTPUT_LINE("value again: 99", cached) \
    OUTPUT_LINE("other: 99", cached) \
    OUTPUT_LINE("variable of a script: 99", notCached) \
    "read by name: 99\n" \
    OUTPUT_LINE("inherited: 99", cached) \
    OUTPUT_LINE("other realm: 99", notCached)

static void testStoresByKeyFollowTheGlobalObject()
{
    checkOutputOfProgramInShell(programWithStoresByKey, OUTPUT_OF_STORES_BY_KEY(", cached", ", not cached") ""_s, { }, "globalThis.printsCalls = true;" STATEMENTS_WITH_STORES_BY_KEY ""_s, { "--aotCountsPath=/dev/null"_s });
    for (ASCIILiteral word : { "validate"_s, "heat=EverythingIsHot"_s, "heat=EverythingIsCold"_s })
        checkOutputOfProgramInShell(programWithStoresByKey, OUTPUT_OF_STORES_BY_KEY("", "") ""_s, { word }, STATEMENTS_WITH_STORES_BY_KEY ""_s, { "--aotCountsPath=/dev/null"_s });
}

std::span<const NativeTest> globalProxyReadProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "global-proxy-reads/programs/reads-follow-the-global-object"_s, testReadsFollowTheGlobalObject },
        { "global-proxy-reads/programs/other-receivers-are-not-cached"_s, testOtherReceiversAreNotCached },
        { "global-proxy-reads/programs/stores-by-key-follow-the-global-object"_s, testStoresByKeyFollowTheGlobalObject },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
