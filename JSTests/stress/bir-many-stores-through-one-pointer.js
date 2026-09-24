//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// `int a[50000] = { x, x + 1, ... };` in a function is fifty thousand stores through one address, none of which
// changes what another stored: what it takes to compile them grows with their number, not with its square.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });
const N = 50000;

// noinline long use(int* a) { return a[0] + a[N - 1] + a[12345]; }
// long local(int x) { int a[N] = { x, x + 1, ... }; long sum = a[0] + a[1000] + ... (every thousandth); return sum + use(a); }
// long through(int x, int* a) { the same stores through a; return a[N - 1]; }
function body(base, firstValue) {
    const block = new Block(firstValue);
    const address = base === "slot" ? block.def("SlotAddr", 0) : 1;
    for (let i = 0; i < N; i++)
        block.run("Store", b(MEM.i32), block.def("Add", 0, block.def("ConstI32", s(i))), address, s(i * 4));
    let sum = block.def("ConstI64", s(0));
    for (let i = 0; i < N; i += 1000)
        sum = block.def("Add", sum, block.def("SExt32", block.def("Load", b(MEM.i32), address, s(i * 4))));
    if (base === "slot")
        sum = block.def("Add", sum, block.def("Call", 0, 1, address));
    block.run("Ret", sum);
    return [block.insts];
}
const bytes = assemble({
    sigs: [{ ret: T.i64, params: [T.i64] }, { ret: T.i64, params: [T.i32] }, { ret: T.i64, params: [T.i32, T.i64] }],
    funcs: [
        { name: "use", sig: 0, noinline: true, blocks: [[["Load", b(MEM.i32), 0, s(0)], ["Load", b(MEM.i32), 0, s((N - 1) * 4)], ["Load", b(MEM.i32), 0, s(12345 * 4)], ["Add", 1, 2], ["Add", 4, 3], ["SExt32", 5], ["Ret", 6]]] },
        { name: "local", sig: 1, exported: true, slots: [{ size: N * 4, align: 4 }], blocks: body("slot", 1) },
        { name: "through", sig: 2, exported: true, blocks: body("pointer", 2) },
    ],
    exports: [{ name: "local", func: 1, ret: FFI.i64, args: [FFI.i32] }, { name: "through", func: 2, ret: FFI.i64, args: [FFI.i32, FFI.ptr] }],
});
const before = Date.now();
const c = $vm.cModule(bytes);
const took = Date.now() - before;
let everyThousandth = 0;
for (let i = 0; i < N; i += 1000)
    everyThousandth += 7 + i;
eq(c.local(7), BigInt(everyThousandth + 7 + (7 + N - 1) + (7 + 12345)), "an array of 50,000 in a local");
const memory = new Int32Array(new ArrayBuffer(N * 4));
eq(c.through(7, memory), BigInt(everyThousandth), "50,000 stores through a pointer");
for (let i = 0; i < N; i += 997)
    eq(memory[i], 7 + i, `element ${i}`);
// Well under a second each. The square of 50,000 was half a minute.
if (took > 20000)
    throw new Error(`two functions of ${N} stores took ${took} ms to compile`);
print("many stores through one pointer ok");
