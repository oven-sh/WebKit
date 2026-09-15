//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// The lowering knows a few functions of the C library by name (memcpy, memmove and memset with a constant size; fabs,
// floor, ceil, trunc and sqrt, and those with an f at the end) and emits them itself. A name is only a name: a
// function that has one of them and is not declared the way the library declares it is called like any other, and so
// is a function of the module's own.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });

const constantOf = type => type === T.i32 ? ["ConstI32", s(7)] : type === T.i64 ? ["ConstI64", s(8)] : type === T.f32 ? ["ConstF32", { bytes: [0, 0, 192, 63] }] : ["ConstF64", { f64: 1.5 }];
const typeName = type => Object.keys(T).find(name => T[name] === type);

// void run(int really) { if (really) name(<a constant of each parameter's type>, and `more` more); }
// Compiling it is the test: it is only ever called with 0, because the library's function called with these is
// not something to do.
function compiles(name, sig, more = []) {
    const what = `${name} declared (${sig.params.map(typeName)}${sig.variadic ? ", ..." : ""}) -> ${typeName(sig.ret)}`;
    const types = [...sig.params, ...more];
    const call = types.map(constantOf);
    call.push(["CallExtern", 0, types.length, ...types.map((_, i) => 1 + i)]);
    call.push(["RetVoid"]);
    let exported;
    try {
        exported = $vm.cModule(assemble({
            sigs: [sig, { ret: T.void, params: [T.i32] }],
            externs: [{ name, sig: 0 }],
            funcs: [{ name: "run", sig: 1, exported: true, blocks: [[["Br", 0, 1, 2]], call, [["RetVoid"]]] }],
            exports: [{ name: "run", func: 0, ret: FFI.void, args: [FFI.i32] }],
        }));
    } catch (e) {
        throw new Error(`${what}: ${e}`);
    }
    exported.run(0);
}
for (const name of ["memcpy", "memmove", "memset"]) {
    for (const params of [
        [T.f64, T.i64, T.i64], [T.f32, T.i64, T.i64], [T.i32, T.i64, T.i64], [T.i64, T.i32, T.i64], [T.i64, T.i64, T.i64], [T.i64, T.f64, T.i64],
        [T.i64, T.i64, T.i32], [T.i64, T.i32, T.i32], [T.i64, T.i64, T.f64], [T.i32, T.i32, T.i64], [T.i64, T.i64], [T.i64], [], [T.i64, T.i64, T.i64, T.i64]]) {
        for (const ret of [T.i64, T.i32, T.f64, T.void])
            compiles(name, { ret, params });
    }
    compiles(name, { ret: T.i64, params: [T.i64], variadic: true }, [T.i64, T.i64]);
    compiles(name, { ret: T.i64, params: [T.i64, T.i64, T.i64], variadic: true });
    compiles(name, { ret: T.i64, params: [T.f64], variadic: true }, [T.i64, T.i64]);
}
for (const name of ["fabs", "floor", "ceil", "trunc", "sqrt", "fabsf", "floorf", "ceilf", "truncf", "sqrtf"]) {
    for (const params of [[T.i64], [T.i32], [T.f32], [T.f64], [T.f64, T.f64], []]) {
        for (const ret of [T.f64, T.f32, T.i64, T.i32, T.void])
            compiles(name, { ret, params });
    }
    compiles(name, { ret: T.f64, params: [T.f64], variadic: true });
    compiles(name, { ret: T.f64, params: [], variadic: true }, [T.f64]);
}

