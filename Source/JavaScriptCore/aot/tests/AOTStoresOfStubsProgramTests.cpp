/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral programOfStores = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Record" fields=[{key="n" declared={type=anything} flags=[IsRequired]}]
layout L1 kind=Instance name="Counter" constructor=f5 fields=[{key="x" declared={type=anything} flags=[IsRequired]}, {key="run" declared={type=function} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f5
    v1 = NewClass v0 layout=L1
    v2 = FinishClass v1 layout=L1
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="publish" [isFreeName]
    v5 = NewClosure function=f1
    v6 = NewClosure function=f2
    v7 = NewClosure function=f3
    v8 = NewClosure function=f4
    v9 = NewClosure function=f7
    v10 = NewClosure function=f8
    v11 = NewClosure function=f9
    v12 = Call v4, -, v2, v5, v6, v7, v8, v9, v10, v11
    Return
}
function f1 name="make" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    Return v2
}
function f2 name="store" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    ElemSet v0, v1, v2
    Return
}
function f3 name="define" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = NewObject -, v1 layout=L0 fields=[0]
    v3 = ToPropertyKey v0
    ElemDefine v2, v3, v1
    v5 = FinishObject v2
    Return v5
}
function f4 name="readRun" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="run"
    Return v1
}
function f5 name="Counter" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L1
    v2 = NewObject v1 layout=L1
    v3 = Integer integer=7
    PropDefine v2, v3 key="x"
    v5 = NewClosure v2 function=f6
    PropDefine v2, v5 key="run"
    Return v2
}
function f6 name="run" kind=Arrow captures=[{type=L1}] results=[{type=anything}] {
    v1 = PropGet v0 key="x"
    Return v1
}
function f7 name="put" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    PropSet v0, v1 key="shared"
    Return
}
function f8 name="get" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="shared"
    Return v1
}
function f9 name="load" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = ElemGet v0, v1
    Return v2
}
)IR"_s;

