//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1", "--useConcurrentJIT=0")

// A 128-bit value that is live across a call, in a C function the optimizing JIT inlines into JavaScript: all of it
// comes back, the upper lanes too, whichever register it was kept in.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });

// float f(void* to, const void* from, float k) { v4sf v = { k, k, k, k } * (v4sf){ 1, 2, 3, 4 }; ...eight more of them...; memcpy(to, from, 128); return v[0] + v[1] + v[2] + v[3] + ...; }
const block = new Block(3);
const vectors = [];
for (let i = 0; i < 10; i++) {
    const scale = new Float32Array([1 + i, 2 + i, 3 + i, 4 + i]);
    vectors.push(block.def("VMul", b(LANE.f32x4), block.def("VSplat", b(LANE.f32x4), 2), block.def("ConstV128", { bytes: Array.from(new Uint8Array(scale.buffer)) })));
}
block.run("MemCopy", 0, 1, block.def("ConstI64", s(128)));
let sum = block.def("ConstF32", { bytes: [0, 0, 0, 0] });
for (const vector of vectors) {
    for (let lane = 0; lane < 4; lane++)
        sum = block.def("Add", sum, block.def("VExtract", b(LANE.f32x4), b(0), b(lane), vector));
}
block.run("Ret", sum);
const f = $vm.cModule(assemble({
    sigs: [{ ret: T.f32, params: [T.i64, T.i64, T.f32] }],
    funcs: [{ name: "f", sig: 0, exported: true, blocks: [block.insts] }],
    exports: [{ name: "f", func: 0, ret: FFI.f32, args: [FFI.ptr, FFI.ptr, FFI.f32] }],
})).f;

const from = new Uint8Array(128).map((_, i) => i), to = new Uint8Array(128);
function reference(k) { let total = 0; for (let i = 0; i < 10; i++) for (let lane = 0; lane < 4; lane++) total = Math.fround(total + Math.fround(k * (1 + i + lane))); return total; }
const before = $vm.ffiCompileCounts().ftlInlineC;
function run(n) { let total = 0; for (let i = 0; i < n; i++) total += f(to, from, (i & 15) + 0.5); return total; }
let expected = 0;
for (let i = 0; i < 200000; i++) expected += reference((i & 15) + 0.5);
for (let round = 0; round < 4; round++)
    eq(run(200000), expected, `round ${round}`);
eq(to[127], 127, "the copy was made");
if ($vm.useFTLJIT() && $vm.ffiCompileCounts().ftlInlineC <= before)
    throw new Error("the function was not inlined into JavaScript, so this tested nothing");
print("vector across a call inlined into javascript ok");
