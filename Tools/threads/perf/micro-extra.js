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
