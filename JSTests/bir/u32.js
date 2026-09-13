load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const module = assemble({
    sigs: [{ ret: T.i32, params: [T.i32] }],
    funcs: [
        { name: "mixu", sig: 0, exported: true, blocks: [[
            ["ConstI32", s(16)], ["ShrU", 0, 1], ["Xor", 0, 2], ["ConstI32", s(0x7feb352d)], ["Mul", 3, 4],
            ["ConstI32", s(15)], ["ShrU", 5, 6], ["Xor", 5, 7], ["ConstI32", s(0x846ca68b | 0)], ["Mul", 8, 9],
            ["ConstI32", s(16)], ["ShrU", 10, 11], ["Xor", 10, 12], ["Ret", 13]]] },
        { name: "ident", sig: 0, exported: true, blocks: [[["Ret", 0]]] },
    ],
    exports: [{ name: "mixu", func: 0, ret: FFI.u32, args: [FFI.u32] }, { name: "ident", func: 1, ret: FFI.u32, args: [FFI.u32] }],
});
const m = $vm.cModule(module);
function mixJS(x) { x ^= x >>> 16; x = Math.imul(x, 0x7feb352d); x ^= x >>> 15; x = Math.imul(x, 0x846ca68b); x ^= x >>> 16; return x >>> 0; }
// the values themselves must be exact in every tier, in every kind of use
function uses(n) { let xor = 0, sum = 0, big = 0, str = ""; for (let i = 0; i < n; i++) { const v = m.mixu(i); xor ^= v; sum += v; if (v > 0x7fffffff) big++; if (i < 3) str += v + ","; } return [xor, sum, big, str].join("|"); }
function usesJS(n) { let xor = 0, sum = 0, big = 0, str = ""; for (let i = 0; i < n; i++) { const v = mixJS(i); xor ^= v; sum += v; if (v > 0x7fffffff) big++; if (i < 3) str += v + ","; } return [xor, sum, big, str].join("|"); }
for (const n of [10, 1000, 100000, 2000000]) eq(uses(n), usesJS(n), "u32 results, n=" + n);
eq(m.ident(0xffffffff), 4294967295, "max"); eq(m.ident(0x80000000), 2147483648, "2^31"); eq(m.ident(5), 5, "small");
function escape(n) { const a = []; for (let i = 0; i < n; i++) a.push(m.ident(0xfffffff0 + (i & 15))); return a[n - 1]; }
eq(escape(200000), 0xfffffff0 + ((200000 - 1) & 15), "escapes to the heap as a number");

function loopC(n) { let t = 0; for (let i = 0; i < n; i++) t ^= m.mixu(i); return t; }
function loopJS(n) { let t = 0; for (let i = 0; i < n; i++) t ^= mixJS(i); return t; }
function time(name, f) { f(1e5); f(1e6); let best = Infinity, r; for (let k = 0; k < 5; k++) { const t0 = preciseTime(); r = f(5e7); best = Math.min(best, preciseTime() - t0); } print(name.padEnd(18), (best * 1e9 / 5e7).toFixed(2), "ns/iter", r); }
time("C mixu (u32)", loopC); time("JS mix", loopJS);
