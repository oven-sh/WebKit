//@ runDefault("--watchdog=300", "--watchdog-exception-ok")
//@ skip if $memoryLimited
// The divisor has 55 64-bit digits, which is below the Burnikel-Ziegler threshold, so each
// remainder is one schoolbook division with a remainder buffer of exactly 55 digits. The
// TerminationException is thrown from the check at the top of a row, while the running remainder
// still has all the digits of the rows that are not reduced yet. The operands are small enough
// for a debug build to create them before the watchdog fires, and the loop keeps the VM in a
// division at that time whatever the speed of the build.

const x = (1n << BigInt(1 << 27)) - 12345678901234567891n;
const y = (1n << 3500n) - 987654321987654321n;
for (;;)
    x % y;
