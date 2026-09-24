//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// A switch can have any value of its type as a case: the largest and smallest, the two next to them, a dense run
// next to a sparse one.
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });

// int which(T x) { switch (x) { case cases[0]: return 1; case cases[1]: return 2; ... default: return 0; } }
function switchModule(type, cases) {
    const blocks = [[["Switch", 0, 1, cases.length, ...cases.flatMap((value, i) => [s(value), i + 2])]], [["ConstI32", s(0)], ["Ret", 1]]];
    // Values are numbered in block order: block k + 2 defines value k + 2.
    cases.forEach((value, i) => blocks.push([["ConstI32", s(i + 1)], ["Ret", i + 2]]));
    return {
        sigs: [{ ret: T.i32, params: [type] }],
        funcs: [{ name: "which", sig: 0, exported: true, blocks }],
        exports: [{ name: "which", func: 0, ret: FFI.i32, args: [type === T.i64 ? FFI.i64 : FFI.i32] }],
    };
}
const MAX64 = 2n ** 63n - 1n, MIN64 = -(2n ** 63n);
const MAX32 = 2 ** 31 - 1, MIN32 = -(2 ** 31);
const sets = [
    [T.i64, [MAX64, MAX64 - 1n, MIN64, MIN64 + 1n, -1n, 0n], [MAX64 - 2n, MIN64 + 2n, 1n, -2n, 2n ** 32n, -(2n ** 32n), 2n ** 31n]],
    [T.i64, [MAX64], [MAX64 - 1n, MIN64, 0n]],
    [T.i64, [MAX64 - 1n], [MAX64, 0n]],
    // Dense next to sparse, around the 32-bit boundaries.
    [T.i64, [0n, 1n, 2n, 3n, 4n, 5n, 6n, 7n, 2n ** 31n, 2n ** 32n, -(2n ** 31n) - 1n, MAX64], [8n, -1n, 2n ** 31n - 1n, 2n ** 32n + 1n, MAX64 - 1n]],
    [T.i32, [MAX32, MIN32, -1, 0, MAX32 - 1, MIN32 + 1], [1, -2, 65536, MAX32 - 2, MIN32 + 2]],
    [T.i32, [-128, 127, -129, 128, 255, 256], [0, -127, 126, 254]],
    [T.i32, Array.from({ length: 300 }, (_, i) => i * 3 - 450), [1, 2, -449, 449, MAX32, MIN32]],
];
for (const [type, cases, others] of sets) {
    const which = $vm.cModule(assemble(switchModule(type, cases))).which;
    for (let round = 0; round < 200; round++) {
        cases.forEach((value, i) => eq(which(value), i + 1, `case ${value}`));
        others.forEach(value => eq(which(value), 0, `${value}, which is not a case`));
    }
}
print("switch extreme values ok");
