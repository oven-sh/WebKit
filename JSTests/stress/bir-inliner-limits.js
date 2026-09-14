//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// What bounds the C-to-C inliner besides instruction counts: how deep it nests, how much frame a callee brings
// with it, and how much an always_inline nest may add to one function.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });
const intToInt = { ret: T.i32, params: [T.i32] };
const exportOf = (name, func) => ({ name, func, ret: FFI.i32, args: [FFI.i32] });

// f0 calls f1 calls ... calls the last, which returns its argument. Each level of inlining is a level of
// recursion in the lowering, so a chain is inlined a bounded number of levels at a time.
for (const flag of [undefined, "alwaysInline", "inlineHint"]) {
    for (const length of [2, 64, 65, 100, 1000, ...(flag === "alwaysInline" ? [20000] : [])]) {
        const funcs = [];
        for (let i = 0; i < length; i++)
            funcs.push({ name: "f" + i, sig: 0, exported: !i, [flag]: i > 0, blocks: [i + 1 < length ? [["Call", i + 1, 1, 0], ["Ret", 1]] : [["Ret", 0]]] });
        const m = $vm.cModule(assemble({ sigs: [intToInt], funcs, exports: [exportOf("f0", 0)] }));
        eq(m.f0(length), length, `a chain of ${length} functions${flag ? ` marked ${flag}` : ""}`);
    }
}
// Recursion is not inlined into itself, directly or through another function.
{
    // int down(int n) { return n ? down(n - 1) + 1 : 0; }     int a(int n) { return n ? b(n - 1) + 1 : 0; }  and b likewise
    const counting = callee => [[["Br", 0, 1, 2]], [["ConstI32", s(1)], ["Sub", 0, 1], ["Call", callee, 1, 2], ["Add", 3, 1], ["Ret", 4]], [["ConstI32", s(0)], ["Ret", 5]]];
    const m = $vm.cModule(assemble({
        sigs: [intToInt],
        funcs: [{ name: "down", sig: 0, exported: true, alwaysInline: true, blocks: counting(0) }, { name: "a", sig: 0, exported: true, blocks: counting(2) }, { name: "b", sig: 0, alwaysInline: true, blocks: counting(1) }],
        exports: [exportOf("down", 0), exportOf("a", 1)],
    }));
    eq(m.down(1000), 1000, "a function that calls itself");
    eq(m.a(1001), 1001, "two functions that call each other");
}

// A callee's locals become the caller's for as long as the caller runs. A helper with a large buffer, called on a
// rare path of a recursive function, is therefore not inlined: the recursion would carry the buffer at every level.
//   static int leaf(int n) { char buffer[SIZE]; memset(buffer, n, SIZE); return buffer[n & (SIZE - 1)]; }
//   int recurse(int n) { if (!n) return 0; int r = n % 1000 == 0 ? leaf(n) : 0; return r + recurse(n - 1) + 1; }
function recursion(bufferSize, leafFlag, viaMiddle) {
    const leaf = { name: "leaf", sig: 0, [leafFlag]: true, slots: [{ size: bufferSize, align: 1 }], blocks: [[
        ["SlotAddr", 0], ["ConstI64", s(bufferSize)], ["MemSet", 1, 0, 2], ["ConstI32", s(bufferSize - 1)], ["And", 0, 3], ["SExt32", 4], ["Add", 1, 5],
        ["Load", b(MEM.i8s), 6, s(0)], ["Ret", 7]]] };
    // int middle(int n) { return leaf(n); }
    const middle = { name: "middle", sig: 0, blocks: [[["Call", 0, 1, 0], ["Ret", 1]]] };
    const recurse = { name: "recurse", sig: 0, exported: true, locals: [T.i32], blocks: [
        [["Br", 0, 1, 5]],
        [["ConstI32", s(1000)], ["Rem", 0, 1], ["Br", 2, 3, 2]],
        [["Call", viaMiddle ? 1 : 0, 1, 0], ["LocalSet", 0, 3], ["Jump", 3]],
        [["ConstI32", s(1)], ["Sub", 0, 4], ["Call", 2, 1, 5], ["LocalGet", 0], ["Add", 6, 7], ["Add", 8, 4], ["Ret", 9]],
        [["Unreachable"]],
        [["ConstI32", s(0)], ["Ret", 10]],
    ] };
    return { sigs: [intToInt], funcs: [leaf, middle, recurse], exports: [exportOf("recurse", 2)] };
}
function expectedRecursion(n) { let total = 0; for (let i = 1; i <= n; i++) total += 1 + (i % 1000 === 0 ? (i << 24 >> 24) : 0); return total; }
// (512 bytes are few enough to come along: 5000 levels of that fit the stack; 5000 levels of any of the others would not.)
for (const bufferSize of [512, 2048, 4096, 16384, 1 << 20]) {
    for (const viaMiddle of [false, true]) {
        const m = $vm.cModule(assemble(recursion(bufferSize, undefined, viaMiddle)));
        eq(m.recurse(5000), expectedRecursion(5000), `5000 levels of recursion around a helper with ${bufferSize} bytes of locals${viaMiddle ? ", reached through another helper" : ""}`);
    }
}
// always_inline is the author's decision and stays one: the buffer is in every frame, so this recursion is a short one.
eq($vm.cModule(assemble(recursion(4096, "alwaysInline", false))).recurse(200), expectedRecursion(200), "recursion around an always_inline helper with 4096 bytes of locals");
eq($vm.cModule(assemble(recursion(16384, "noinline", false))).recurse(5000), expectedRecursion(5000), "recursion around a noinline helper");

// An always_inline nest that doubles at every level: what one function absorbs is bounded, the rest is called.
//   static inline int f0(int x) { return x; }    static inline int fK(int x) { return fK-1(x) + fK-1(x + 1); }
for (const depth of [4, 12, 16, 20]) {
    const funcs = [{ name: "f0", sig: 0, alwaysInline: true, blocks: [[["Ret", 0]]] }];
    for (let k = 1; k <= depth; k++)
        funcs.push({ name: "f" + k, sig: 0, alwaysInline: true, exported: k === depth, blocks: [[["Call", k - 1, 1, 0], ["ConstI32", s(1)], ["Add", 0, 2], ["Call", k - 1, 1, 3], ["Add", 1, 4], ["Ret", 5]]] });
    const before = preciseTime();
    const m = $vm.cModule(assemble({ sigs: [intToInt], funcs, exports: [exportOf("top", depth)] }));
    const seconds = preciseTime() - before;
    // fK(x) = 2^K * x + K * 2^(K-1)
    eq(m.top(3), (2 ** depth * 3 + depth * 2 ** (depth - 1)) | 0, `an always_inline nest ${depth} deep`);
    if (seconds > 20)
        throw new Error(`an always_inline nest ${depth} deep took ${seconds.toFixed(1)} s to compile`);
}
print("inliner limits ok");
