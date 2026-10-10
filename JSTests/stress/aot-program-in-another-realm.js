//@ skip if $architecture != "arm64"
//@ requireOptions("--compileMainScriptAheadOfTime=1")
//@ defaultRun
//@ run("gc-stress", "--slowPathAllocsBetweenGCs=20")

var counter = 0;
let lexical = 10;
class Counter { static made = 0; constructor() { Counter.made++; } get next() { return count(); } }
class Empty { }
function count() { return ++counter + lexical; }
function call(f) { return f() * 100 + count(); }
function state() { return [counter, lexical, Counter.made].join(); }
function bump() { lexical++; return new Counter().next; }

if (!globalThis.isOther) {
    const others = [createGlobalObject(), createGlobalObject()];
    for (const other of others) {
        other.isOther = true;
        other.load("aot-program-in-another-realm.js");
    }
    const [a, b] = others;
    const actual = JSON.stringify([
        count(), a.count(), a.count(), b.count(), a.bump(), state(), a.state(), b.state(),
        call(a.count), a.call(b.count), b.call(count),
        a.count === b.count, new a.Function("return 1")() === 1, a.eval("new Empty() instanceof Empty"),
        [count, a.count, b.count, a.call, b.bump, a.state].map(isAOTCompiled),
    ]);
    const expected = JSON.stringify([
        11, 11, 12, 11, 14, "1,10,0", "3,11,1", "1,10,0",
        1512, 1216, 1313,
        false, true, true,
        [true, true, true, true, true, true],
    ]);
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}
