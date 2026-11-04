//@ runDefault("--usePollingTraps=1", "--validateDFGClobberize=1", "--jitPolicyScale=0.0001")
// With polling traps every loop header carries a CheckTraps node. The loop
// unrolling phase clones the loop body, so CheckTraps must be cloneable, or
// no loop in a polled build is ever unrolled. Its clones are no-ops: the
// original header keeps the poll. sum4 is fully unrolled and xorSum partially.

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

function xorSumExpected(n) {
    let s = 0;
    for (let i = 0; i < n; i++)
        s = (s + (i ^ (i >> 3))) | 0;
    return s;
}
noDFG(xorSumExpected);

const a = [1, 2, 3, 4];
const expected100 = xorSumExpected(100);
const expected7 = xorSumExpected(7);
for (let i = 0; i < 1e4; i++) {
    if (sum4(a) !== 10)
        throw new Error("bad sum " + sum4(a));
    if (xorSum(100) !== expected100)
        throw new Error("bad xorSum(100) " + xorSum(100));
    if (xorSum(7) !== expected7)
        throw new Error("bad xorSum(7) " + xorSum(7));
}
