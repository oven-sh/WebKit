//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// A function whose frame, alloca or outgoing arguments are a page or more moves the stack pointer past pages nothing
// has touched yet; on x86-64 it touches each on the way down first, which is what makes a stack that is committed a
// page at a time (Windows) grow, and what finds a guard page instead of what is beyond it. Here: it still computes.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });

// long frame(long x) { char local[SIZE]; local[0] = x; local[SIZE - 1] = x + 1; return opaque(local)[0] + local[SIZE - 1]; }
// long dynamic(long x, long n) { char* p = alloca(n); p[0] = x; p[n - 1] = x + 1; return opaque(p)[0] + p[n - 1]; }
// noinline char* opaque(char* p) { return p; }
const sizes = [1, 4000, 4080, 4096, 4097, 5000, 8191, 8192, 8193, 65536, 1 << 20];
const funcs = [{ name: "opaque", sig: 0, noinline: true, blocks: [[["Ret", 0]]] }];
const exports = [];
const touch = (pointer, last, next) => [
    ["Trunc", 0], ["Store", b(MEM.i8u), next, pointer, s(0)],
    ["ConstI32", s(1)], ["Add", next, next + 1], ["Store", b(MEM.i8u), next + 2, last, s(0)],
    ["Call", 0, 1, pointer], ["Load", b(MEM.i8u), next + 3, s(0)], ["Load", b(MEM.i8u), last, s(0)], ["Add", next + 4, next + 5], ["ZExt32", next + 6], ["Ret", next + 7]];
for (const size of sizes) {
    funcs.push({ name: "frame" + size, sig: 0, exported: true, slots: [{ size, align: 1 }], blocks: [[["SlotAddr", 0], ["ConstI64", s(size - 1)], ["Add", 1, 2], ...touch(1, 3, 4)]] });
    exports.push({ name: "frame" + size, func: funcs.length - 1, ret: FFI.i64, args: [FFI.i64] });
}
funcs.push({ name: "dynamic", sig: 1, exported: true, blocks: [[["StackAlloc", 1, 16], ["ConstI64", s(1)], ["Sub", 1, 3], ["Add", 2, 4], ...touch(2, 5, 6)]] });
exports.push({ name: "dynamic", func: funcs.length - 1, ret: FFI.i64, args: [FFI.i64, FFI.i64] });
funcs.push({ name: "dynamicAligned", sig: 1, exported: true, blocks: [[["StackAlloc", 1, 4096], ["ConstI64", s(1)], ["Sub", 1, 3], ["Add", 2, 4], ...touch(2, 5, 6)]] });
exports.push({ name: "dynamicAligned", func: funcs.length - 1, ret: FFI.i64, args: [FFI.i64, FFI.i64] });
const c = $vm.cModule(assemble({ sigs: [{ ret: T.i64, params: [T.i64] }, { ret: T.i64, params: [T.i64, T.i64] }], funcs, exports }));
for (const size of sizes) {
    for (let round = 0; round < 3; round++) {
        eq(c["frame" + size](10), size === 1 ? 22n : 21n, `a frame of ${size} bytes`);
        eq(c.dynamic(10, size), size === 1 ? 22n : 21n, `alloca(${size})`);
        eq(c.dynamicAligned(10, size), size === 1 ? 22n : 21n, `alloca(${size}) aligned to a page`);
    }
}
print("large frames ok");
