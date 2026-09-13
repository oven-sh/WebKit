load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const t8 = n => ({ u8: n });
const bytes16 = a => ({ bytes: a });
const rev32 = [12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3];
const module = assemble({
    sigs: [
        { ret: T.void, params: [T.i64, T.i64, T.f32, T.i32] },  // 0: void saxpy(float* y, const float* x, float a, int n)   n % 4 == 0
        { ret: T.void, params: [T.i64, T.i64] },                 // 1: void f(out*, in*)
        { ret: T.i32, params: [T.i64, T.i32] },                  // 2: int find_byte(const uint8_t* p16, int c)
        { ret: T.v128, params: [T.v128, T.v128] },               // 3: v128 vadd(v128, v128)
        { ret: T.i64, params: [T.i64, T.i64] },                  // 4: long(long*, long)
        { ret: T.i32, params: [T.i64, T.i32, T.i32] },           // 5: int(int*, int, int)
    ],
    funcs: [
        // for (i = 0; i < n; i += 4) y[i..] = a * x[i..] + y[i..]     locals: 0 = i (i32)
        { name: "saxpy", sig: 0, exported: true, locals: [T.i32], blocks: [
            [["Jump", 1]],
            [["LocalGet", 0], ["Lt", 4, 3], ["Br", 5, 2, 3]],
            [["LocalGet", 0], ["SExt32", 6], ["ConstI64", s(4)], ["Mul", 7, 8], ["Add", 1, 9], ["Add", 0, 9],
             ["Load", t8(MEM.v128), 10, s(0)], ["Load", t8(MEM.v128), 11, s(0)], ["VSplat", t8(LANE.f32x4), 2], ["VMul", t8(LANE.f32x4), 14, 12], ["VAdd", t8(LANE.f32x4), 15, 13],
             ["Store", t8(MEM.v128), 16, 11, s(0)], ["ConstI32", s(4)], ["Add", 6, 17], ["LocalSet", 0, 18], ["Jump", 1]],
            [["RetVoid"]],
        ] },
        // out[0..4) = reverse lanes of in; out[4..8) = max(in, reversed) (signed i32); out[8..12) = in / reversed (signed int division: no SIMD instruction)
        { name: "lanes", sig: 1, exported: true, blocks: [[
            ["Load", t8(MEM.v128), 1, s(0)], ["VShuffle", 2, 2, bytes16(rev32)], ["Store", t8(MEM.v128), 3, 0, s(0)],
            ["VMax", t8(LANE.i32x4), t8(1), 2, 3], ["Store", t8(MEM.v128), 4, 0, s(16)],
            ["VDiv", t8(LANE.i32x4), t8(1), 2, 3], ["Store", t8(MEM.v128), 5, 0, s(32)],
            ["RetVoid"],
        ]] },
        // memchr-style: compare 16 bytes against a splat, bitmask, ctz  -> index or 32 when absent
        { name: "find_byte", sig: 2, exported: true, blocks: [[
            ["Load", t8(MEM.v128), 0, s(0)], ["VSplat", t8(LANE.i8x16), 1], ["VEq", t8(LANE.i8x16), t8(0), 2, 3], ["VBitmask", t8(LANE.i8x16), 4], ["Ctz", 5], ["Ret", 6],
        ]] },
        { name: "vadd", sig: 3, blocks: [[["VAdd", t8(LANE.i32x4), 0, 1], ["Ret", 2]]] },
        // out[0..4) = vadd(in, in) through a real call taking and returning vectors in registers; out[4..8) = i8x16 multiply (expanded per lane) of in by itself
        { name: "through_call", sig: 1, exported: true, blocks: [[
            ["Load", t8(MEM.v128), 1, s(0)], ["Call", 3, 2, 2, 2], ["Store", t8(MEM.v128), 3, 0, s(0)],
            ["VMul", t8(LANE.i8x16), 2, 2], ["Store", t8(MEM.v128), 4, 0, s(16)], ["RetVoid"],
        ]] },
        // long fetch_add(long* p, long v) { return atomic_fetch_add(p, v); }
        { name: "fetch_add", sig: 4, exported: true, blocks: [[["AtomicRmw", t8(0), t8(MEM.i64), t8(4), 1, 0], ["Ret", 2]]] },
        // int cas(int* p, int expected, int desired) -> old value
        { name: "cas", sig: 5, exported: true, blocks: [[["AtomicCas", t8(MEM.i32), t8(4), t8(4), 1, 2, 0], ["Ret", 3]]] },
        // int xchg_then_load(int* p, int v, int unused): store-release v, fence, load-acquire
        { name: "store_load", sig: 5, exported: true, blocks: [[["AtomicStore", t8(MEM.i32), t8(4), 1, 0], ["Fence", t8(4)], ["AtomicLoad", t8(MEM.i32), t8(1), 0], ["Ret", 3]]] },
    ],
    exports: [
        { name: "saxpy", func: 0, ret: FFI.void, args: [FFI.ptr, FFI.ptr, FFI.f32, FFI.i32] },
        { name: "lanes", func: 1, ret: FFI.void, args: [FFI.ptr, FFI.ptr] },
        { name: "find_byte", func: 2, ret: FFI.i32, args: [FFI.ptr, FFI.i32] },
        { name: "through_call", func: 4, ret: FFI.void, args: [FFI.ptr, FFI.ptr] },
        { name: "fetch_add", func: 5, ret: FFI.i64, args: [FFI.ptr, FFI.i64] },
        { name: "cas", func: 6, ret: FFI.i32, args: [FFI.ptr, FFI.i32, FFI.i32] },
        { name: "store_load", func: 7, ret: FFI.i32, args: [FFI.ptr, FFI.i32, FFI.i32] },
    ],
});
const m = $vm.cModule(module);

