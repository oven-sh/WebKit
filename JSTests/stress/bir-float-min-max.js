//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1", "--validateGraphAtEachPhase=1", "--useConcurrentJIT=0")

// The two rules for a floating-point minimum and maximum (ffi/BIR.h), each for f32, f64, f32x4 and f64x2:
//   IEEE 754-2019 minimum/maximum (AArch64's FMIN/FMAX):  FMin FMax VFMin VFMax.
//       A quiet NaN when either operand is a NaN; -0 is less than +0.
//   x86's MINSS/MINPS(a, b) = a < b ? a : b and MAXSS/MAXPS(a, b) = a > b ? a : b:  Select(Lt(a, b), a, b),
//       Select(Gt(a, b), a, b), VMin(b, a), VMax(b, a).  b, bit for bit, when either is a NaN and when both are zeros.
// Operands and results go through memory as bit patterns, so a NaN's sign and payload and a zero's sign are seen.
load("./resources/bir-assembler.js", "caller relative");
const s = n => ({ s: n }), t8 = n => ({ u8: n });

// name: [f32 bits, f64 bits]
const values = {
    "+0": [0x00000000, 0x0000000000000000n], "-0": [0x80000000, 0x8000000000000000n],
    "1": [0x3f800000, 0x3ff0000000000000n], "2": [0x40000000, 0x4000000000000000n],
    "-1": [0xbf800000, 0xbff0000000000000n], "-2": [0xc0000000, 0xc000000000000000n],
    "1.5": [0x3fc00000, 0x3ff8000000000000n], "1.5-": [0x3fbfffff, 0x3ff7ffffffffffffn], // the value just below 1.5
    "+inf": [0x7f800000, 0x7ff0000000000000n], "-inf": [0xff800000, 0xfff0000000000000n],
    "+max": [0x7f7fffff, 0x7fefffffffffffffn], "-max": [0xff7fffff, 0xffefffffffffffffn],
    "+tiny": [0x00000001, 0x0000000000000001n], "-tiny": [0x80000001, 0x8000000000000001n], // the smallest denormals
    "+denormal": [0x007fffff, 0x000fffffffffffffn], // the largest one
    "+normal": [0x00800000, 0x0010000000000000n],   // the smallest normal number
    "qnan": [0x7fc00000, 0x7ff8000000000000n], "-qnan": [0xffc00000, 0xfff8000000000000n],
    "qnan+payload": [0x7fc12345, 0x7ff8000012345678n], "-qnan+payload": [0xffeabcde, 0xfffabcdef0123456n],
    "snan": [0x7f800001, 0x7ff0000000000001n], "-snan+payload": [0xffa00055, 0xfff4000000000055n],
};
// [a, b, IEEE minimum, IEEE maximum, x86 min(a, b), x86 max(a, b)]; "nan" is any quiet NaN.
const table = [
    // Ordinary numbers, both orders.
    ["1", "2", "1", "2", "1", "2"],
    ["2", "1", "1", "2", "1", "2"],
    ["-1", "-2", "-2", "-1", "-2", "-1"],
    ["-2", "-1", "-2", "-1", "-2", "-1"],
    ["-1", "1", "-1", "1", "-1", "1"],
    ["1", "1", "1", "1", "1", "1"],
    ["1.5", "1.5-", "1.5-", "1.5", "1.5-", "1.5"],
    ["1.5-", "1.5", "1.5-", "1.5", "1.5-", "1.5"],
    // Zeros: the IEEE rule orders them, x86's gives the second.
    ["+0", "-0", "-0", "+0", "-0", "-0"],
    ["-0", "+0", "-0", "+0", "+0", "+0"],
    ["+0", "+0", "+0", "+0", "+0", "+0"],
    ["-0", "-0", "-0", "-0", "-0", "-0"],
    ["-0", "1", "-0", "1", "-0", "1"],
    ["-1", "-0", "-1", "-0", "-1", "-0"],
    // Infinities and the largest finite numbers.
    ["+inf", "1", "1", "+inf", "1", "+inf"],
    ["1", "+inf", "1", "+inf", "1", "+inf"],
    ["-inf", "1", "-inf", "1", "-inf", "1"],
    ["1", "-inf", "-inf", "1", "-inf", "1"],
    ["+inf", "-inf", "-inf", "+inf", "-inf", "+inf"],
    ["-inf", "+inf", "-inf", "+inf", "-inf", "+inf"],
    ["+inf", "+inf", "+inf", "+inf", "+inf", "+inf"],
    ["-inf", "-inf", "-inf", "-inf", "-inf", "-inf"],
    ["+max", "+inf", "+max", "+inf", "+max", "+inf"],
    ["-inf", "-max", "-inf", "-max", "-inf", "-max"],
    ["+max", "-max", "-max", "+max", "-max", "+max"],
    // Denormals are numbers.
    ["+tiny", "+0", "+0", "+tiny", "+0", "+tiny"],
    ["+0", "+tiny", "+0", "+tiny", "+0", "+tiny"],
    ["-tiny", "-0", "-tiny", "-0", "-tiny", "-0"],
    ["-0", "-tiny", "-tiny", "-0", "-tiny", "-0"],
    ["+tiny", "-tiny", "-tiny", "+tiny", "-tiny", "+tiny"],
    ["+denormal", "+normal", "+denormal", "+normal", "+denormal", "+normal"],
    ["+normal", "+denormal", "+denormal", "+normal", "+denormal", "+normal"],
    ["+tiny", "+denormal", "+tiny", "+denormal", "+tiny", "+denormal"],
    // A NaN in either place or both: the IEEE rule gives a quiet NaN, x86's gives b whatever it is.
    ["qnan", "1", "nan", "nan", "1", "1"],
    ["1", "qnan", "nan", "nan", "qnan", "qnan"],
    ["-qnan", "1", "nan", "nan", "1", "1"],
    ["1", "-qnan", "nan", "nan", "-qnan", "-qnan"],
    ["qnan+payload", "2", "nan", "nan", "2", "2"],
    ["2", "qnan+payload", "nan", "nan", "qnan+payload", "qnan+payload"],
    ["2", "-qnan+payload", "nan", "nan", "-qnan+payload", "-qnan+payload"],
    ["snan", "1", "nan", "nan", "1", "1"],
    ["1", "snan", "nan", "nan", "snan", "snan"],
    ["-1", "-snan+payload", "nan", "nan", "-snan+payload", "-snan+payload"],
    ["qnan", "qnan", "nan", "nan", "qnan", "qnan"],
    ["qnan", "qnan+payload", "nan", "nan", "qnan+payload", "qnan+payload"],
    ["qnan+payload", "qnan", "nan", "nan", "qnan", "qnan"],
    ["-qnan+payload", "snan", "nan", "nan", "snan", "snan"],
    ["snan", "-qnan", "nan", "nan", "-qnan", "-qnan"],
    ["qnan", "+inf", "nan", "nan", "+inf", "+inf"],
    ["-inf", "qnan", "nan", "nan", "qnan", "qnan"],
    ["qnan", "-0", "nan", "nan", "-0", "-0"],
    ["+0", "qnan", "nan", "nan", "qnan", "qnan"],
    ["-qnan", "+tiny", "nan", "nan", "+tiny", "+tiny"],
];

