//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1", "--useConcurrentJIT=0")

// What saves registers around JavaScript's slow paths (an allocation that misses the free list, an inline cache
// that misses, a getter, a call) keeps 64 bits of each. A C body with a 128-bit value in it is therefore never
// lowered into JavaScript's code, where that value could be kept in a register across one of them: it is called.
// A body without one still is lowered there, whatever else its module has.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });
const isWindows = $vm.cModuleHost()[1] === 2;

const twoDoubles = 0, threePointers = 1, twoVectors = 2, pointersAndFloat = 3;
const sigs = [
    { ret: T.f64, params: [T.f64, T.f64] },
    { ret: T.void, params: [T.i64, T.i64, T.i64] },
    // Win64 has no 128-bit parameters.
    isWindows ? { ret: T.f64, params: [T.f64, T.f64] } : { ret: T.v128, params: [T.v128, T.v128] },
    { ret: T.f32, params: [T.i64, T.i64, T.f32] },
];

// double f64x2(double k, double j) { v2df v = { k, k }, u = { j, j }; return (v + u)[1]; }
const f64x2Body = [[
    ["VSplat", b(LANE.f64x2), 0], ["VSplat", b(LANE.f64x2), 1], ["VAdd", b(LANE.f64x2), 2, 3],
    ["VExtract", b(LANE.f64x2), b(0), b(1), 4], ["Ret", 5]]];

// double f32x4Local(double k, double j) { v4sf v = splat((float)k); <a branch> return (double)(v + splat((float)j))[3]; }
// The only 128-bit thing that crosses the blocks is a local.
const f32x4LocalBody = [
    [["FDemote", 0], ["VSplat", b(LANE.f32x4), 2], ["LocalSet", 0, 3], ["Jump", 1]],
    [["LocalGet", 0], ["FDemote", 1], ["VSplat", b(LANE.f32x4), 5], ["VAdd", b(LANE.f32x4), 4, 6],
        ["VExtract", b(LANE.f32x4), b(0), b(3), 7], ["FPromote", 8], ["Ret", 9]]];

// double i8x16Lanes(double k, double j) { v16qi v = splat((int)k) + splat((int)j); return v[1] + v[2] + ... + v[15]; }
function i8x16LanesBody() {
    const block = new Block(2);
    const sum = block.def("VAdd", b(LANE.i8x16),
        block.def("VSplat", b(LANE.i8x16), block.def("FToS", b(T.i32), 0)),
        block.def("VSplat", b(LANE.i8x16), block.def("FToS", b(T.i32), 1)));
    let total = block.def("ConstI32", s(0));
    for (let lane = 1; lane < 16; lane++)
        total = block.def("Add", total, block.def("VExtract", b(LANE.i8x16), b(1), b(lane), sum));
    block.run("Ret", block.def("SToF", b(T.f64), total));
    return [block.insts];
}

// void i64x2ThroughPointers(v2di* out, const v2di* a, const v2di* b) { *out = *a + *b; }
const i64x2ThroughPointersBody = [[
    ["Load", b(MEM.v128), 1, s(0)], ["Load", b(MEM.v128), 2, s(0)], ["VAdd", b(LANE.i64x2), 3, 4],
    ["Store", b(MEM.v128), 5, 0, s(0)], ["RetVoid"]]];

// double callsScalarHelper(double k, double j) { return helper(k, j); }   helper is f64x2 above, static.
const callsBody = callee => [[["Call", callee, 2, 0, 1], ["Ret", 2]]];

// double callsVectorHelper(double k, double j) { return vadd(splat(k), splat(j))[0]; }   static v2df vadd(v2df, v2df)
const callsVectorHelperBody = callee => [[
    ["VSplat", b(LANE.f64x2), 0], ["VSplat", b(LANE.f64x2), 1], ["Call", callee, 2, 2, 3],
    ["VExtract", b(LANE.f64x2), b(0), b(0), 4], ["Ret", 5]]];