const y = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]), x = new Float32Array([10, 20, 30, 40, 50, 60, 70, 80]);
m.saxpy(y, x, 0.5, 8);
eq(Array.from(y).join(), "6,12,18,24,30,36,42,48", "saxpy");

const out = new Int32Array(12), input = new Int32Array([100, -7, 3, -50]);
m.lanes(out, input);
eq(Array.from(out.subarray(0, 4)).join(), "-50,3,-7,100", "shuffle reverse");
eq(Array.from(out.subarray(4, 8)).join(), "100,3,3,100", "signed max");
eq(Array.from(out.subarray(8, 12)).join(), "-2,-2,0,0", "integer divide (lane-wise expansion): 100/-50, -7/3, 3/-7, -50/100");

const hay = new Uint8Array(16); for (let i = 0; i < 16; i++) hay[i] = 65 + i;
eq(m.find_byte(hay, 65 + 11), 11, "find_byte hit"); eq(m.find_byte(hay, 33), 32, "find_byte miss");

const o2 = new Int32Array(8), in2 = new Int32Array([1, 2, 3, 0x7f030201]);
m.through_call(o2, in2);
eq(Array.from(o2.subarray(0, 4)).join(), [2, 4, 6, (0x7f030201 * 2) | 0].join(), "vector through a real call");
eq(new Uint8Array(o2.buffer, 16, 16)[0], 1, "i8 mul lane 0"); eq(new Uint8Array(o2.buffer, 16, 16)[12], 1, "i8 mul"); eq(new Uint8Array(o2.buffer, 16, 16)[15], (0x7f * 0x7f) & 0xff, "i8 mul wraps");

const cell = new BigInt64Array([40n]);
eq(m.fetch_add(cell, 2n), 40n, "fetch_add returns old"); eq(cell[0], 42n, "fetch_add stored");
const word = new Int32Array([7]);
eq(m.cas(word, 7, 9), 7, "cas success returns old"); eq(word[0], 9, "cas stored");
eq(m.cas(word, 7, 11), 9, "cas failure returns current"); eq(word[0], 9, "cas failure leaves value");
eq(m.store_load(word, 123, 0), 123, "atomic store then load");
print("simd + atomics ok");
