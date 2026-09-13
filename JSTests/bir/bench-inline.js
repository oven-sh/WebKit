load("./asm.js", "caller relative");
const s = n => ({ s: n });
const bytes = assemble({
    sigs: [{ ret: T.i32, params: [T.i32, T.i32] }, { ret: T.i32, params: [T.i32] }],
    funcs: [
        { name: "add", sig: 0, exported: true, blocks: [[["Add", 0, 1], ["Ret", 2]]] },
        // uint32_t mix(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
        { name: "mix", sig: 1, exported: true, blocks: [[
            ["ConstI32", s(16)], ["ShrU", 0, 1], ["Xor", 0, 2],            // v3
            ["ConstI32", s(0x7feb352d)], ["Mul", 3, 4],                     // v5
            ["ConstI32", s(15)], ["ShrU", 5, 6], ["Xor", 5, 7],            // v8
            ["ConstI32", s(0x846ca68b | 0)], ["Mul", 8, 9],                 // v10
            ["ConstI32", s(16)], ["ShrU", 10, 11], ["Xor", 10, 12],        // v13
            ["Ret", 13],
        ]] },
    ],
    exports: [
        { name: "add", func: 0, ret: FFI.i32, args: [FFI.i32, FFI.i32] },
        { name: "mix", func: 1, ret: FFI.i32, args: [FFI.i32] },
    ],
});
const m = $vm.cModule(bytes);
function mixJS(x) { x ^= x >>> 16; x = Math.imul(x, 0x7feb352d); x ^= x >>> 15; x = Math.imul(x, 0x846ca68b); x ^= x >>> 16; return x | 0; }

function runAdd(n) { let t = 0; for (let i = 0; i < n; i++) t = m.add(t, i); return t; }
function runMix(n) { let t = 0; for (let i = 0; i < n; i++) t ^= m.mix(i); return t; }
function runMixJS(n) { let t = 0; for (let i = 0; i < n; i++) t ^= mixJS(i); return t; }

function time(name, f, n) {
    f(1e5); f(1e5); f(1e6); // warm through FTL
    let best = Infinity, r;
    for (let k = 0; k < 5; k++) { const t0 = preciseTime(); r = f(n); best = Math.min(best, preciseTime() - t0); }
    print(name.padEnd(10), (best * 1e9 / n).toFixed(2), "ns/iter", "result", r);
}
const N = 5e7;
time("add", runAdd, N);
time("mix C", runMix, N);
time("mix JS", runMixJS, N);
if (runMix(1000) !== runMixJS(1000)) throw new Error("mix mismatch");
print(JSON.stringify($vm.ffiCompileCounts()));
