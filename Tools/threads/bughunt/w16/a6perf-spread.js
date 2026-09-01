// A6 amend round V5b spot-check: flag-off LLInt-only varargs/spread/apply
// microbench. Maximizes the slow path the A6 echo touches
// (slow_path_size_frame_for_varargs + varargsSetup): run with --useJIT=0 and
// NO threads flags. Prints total ms and a checksum (identity check across
// baseline/current binaries).
"use strict";
function callee3(a, b, c) { return (a | 0) + (b | 0) + (c | 0); }
function calleeN() { return arguments.length; }
function spreadSite(arr) { return callee3(...arr); }
function applySite(arr) { return calleeN.apply(null, arr); }
function fwdSite() { return calleeN(...arguments); }
const small = [1, 2, 3];
const mid = [1, 2, 3, 4, 5, 6, 7, 8];
const big = new Array(64).fill(7);
let sum = 0;
const t0 = Date.now();
for (let i = 0; i < 400000; i++) {
    sum += spreadSite(small);
    sum += spreadSite(mid);
    sum += applySite(small);
    sum += applySite(big);
    sum += fwdSite(1, 2, 3, 4, 5);
    if ((i & 0xFFF) === 0)
        sum += spreadSite(big);
}
const ms = Date.now() - t0;
print("checksum=" + sum);
print("ms=" + ms);
