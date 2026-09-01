// dw1-sort-comparator-callsite-shapes.js — deepwater LEDGER row 1 (DW-1)
// ROOT CAUSE regression test (deterministic, single-threaded, GIL-on):
//
// The DFG ArraySortIntrinsic's comparator-return trampoline
// (llint_slow_path_array_sort_comparator_return) recovers the caller pc from
// the CallSiteIndex reifyInlinedCallFrames stashed and re-dispatches the
// sort's call instruction. The original contract asserted the recovered pc is
// op_call — but handleArraySort can be hosted at ANY handleCall site:
//   - op_call:               var r = array.sort(comparator)
//   - op_call_ignore_result: array.sort(comparator);   <- the DW-1 signature
//   - op_tail_call:          "use strict"; return array.sort(comparator)
// A result-discarded sort site (the common shape, and exactly what the W32
// bench used: entries.sort(comparator);) recovers to op_call_ignore_result
// (opcode 26), tripping the debug ASSERT / GIL-off RELEASE_ASSERT even though
// the stash matched perfectly (same thread, same CodeBlock, same bits).
//
// Mechanism: per-shape sort function tiers to DFG with the literal comparator
// body-inlined (ArraySortComparatorCall); after warmup the entry values flip
// from int32 to double, so the comparator's `a.k - b.k` int32 speculation
// OSR-exits inside the inlined comparator and returns through
// array_sort_comparator_return_trampoline.
//
// Passes flag-off, GIL-on, and under the pinned GIL-off flags.

function sortIgnoreResult(arr) {
    arr.sort(function comparator(a, b) {
        return a.k - b.k; // op_call_ignore_result host site
    });
}
noInline(sortIgnoreResult);

function sortUseResult(arr) {
    var r = arr.sort(function comparator(a, b) {
        return a.k - b.k; // op_call host site
    });
    return r;
}
noInline(sortUseResult);

function sortTailCall(arr) {
    "use strict";
    return arr.sort(function comparator(a, b) {
        return a.k - b.k; // op_tail_call host site
    });
}
noInline(sortTailCall);

function makeArr(seed, flipped) {
    var a = [];
    var s = seed >>> 0;
    for (var i = 0; i < 12; ++i) {
        s = (s * 1103515245 + 12345) >>> 0;
        var v = (s % 1000) | 0;
        // Post-flip doubles break the comparator's int32 ArithSub
        // speculation -> OSR exit inside the inlined comparator.
        a.push({ k: flipped ? v + 0.5 : v });
    }
    return a;
}

function exercise(sortFn, tag) {
    for (var i = 0; i < 100000; ++i)
        sortFn(makeArr(i, false));

    for (var i = 0; i < 100; ++i) {
        var arr = makeArr(i, true);
        var ref = arr.slice().sort(function (a, b) { return a.k - b.k; });
        sortFn(arr);
        for (var j = 0; j < arr.length; ++j) {
            if (arr[j].k !== ref[j].k)
                throw new Error(tag + ": wrong sort at " + j + ": " + arr[j].k + " != " + ref[j].k);
        }
    }
}

exercise(sortIgnoreResult, "ignore-result");
exercise(sortUseResult, "use-result");
exercise(sortTailCall, "tail-call");
