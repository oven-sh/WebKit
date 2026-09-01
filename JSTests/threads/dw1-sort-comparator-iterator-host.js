// DW-1 amend regression (deepwater LEDGER row 1): ArraySortIntrinsic must NOT
// be hosted at op_iterator_open / op_iterator_next. Without the host-opcode
// guard in ByteCodeParser::handleArraySort, BoundFunctionCallIntrinsic
// expansion of a bound sort (bound args defeat the argc < 2 rejection) hosts
// the intrinsic at op_iterator_open; an OSR exit inside the body-inlined
// comparator then recovers through arraySortComparatorReturnTrampoline to an
// iterator pc outside the {op_call, op_call_ignore_result, op_tail_call}
// recovery set -> ASSERT (debug) / DW-1 RELEASE_ASSERT (GIL-off).
// Green GIL-on and GIL-off. Post-flip double `k` values are the comparator
// BadType OSR-exit trigger; iterator protocol terminates immediately so the
// DFG code stays live across the type flip.
var target = [];
function comparator(a, b) {
    return a.k - b.k;
}

// sort() returns `target` (the bound this); give arrays a callable next so
// the iterator protocol terminates immediately without throwing.
Array.prototype.next = function () { return { done: true, value: undefined }; };
Array.prototype[Symbol.iterator] = Array.prototype.sort.bind(target, comparator);

var iterable = [1, 2, 3];

function test() {
    for (var x of iterable) { }
}
noInline(test);

function fill(seed, flipped) {
    target.length = 0;
    var s = seed >>> 0;
    for (var i = 0; i < 12; ++i) {
        s = (s * 1103515245 + 12345) >>> 0;
        var v = (s % 1000) | 0;
        target.push({ k: flipped ? v + 0.5 : v });
    }
}

for (var i = 0; i < 100000; ++i) {
    fill(i, false);
    test();
}

for (var i = 0; i < 200; ++i) {
    fill(i, true);
    test();
}
print("PASS");