const vaddBody = [[["VAdd", b(LANE.f64x2), 0, 1], ["Ret", 2]]];

// double scalar(double k, double j) { return k + j; }
const scalarBody = [[["Add", 0, 1], ["Ret", 2]]];

// float acrossACall(void* to, const void* from, float k) { ten vectors made from k; memcpy(to, from, 128); return the sum of their forty lanes; }
function acrossACallBody() {
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
    return [block.insts];
}

const funcs = [
    { name: "f64x2", sig: twoDoubles, exported: true, blocks: f64x2Body },
    { name: "f32x4Local", sig: twoDoubles, exported: true, locals: [T.v128], blocks: f32x4LocalBody },
    { name: "i8x16Lanes", sig: twoDoubles, exported: true, blocks: i8x16LanesBody() },
    { name: "i64x2ThroughPointers", sig: threePointers, exported: true, blocks: i64x2ThroughPointersBody },
    { name: "helper", sig: twoDoubles, blocks: f64x2Body },
    { name: "callsScalarHelper", sig: twoDoubles, exported: true, blocks: callsBody(4) },
    { name: "vadd", sig: twoVectors, blocks: isWindows ? scalarBody : vaddBody },
    { name: "callsVectorHelper", sig: twoDoubles, exported: true, blocks: isWindows ? callsBody(6) : callsVectorHelperBody(6) },
    { name: "scalar", sig: twoDoubles, exported: true, blocks: scalarBody },
    { name: "acrossACall", sig: pointersAndFloat, exported: true, blocks: acrossACallBody() },
];
const twoDoublesExport = (name, func) => ({ name, func, ret: FFI.f64, args: [FFI.f64, FFI.f64] });
const c = $vm.cModule(assemble({
    sigs, funcs,
    exports: [
        twoDoublesExport("f64x2", 0), twoDoublesExport("f32x4Local", 1), twoDoublesExport("i8x16Lanes", 2),
        { name: "i64x2ThroughPointers", func: 3, ret: FFI.void, args: [FFI.ptr, FFI.ptr, FFI.ptr] },
        twoDoublesExport("callsScalarHelper", 5), twoDoublesExport("callsVectorHelper", 7), twoDoublesExport("scalar", 8),
        { name: "acrossACall", func: 9, ret: FFI.f32, args: [FFI.ptr, FFI.ptr, FFI.f32] },
    ],
}));

// Sixteen shapes, so the access to `x` is a polymorphic inline cache; one of them answers with a getter.
const objects = [];
for (let i = 0; i < 16; i++) {
    const object = {};
    for (let j = 0; j < i; j++)
        object["p" + j] = j;
    if (i === 7)
        Object.defineProperty(object, "x", { get() { return 1; } });
    else
        object.x = 1;
    objects.push(object);
}
function other(i) { return i & 1; }
noInline(other);
const state = { sink: null, text: "" };

// What runs between the two calls of the C function. Each has a slow path of its own kind: an allocation that
// misses the free list, an inline cache that misses (and one that calls a getter), a call, a string being made.
const between = {
    allocation: "state.sink = { a: i, b: total };",
    access: "x = objects[i & 15].x;",
    call: "total += other(i) - (i & 1);",
    string: "state.text = 'i' + i;",
    everything: "state.sink = { a: i, b: total }; x = objects[i & 15].x; total += other(i) - (i & 1); state.text = 'i' + i;",
};

// A function of its own for each C function and each of those: the optimizing JIT only lowers a callee into a call
// site that has seen no other.
let hotFunctions = 0;
function makeHot(f, what) {
    return Function("f", "objects", "other", "state", `return function hot${++hotFunctions}(k, n) {
        let total = 0;
        for (let i = 0; i < n; i++) {
            let x = 1;
            total += f(k, 1);
            ${between[what]}
            total += f(k, x + 1);
        }
        return total;
    }`)(f, objects, other, state);
}
function inlinedBy(run) {
    const before = $vm.ffiCompileCounts().ftlInlineC;
    run();
    return $vm.ffiCompileCounts().ftlInlineC - before;
}

