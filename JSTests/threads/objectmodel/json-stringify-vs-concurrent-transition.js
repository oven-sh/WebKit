//@ requireOptions("--useJSThreads=1")
// JSON.stringify's fast path walks an object's structure once and asserted
// (Debug) that the structure cannot change during the walk - true for one
// thread (it ruled out getters, toJSON and proxies first), not when another
// thread transitions the shared object meanwhile. Flag-on the fast path now
// gives up when it sees the structure move and the generic stringifier
// finishes the job. Found by the mirror harness (stress/symbol-with-json.js,
// Debug). Also a semantic check: every result parses, and only ever holds
// property values some thread stored.
load("../harness.js", "caller relative");

const shared = { o: { a: 1, b: "x" } };
const stable = [];
for (let i = 0; i < 16; ++i) stable.push({ a: 1, b: "x", c: [1, 2], d: { e: i } });

const box = { stop: 0 };
const mutator = new Thread(() => {
    let n = 0;
    do {
        // Build objects in place under the reader's nose: every store below is
        // a structure transition of the object the reader may be walking.
        const o = { a: 1, b: "x" };
        shared.o = o;
        o.c = n; o.d = n + 1; o.e = "s" + (n & 3); o.f = null; o.g = true; o.h = n & 1;
        delete o.c;                       // and a removal (dictionary)
        o.i = n;
        const t = stable[n & 15];
        t["p" + (n & 7)] = n; delete t["p" + ((n + 4) & 7)];
        n++;
    } while (!Atomics.load(box, "stop"));
    return n;
});

let count = 0;
const deadline = Date.now() + 1500;
while (Date.now() < deadline) {
    for (let k = 0; k < 64; ++k) {
        const s = JSON.stringify(shared.o);
        const back = JSON.parse(s);          // must be well-formed
        if (back.a !== 1 || back.b !== "x") throw new Error("lost a stable property: " + s);
        count++;
    }
    JSON.parse(JSON.stringify(stable));      // nested walk over objects being reshaped
}
Atomics.store(box, "stop", 1);
if (!(mutator.join() > 0 && count > 0)) throw new Error("did not run");
print("PASS");