// Declared the library's way, they do what the library's do, whether the size is small enough to be done in place
// or not, and give back what the library's give back.
{
    const threePointers = { ret: T.i64, params: [T.i64, T.i64, T.i64] };
    const sizes = [0, 1, 8, 13, 64, 65, 4096];
    const funcs = [{ name: "address", sig: 2, exported: true, blocks: [[["Ret", 0]]] }];
    const exports = [{ name: "address", func: 0, ret: FFI.ptr, args: [FFI.ptr] }];
    for (const [extern, name] of [[0, "memcpy"], [1, "memmove"]]) {
        for (const size of sizes) {
            funcs.push({ name: name + size, sig: 3, exported: true, blocks: [[["ConstI64", s(size)], ["CallExtern", extern, 3, 0, 1, 2], ["Ret", 3]]] });
            exports.push({ name: name + size, func: funcs.length - 1, ret: FFI.ptr, args: [FFI.ptr, FFI.ptr] });
        }
    }
    for (const size of sizes) {
        funcs.push({ name: "memset" + size, sig: 4, exported: true, blocks: [[["ConstI64", s(size)], ["CallExtern", 2, 3, 0, 1, 2], ["Ret", 3]]] });
        exports.push({ name: "memset" + size, func: funcs.length - 1, ret: FFI.ptr, args: [FFI.ptr, FFI.i32] });
    }
    const c = $vm.cModule(assemble({
        sigs: [threePointers, { ret: T.i64, params: [T.i64, T.i32, T.i64] }, { ret: T.i64, params: [T.i64] }, { ret: T.i64, params: [T.i64, T.i64] }, { ret: T.i64, params: [T.i64, T.i32] }],
        externs: [{ name: "memcpy", sig: 0 }, { name: "memmove", sig: 0 }, { name: "memset", sig: 1 }],
        funcs, exports,
    }));
    for (const size of sizes) {
        for (const name of ["memcpy", "memmove"]) {
            const from = new Uint8Array(size + 16).map((_, i) => (i * 7 + 3) & 255), to = new Uint8Array(size + 16).fill(0xee);
            eq(c[name + size](to.subarray(8), from.subarray(3)), c.address(to.subarray(8)), `${name} of ${size}: what it gives back`);
            for (let i = 0; i < to.length; i++)
                eq(to[i], i >= 8 && i < 8 + size ? from[i - 8 + 3] : 0xee, `${name} of ${size}: byte ${i}`);
        }
        const filled = new Uint8Array(size + 16).fill(0x11);
        eq(c["memset" + size](filled.subarray(5), 0x1a7), c.address(filled.subarray(5)), `memset of ${size}: what it gives back`);
        for (let i = 0; i < filled.length; i++)
            eq(filled[i], i >= 5 && i < 5 + size ? 0xa7 : 0x11, `memset of ${size}: byte ${i}`);
    }
}
{
    const c = $vm.cModule(assemble({
        sigs: [{ ret: T.f64, params: [T.f64] }, { ret: T.f32, params: [T.f32] }],
        externs: ["fabs", "floor", "ceil", "trunc", "sqrt"].flatMap(name => [{ name, sig: 0 }, { name: name + "f", sig: 1 }]),
        funcs: ["fabs", "floor", "ceil", "trunc", "sqrt"].flatMap((name, i) => [
            { name, sig: 0, exported: true, blocks: [[["CallExtern", 2 * i, 1, 0], ["Ret", 1]]] },
            { name: name + "f", sig: 1, exported: true, blocks: [[["CallExtern", 2 * i + 1, 1, 0], ["Ret", 1]]] }]),
        exports: ["fabs", "floor", "ceil", "trunc", "sqrt"].flatMap((name, i) => [
            { name, func: 2 * i, ret: FFI.f64, args: [FFI.f64] }, { name: name + "f", func: 2 * i + 1, ret: FFI.f32, args: [FFI.f32] }]),
    }));
    const references = { fabs: Math.abs, floor: Math.floor, ceil: Math.ceil, trunc: Math.trunc, sqrt: Math.sqrt };
    for (const [name, reference] of Object.entries(references)) {
        for (const x of [0, -0, 2.5, -2.5, 1e300, -1e300, 0.1, 16, Infinity, -Infinity, NaN, -1]) {
            if (!Object.is(c[name](x), reference(x)))
                throw new Error(`${name}(${x}): expected ${reference(x)}, got ${c[name](x)}`);
            if (!Object.is(c[name + "f"](x), Math.fround(reference(Math.fround(x)))))
                throw new Error(`${name}f(${x}): expected ${Math.fround(reference(Math.fround(x)))}, got ${c[name + "f"](x)}`);
        }
    }
}

// A function of the module's own with one of the names is that function.
{
    const names = ["memcpy", "memmove", "memset", "sqrt", "fabs", "floorf"];
    const c = $vm.cModule(assemble({
        sigs: [{ ret: T.i64, params: [T.i64, T.i64, T.i64] }],
        funcs: [
            ...names.map((name, i) => ({ name, sig: 0, noinline: true, blocks: [[["ConstI64", s(100 + i)], ["Add", 0, 3], ["Ret", 4]]] })),
            ...names.map((name, i) => ({ name: "calls_" + name, sig: 0, exported: true, blocks: [[["Call", i, 3, 0, 1, 2], ["Ret", 3]]] })),
        ],
        exports: names.map((name, i) => ({ name: "calls_" + name, func: names.length + i, ret: FFI.i64, args: [FFI.i64, FFI.i64, FFI.i64] })),
    }));
    names.forEach((name, i) => eq(c["calls_" + name](1000, 2000, 8), BigInt(1100 + i), `the module's own ${name}`));
}
print("library functions by name ok");
