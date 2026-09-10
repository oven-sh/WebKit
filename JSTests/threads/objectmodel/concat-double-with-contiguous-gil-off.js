//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// SPEC-objectmodel T4-C extension (eighth landing round): GIL off, arrays that
// would be Double elsewhere are often Contiguous (Int32->Double is executed as
// Int32->Contiguous, copies of Double sources are Contiguous), so concat/slice
// meet Double + Contiguous pairs that flag-off never produces; the fast copy
// now merges them into a Contiguous result with the Double lanes boxed instead
// of falling to the generic element-by-element concat (a micro of
// `doubleArray.concat(intArray)` GIL off: 181 ms before, 41 ms after; flag-off
// 59 ms). This test pins the RESULTS of every such pairing - values, holes,
// NaN, order, length, result shape - in all modes.
load("../harness.js", "caller relative");

function mkInt(n) { const a = []; for (let i = 0; i < n; ++i) a.push(i); return a; }
function mkDouble(n) { const a = JSON.parse("[" + Array.from({ length: n }, (_, i) => i + 0.25).join(",") + "]"); return a; } // Double in every mode (built in C++)
function mkObj(n) { const a = []; for (let i = 0; i < n; ++i) a.push({ i }); return a; }
function mkHoleyDouble() { const a = JSON.parse("[0.5, 1.5, 2.5, 3.5]"); delete a[1]; a[2] = NaN; return a; }

function check(name, result, expectedList) {
    if (result.length !== expectedList.length) throw new Error(name + ": length " + result.length + " != " + expectedList.length);
    for (let i = 0; i < expectedList.length; ++i) {
        const e = expectedList[i], g = result[i];
        const same = (typeof e === "number" && e !== e) ? (g !== g) : (typeof e === "object" && e !== null ? g === e : (e === undefined ? !(i in result) || g === undefined : g === e));
        if (!same) throw new Error(name + ": element " + i + " is " + String(g) + ", expected " + String(e));
    }
    if ($vm.indexingMode(result).indexOf("ArrayStorage") >= 0) throw new Error(name + ": result went to ArrayStorage: " + $vm.indexingMode(result));
}

for (let iter = 0; iter < 300; ++iter) { // enough to tier the sites up
    const I = mkInt(6), D = mkDouble(5), O = mkObj(3), H = mkHoleyDouble();
    check("D+I", D.concat(I), [...D, ...I]);
    check("I+D", I.concat(D), [...I, ...D]);
    check("D+O", D.concat(O), [...D, ...O]);
    check("O+D", O.concat(D), [...O, ...D]);
    check("D+D", D.concat(D), [...D, ...D]);
    check("H+I", H.concat(I), [0.5, undefined, NaN, 3.5, ...I]);
    check("O+H", O.concat(H), [...O, 0.5, undefined, NaN, 3.5]);
    check("D+I+O", D.concat(I, O), [...D, ...I, ...O]);
    check("D+scalar", D.concat(7.5), [...D, 7.5]);
    const C = mkInt(4); C[0] = "x"; C[0] = 0; // Contiguous numerics
    check("C+D", C.concat(D), [...C, ...D]);
    check("D+C", D.concat(C), [...D, ...C]);
    check("C+D+H", C.concat(D, H), [...C, ...D, 0.5, undefined, NaN, 3.5]);
    check("slice D", D.slice(1, 4), [1.25, 2.25, 3.25]);
}
if (typeof AMPLIFY_VERBOSE !== "undefined") {
    const D = mkDouble(8), C = mkInt(16); C[0] = "x"; C[0] = 0; // C: Contiguous holding numbers (what GIL off makes of most would-be Double arrays)
    const t = Date.now(); let s = 0; for (let i = 0; i < 1000000; ++i) s += C.concat(D).length;
    print("1e6 x contiguousArray.concat(doubleArray): " + (Date.now() - t) + " ms, shapes " + $vm.indexingMode(C) + " + " + $vm.indexingMode(D) + " -> " + $vm.indexingMode(C.concat(D)));
}
print("PASS");
