//@ skip if !$isFTLPlatform
//@ skip if $hostOS == "windows"
//@ requireOptions("--useDollarVM=1")

// A call that passes a lot on the stack (a large structure by value) gets the room when it is made and gives it back:
// it is not part of every activation of the function that makes the call, so a recursive function that passes 16 KB
// by value once in a thousand levels goes as deep as one that does not.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });

// struct Big { char c[SIZE]; };
// noinline long leaf(struct Big big, long a0, ..., long a<WORDS - 1>) { return big.c[5] + big.c[SIZE - 1] + a<WORDS - 1>; }
// long rec(long n, struct Big* p) { if (!n) return 0; long r = n % 1000 == 0 ? leaf(*p, 1, ..., WORDS) : 0; return r + rec(n - 1, p) + 1; }
// `leafIsNoinline` or not: left to the loader, a callee with a caller is inlined when its own objects, the copy of what it
// is passed by value among them, are small.
function moduleOf(SIZE, WORDS, leafIsNoinline) {
    const leafParams = [{ byval: SIZE }, ...new Array(WORDS).fill(T.i64)];
    const leaf = new Block(1 + WORDS);
    const low = leaf.def("ZExt32", leaf.def("Load", b(MEM.i8u), 0, s(5)));
    const high = leaf.def("ZExt32", leaf.def("Load", b(MEM.i8u), 0, s(SIZE - 1)));
    leaf.run("Ret", leaf.def("Add", leaf.def("Add", low, high), WORDS));
    const entry = new Block(2);
    entry.run("Br", entry.def("Eq", 0, entry.def("ConstI64", s(0))), 1, 2);
    const zero = new Block(entry.next);
    zero.run("Ret", zero.def("ConstI64", s(0)));
    const test = new Block(zero.next);
    test.run("LocalSet", 0, test.def("ConstI64", s(0)));
    test.run("Br", test.def("Eq", test.def("Rem", 0, test.def("ConstI64", s(1000))), test.def("ConstI64", s(0))), 3, 4);
    const rare = new Block(test.next);
    const words = [];
    for (let i = 0; i < WORDS; i++)
        words.push(rare.def("ConstI64", s(i + 1)));
    rare.run("LocalSet", 0, rare.def("Call", 0, 1 + WORDS, 1, ...words));
    rare.run("Jump", 4);
    const rest = new Block(rare.next);
    const deeper = rest.def("Call", 1, 2, rest.def("Sub", 0, rest.def("ConstI64", s(1))), 1);
    rest.run("Ret", rest.def("Add", rest.def("Add", rest.def("LocalGet", 0), deeper), rest.def("ConstI64", s(1))));
    return assemble({
        sigs: [{ ret: T.i64, params: leafParams }, { ret: T.i64, params: [T.i64, T.i64] }],
        funcs: [
            { name: "leaf", sig: 0, noinline: leafIsNoinline, blocks: [leaf.insts] },
            { name: "rec", sig: 1, exported: true, noinline: true, locals: [T.i64], blocks: [entry.insts, zero.insts, test.insts, rare.insts, rest.insts] },
        ],
        exports: [{ name: "rec", func: 1, ret: FFI.i64, args: [FFI.i64, FFI.ptr] }],
    });
}
const DEPTH = 5000;
for (const SIZE of [200, 264, 512, 4096, 16384, 1 << 20]) {
    for (const WORDS of [1, 12]) {
        const big = new Uint8Array(new ArrayBuffer(SIZE));
        big[5] = 3;
        big[SIZE - 1] = 4;
        for (const leafIsNoinline of [true, false]) {
            const rec = $vm.cModule(moduleOf(SIZE, WORDS, leafIsNoinline)).rec;
            eq(rec(DEPTH, big), BigInt(DEPTH + (DEPTH / 1000) * (7 + WORDS)), `a structure of ${SIZE} bytes and ${WORDS} integers passed on a rare path${leafIsNoinline ? " to a noinline function" : ""}, ${DEPTH} levels deep`);
        }
    }
}
// Many plain arguments make as much, without a structure.
{
    const WORDS = 300;
    const callee = new Block(WORDS);
    let sum = 0;
    for (let i = 1; i < WORDS; i++)
        sum = callee.def("Add", sum, i);
    callee.run("Ret", sum);
    const caller = new Block(1);
    const words = [];
    for (let i = 0; i < WORDS; i++)
        words.push(caller.def("Add", 0, caller.def("ConstI64", s(i))));
    caller.run("Ret", caller.def("Call", 0, WORDS, ...words));
    const many = $vm.cModule(assemble({
        sigs: [{ ret: T.i64, params: new Array(WORDS).fill(T.i64) }, { ret: T.i64, params: [T.i64] }],
        funcs: [{ name: "callee", sig: 0, noinline: true, blocks: [callee.insts] }, { name: "caller", sig: 1, exported: true, blocks: [caller.insts] }],
        exports: [{ name: "caller", func: 1, ret: FFI.i64, args: [FFI.i64] }],
    })).caller;
    eq(many(1000), BigInt(WORDS * 1000 + (WORDS * (WORDS - 1)) / 2), `${WORDS} integer arguments`);
}
print("arguments make room at the call ok");