// void f(T *out, const T *a, const T *b) for each rule, operation and shape.
const IEEE_MIN = 2, IEEE_MAX = 3, X86_MIN = 4, X86_MAX = 5;
const shapes = [
    { name: "f32", kind: MEM.f32, lanes: 1, width: 0 },
    { name: "f64", kind: MEM.f64, lanes: 1, width: 1 },
    { name: "f32x4", kind: MEM.v128, lanes: 4, width: 0, lane: LANE.f32x4 },
    { name: "f64x2", kind: MEM.v128, lanes: 2, width: 1, lane: LANE.f64x2 },
];
function operation(shape, column, a, b) {
    if (shape.lanes === 1) {
        switch (column) {
        case IEEE_MIN: return [["FMin", a, b]];
        case IEEE_MAX: return [["FMax", a, b]];
        case X86_MIN: return [["Lt", a, b], ["Select", b + 1, a, b]];
        case X86_MAX: return [["Gt", a, b], ["Select", b + 1, a, b]];
        }
    }
    switch (column) {
    case IEEE_MIN: return [["VFMin", t8(shape.lane), a, b]];
    case IEEE_MAX: return [["VFMax", t8(shape.lane), a, b]];
    case X86_MIN: return [["VMin", t8(shape.lane), t8(0), b, a]];
    case X86_MAX: return [["VMax", t8(shape.lane), t8(0), b, a]];
    }
}
const funcs = [], exports = [];
for (const shape of shapes) {
    for (const column of [IEEE_MIN, IEEE_MAX, X86_MIN, X86_MAX]) {
        // Values 0..2 are out, a and b; 3 and 4 what a and b point to.
        const insts = operation(shape, column, 3, 4);
        const name = `${shape.name}_${column}`;
        funcs.push({ name, sig: 0, exported: true, blocks: [[
            ["Load", t8(shape.kind), 1, s(0)], ["Load", t8(shape.kind), 2, s(0)], ...insts, ["Store", t8(shape.kind), 4 + insts.length, 0, s(0)], ["RetVoid"],
        ]] });
        exports.push({ name, func: funcs.length - 1, ret: FFI.void, args: [FFI.ptr, FFI.ptr, FFI.ptr] });
    }
}
// float (double) over values rather than memory, small enough to be inlined into a JavaScript caller.
for (const [name, sig, op] of [["fmin32", 1, "FMin"], ["fmax32", 1, "FMax"], ["fmin64", 2, "FMin"], ["fmax64", 2, "FMax"]]) {
    funcs.push({ name, sig, exported: true, blocks: [[[op, 0, 1], ["Ret", 2]]] });
    exports.push({ name, func: funcs.length - 1, ret: sig === 1 ? FFI.f32 : FFI.f64, args: sig === 1 ? [FFI.f32, FFI.f32] : [FFI.f64, FFI.f64] });
}
const m = $vm.cModule(assemble({
    sigs: [{ ret: T.void, params: [T.i64, T.i64, T.i64] }, { ret: T.f32, params: [T.f32, T.f32] }, { ret: T.f64, params: [T.f64, T.f64] }],
    funcs, exports,
}));

