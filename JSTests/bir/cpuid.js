load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const t8 = n => ({ u8: n });
// void cpuid(int leaf, int subleaf, unsigned out[4])
const m = $vm.cModule(assemble({
    sigs: [{ ret: T.void, params: [T.i32, T.i32, T.i64] }],
    funcs: [{ name: "cpuid", sig: 0, exported: true, blocks: [[
        ["CpuId", 0, 1],
        ["Store", t8(MEM.i32), 3, 2, s(0)], ["Store", t8(MEM.i32), 4, 2, s(4)], ["Store", t8(MEM.i32), 5, 2, s(8)], ["Store", t8(MEM.i32), 6, 2, s(12)],
        ["RetVoid"]]] }],
    exports: [{ name: "cpuid", func: 0, ret: FFI.void, args: [FFI.i32, FFI.i32, FFI.ptr] }],
}));
const out = new Uint32Array(4);
m.cpuid(0, 0, out);
const vendor = String.fromCharCode(...new Uint8Array(new Uint32Array([out[1], out[3], out[2]]).buffer));
print("cpuid(0): max leaf", out[0], "vendor", JSON.stringify(vendor));
if (!["AuthenticAMD", "GenuineIntel"].includes(vendor)) throw new Error("unexpected vendor string");
m.cpuid(1, 0, out);
eq((out[3] >>> 26) & 1, 1, "SSE2 bit");
for (let i = 0; i < 1e5; i++) m.cpuid(7, 0, out);
print("leaf 7 ebx: avx2", (out[1] >>> 5) & 1, " bmi2", (out[1] >>> 8) & 1);
print("cpuid ok");
