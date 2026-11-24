//@ runDefault("--watchdog=300", "--watchdog-exception-ok")
//@ skip if $memoryLimited
// The divisor has 112 64-bit digits, so each division is a Burnikel-Ziegler division. Its base
// case is a schoolbook division of at most 112 digits by 56 with a remainder buffer of exactly 56
// digits. The low half of the divisor is zero, so the multiplication in each step counts no work:
// every termination check is at the top of a row of the base case. The TerminationException is
// thrown there, while the running remainder still has all the digits of the rows that are not
// reduced yet. The operands are small enough for a debug build to create them before the watchdog
// fires, and the loop keeps the VM in a division at that time whatever the speed of the build.

const x = (1n << BigInt(1 << 27)) - 12345678901234567891n;
const half = 56n * 64n;
const y = ((1n << half) - 987654321987654321n) << half;
for (;;)
    x / y;
