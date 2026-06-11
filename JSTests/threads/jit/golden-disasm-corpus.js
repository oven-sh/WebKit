// golden-disasm-corpus.js — fixed, deterministic corpus for the I1 golden
// disassembly diff (golden-disasm.sh). Exercises every emission family the
// threads work touches, so any flag-OFF instruction-shape change shows up:
//   - get_by_id / put_by_id (LLInt metadata + Baseline packed inline path,
//     §4.2/§4.3 repacks: only field-offset immediates may move, I1)
//   - get_by_val / put_by_val contiguous + ArrayStorage (§5.5 choke points)
//   - monomorphic + polymorphic calls (§5.8 records: flag-off must emit no
//     record reads)
//   - transitions (allocate/reallocate property storage)
//   - scope access (get/put_to_scope GlobalProperty)
//
// Determinism rules: no Date/Math.random, fixed iteration counts, noInline on
// every measured function so inlining decisions cannot reshuffle code.

function getById(o) { return o.f; }
noInline(getById);

function putById(o, v) { o.f = v; }
noInline(putById);

function getByVal(a, i) { return a[i]; }
noInline(getByVal);

function putByVal(a, i, v) { a[i] = v; }
noInline(putByVal);

function getByValAS(a, i) { return a[i]; }
noInline(getByValAS);

function callMono(f) { return f(1); }
noInline(callMono);

function callPoly(f, x) { return f(x); }
noInline(callPoly);

function transition() {
    const o = { a: 1 };
    o.b = 2; o.c = 3; o.d = 4; o.e = 5; o.f = 6; o.g = 7; o.h = 8;
    return o.h;
}
noInline(transition);

var globalScopeVar = 0;
function scopeAccess(v) { globalScopeVar = v; return globalScopeVar; }
noInline(scopeAccess);

const addOne = x => x + 1;
const addTwo = x => x + 2;

const flat = { f: 1, g: 2 };
// Force out-of-line properties on a second receiver (polymorphic site).
const fat = { x0: 0 };
for (let i = 1; i < 12; ++i)
    fat["x" + i] = i;
fat.f = 3;

const contiguous = [1, 2, 3, 4, 5, 6, 7, 8];
const arrayStorage = [1, 2, 3, 4];
if (typeof $vm !== "undefined" && $vm.ensureArrayStorage)
    $vm.ensureArrayStorage(arrayStorage);

let sink = 0;
for (let i = 0; i < 100000; ++i) {
    sink += getById(flat);
    sink += getById(fat);
    putById(flat, i & 7);
    sink += getByVal(contiguous, i & 7);
    putByVal(contiguous, i & 7, i & 15);
    sink += getByValAS(arrayStorage, i & 3);
    sink += callMono(addOne);
    sink += callPoly((i & 1) ? addOne : addTwo, i & 3);
    sink += transition();
    sink += scopeAccess(i & 7);
}
print("CORPUS-SINK " + sink);
