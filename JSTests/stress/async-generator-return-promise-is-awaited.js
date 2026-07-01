// Strict mode enables proper tail calls, which is required to exercise the
// bug: `return <call-expr>` would otherwise be emitted as op_tail_call.
"use strict";

function shouldBe(actual, expected) {
  if (actual !== expected)
    throw new Error(`Bad value: ${actual}, expected: ${expected}`);
}

// A `return <expr>` in an async generator body Awaits the operand before it
// completes (ReturnStatement, GetGeneratorKind() === async). The operand must
// not be emitted as a tail call: a proper tail call would return the operand
// directly and skip that Await, so `return Promise.resolve(v)` would resolve
// next() with the Promise itself instead of v.

let results = [];
function run(label, makeGen) {
  makeGen().next().then(
    arg => { results.push(label + ":value=" + arg.value + ":done=" + arg.done); },
    err => { results.push(label + ":reject=" + err); }
  );
}

// Call expressions in return position (the previously-broken cases).
run("resolve", () => { async function* g() { return Promise.resolve("a"); } return g(); });
run("all", () => { async function* g() { return Promise.all([Promise.resolve("b")]); } return g(); });
run("race", () => { async function* g() { return Promise.race([Promise.resolve("c")]); } return g(); });
run("asyncIIFE", () => { async function* g() { return (async () => "d")(); } return g(); });

// Rejected operand must turn into a rejected next() promise.
run("reject", () => { async function* g() { return Promise.reject("boom"); } return g(); });

// A thenable (not a native Promise) is also awaited.
run("thenable", () => { async function* g() { return { then(res) { res("e"); } }; } return g(); });

// Non-call operands that already worked must keep working.
run("plain", () => { async function* g() { return "f"; } return g(); });
run("explicitAwait", () => { async function* g() { return await Promise.resolve("g"); } return g(); });
run("newPromise", () => { async function* g() { return new Promise(r => r("h")); } return g(); });

drainMicrotasks();

results.sort();
shouldBe(results.join("\n"),
  [
    "all:value=b:done=true",
    "asyncIIFE:value=d:done=true",
    "explicitAwait:value=g:done=true",
    "newPromise:value=h:done=true",
    "plain:value=f:done=true",
    "race:value=c:done=true",
    "reject:reject=boom",
    "resolve:value=a:done=true",
    "thenable:value=e:done=true",
  ].join("\n"));

// yield then return: the returned Promise is still awaited for the final result.
let seq = [];
async function* withYield() {
  yield 1;
  return Promise.resolve("last");
}
(async () => {
  const it = withYield();
  const a = await it.next();
  const b = await it.next();
  seq.push("yield:" + a.value + ":" + a.done);
  seq.push("return:" + b.value + ":" + b.done + ":" + (b.value instanceof Promise));
})();

drainMicrotasks();

shouldBe(seq.join("\n"),
  ["yield:1:false", "return:last:true:false"].join("\n"));
