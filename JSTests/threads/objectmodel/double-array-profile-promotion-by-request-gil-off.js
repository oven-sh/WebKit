//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-objectmodel T4-P promotion, history §40 (tenth round). GIL off an Int32
// array that meets a double becomes Contiguous (T4-O), and the promotion of
// history §28 lets the allocation site recommend Double when the array that
// demoted it looks numeric. It looked at sampled lanes only - 24 from the
// front, 8 spread over the rest - so a site whose arrays are large, zero-
// filled and receive few or late doubles (navier-stokes: six arrays of 16,900
// lanes per iteration; ML's zero-filled matrix rows) stayed Contiguous for the
// whole run and its arithmetic ran on boxed doubles. The request itself is now
// remembered: an array on which T4-O substituted Int32->Contiguous for an
// Int32->Double request marks its site numeric whatever its lanes hold.
//  (1) field(): a zero-filled array of 4,096 lanes whose only doubles sit in
//      lanes the sampling never reads. Before: ArrayWithContiguous on every
//      call GIL off. After: ArrayWithDouble from the third call on, as flag off
//      and GIL on always did.
//  (2) The same shape of site whose arrays later take a string settles
//      Contiguous and does not keep converting.
//  (3) Four threads agree on the sums.
load("../harness.js", "caller relative");

const N = 4096;
// Lanes that are neither among the first 24 nor any of first + (N - 24) * k / 9, k = 1..8.
const hidden = [100, 1000, 3000];
function field(seed) {
    // fill(), not a loop: a long loop here runs the tier-up checks of the lower tiers, which consume the profile's
    // last-array word while the array is still all int32 (the site then learns Int32 and forgets the array).
    const a = new Array(N).fill(0);
    a[hidden[0]] = seed + 0.5; // the Int32 -> Double request
    a[hidden[1]] = seed + 0.5;
    a[hidden[2]] = seed + 0.5;
    return a;
}
noInline(field);
function sum(a) { let s = 0; for (let i = 0; i < a.length; ++i) s += a[i]; return s; }
noInline(sum);

let total = 0;
for (let i = 0; i < 40; ++i)
    total += sum(field(i));
const expected = hidden.length * (40 * 39 / 2 + 40 * 0.5);
if (Math.abs(total - expected) > 1e-6)
    throw new Error("sum " + total + " expected " + expected);
const mode = $vm.indexingMode(field(7));
if (typeof AMPLIFY_VERBOSE !== "undefined")
    print("field() arrays after 40 calls: " + mode + "; promotions: " + $vm.jsThreadsCounter("arrayAllocationProfilePromotedToDoubleGILOff"));
if (mode !== "ArrayWithDouble")
    throw new Error("a zero-filled numeric site mints " + mode + " after 40 calls (expected ArrayWithDouble)");

// (2) A site of the same shape whose arrays are not numeric after all.
function table(seed) {
    const a = new Array(64).fill(0);
    a[40] = seed + 0.5;
    a[41] = (seed & 1) ? "odd" : null;
    return a;
}
noInline(table);
const stops0 = $vm.jsThreadsCounter("stwRequest");
let m = 0;
for (let i = 0; i < 20000; ++i) {
    const a = table(i);
    m += a[40];
    if (a[41] === "odd")
        m += 1;
}
const stops = $vm.jsThreadsCounter("stwRequest") - stops0;
const modeTable = $vm.indexingMode(table(8));
if (typeof AMPLIFY_VERBOSE !== "undefined")
    print("table() arrays: " + modeTable + "; stop requests over 20000 arrays: " + stops);
if (modeTable !== "ArrayWithContiguous")
    throw new Error("a mixed site mints " + modeTable);
if (!$vm.useThreadGIL() && stops > 2000)
    throw new Error("a mixed site keeps converting its arrays: " + stops + " stop requests for 20000 arrays");

// (3)
function work() { let t = 0; for (let i = 0; i < 60; ++i) t += sum(field(i)); for (let i = 0; i < 2000; ++i) t += table(i)[40]; return t; }
const threads = [];
for (let i = 0; i < 3; ++i)
    threads.push(new Thread(work));
const mine = work();
for (const t of threads) {
    const theirs = t.join();
    if (Math.abs(theirs - mine) > 1e-6)
        throw new Error("thread sum " + theirs + " vs " + mine);
}
