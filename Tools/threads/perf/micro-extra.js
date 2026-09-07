// Extra micro set for PERF-RESULTS (fifth round): prints "BENCH <name> <ms>"
// lines like JSTests/threads/bench/harness.js; best-of-5 inner runs each.
function time(name, f) {
    f();
    let best = 1e9;
    for (let r = 0; r < 5; ++r) { const t0 = preciseTime(); f(); best = Math.min(best, (preciseTime() - t0) * 1000); }
    print("BENCH", name, best.toFixed(3));
}
time("obj-literal-5", () => { let s = 0; for (let i = 0; i < 2e6; ++i) { const o = { a: i, b: 2, c: 3, d: 4, e: 5 }; s += o.a + o.e; } return s; });
time("class-ctor-4", () => { class P { constructor(i) { this.x = i; this.y = 1; this.z = 2; this.w = 3; } } let s = 0; for (let i = 0; i < 2e6; ++i) s += new P(i).w; return s; });
time("add-props-escaped", () => { function mk() { return {}; } noInline(mk); let s = 0; for (let i = 0; i < 2e6; ++i) { const o = mk(); o.a = i; o.b = 1; o.c = 2; s += o.c; } return s; });
time("int-loop-3e8", () => { let s = 0; for (let i = 0; i < 3e8; ++i) s = (s + i) | 0; return s; });
time("closure-calls-20M", () => { let k = 0; const f = (x) => x + k; let s = 0; for (let i = 0; i < 2e7; ++i) s += f(i); return s; });
time("map-set-get-2M", () => { const m = new Map(); for (let i = 0; i < 2e6; ++i) m.set("k" + (i & 65535), i); let s = 0; for (let i = 0; i < 2e6; ++i) s += m.get("k" + (i & 65535)) | 0; return s; });
time("regexp-exec-1M", () => { const re = /(\d+)-(\w+)/; let s = 0; for (let i = 0; i < 1e6; ++i) { const m = re.exec("id " + i + "-abc"); s += m[1].length; } return s; });
time("throw-catch-200k", () => { let s = 0; for (let i = 0; i < 2e5; ++i) { try { throw new Error("x" + i); } catch (e) { s += e.message.length; } } return s; });
time("json-parse-200k", () => { const src = JSON.stringify({ a: 1, b: [1, 2, 3], c: { d: "x", e: null }, f: "hello" }); let s = 0; for (let i = 0; i < 2e5; ++i) s += JSON.parse(src).b.length; return s; });
time("json-stringify-200k", () => { const o = { a: 1, b: [1, 2, 3], c: { d: "x", e: null }, f: "hello" }; let s = 0; for (let i = 0; i < 2e5; ++i) s += JSON.stringify(o).length; return s; });
time("string-concat-2M", () => { let s = ""; for (let i = 0; i < 2e6; ++i) { s += "ab"; if (s.length > 4096) s = ""; } return s.length; });
time("array-push-pop-10M", () => { const a = []; let s = 0; for (let i = 0; i < 1e7; ++i) { a.push(i); if (a.length > 64) s += a.pop() + a.shift(); } return s; });
time("typed-array-sum-50M", () => { const a = new Float64Array(1024); for (let i = 0; i < 1024; ++i) a[i] = i; let s = 0; for (let r = 0; r < 5e4; ++r) for (let i = 0; i < 1024; ++i) s += a[i]; return s; });
time("proto-method-calls-20M", () => { class A { f(x) { return x + 1; } } const a = new A(); let s = 0; for (let i = 0; i < 2e7; ++i) s = a.f(s); return s; });
// Sixth round.
time("array-int32-to-double-relabel-200k", () => { let s = 0; for (let i = 0; i < 2e5; ++i) { const a = [1, 2, 3, 4]; a[1] = 2.5; a.push(i); s += a.length; } return s; });
time("astar-like-nodes", () => { function GridNode(x, y, w) { this.x = x; this.y = y; this.weight = w; } function clean(n) { n.f = 0; n.g = 0; n.h = 0; n.visited = false; n.closed = false; n.parent = null; } let keep; for (let it = 0; it < 20; ++it) { keep = []; for (let i = 0; i < 10000; ++i) keep.push(new GridNode(i, i, 1)); for (const nd of keep) clean(nd); for (let r = 0; r < 5; ++r) for (const nd of keep) clean(nd); } return keep.length; });
time("megamorphic-put-transition-1M", () => { const ctors = []; for (let k = 0; k < 40; ++k) ctors.push(new Function("this.k" + k + " = 1;")); function add(o, v) { o.p = v; o.q = v; } let s = 0; for (let i = 0; i < 1e6; ++i) { const o = new ctors[i % 40](); add(o, i); s += o.q; } return s; });
time("out-of-line-replace-poly-3M", () => { function A(x) { this.x = x; this.y = x; this.w = x; } function B(x) { this.w = x; this.q = x; this.y = x; this.x = x; } const objs = []; for (let i = 0; i < 1000; ++i) { const o = (i & 1) ? new A(1) : new B(2); o.f = 0; o.g = 0; o.h = 0; o.v = 0; o.c = 0; o.p = 0; objs.push(o); } function set(o, k) { o.f = k; o.g = k; o.h = k; } let s = 0; for (let i = 0; i < 3e6; ++i) { const o = objs[i % 1000]; set(o, i); s += o.h; } return s; });
if (typeof Thread === "function") {
    // Owner transitions after the structure's thread-local sets fired (SPEC-objectmodel E4-C).
    time("transitions-after-fire-2M", () => { function mk() { return {}; } const probe = []; for (let k = 0; k <= 4; ++k) { const o = mk(); if (k > 0) o.a = 1; if (k > 1) o.b = 1; if (k > 2) o.c = 1; if (k > 3) o.d = 1; probe.push(o); } new Thread(() => { for (const o of probe) o.zz = 1; }).join(); let s = 0; for (let i = 0; i < 2e6; ++i) { const o = mk(); o.a = i; o.b = 1; o.c = 2; o.d = 3; s += o.d; } return s; });
}
