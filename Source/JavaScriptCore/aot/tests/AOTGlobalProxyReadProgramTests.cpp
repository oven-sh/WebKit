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

#define STATEMENTS_DEFINING_REPEAT COUNTED_CALLS(R"("GetByIdAndCache", "GetById", "GetGeneric", "GetByValAndCache", "GetByVal", "InByIdAndCache", "HasOwnByIdAndCache", "HasOwnByValAndCache")") R"JS(
const repeat = (what, access) => {
    const before = countedCalls();
    let result;
    for (let i = 0; i < 100; i++) result = access();
    print(what + ": " + result + servedBy(before, 100));
};
)JS"

static constexpr ASCIILiteral statementsChangingTheGlobalObject = STATEMENTS_DEFINING_REPEAT R"JS(
globalThis.publish = (read, has, hasOwn, readByKey, isTruthy) => {
    globalThis.shared = 7;
    repeat("own value", () => read(globalThis));
    repeat("own value tested", () => isTruthy(globalThis));
    repeat("own value by key", () => readByKey(globalThis, "shared"));
    repeat("own value, in", () => has(globalThis));
    repeat("own value, hasOwn", () => hasOwn(globalThis));
    globalThis.shared = 8;
    repeat("replaced", () => read(globalThis));
    delete globalThis.shared;
    repeat("deleted", () => read(globalThis));
    repeat("deleted, tested", () => isTruthy(globalThis));
    repeat("deleted, by key", () => readByKey(globalThis, "shared"));
    repeat("deleted, in", () => has(globalThis));
    repeat("deleted, hasOwn", () => hasOwn(globalThis));
    globalThis.shared = 9;
    repeat("added", () => read(globalThis));
    repeat("added, by key", () => readByKey(globalThis, "shared"));
    repeat("added, hasOwn", () => hasOwn(globalThis));
    let reads = 0;
    Object.defineProperty(globalThis, "shared", { get() { return ++reads; }, configurable: true });
    repeat("getter", () => read(globalThis));
    repeat("getter, hasOwn", () => hasOwn(globalThis));
    Object.defineProperty(globalThis, "shared", { get() { Object.defineProperty(globalThis, "shared", { value: 10, writable: true, configurable: true }); return -10; }, configurable: true });
    print("getter that replaces itself: " + read(globalThis));
    repeat("value of that getter", () => read(globalThis));
    repeat("inherited", () => readByKey(globalThis, "isPrototypeOf") === Object.prototype.isPrototypeOf);
    try { globalThis.isPrototypeOf = 5; } catch { }
    repeat("inherited and stored to", () => readByKey(globalThis, "isPrototypeOf") === globalThis.isPrototypeOf);
    delete globalThis.shared;
    repeat("deleted again", () => read(globalThis));
    loadString("var shared = 11;");
    repeat("variable of a script", () => read(globalThis));
    repeat("variable of a script, hasOwn", () => hasOwn(globalThis));
    loadString("shared = 12;");
    repeat("variable stored to", () => read(globalThis));
};
)JS"_s;

static constexpr ASCIILiteral outputOfReads =
    "own value: 7\n"
    "own value tested: 1\n"
    "own value by key: 7\n"
    "own value, in: true\n"
    "own value, hasOwn: true\n"
    "replaced: 8\n"
    "deleted: undefined\n"
    "deleted, tested: 0\n"
    "deleted, by key: undefined\n"
    "deleted, in: false\n"
    "deleted, hasOwn: false\n"
    "added: 9\n"
    "added, by key: 9\n"
    "added, hasOwn: true\n"
    "getter: 100\n"
    "getter, hasOwn: true\n"
    "getter that replaces itself: -10\n"
    "value of that getter: 10\n"
    "inherited: true\n"
    "inherited and stored to: true\n"
    "deleted again: undefined\n"
    "variable of a script: 11\n"
    "variable of a script, hasOwn: true\n"
    "variable stored to: 12\n"_s;

static void testReadsFollowTheGlobalObject()
{
    checkOutputOfProgramInShell(programText, outputOfReads, { }, statementsChangingTheGlobalObject);
    checkServicesInShell(__FILE__, __LINE__, programText, outputOfReads, statementsChangingTheGlobalObject, wordsOfCaches, "ccccnccccccccnnn.ccccnnn"_s);
}

