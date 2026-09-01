// A5 round (K4.II.18/II.19 interim closure) targeted gate.
// Per-thread hot loops driving the two formerly-shared caches:
//  - megamorphic get/put/in by id and by val (>= maxAccessVariantListSize
//    distinct shapes at one site, hot enough to tier up when JIT is on);
//  - Object.prototype.hasOwnProperty / Object.hasOwn, hit + miss keys.
// PASS criterion: output contains A5-HAMMER-PASS and no MISMATCH line.
"use strict";

const W = (globalThis.A5_THREADS | 0) || 8;
const ITERS = (globalThis.A5_ITERS | 0) || 60000;
const SHAPES = 24; // > default maxAccessVariantListSize => megamorphic site

function makeShapes() {
    const objs = [];
    for (let s = 0; s < SHAPES; ++s) {
        const o = {};
        // distinct property orders => distinct structures
        for (let p = 0; p < 6; ++p)
            o["p" + ((p + s) % 6)] = (s * 7 + p) | 0;
        o["k" + s] = s;
        objs.push(o);
    }
    return objs;
}

function work() {
    const objs = makeShapes();
    const names = ["p0", "p1", "p2", "p3", "p4", "p5"];
    let acc = 0 >>> 0;
    for (let i = 0; i < ITERS; ++i) {
        const o = objs[i % SHAPES];
        const n = names[i % 6];
        // megamorphic get_by_id / get_by_val
        acc = (acc + o.p3 + o[n]) >>> 0;
        // megamorphic put_by_id / put_by_val
        o.p3 = (acc ^ i) | 0;
        o[n] = (acc + i) | 0;
        // megamorphic in_by_id / in_by_val
        if ("p4" in o) acc = (acc + 1) >>> 0;
        if (n in o) acc = (acc + 2) >>> 0;
        // hasOwnProperty: hit + miss, string + computed key
        if (o.hasOwnProperty("p5")) acc = (acc + 3) >>> 0;
        if (o.hasOwnProperty("nope" + (i & 3))) acc = (acc + 1000000) >>> 0;
        if (Object.hasOwn(o, "k" + (i % SHAPES))) acc = (acc + 5) >>> 0;
    }
    return acc >>> 0;
}

function threadBody(expected) {
    const r = work();
    if (r !== expected)
        print("MISMATCH spawned got=" + r + " want=" + expected);
}

// Single-threaded reference value (computed before any spawn).
const expected = work();

if (typeof Thread === "function") {
    const threads = [];
    for (let t = 1; t < W; ++t)
        threads.push(new Thread(threadBody, expected));
    const mine = work();
    if (mine !== expected)
        print("MISMATCH main got=" + mine + " want=" + expected);
    for (const th of threads)
        th.join();
    print("A5-HAMMER-PASS checksum=" + expected.toString(16) + " W=" + W);
} else
    print("A5-HAMMER-PASS(single) checksum=" + expected.toString(16));
