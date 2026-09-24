//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1", "--jitMemoryReservationSize=4194304")

// The code of a C module stays for as long as the process does, in the one pool all of the JITs share. When there
// is no room left in it for a function, loading the module fails with an error that names the function: nothing of
// the module has run or can be reached, what was loaded before still runs, and so does JavaScript.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });

// unsigned name(unsigned x) { for each of `rounds`: x = x * (salt + i) ^ i; return x; }   unrolled
function mixBody(salt, rounds) {
    const block = new Block(1);
    let x = 0;
    for (let i = 0; i < rounds; i++)
        x = block.def("Xor", block.def("Mul", x, block.def("ConstI32", s(salt + i))), block.def("ConstI32", s(i)));
    block.run("Ret", x);
    return [block.insts];
}
function mixReference(x, salt, rounds) {
    x >>>= 0;
    for (let i = 0; i < rounds; i++)
        x = (Math.imul(x, salt + i) ^ i) >>> 0;
    return x;
}
const unsignedToUnsigned = { ret: T.i32, params: [T.i32] };
const exportOf = (name, func) => ({ name, func, ret: FFI.u32, args: [FFI.u32] });

// A module loaded first, while there is room: a counter a later module's constructor would set, and its address.
// static int counter; int* address(void) { return &counter; }   int read(void) { return counter; }
const witness = $vm.cModule(assemble({
    sigs: [{ ret: T.i64, params: [] }, { ret: T.i32, params: [] }],
    data: { size: 4, align: 4, init: [], relocs: [] },
    funcs: [
        { name: "address", sig: 0, exported: true, blocks: [[["DataAddr", 0], ["Ret", 0]]] },
        { name: "read", sig: 1, exported: true, blocks: [[["DataAddr", 0], ["Load", b(MEM.i32), 0, s(0)], ["Ret", 1]]] },
    ],
    exports: [{ name: "address", func: 0, ret: FFI.u64, args: [] }, { name: "read", func: 1, ret: FFI.i32, args: [] }],
}));
const counterAddress = BigInt(witness.address());
eq(witness.read(), 0, "the counter starts at zero");

// The shapes loaded until one does not fit. Every one has a constructor that sets the counter.
const large = 1500, small = 3;
const setCounter = [[["ConstI64", s(counterAddress)], ["ConstI32", s(1)], ["Store", b(MEM.i32), 1, 0, s(0)], ["RetVoid"]]];
const voidToVoid = { ret: T.void, params: [] };
const shapes = [
    // One large function.
    salt => ({
        funcs: [{ name: "only", sig: 0, exported: true, blocks: mixBody(salt, large) }],
        exports: [exportOf("only", 0)],
        reference: { only: x => mixReference(x, salt, large) },
    }),
    // A small one and then a large one, and the other way round: whichever is compiled first, none of the module is left.
    salt => ({
        funcs: [{ name: "little", sig: 0, exported: true, blocks: mixBody(salt, small) }, { name: "big", sig: 0, exported: true, blocks: mixBody(salt, large) }],
        exports: [exportOf("little", 0), exportOf("big", 1)],
        reference: { little: x => mixReference(x, salt, small), big: x => mixReference(x, salt, large) },
    }),
    salt => ({
        funcs: [{ name: "big", sig: 0, exported: true, blocks: mixBody(salt, large) }, { name: "little", sig: 0, exported: true, blocks: mixBody(salt, small) }],
        exports: [exportOf("big", 0), exportOf("little", 1)],
        reference: { little: x => mixReference(x, salt, small), big: x => mixReference(x, salt, large) },
    }),
    // A small exported function that calls a large static one marked noinline, and one that calls a static one small
    // enough to have no code of its own until the second round of compiling gives it some.
    salt => ({
        funcs: [{ name: "helper", sig: 0, noinline: true, blocks: mixBody(salt, large) }, { name: "wrapper", sig: 0, exported: true, blocks: [[["Call", 0, 1, 0], ["Ret", 1]]] }],
        exports: [exportOf("wrapper", 1)],
        reference: { wrapper: x => mixReference(x, salt, large) },
    }),
    salt => ({
        funcs: [
            { name: "inlined", sig: 0, blocks: mixBody(salt, small) },
            { name: "wrapper", sig: 0, exported: true, blocks: [[["Call", 0, 1, 0], ["Ret", 1]]] },
            { name: "filler", sig: 0, exported: true, blocks: mixBody(salt, large) },
        ],
        exports: [exportOf("wrapper", 1), exportOf("filler", 2)],
        reference: { wrapper: x => mixReference(x, salt, small), filler: x => mixReference(x, salt, large) },
    }),
];
function moduleOf(shape, withConstructor) {
    const funcs = shape.funcs.slice();
    const module = { sigs: [unsignedToUnsigned, voidToVoid], funcs, exports: shape.exports };
    if (withConstructor) {
        funcs.push({ name: "constructor", sig: 1, blocks: setCounter });
        module.constructors = [funcs.length - 1];
    }
    return assemble(module);
}

// JavaScript that has run before the pool is full, so whatever it needs from the pool it has.
function javaScript(n) { let total = 0; for (let i = 0; i < n; i++) total = (total + Math.imul(i, 7)) | 0; return total; }
noInline(javaScript);
const javaScriptResult = javaScript(1000);

const loaded = [];
let error = null;
for (let salt = 1; salt < 100000 && !error; salt++) {
    const shape = shapes[salt % shapes.length](salt);
    try {
        // Until one fails, none has a constructor: the counter stays clear for the one that does.
        loaded.push({ exports: $vm.cModule(moduleOf(shape, false)), reference: shape.reference });
    } catch (e) {
        error = e;
        // The same again, this time with a constructor: it fails again, before the constructor can run.
        for (const factory of shapes) {
            const again = factory(salt);
            let secondError = null;
            try {
                $vm.cModule(moduleOf(again, true));
            } catch (e) {
                secondError = e;
            }
            if (secondError) {
                if (!(secondError instanceof TypeError) || !/^out of executable memory for '(only|big|little|helper|wrapper|inlined|filler|constructor)'$/.test(secondError.message))
                    throw new Error(`a module that does not fit: ${secondError}`);
                eq(witness.read(), 0, "the constructor of a module that did not load did not run");
            } else
                $vm.cModule(assemble({ sigs: [voidToVoid], funcs: [{ name: "clear", sig: 0, exported: true, blocks: [[["ConstI64", s(counterAddress)], ["ConstI32", s(0)], ["Store", b(MEM.i32), 1, 0, s(0)], ["RetVoid"]]] }], exports: [{ name: "clear", func: 0, ret: FFI.void, args: [] }] })).clear();
        }
    }
}
if (!error)
    throw new Error("the pool never filled up");
if (!(error instanceof TypeError) || !/^out of executable memory for '(only|big|little|helper|wrapper|inlined|filler)'$/.test(error.message))
    throw new Error(`the module that did not fit: ${error}`);
if (loaded.length < 10)
    throw new Error(`only ${loaded.length} modules fit`);

// Everything loaded before still runs, and so does JavaScript.
for (const { exports, reference } of loaded) {
    for (const [name, f] of Object.entries(reference))
        eq(exports[name](12345), f(12345), name);
}
eq(javaScript(1000), javaScriptResult, "JavaScript after the pool filled up");
eq(witness.read(), 0, "the counter at the end");
print("out of executable memory ok");
