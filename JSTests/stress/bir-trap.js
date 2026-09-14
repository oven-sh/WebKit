//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1", "--validateGraphAtEachPhase=1")

// Trap (`__builtin_trap()`) and Unreachable end a block wherever they are: in a branch not taken, in the entry
// block, in a callee inlined into a C caller, in a callee the optimizing JIT inlines into JavaScript. B3's
// validation runs after each of its phases here, so IR it would refuse does not get as far as running.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });

const intToInt = { ret: T.i32, params: [T.i32] };
// int checked(int x) { if (x == 12345) <ending>; return x + 1; }
const checked = ending => [
    [["ConstI32", s(12345)], ["Eq", 0, 1], ["Br", 2, 1, 2]],
    [[ending]],
    [["ConstI32", s(1)], ["Add", 0, 3], ["Ret", 4]],
];
// int wrapper(int x) { return callee(x) + 1; }
const wrapperOf = callee => [[["Call", callee, 1, 0], ["ConstI32", s(1)], ["Add", 1, 2], ["Ret", 3]]];

for (const ending of ["Trap", "Unreachable"]) {
    const m = $vm.cModule(assemble({
        sigs: [intToInt, { ret: T.void, params: [] }],
        funcs: [
            { name: "checked", sig: 0, exported: true, blocks: checked(ending) },
            { name: "helper", sig: 0, blocks: checked(ending) },
            { name: "inlined", sig: 0, exported: true, blocks: wrapperOf(1) },
            { name: "never", sig: 1, exported: true, blocks: [[[ending]]] },
            { name: "noreturn", sig: 0, exported: true, noinline: true, blocks: [[[ending]]] },
            // int guarded(int x) { if (x != 12345) return x; noreturn(x); }   the call's continuation is the ending too
            { name: "guarded", sig: 0, exported: true, blocks: [
                [["ConstI32", s(12345)], ["Ne", 0, 1], ["Br", 2, 1, 2]],
                [["Ret", 0]],
                [["Call", 4, 1, 0], [ending]],
            ] },
            // After a call and in a switch's default.
            { name: "switched", sig: 0, exported: true, blocks: [
                [["Call", 1, 1, 0], ["Switch", 1, 2, 1, s(8), 1]],
                [["Ret", 0]],
                [[ending]],
            ] },
        ],
        exports: [
            { name: "checked", func: 0, ret: FFI.i32, args: [FFI.i32] },
            { name: "inlined", func: 2, ret: FFI.i32, args: [FFI.i32] },
            { name: "guarded", func: 5, ret: FFI.i32, args: [FFI.i32] },
            { name: "switched", func: 6, ret: FFI.i32, args: [FFI.i32] },
        ],
    }));
    eq(m.checked(1), 2, `${ending} in a branch not taken`);
    eq(m.inlined(1), 3, `${ending} in an inlined callee`);
    eq(m.guarded(5), 5, `${ending} after a call that does not return`);
    eq(m.switched(7), 7, `${ending} in a switch's default`);
    const { checked: checkedFunction, inlined } = m;
    const hot = Function("checked", "inlined", `return function hot${ending}(n) { let t = 0; for (let i = 0; i < n; i++) t = (t + checked(i & 1023) + inlined(i & 1023)) | 0; return t; }`)(checkedFunction, inlined);
    let expected = 0;
    for (let i = 0; i < 300000; i++) expected = (expected + 2 * (i & 1023) + 3) | 0;
    for (let round = 0; round < 3; round++)
        eq(hot(300000), expected, `${ending} in a function inlined into JavaScript, round ${round}`);
}
print("trap ok");
