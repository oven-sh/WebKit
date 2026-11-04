//@ skip if !$isSIMDPlatform
//@ requireOptions("--useWasmSIMD=1", "--validateGraph=1", "--useConcurrentJIT=0", "--thresholdForBBQOptimizeAfterWarmUp=10", "--thresholdForBBQOptimizeSoon=10", "--thresholdForOMGOptimizeAfterWarmUp=20", "--thresholdForOMGOptimizeSoon=20")

// (func (export "test") (param $shift i32) (param $low i64) (param $high i64) (result i64)
//   (local $shifted v128)
//   (local.set $shifted (i64x2.shr_s (i64x2.replace_lane 1 (i64x2.splat (local.get $low)) (local.get $high)) (local.get $shift)))
//   (i64.xor (i64x2.extract_lane 0 (local.get $shifted)) (i64x2.extract_lane 1 (local.get $shifted))))
const bytes = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x08, 0x01, 0x60, 0x03, 0x7f, 0x7e, 0x7e, 0x01, 0x7e,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x08, 0x01, 0x04, 0x74, 0x65, 0x73, 0x74, 0x00, 0x00,
    0x0a, 0x21, 0x01, 0x1f, 0x01, 0x01, 0x7b,
    0x20, 0x01, 0xfd, 0x12,
    0x20, 0x02, 0xfd, 0x1e, 0x01,
    0x20, 0x00, 0xfd, 0xcc, 0x01,
    0x21, 0x03,
    0x20, 0x03, 0xfd, 0x1d, 0x00,
    0x20, 0x03, 0xfd, 0x1d, 0x01,
    0x85, 0x0b,
]);

const { test } = new WebAssembly.Instance(new WebAssembly.Module(bytes)).exports;

for (let i = 0; i < 20000; ++i) {
    const shift = i & 63;
    const low = BigInt.asIntN(64, BigInt(i) * 0x9e3779b97f4a7c15n);
    const high = BigInt.asIntN(64, ~low + BigInt(i));
    const expected = BigInt.asIntN(64, (low >> BigInt(shift)) ^ (high >> BigInt(shift)));
    const actual = test(shift, low, high);
    if (actual !== expected)
        throw new Error(`test(${shift}, ${low}, ${high}) is ${actual}, expected ${expected}`);
}
