//@ runDefault("--watchdog=1000", "--watchdog-exception-ok", "--usePollingTraps=1", "--jitPolicyScale=0.0001")
// A termination request stops polled code whose loops were unrolled: the
// clones of CheckTraps compile, run, and the outer loop's poll ends the run.

function sum4(a) {
    let s = 0;
    for (let i = 0; i < 4; i++)
        s += a[i];
    return s;
}
noInline(sum4);

function xorSum(n) {
    let s = 0;
    for (let i = 0; i < n; i++)
        s = (s + (i ^ (i >> 3))) | 0;
    return s;
}
noInline(xorSum);

const a = [1, 2, 3, 4];
for (let i = 0; i < 1e4; i++) {
    sum4(a);
    xorSum(100);
}

let total = 0;
for (;;)
    total += sum4(a) + xorSum(1e6);
