//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1", "--useConcurrentJIT=0")

// A private helper with one call site is inlined into its caller when the module loads, so it gets no
// code of its own. The optimizing JIT lowers the small exported caller a second time, into JavaScript,
// after the large helper's body has been released: that lowering calls the helper, which has to exist.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });

// unsigned mix(unsigned x) { for each of `rounds`: x = x * 3 + i; x ^= x >> 7; x += x << 3; return x; }   (unrolled: 9 instructions a round)
function mixBody(rounds) {
    const block = new Block(1);
    let x = 0;
    for (let i = 1; i <= rounds; i++) {
        x = block.def("Add", block.def("Mul", x, block.def("ConstI32", s(3))), block.def("ConstI32", s(i)));
        x = block.def("Xor", x, block.def("ShrU", x, block.def("ConstI32", s(7))));
        x = block.def("Add", x, block.def("Shl", x, block.def("ConstI32", s(3))));
    }
    block.run("Ret", x);
    return [block.insts];
}
function mixReference(x, rounds) {
    x >>>= 0;
    for (let i = 1; i <= rounds; i++) { x = (Math.imul(x, 3) + i) >>> 0; x = (x ^ (x >>> 7)) >>> 0; x = (x + (x << 3)) >>> 0; }
    return x;
}
// unsigned wrapper(unsigned x) { return helper(x) + 1; }
const wrapperOf = callee => [[["Call", callee, 1, 0], ["ConstI32", s(1)], ["Add", 1, 2], ["Ret", 3]]];

const unsignedToUnsigned = { ret: T.i32, params: [T.i32] };
const exportOf = (name, func) => ({ name, func, ret: FFI.u32, args: [FFI.u32] });

let runs = 0;
function check(what, module, expected) {
    const before = $vm.ffiCompileCounts().ftlInlineC;
    const exports = $vm.cModule(assemble(module));
    for (const [name, reference] of Object.entries(expected)) {
        const f = exports[name];
        eq(f(5), reference(5), `${what}: ${name}, first call`);
        // Code of its own (so source of its own) for each function under test: the optimizing JIT only
        // splices in a callee that is the only one a call site has seen.
        const run = Function("f", `return function run${++runs}(n) { let t = 0; for (let i = 0; i < n; i++) t = (t ^ f(i)) >>> 0; return t; }`)(f);
        function runReference(n) { let t = 0; for (let i = 0; i < n; i++) t = (t ^ reference(i)) >>> 0; return t; }
        for (let round = 0; round < 6; round++)
            eq(run(100000), runReference(100000), `${what}: ${name}, round ${round}`);
    }
    if ($vm.useFTLJIT() && $vm.ffiCompileCounts().ftlInlineC <= before)
        throw new Error(`${what}: no C body was inlined into JavaScript, so this tested nothing`);
}

// The instruction counts around Options::maximumFFIInlineCInstructionCount (200), where a body stops being kept.
for (const rounds of [1, 22, 23, 28, 100, 220]) {
    check(`helper of ${rounds * 9 + 1} instructions`, {
        sigs: [unsignedToUnsigned],
        funcs: [
            { name: "helper", sig: 0, blocks: mixBody(rounds) },
            { name: "wrapper", sig: 0, exported: true, blocks: wrapperOf(0) },
        ],
        exports: [exportOf("wrapper", 1)],
    }, { wrapper: x => (mixReference(x, rounds) + 1) >>> 0 });
}

// wrapper -> small (kept) -> large (released): the small one is inlined again, the large one called.
check("two levels", {
    sigs: [unsignedToUnsigned],
    funcs: [
        { name: "large", sig: 0, blocks: mixBody(40) },
        { name: "small", sig: 0, blocks: wrapperOf(0) },
        { name: "wrapper", sig: 0, exported: true, blocks: wrapperOf(1) },
    ],
    exports: [exportOf("wrapper", 2)],
}, { wrapper: x => (mixReference(x, 40) + 2) >>> 0 });

// A helper with two callers, one whose address is taken (it has code anyway), one marked always_inline and
// one marked noinline.
check("two wrappers of one helper", {
    sigs: [unsignedToUnsigned],
    funcs: [
        { name: "helper", sig: 0, blocks: mixBody(30) },
        { name: "first", sig: 0, exported: true, blocks: wrapperOf(0) },
        { name: "second", sig: 0, exported: true, blocks: wrapperOf(0) },
    ],
    exports: [exportOf("first", 1), exportOf("second", 2)],
}, { first: x => (mixReference(x, 30) + 1) >>> 0, second: x => (mixReference(x, 30) + 1) >>> 0 });

check("helper whose address is taken", {
    sigs: [unsignedToUnsigned, { ret: T.i64, params: [] }],
    funcs: [
        { name: "helper", sig: 0, blocks: mixBody(30) },
        { name: "wrapper", sig: 0, exported: true, blocks: wrapperOf(0) },
        { name: "address", sig: 1, exported: true, blocks: [[["FuncAddr", 0], ["Ret", 0]]] },
    ],
    exports: [exportOf("wrapper", 1)],
}, { wrapper: x => (mixReference(x, 30) + 1) >>> 0 });

for (const flag of ["alwaysInline", "noinline"]) {
    check(`${flag} helper`, {
        sigs: [unsignedToUnsigned],
        funcs: [
            { name: "helper", sig: 0, [flag]: true, blocks: mixBody(30) },
            { name: "wrapper", sig: 0, exported: true, blocks: wrapperOf(0) },
        ],
        exports: [exportOf("wrapper", 1)],
    }, { wrapper: x => (mixReference(x, 30) + 1) >>> 0 });
}

// A wrapper that is itself too large to be inlined into JavaScript is simply called.
{
    const exports = $vm.cModule(assemble({
        sigs: [unsignedToUnsigned],
        funcs: [{ name: "large", sig: 0, exported: true, blocks: mixBody(30) }],
        exports: [exportOf("large", 0)],
    }));
    const large = exports.large;
    function run(n) { let t = 0; for (let i = 0; i < n; i++) t = (t ^ large(i)) >>> 0; return t; }
    function runReference(n) { let t = 0; for (let i = 0; i < n; i++) t = (t ^ mixReference(i, 30)) >>> 0; return t; }
    for (let round = 0; round < 6; round++)
        eq(run(100000), runReference(100000), `large exported function, round ${round}`);
}
print("inlined callee without code ok");