static constexpr ASCIILiteral statementsWithOtherReceivers = STATEMENTS_DEFINING_REPEAT R"JS(
globalThis.publish = (read, has, hasOwn) => {
    const other = createGlobalObject();
    other.shared = 3;
    repeat("other realm", () => read(other));
    other.shared = 4;
    repeat("other realm, replaced", () => read(other));
    delete other.shared;
    repeat("other realm, deleted", () => read(other));
    repeat("other realm, hasOwn", () => hasOwn(other));
    globalThis.shared = 5;
    const below = Object.create(globalThis);
    repeat("below the proxy", () => read(below));
    delete globalThis.shared;
    repeat("below the proxy, deleted", () => read(below));
    repeat("below the proxy, in", () => has(below));
};
)JS"_s;

static constexpr ASCIILiteral outputOfOtherReceivers =
    "other realm: 3\n"
    "other realm, replaced: 4\n"
    "other realm, deleted: undefined\n"
    "other realm, hasOwn: false\n"
    "below the proxy: 5\n"
    "below the proxy, deleted: undefined\n"
    "below the proxy, in: false\n"_s;

static void testReadsFollowOtherReceivers()
{
    checkOutputOfProgramInShell(programText, outputOfOtherReceivers, { }, statementsWithOtherReceivers);
    checkServicesInShell(__FILE__, __LINE__, programText, outputOfOtherReceivers, statementsWithOtherReceivers, wordsOfCaches, "nnnnnnn"_s);
}

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

static constexpr ASCIILiteral statementsWithStoresByKey = COUNTED_CALLS(R"("PutByVal", "PutByValAndCache", "PutByValGeneric")") R"JS(
const repeat = (what, write, key) => {
    const before = countedCalls();
    let threw = false;
    for (let i = 0; i < 100; i++) {
        try { write(globalThis, key, i); } catch { threw = true; }
    }
    print(what + ": " + globalThis[key] + (threw ? ", threw" : "") + servedBy(before, 100, 7));
};
globalThis.publish = (write) => {
    repeat("first store adds it", write, "shared");
    repeat("own value", write, "shared");
    const symbol = Symbol("shared");
    write(globalThis, symbol, 1);
    repeat("own value under a symbol", write, symbol);
    let stored;
    Object.defineProperty(globalThis, "shared", { set(value) { stored = value; }, get() { return stored; }, configurable: true });
    repeat("setter", write, "shared");
    Object.defineProperty(globalThis, "shared", { value: -1, writable: false, configurable: true });
    repeat("read only", write, "shared");
    delete globalThis.shared;
    print("deleted: " + ("shared" in globalThis));
    repeat("added again", write, "shared");
    repeat("value again", write, "shared");
    globalThis.other = 5;
    Object.freeze(globalThis.other);
    repeat("other", write, "other");
    loadString("var ofScript = 1;");
    repeat("variable of a script", write, "ofScript");
    print("read by name: " + loadString("ofScript"));
    repeat("inherited", write, "isPrototypeOf");
    const text = ["sha", "red"].join("");
    repeat("a key that is another string of the same text", write, text);
    Object.defineProperty(globalThis, "shared", { value: -2, writable: false, configurable: true });
    repeat("read only under that string", write, text);
    Object.defineProperty(globalThis, "shared", { value: -3, writable: true, configurable: true });
    repeat("writable again under that string", write, text);
    gc();
    repeat("behind a collection", write, text);
    const other = createGlobalObject();
    other.shared = 3;
    { const before = countedCalls(); for (let i = 0; i < 100; i++) write(other, "shared", i); print("other realm: " + other.shared + servedBy(before, 100, 7)); }
};
)JS"_s;

static constexpr ASCIILiteral outputOfStoresByKey =
    "first store adds it: 99\n"
    "own value: 99\n"
    "own value under a symbol: 99\n"
    "setter: 99\n"
    "read only: -1, threw\n"
    "deleted: false\n"
    "added again: 99\n"
    "value again: 99\n"
    "other: 99\n"
    "variable of a script: 99\n"
    "read by name: 99\n"
    "inherited: 99\n"
    "a key that is another string of the same text: 99\n"
    "read only under that string: -2, threw\n"
    "writable again under that string: 99\n"
    "behind a collection: 99\n"
    "other realm: 99\n"_s;

static void testStoresByKeyFollowTheGlobalObject()
{
    checkOutputOfProgramInShell(programWithStoresByKey, outputOfStoresByKey, { }, statementsWithStoresByKey);
    checkServicesInShell(__FILE__, __LINE__, programWithStoresByKey, outputOfStoresByKey, statementsWithStoresByKey, wordsOfCaches, "cccnn.cccn.ccnccn"_s);
}

static constexpr ASCIILiteral programWithStoresByName = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = Call v1, -, v2, v3, v4
    Return
}
function f1 name="write" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    PropSet v0, v1 key="shared" mode=Strict
    Return
}
function f2 name="writeSloppy" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    PropSet v0, v1 key="shared" mode=Sloppy
    Return
}
function f3 name="writeVariable" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    PropSet v0, v1 key="ofScript" mode=Strict
    Return
}
)IR"_s;

