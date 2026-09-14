//@ requireOptions("--useJSThreads=1")
// SPEC-heap history §34. GIL off, with two clients attached (a spawned thread,
// and its parent waiting in join), the window-liveness constraint rooted every
// precise allocation that a Full collection's flip() had marked "newly
// allocated" - every one that had been marked before - so a large array's
// storage that was ever marked was never freed again: a worker allocating and
// dropping 64 KB arrays saw the heap grow by each round's allocation (37.7,
// 75.2, ... 225.2 MB after each full collection). The heap must stay flat.
// Small objects never leaked (their witness is per block); they are checked
// too. GIL on and flag off: no shared heap, the same checks hold.

if (typeof Thread !== "function" || typeof gcHeapSize !== "function" || typeof fullGC !== "function") {
    print("SKIP: needs jsc shell with Thread, gcHeapSize and fullGC");
    quit(0);
}

function work(big) {
    const sizes = [];
    let sink = 0;
    for (let round = 0; round < 6; ++round) {
        for (let g = 0; g < 600; ++g) {
            if (big) {
                const a = new Array(8192); // a 64 KB butterfly: a precise allocation
                for (let i = 0; i < 8192; i += 64)
                    a[i] = i;
                sink += a.length;
            } else {
                for (let i = 0; i < 512; ++i) {
                    const o = { x: i, y: [i, i] };
                    sink += o.x;
                }
            }
        }
        fullGC();
        sizes.push(gcHeapSize() / 1048576);
    }
    return sizes;
}

for (const big of [true, false]) {
    const t = new Thread(() => work(big));
    const sizes = t.join();
    // Each big round allocates about 38 MB and keeps none of it; allow slack for the collector's own float.
    const growth = sizes[5] - sizes[1];
    if (growth > 20)
        throw new Error((big ? "large arrays" : "small objects") + ": the heap grew by " + growth.toFixed(1) + " MB over four rounds of garbage (after each full collection: " + sizes.map(v => v.toFixed(1)).join(", ") + " MB)");
}
print("PASS");