const hex = (bits, width) => "0x" + BigInt(bits).toString(16).padStart(width ? 16 : 8, "0");
function isQuietNaN(bits, width) {
    bits = BigInt(bits);
    return width ? (bits & 0x7ff8000000000000n) === 0x7ff8000000000000n : (bits & 0x7fc00000n) === 0x7fc00000n;
}
let checks = 0;
function check(what, width, actual, expectedName) {
    checks++;
    if (expectedName === "nan") {
        if (!isQuietNaN(actual, width))
            throw new Error(`${what}: expected a quiet NaN, got ${hex(actual, width)}`);
        return;
    }
    const expected = values[expectedName][width];
    if (BigInt(actual) !== BigInt(expected))
        throw new Error(`${what}: expected ${expectedName} (${hex(expected, width)}), got ${hex(actual, width)}`);
}
const ruleName = { [IEEE_MIN]: "IEEE minimum", [IEEE_MAX]: "IEEE maximum", [X86_MIN]: "x86 min", [X86_MAX]: "x86 max" };

for (const shape of shapes) {
    const make = () => shape.width ? new BigUint64Array(shape.lanes) : new Uint32Array(shape.lanes);
    const a = make(), b = make(), out = make();
    for (const column of [IEEE_MIN, IEEE_MAX, X86_MIN, X86_MAX]) {
        const f = m[`${shape.name}_${column}`];
        // `lanes` rows at a time, the last group wrapping round to the first rows.
        for (let first = 0; first < table.length; first += shape.lanes) {
            const rows = Array.from({ length: shape.lanes }, (_, i) => table[(first + i) % table.length]);
            rows.forEach((row, i) => { a[i] = values[row[0]][shape.width]; b[i] = values[row[1]][shape.width]; });
            out.fill(shape.width ? 0x5555555555555555n : 0x55555555);
            f(out, a, b);
            rows.forEach((row, i) => check(`${shape.name} ${ruleName[column]}(${row[0]}, ${row[1]})`, shape.width, out[i], row[column]));
        }
        // Every lane position sees every row.
        if (shape.lanes > 1) {
            for (let rotation = 1; rotation < shape.lanes; rotation++) {
                for (let first = 0; first < table.length; first += shape.lanes) {
                    const rows = Array.from({ length: shape.lanes }, (_, i) => table[(first + i + rotation * 7) % table.length]);
                    rows.forEach((row, i) => { a[i] = values[row[0]][shape.width]; b[i] = values[row[1]][shape.width]; });
                    f(out, a, b);
                    rows.forEach((row, i) => check(`${shape.name} ${ruleName[column]}(${row[0]}, ${row[1]}) in lane ${i}`, shape.width, out[i], row[column]));
                }
            }
        }
    }
}

// Constant operands: whoever folds the operation follows the same rule.
{
    const constFuncs = [], constExports = [];
    const rows = table.filter(row => ["+0", "-0", "qnan", "1", "-inf", "snan"].includes(row[0]));
    for (const width of [0, 1]) {
        rows.forEach((row, index) => {
            for (const [column, op] of [[IEEE_MIN, "FMin"], [IEEE_MAX, "FMax"]]) {
                const constant = name => width
                    ? ["ConstF64", { bytes: Array.from(new Uint8Array(new BigUint64Array([values[name][1]]).buffer)) }]
                    : ["ConstF32", { bytes: Array.from(new Uint8Array(new Uint32Array([values[name][0]]).buffer)) }];
                constFuncs.push({ name: `c${width}_${index}_${column}`, sig: 0, exported: true, blocks: [[
                    constant(row[0]), constant(row[1]), [op, 1, 2], ["Store", t8(width ? MEM.f64 : MEM.f32), 3, 0, s(0)], ["RetVoid"],
                ]] });
                constExports.push({ name: `c${width}_${index}_${column}`, func: constFuncs.length - 1, ret: FFI.void, args: [FFI.ptr] });
            }
        });
    }
    const folded = $vm.cModule(assemble({ sigs: [{ ret: T.void, params: [T.i64] }], funcs: constFuncs, exports: constExports }));
    for (const width of [0, 1]) {
        const out = width ? new BigUint64Array(1) : new Uint32Array(1);
        rows.forEach((row, index) => {
            for (const column of [IEEE_MIN, IEEE_MAX]) {
                folded[`c${width}_${index}_${column}`](out);
                check(`constant ${width ? "f64" : "f32"} ${ruleName[column]}(${row[0]}, ${row[1]})`, width, out[0], row[column]);
            }
        });
    }
}

