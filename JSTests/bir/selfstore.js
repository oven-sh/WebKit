// `*p = p; return p + n`: B3 fuses a store and an increment of its address into one post-indexed store on ARM64,
// but must not when the stored value is that address: `str x0, [x0], #imm` is UNPREDICTABLE and traps on Apple silicon.
load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n }), t8 = n => ({ u8: n });
const m = $vm.cModule(assemble({
    sigs: [{ ret: T.i64, params: [T.i64] }],
    funcs: [
        // v0 = p; Store p -> [p]; v1 = 0x78; v2 = p + v1; Ret v2
        { name: "post", sig: 0, exported: true, blocks: [[["Store", t8(MEM.i64), 0, 0, s(0)], ["ConstI64", s(0x78)], ["Add", 0, 1], ["Ret", 2]]] },
        // The increment first, as a compiler emits `*p = p; p += 15;`: v1 = 0x78; v2 = p + v1; Store p -> [p]; Ret v2
        { name: "postAfter", sig: 0, exported: true, blocks: [[["ConstI64", s(0x78)], ["Add", 0, 1], ["Store", t8(MEM.i64), 0, 0, s(0)], ["Ret", 2]]] },
        // v1 = 0x10; v2 = p + v1; Store v2 -> [p + 0x10]; Ret v2
        { name: "pre", sig: 0, exported: true, blocks: [[["ConstI64", s(0x10)], ["Add", 0, 1], ["Store", t8(MEM.i64), 2, 0, s(0x10)], ["Ret", 2]]] },
    ],
    exports: [{ name: "post", func: 0, ret: FFI.u64, args: [FFI.ptr] }, { name: "postAfter", func: 1, ret: FFI.u64, args: [FFI.ptr] }, { name: "pre", func: 2, ret: FFI.u64, args: [FFI.ptr] }],
}));
const words = new BigUint64Array(32);
const next = m.post(words);
eq(next - words[0], 0x78n, "post: the returned address is 0x78 past the stored one");
eq(words[0] !== 0n, true, "post: the address was stored");
words[0] = 0n;
const after = m.postAfter(words);
eq(after - words[0], 0x78n, "postAfter: the returned address is 0x78 past the stored one");
const at = m.pre(words);
eq(words[2], at, "pre: the incremented address was stored at itself");
eq(at - words[0], 0x10n, "pre: and is 0x10 past the base");
print("self store ok");
