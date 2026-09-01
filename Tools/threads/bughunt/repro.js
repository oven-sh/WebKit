// Minimal targeted repro for the W>=16 scalebench crash family (SCALEBENCH.md §11.3).
// (Previous hunt's repro archived in prev-stw-watchdog-20260610/repro.js.)
//
// Harness: N threads concurrently execute the ONE construct every scalebench
// core dies in — a spread varargs call `String.fromCharCode(...codes)` on a
// fresh thread-local array — and every result is checked for value corruption,
// with per-thread-decodable encodings:
//   - thread t pushes ONLY char code (97+t) ('a'+t), so any foreign letter in
//     a result names the thread whose ARGUMENT VALUES leaked in;
//   - thread t uses ONLY length 2+(t%8), so a wrong-length result names the
//     thread whose vm.varargsLength leaked in.
// Run:
//   WebKitBuild/Release/bin/jsc --useJSThreads=1 --useThreadGIL=0 --useVMLite=1 \
//     --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1 \
//     --useJIT=0 -e "globalThis.REPRO_THREADS=16;" Tools/threads/bughunt/repro.js
// (--useJIT=0 keeps op_call_varargs on the LLInt slow-path pair that round-trips
//  vm.newCallFrameReturnValue/vm.varargsLength through the shared VM block.)
"use strict";
const W = globalThis.REPRO_THREADS || 16;
const ITERS = globalThis.REPRO_ITERS || 2000000;
let mismatches = 0;

function body(id) {
    const len = 2 + (id % 8);
    const myCode = 97 + id; // 'a'+id, unique per thread (W<=26 stays decodable)
    let want = "";
    for (let k = 0; k < len; ++k)
        want += String.fromCharCode(myCode); // non-spread call: not the racy path
    for (let iter = 0; iter < ITERS; ++iter) {
        const codes = []; // fresh thread-local array, like bench.js tokenize()
        for (let k = 0; k < len; ++k)
            codes.push(myCode);
        const got = String.fromCharCode(...codes); // the racy spread/varargs site
        if (got !== want) {
            mismatches++;
            const gotCodes = [];
            for (let k = 0; k < got.length; ++k)
                gotCodes.push(got.charCodeAt(k));
            // Decode: foreign code c => leaked from thread (c-97); wrong length
            // L => leaked varargsLength from a thread with 2+(t%8)==L.
            print("MISMATCH thread=" + id + " iter=" + iter
                + " wantLen=" + len + " gotLen=" + got.length
                + " wantCode=" + myCode + " gotCodes=[" + gotCodes.join(",") + "]"
                + " srcArrayNow=[" + codes.join(",") + "]");
        }
    }
}

const threads = [];
for (let id = 1; id < W; ++id)
    threads.push(new Thread(body, id));
body(0);
for (const t of threads)
    t.join();
print("done W=" + W + " iters=" + ITERS + " mismatches=" + mismatches);
