//@ runDefault("--watchdog=300", "--watchdog-exception-ok")
//@ skip if $memoryLimited
// This single operation takes far longer than the watchdog timeout, so the TerminationException
// has to be thrown from inside the sub-quadratic BigInt algorithm, which polls for it every few
// million digit multiplications.

const bits = 1 << 29;
const x = (1n << BigInt(bits)) - 12345n;
const y = (1n << BigInt(bits - 1)) + 777n;
3n ** 600000000n;
throw new Error("not terminated");
