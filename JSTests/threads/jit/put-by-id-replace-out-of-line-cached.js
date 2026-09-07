//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--useFTLJIT=false")
// (FTL off: the FTL inlines these two-shape puts as MultiPutByOffset and would
// hide the inline-cache path this test is about.)
// Flag-on the baseline put_by_id call-site fast path stores inline offsets
// only; an out-of-line Replace must therefore live in the handler CHAIN (with
// the tagged-butterfly write predicate), not be installed as the site's
// inlined handler - else the chain holds only the slow-path handler, every
// out-of-line replace calls the optimize operation, the new Replace case is
// refused as a duplicate of the inlined one, and the site never caches
// (measured 10x on an owner's own objects, sixth landing round). Checks that
// out-of-line replaces run at inline-replace speed within a small factor, on
// the main thread and on a spawned thread (its own objects), and that a
// foreign thread's replaces on shared objects still land.
load("../harness.js", "caller relative");

function A(x) { this.x = x; this.y = x; this.w = x; }
function B(x) { this.w = x; this.q = x; this.y = x; this.x = x; }
// f, g, h are out-of-line on both shapes, at different offsets, so the put
// sites below stay polymorphic Replace sites served by the inline-cache
// handlers in every tier below FTL (a monomorphic site would be inlined by
// the DFG and not exercise them).
let flip = 0;
function mk() { const o = (flip++ & 1) ? new A(1) : new B(2); o.f = 0; o.g = 0; o.h = 0; o.v = 0; o.c = 0; o.p = 0; return o; }
function setInline(o, k) { o.x = k; o.y = k; o.w = k; }
noInline(setInline);
function setOutOfLine(o, k) { o.f = k; o.g = k; o.h = k; }
noInline(setOutOfLine);

function measure(objs) {
    const N = 2e6;
    for (let i = 0; i < 20000; ++i) { setInline(objs[i % objs.length], i); setOutOfLine(objs[i % objs.length], i); }
    let t0 = preciseTime();
    for (let i = 0; i < N; ++i) setInline(objs[i % objs.length], i);
    const tInline = preciseTime() - t0;
    t0 = preciseTime();
    for (let i = 0; i < N; ++i) setOutOfLine(objs[i % objs.length], i);
    const tOutOfLine = preciseTime() - t0;
    return [tInline, tOutOfLine];
}

const objs = []; for (let i = 0; i < 1000; ++i) objs.push(mk());
const [ti, to] = measure(objs);
if (typeof AMPLIFY_VERBOSE !== "undefined") print("main: inline " + (ti * 1e3).toFixed(1) + " ms, out-of-line " + (to * 1e3).toFixed(1) + " ms");
if (to > ti * 4) throw new Error("out-of-line replace on own objects is " + (to / ti).toFixed(1) + "x slower than inline replace (site not cached?)");
for (const o of objs) if (o.f !== o.g || o.g !== o.h) throw new Error("bad values");

// A spawned thread, its own objects.
const r = new Thread(() => {
    const mine = []; for (let i = 0; i < 1000; ++i) mine.push(mk());
    const [ti, to] = measure(mine);
    return [ti, to];
}).join();
if (typeof AMPLIFY_VERBOSE !== "undefined") print("thread: inline " + (r[0] * 1e3).toFixed(1) + " ms, out-of-line " + (r[1] * 1e3).toFixed(1) + " ms");
if (r[1] > r[0] * 4) throw new Error("thread: out-of-line replace on own objects is " + (r[1] / r[0]).toFixed(1) + "x slower than inline replace");

// Foreign replaces (shared objects, generic path) must still land.
new Thread(() => { for (let i = 0; i < objs.length; ++i) setOutOfLine(objs[i], -i); }).join();
for (let i = 0; i < objs.length; ++i) if (objs[i].f !== -i || objs[i].h !== -i) throw new Error("foreign out-of-line replace lost at " + i);
print("PASS");
