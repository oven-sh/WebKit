//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-objectmodel history §28 (eighth landing round). GIL off an Int32 array
// that meets a double is relabelled Contiguous (T4-O: the in-place
// Int32->Double rewrite is not safe against a concurrent reader), and its
// allocation site's profile used to learn Contiguous from it, so every later
// array from that site kept its doubles boxed for good. Now the profile
// recommends Double when the array that demoted it holds only numbers (and at
// least one non-int32), once per site; arrays born Double take int32 and
// double stores without any conversion. (1) A numeric site (`[0, 0, 0]` filled
// with doubles) mints ArrayWithDouble after warm-up in every mode (GIL off
// before: ArrayWithContiguous). (2) A site whose arrays really are mixed
// settles Contiguous and does not keep converting (stops stay bounded). (3)
// Four threads running both sites agree on the sums.
load("../harness.js", "caller relative");

function vec(n) { const v = [0, 0, 0]; v[0] = n * 0.5; v[1] = n * 0.25 + 1; v[2] = n / 3; return v; }
noInline(vec);
function mixed(n) { const a = [0, 0]; a[0] = n + 0.5; a[1] = (n & 1) ? "odd" : null; return a; }
noInline(mixed);
function norm(v) { return Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
noInline(norm);

let s = 0;
for (let i = 0; i < 20000; ++i) s += norm(vec(i));
const modeVec = $vm.indexingMode(vec(7));
if (typeof AMPLIFY_VERBOSE !== "undefined") print("vec() arrays after warm-up: " + modeVec + "; promotions: " + $vm.jsThreadsCounter("arrayAllocationProfilePromotedToDoubleGILOff"));
if (modeVec !== "ArrayWithDouble") throw new Error("a numeric [0,0,0] site mints " + modeVec + " after warm-up (expected ArrayWithDouble)");

const stops0 = $vm.jsThreadsCounter("stwRequest");
let m = 0;
for (let i = 0; i < 50000; ++i) { const a = mixed(i); m += a[0]; if (a[1] === "odd") m += 1; }
const stops = $vm.jsThreadsCounter("stwRequest") - stops0;
const modeMixed = $vm.indexingMode(mixed(8));
if (typeof AMPLIFY_VERBOSE !== "undefined") print("mixed() arrays: " + modeMixed + "; stop requests over 50000 arrays: " + stops);
if (modeMixed !== "ArrayWithContiguous") throw new Error("a mixed site mints " + modeMixed);
if (!$vm.useThreadGIL() && stops > 2000) throw new Error("a mixed site keeps converting its arrays: " + stops + " stop requests for 50000 arrays");

function work(k) { let t = 0; for (let i = 0; i < 20000; ++i) { t += norm(vec(i + k)); const a = mixed(i); t += a[0]; } return t; }
const threads = []; for (let i = 0; i < 3; ++i) threads.push(new Thread(() => work(0)));
const mine = work(0);
for (const t of threads) { const theirs = t.join(); if (Math.abs(theirs - mine) > 1e-6) throw new Error("thread sum " + theirs + " vs " + mine); }
print("PASS");
