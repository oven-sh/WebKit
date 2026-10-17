//@ runDefault("--watchdog=300", "--watchdog-exception-ok")
//@ skip if $memoryLimited
// This single operation takes far longer than the watchdog timeout, so the TerminationException
// has to be thrown from inside the sub-quadratic BigInt algorithm, which polls for it every few
// million digit multiplications. The operands are dense so that no part of the division is
// trivial.

const bits = 1 << 29;
let mix = 0x9e3779b97f4a7c15n;
const parts = [];
for (let i = 0; i < bits / 64 / 4096; i++) {
    mix = BigInt.asUintN(64, mix * 6364136223846793005n + 1442695040888963407n);
    parts.push(mix.toString(16).padStart(16, "0"));
}
const block = parts.join("");
const y = BigInt("0x" + block.repeat(2048)) | (1n << BigInt(bits - 1));
const x = (y << BigInt(bits)) | BigInt("0x" + block.repeat(2048));
x % y;
throw new Error("not terminated");