static constexpr ASCIILiteral statementsThatCount = R"JS(
globalThis.shared = 0;
globalThis.publish = (Counter, make, store, define, readRun, put, get, load) => {
    const counted = (what, names, work, most = 3) => {
        const sum = () => { let total = 0; for (let at = 0; at < names.length; at++) total += Number(aotOperationCount("operationAOT" + names[at])); return total; };
        for (let i = 0; i < 8; i++) work(i);
        const before = sum();
        for (let i = 0; i < 400; i++) work(i);
        print(what + ": " + (sum() - before <= most ? "by the stub" : (sum() - before) + " calls"));
    };
    const record = make(0), counter = new Counter, grown = make(1);
    counted("replaced", ["PutByVal"], i => store(record, i & 7, i));
    counted("replaced in an instance", ["PutByVal"], i => store(counter, i & 7, i));
    counted("read in an instance", ["GetByVal", "GetByValAndCache"], i => load(counter, i & 7));
    const whereFreeListsRunOut = 8;
    counted("first", ["PutByVal"], i => { const t = make(i); store(t, 0, i); store(t, 1, t); }, whereFreeListsRunOut);
    counted("grown", ["PutByVal"], i => store(grown, i & 63, i), whereFreeListsRunOut);
    counted("computed: the key", ["ToKey"], i => define(i & 7, i));
    counted("computed: the definition", ["DefineField"], i => define(i & 7, i), whereFreeListsRunOut);
};
)JS"_s;
static void testStubsServe()
{
    String counted = outputOfProgramInShell(programOfStores, { "references=narrow"_s }, statementsThatCount, { "--aotCountsPath=/dev/null"_s }, waysToCompilePrograms[0]);
    checkPrintedLines(__FILE__, __LINE__, optionWordsOfReason(waysToCompilePrograms[0], { "references=narrow"_s }), counted, "replaced: by the stub\nreplaced in an instance: by the stub\nread in an instance: by the stub\nfirst: by the stub\ngrown: by the stub\ncomputed: the key: by the stub\ncomputed: the definition: by the stub\n"_s);
}
static constexpr ASCIILiteral statementsThatAnswer = R"JS(
globalThis.publish = (Counter, make, store, define, readRun) => {
    const quietly = work => { try { work(); } catch { } };
    const show = (what, o) => print(what + ": " + JSON.stringify(o) + " " + Object.keys(o));
    for (let i = 0; i < 4; i++) store(make(i), 0, i);
    const t = make(0); store(t, 2, "c"); store(t, 0, "a"); show("order", t);
    store(t, 1, 1.5); store(t, -1, "m"); show("hole and name", t);
    const u = make(1); store(u, 0, 1); store(u, 1, 2); store(u, 0, 0.5); delete u[1]; store(u, 1, null); show("other kinds", u);
    for (const close of [Object.freeze, Object.seal, Object.preventExtensions]) { const f = make(2); store(f, 0, 1); close(f); quietly(() => store(f, 0, 9)); quietly(() => store(f, 1, 9)); print(close.name + " " + f[0] + " " + f[1]); const bare = make(3); close(bare); quietly(() => store(bare, 0, 9)); print(bare[0]); }
    let seen = []; const a = make(3); Object.defineProperty(a, 0, { get() { return "got"; }, set(v) { seen.push(v); }, configurable: true }); store(a, 0, 5); print(seen + " " + a[0]);
    seen = []; const b = make(4); store(b, 5, 1); Object.setPrototypeOf(b, { set 0(v) { seen.push(v); } }); store(b, 0, "x"); store(b, 5, 2); print(seen + " " + Object.keys(b) + " " + b[5]);
    const d = make(6), e = Object.create(d); store(d, 0, "d0"); store(d, 0, "d1"); store(d, 1, "new"); print(e[0] + " " + e[1]);
    show("defined", define(3, "v")); show("defined under a text", define("p", "v")); show("defined under a negative", define(-2, "v"));
    const kept = []; for (let i = 0; i < 2000; i++) { const r = make(i); store(r, i & 7, { i }); store(r, 8 + (i & 31), { i }); kept.push(r, new Counter); }
    let holds = true;
    for (let k = 0; k < kept.length; k++) { const r = kept[k]; holds &&= k & 1 ? readRun(r)() === 7 && readRun(r) === readRun(r) : r[k / 2 & 7].i === k / 2 && r[8 + (k / 2 & 31)].i === k / 2; }
    print(holds);
    const churn = () => { for (let k = 0; k < 3000; k++) { const r = make(k); store(r, 0, -1); store(r, 8 + (k & 31), -1); } };
    holds = true;
    const old = make(9); store(old, 0, 0); store(old, 1, 0); fullGC();
    for (let round = 0; round < 40; round++) { store(old, 0, { round }); edenGC(); churn(); holds &&= old[0].round === round; }
    print("replaced in an old object: " + holds);
    holds = true;
    for (let round = 0; round < 40; round++) { const t = make(round); fullGC(); store(make(0), 0, 0); store(t, 0, { round }); edenGC(); churn(); holds &&= t[0].round === round && t.n === round; }
    print("first in an old object: " + holds);
    holds = true;
    for (let round = 0; round < 20; round++) { const t = make(round); for (let k = 0; k < 4; k++) store(t, k, k); fullGC(); for (let k = 4; k < 40; k++) store(t, k, k); edenGC(); churn(); for (let k = 0; k < 40; k++) holds &&= t[k] === k; }
    print("grown in an old object: " + holds);
};
)JS"_s;
static constexpr ASCIILiteral answersOfStores = "order: {\"0\":\"a\",\"2\":\"c\",\"n\":0} 0,2,n\nhole and name: {\"0\":\"a\",\"1\":1.5,\"2\":\"c\",\"n\":0,\"-1\":\"m\"} 0,1,2,n,-1\nother kinds: {\"0\":0.5,\"1\":null,\"n\":1} 0,1,n\nfreeze 1 undefined\nundefined\nseal 9 undefined\nundefined\npreventExtensions 9 undefined\nundefined\n5 got\nx 5,n 2\nd1 new\ndefined: {\"3\":\"v\",\"n\":\"v\"} 3,n\ndefined under a text: {\"n\":\"v\",\"p\":\"v\"} n,p\ndefined under a negative: {\"n\":\"v\",\"-2\":\"v\"} n,-2\ntrue\nreplaced in an old object: true\nfirst in an old object: true\ngrown in an old object: true\n"_s;

static void testAnswers()
{
    checkOutputOfProgramInShell(programOfStores, answersOfStores, { "references=narrow"_s }, statementsThatAnswer);
    checkOutputOfProgramInShell(programOfStores, answersOfStores, { "references=wide"_s }, statementsThatAnswer);
    checkOutputOfProgramInShell(programOfStores, answersOfStores, { "references=narrow"_s }, statementsThatAnswer, { "--collectContinuously=1"_s });
}

std::span<const NativeTest> storesOfStubsProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "stores-of-stubs/the-stubs-serve"_s, testStubsServe },
        { "stores-of-stubs/answers"_s, testAnswers },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
