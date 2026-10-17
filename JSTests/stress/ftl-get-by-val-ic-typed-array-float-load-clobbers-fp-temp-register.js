//@ runDefault("--useConcurrentJIT=0")
// The IndexedTypedArrayFloat{16,32,64}Load inline cache stubs call AssemblyHelpers::purifyNaN(),
// which materializes PNaN in MacroAssembler::fpTempRegister (xmm15 on x86-64, q31 on ARM64).
// FTL lets Air allocate that register, so a double that is live across a generic GetByVal
// patchpoint could be silently replaced with NaN once the IC attached a typed-array load case.

function f(arr, idx, d) {
    // Enough doubles live across the access that Air has to use every FP register, including fpTempRegister.
    const a0 = d + 0.5, a1 = d + 1.5, a2 = d + 2.5, a3 = d + 3.5, a4 = d + 4.5, a5 = d + 5.5, a6 = d + 6.5, a7 = d + 7.5;
    const b0 = d * 0.5, b1 = d * 1.5, b2 = d * 2.5, b3 = d * 3.5, b4 = d * 4.5, b5 = d * 5.5, b6 = d * 6.5, b7 = d * 7.5;
    const x = arr[idx];
    return x + a0 * b0 + a1 * b1 + a2 * b2 + a3 * b3 + a4 * b4 + a5 * b5 + a6 * b6 + a7 * b7 + (a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7) - (b0 + b1 + b2 + b3 + b4 + b5 + b6 + b7);
}
noInline(f);

function expected(idx, d) {
    const a0 = d + 0.5, a1 = d + 1.5, a2 = d + 2.5, a3 = d + 3.5, a4 = d + 4.5, a5 = d + 5.5, a6 = d + 6.5, a7 = d + 7.5;
    const b0 = d * 0.5, b1 = d * 1.5, b2 = d * 2.5, b3 = d * 3.5, b4 = d * 4.5, b5 = d * 5.5, b6 = d * 6.5, b7 = d * 7.5;
    return (idx + 0.25) + a0 * b0 + a1 * b1 + a2 * b2 + a3 * b3 + a4 * b4 + a5 * b5 + a6 * b6 + a7 * b7 + (a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7) - (b0 + b1 + b2 + b3 + b4 + b5 + b6 + b7);
}
noInline(expected);
noFTL(expected);

for (const TypedArray of [Float64Array, Float32Array, Float16Array]) {
    const ta = new TypedArray(8);
    for (let i = 0; i < 8; i++)
        ta[i] = i + 0.25;
    const other = { 0: 0.25, 1: 1.25, 2: 2.25, 3: 3.25, 4: 4.25, 5: 5.25, 6: 6.25, 7: 7.25 };
    for (let i = 0; i < 200000; i++) {
        const arr = (i & 1023) === 0 ? other : ta; // keep the GetByVal generic so it goes through an IC
        const idx = i & 7;
        const d = (i % 97) * 0.25;
        const r = f(arr, idx, d);
        const e = expected(idx, d);
        if (r !== e)
            throw new Error(TypedArray.name + ": i=" + i + " got " + r + " expected " + e);
    }
}