static constexpr ASCIILiteral statementsWithStoresByName = COUNTED_CALLS(R"("PutById", "PutByIdSloppy", "PutByIdAndCache")") R"JS(
const repeat = (what, write, read = () => globalThis.shared, base = globalThis) => {
    const before = countedCalls();
    let threw = false;
    for (let i = 0; i < 100; i++) {
        try { write(base, i); } catch { threw = true; }
    }
    print(what + ": " + read() + (threw ? ", threw" : "") + servedBy(before, 100));
};
globalThis.publish = (write, writeSloppy, writeVariable) => {
    repeat("first store adds it", write);
    repeat("own value", write);
    repeat("own value, sloppy", writeSloppy);
    let stored, receiver;
    Object.defineProperty(globalThis, "shared", { set(value) { stored = value; receiver = this; }, get() { return stored; }, configurable: true });
    repeat("setter", write);
    print("the setter's this is globalThis: " + (receiver === globalThis));
    Object.defineProperty(globalThis, "shared", { value: -1, writable: false, configurable: true });
    repeat("read only", write);
    repeat("read only, sloppy", writeSloppy);
    delete globalThis.shared;
    print("deleted: " + ("shared" in globalThis));
    repeat("added again", write);
    repeat("value again", write);
    for (let i = 0; i < 40; i++) globalThis["between" + i] = i;
    repeat("behind other names", write);
    for (let i = 0; i < 40; i += 2) delete globalThis["between" + i];
    repeat("behind deletions", write);
    print("the others keep their values: " + globalThis.between1 + " " + globalThis.between39 + " " + ("between2" in globalThis));
    loadString("var ofScript = 1;");
    repeat("variable of a script", writeVariable, () => loadString("ofScript"));
    gc();
    repeat("behind a collection", write);
    let kept = 0;
    for (let round = 0; round < 20; round++) {
        for (let i = 0; i < 50; i++) write(globalThis, { round, i, filler: [round, i] });
        edenGC();
        if (globalThis.shared.round === round && globalThis.shared.filler[1] === 49) kept++;
    }
    print("kept over collections: " + kept);
    const plain = { shared: 0 };
    repeat("a plain object between", write, () => plain.shared, plain);
    repeat("and globalThis again", write);
    const below = Object.create(globalThis);
    repeat("an object below globalThis", write, () => below.shared + " " + Object.hasOwn(below, "shared") + " " + globalThis.shared, below);
    const other = createGlobalObject();
    other.shared = 3;
    repeat("other realm", write, () => other.shared + " " + globalThis.shared, other);
    Object.freeze(globalThis);
    repeat("frozen", write);
};
)JS"_s;

static constexpr ASCIILiteral outputOfStoresByName =
    "first store adds it: 99\n"
    "own value: 99\n"
    "own value, sloppy: 99\n"
    "setter: 99\n"
    "the setter's this is globalThis: true\n"
    "read only: -1, threw\n"
    "read only, sloppy: -1\n"
    "deleted: false\n"
    "added again: 99\n"
    "value again: 99\n"
    "behind other names: 99\n"
    "behind deletions: 99\n"
    "the others keep their values: 1 39 false\n"
    "variable of a script: 99\n"
    "behind a collection: 99\n"
    "kept over collections: 20\n"
    "a plain object between: 99\n"
    "and globalThis again: 99\n"
    "an object below globalThis: 99 true 99\n"
    "other realm: 99 99\n"
    "frozen: 99, threw\n"_s;

static void testStoresByNameFollowTheGlobalObject()
{
    checkOutputOfProgramInShell(programWithStoresByName, outputOfStoresByName, { }, statementsWithStoresByName);
    checkServicesInShell(__FILE__, __LINE__, programWithStoresByName, outputOfStoresByName, statementsWithStoresByName, wordsOfCaches, "cccn.nn.cccc.nc.cccnn"_s);
}

std::span<const NativeTest> globalProxyReadProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "global-proxy-reads/programs/reads-follow-the-global-object"_s, testReadsFollowTheGlobalObject },
        { "global-proxy-reads/programs/reads-follow-other-receivers"_s, testReadsFollowOtherReceivers },
        { "global-proxy-reads/programs/stores-by-key-follow-the-global-object"_s, testStoresByKeyFollowTheGlobalObject },
        { "global-proxy-reads/programs/stores-by-name-follow-the-global-object"_s, testStoresByNameFollowTheGlobalObject },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