// Called from JavaScript until the optimizing JIT has compiled the caller, the C body inlined into it: the compiler
// threads are off, so by the end of each loop it has.
{
    const numbers = { "+0": 0, "-0": -0, "1": 1, "2": 2, "-1": -1, "-2": -2, "+inf": Infinity, "-inf": -Infinity, "qnan": NaN, "1.5": 1.5 };
    const rows = table.filter(row => row[0] in numbers && row[1] in numbers);
    // Typed arrays, so that every operand is loaded as a double whatever its value.
    const left = Float64Array.from(rows, row => numbers[row[0]]), right = Float64Array.from(rows, row => numbers[row[1]]);
    const before = $vm.ffiCompileCounts().ftlInlineC;
    for (const [name, column] of [["fmin32", IEEE_MIN], ["fmax32", IEEE_MAX], ["fmin64", IEEE_MIN], ["fmax64", IEEE_MAX]]) {
        const expected = Float64Array.from(rows, row => row[column] === "nan" ? NaN : numbers[row[column]]);
        // A loop of its own for each function (the name makes each one's source its own), so each call site
        // only ever sees one callee.
        const loop = new Function("f", "left", "right", "expected", `
            // ${name}
            for (let i = 0; i < 1000000; i++) {
                const row = i % left.length;
                if (!Object.is(f(left[row], right[row]), expected[row]))
                    return i;
            }
            return -1;`);
        const failed = loop(m[name], left, right, expected);
        checks += 1000000;
        if (failed >= 0) {
            const row = rows[failed % rows.length];
            throw new Error(`${name}(${row[0]}, ${row[1]}) from JavaScript, call ${failed}: expected ${row[column]}, got ${m[name](numbers[row[0]], numbers[row[1]])}`);
        }
    }
    if ($vm.useFTLJIT() && $vm.ffiCompileCounts().ftlInlineC < before + 4)
        throw new Error("the four functions were not each inlined into JavaScript by the FTL: " + JSON.stringify($vm.ffiCompileCounts()));
}

// What is refused.
{
    const refused = (what, sig, inst, pattern) => {
        let error = null;
        try { $vm.cModule(assemble({ sigs: [sig], funcs: [{ name: "f", sig: 0, exported: true, blocks: [[inst, ["RetVoid"]]] }] })); } catch (e) { error = e; }
        if (!(error instanceof TypeError) || !pattern.test(error.message))
            throw new Error(`${what}: ${error ? error : "was accepted"}`);
    };
    refused("FMin of integers", { ret: T.void, params: [T.i32, T.i32] }, ["FMin", 0, 1], /same floating-point type/);
    refused("FMax of 64-bit integers", { ret: T.void, params: [T.i64, T.i64] }, ["FMax", 0, 1], /same floating-point type/);
    refused("FMin of a float and a double", { ret: T.void, params: [T.f32, T.f64] }, ["FMin", 0, 1], /same floating-point type/);
    refused("FMax of vectors", { ret: T.void, params: [T.v128, T.v128] }, ["FMax", 0, 1], /same floating-point type/);
    for (const lane of [LANE.i8x16, LANE.i16x8, LANE.i32x4, LANE.i64x2]) {
        refused(`VFMin of integer lanes ${lane}`, { ret: T.void, params: [T.v128, T.v128] }, ["VFMin", t8(lane), 0, 1], /not defined for this lane shape/);
        refused(`VFMax of integer lanes ${lane}`, { ret: T.void, params: [T.v128, T.v128] }, ["VFMax", t8(lane), 0, 1], /not defined for this lane shape/);
    }
    refused("VFMin of lane 6", { ret: T.void, params: [T.v128, T.v128] }, ["VFMin", t8(6), 0, 1], /bad lane/);
    refused("VFMin of doubles", { ret: T.void, params: [T.f64, T.f64] }, ["VFMin", t8(LANE.f64x2), 0, 1], /operand has the wrong type/);
}

print("float min max ok:", checks, "checks");
