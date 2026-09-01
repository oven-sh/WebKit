//@ requireOptions("--useJSThreads=1", "--useJIT=0")
// LLInt only: for (k in o) o[k] and o[k] = v over objects with more properties
// than their inline capacity take the flag-on out-of-line enumerator twins,
// which re-read the cell's structureID against the enumerator's cached
// structure before loading the butterfly. Covers the owner path, an object
// written from a spawned thread (foreign TID: the write predicate routes to
// the slow path), a segmented butterfly (both predicates route to the slow
// path), and a GlobalProperty put_to_scope.

function check(cond, msg) { if (!cond) throw new Error(msg); }

function make(n) {
    var o = {};
    for (var i = 0; i < n; i++)
        o["p" + i] = i;
    return o;
}

function sum(o) {
    var s = 0;
    for (var k in o)
        s += o[k];
    return s;
}

function store(o, v) {
    for (var k in o)
        o[k] = v;
}

const count = 20; // more properties than the literal's inline capacity
const expected = count * (count - 1) / 2;

for (var iter = 0; iter < 200; iter++) {
    var o = make(count);
    check(sum(o) === expected, "iteration " + iter + ": sum " + sum(o));
    store(o, -1);
    for (var i = 0; i < count; i++)
        check(o["p" + i] === -1, "iteration " + iter + ": p" + i + " = " + o["p" + i]);
}

{
    var o = make(count);
    var t = new Thread(() => { store(o, 7); return sum(o); });
    check(t.join() === 7 * count, "foreign store then sum");
    check(sum(o) === 7 * count, "owner sum after foreign store: " + sum(o));
    for (var i = 0; i < count; i++)
        check(o["p" + i] === 7, "foreign p" + i + " = " + o["p" + i]);
}

{
    var o = make(count);
    var t = new Thread(() => { o.foreign = 1; return 1; });
    check(t.join() === 1, "thread did not run");
    check(sum(o) === expected + 1, "segmented sum: " + sum(o));
    store(o, -3);
    for (var i = 0; i < count; i++)
        check(o["p" + i] === -3, "segmented p" + i + " = " + o["p" + i]);
    check(o.foreign === -3, "segmented foreign = " + o.foreign);
}

globalThis.globalPropertyCounter = 0;
function bump() { globalPropertyCounter = globalPropertyCounter + 1; }
for (var i = 0; i < 100; i++)
    bump();
check(globalPropertyCounter === 100, "GlobalProperty put_to_scope: " + globalPropertyCounter);
