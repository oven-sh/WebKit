//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// One local in memory used a few times, ten times, eleven times (where B3 stops folding an address into its
// users) and fifty times: at every offset and width, through a computed index, over-aligned, and as a value that is
// stored and passed along.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });

// long f(long x, long i) { T a[count]; for each k: a[k] = x + k; escape(a); return a[0] + ... + a[count - 1] + 1000 * a[i % count] + ((long)a % align); }
function uses(count, kind, width, align) {
    const block = new Block(2);
    const narrow = kind !== MEM.i64;
    for (let k = 0; k < count; k++) {
        const value = block.def("Add", 0, block.def("ConstI64", s(k)));
        block.run("Store", b(kind), narrow ? block.def("Trunc", value) : value, block.def("SlotAddr", 0), s(k * width));
    }
    // The address escapes into the second slot, so the stores above and the loads below all stay.
    block.run("Store", b(MEM.i64), block.def("SlotAddr", 0), block.def("SlotAddr", 1), s(0));
    const escaped = block.def("Load", b(MEM.i64 | 0x80), block.def("SlotAddr", 1), s(0));
    let sum = block.def("URem", escaped, block.def("ConstI64", s(align)));
    const signedKind = kind === MEM.i8u ? MEM.i8s : kind === MEM.i16u ? MEM.i16s : kind;
    const load = address => { const loaded = block.def("Load", b(signedKind), address, s(0)); return narrow ? block.def("SExt32", loaded) : loaded; };
    for (let k = 0; k < count; k++) {
        const loaded = block.def("Load", b(signedKind), block.def("SlotAddr", 0), s(k * width));
        sum = block.def("Add", sum, narrow ? block.def("SExt32", loaded) : loaded);
    }
    const index = block.def("Mul", block.def("URem", 1, block.def("ConstI64", s(count))), block.def("ConstI64", s(width)));
    sum = block.def("Add", sum, block.def("Mul", load(block.def("Add", block.def("SlotAddr", 0), index)), block.def("ConstI64", s(1000))));
    block.run("Ret", sum);
    return {
        sigs: [{ ret: T.i64, params: [T.i64, T.i64] }],
        funcs: [{ name: "f", sig: 0, exported: true, slots: [{ size: count * width, align }, { size: 8, align: 8 }], blocks: [block.insts] }],
        exports: [{ name: "f", func: 0, ret: FFI.i64, args: [FFI.i64, FFI.i64] }],
    };
}
let checked = 0;
for (const count of [1, 5, 10, 11, 12, 50]) {
    for (const [kind, width] of [[MEM.i8u, 1], [MEM.i16u, 2], [MEM.i32, 4], [MEM.i64, 8]]) {
        for (const align of [width, 16, 32, 256]) {
            const f = $vm.cModule(assemble(uses(count, kind, width, align))).f;
            for (const [x, i] of [[3n, 0n], [-7n, 4n], [100n, 49n]]) {
                const wrap = value => BigInt.asIntN(width * 8, value);
                let expected = 0n;
                for (let k = 0; k < count; k++) expected += wrap(x + BigInt(k));
                expected += 1000n * wrap(x + i % BigInt(count));
                eq(f(x, i), expected, `${count} elements of ${width} bytes aligned to ${align}, f(${x}, ${i})`);
                checked++;
            }
        }
    }
}
print("stack address uses ok:", checked);