const n = 100000, rounds = 4;
function checkTwoDoubles(name, k, valueOf, expectInlined) {
    const expected = n * (valueOf(k, 1) + valueOf(k, 2));
    for (const what of Object.keys(between)) {
        const hot = makeHot(c[name], what);
        noInline(hot);
        const inlined = inlinedBy(() => {
            for (let round = 0; round < rounds; round++)
                eq(hot(k, n), expected, `${name} with ${what} between, round ${round}`);
        });
        if (!$vm.useFTLJIT())
            continue;
        if (expectInlined && !inlined)
            throw new Error(`${name} has no 128-bit value in it and was not lowered into JavaScript`);
        if (!expectInlined && inlined)
            throw new Error(`${name} has a 128-bit value in it and was lowered into JavaScript`);
    }
}

const sum = (k, j) => k + j;
checkTwoDoubles("f64x2", 1000.5, sum, false);
checkTwoDoubles("f32x4Local", 1000.5, sum, false);
checkTwoDoubles("i8x16Lanes", 100, (k, j) => 15 * (k + j), false);
// The function itself is lowered into JavaScript; the helper with the vectors is called from there.
checkTwoDoubles("callsScalarHelper", 1000.5, sum, true);
checkTwoDoubles("callsVectorHelper", 1000.5, sum, isWindows);
checkTwoDoubles("scalar", 1000.5, sum, true);

// Arguments and results through pointers: every lane, after the same slow paths.
{
    const add = c.i64x2ThroughPointers;
    const left = new BigInt64Array(2), right = new BigInt64Array(2), out = new BigInt64Array(2);
    function hotPointers(n) {
        let bad = 0;
        for (let i = 0; i < n; i++) {
            left[0] = 0x1_0000_0001n; left[1] = -0x2_0000_0002n; right[0] = 0x10_0000_0010n; right[1] = 0x20_0000_0020n;
            add(out, left, right);
            state.sink = { a: i, b: bad };
            const x = objects[i & 15].x;
            bad += other(i) - (i & 1);
            state.text = 'i' + i;
            if (out[0] !== 0x11_0000_0011n || out[1] !== 0x1e_0000_001en)
                bad++;
            right[1] = BigInt(x);
            add(out, left, right);
            if (out[0] !== 0x11_0000_0011n || out[1] !== -0x2_0000_0001n)
                bad++;
        }
        return bad;
    }
    noInline(hotPointers);
    const inlined = inlinedBy(() => {
        for (let round = 0; round < rounds; round++)
            eq(hotPointers(n), 0, `i64x2ThroughPointers, round ${round}`);
    });
    if ($vm.useFTLJIT() && inlined)
        throw new Error("i64x2ThroughPointers has a 128-bit value in it and was lowered into JavaScript");
}

// Ten 128-bit values live across a call the C function itself makes.
{
    const f = c.acrossACall;
    const from = new Uint8Array(128).map((_, i) => i), to = new Uint8Array(128);
    function reference(k) { let total = 0; for (let i = 0; i < 10; i++) for (let lane = 0; lane < 4; lane++) total = Math.fround(total + Math.fround(k * (1 + i + lane))); return total; }
    function hotAcrossACall(n) { let total = 0; for (let i = 0; i < n; i++) { total += f(to, from, (i & 15) + 0.5); state.sink = { a: i }; } return total; }
    noInline(hotAcrossACall);
    let expected = 0;
    for (let i = 0; i < n; i++)
        expected += reference((i & 15) + 0.5);
    const inlined = inlinedBy(() => {
        for (let round = 0; round < rounds; round++)
            eq(hotAcrossACall(n), expected, `acrossACall, round ${round}`);
    });
    eq(to[127], 127, "the copy was made");
    if ($vm.useFTLJIT() && inlined)
        throw new Error("acrossACall has a 128-bit value in it and was lowered into JavaScript");
}
print("vector live across javascript slow paths ok");
