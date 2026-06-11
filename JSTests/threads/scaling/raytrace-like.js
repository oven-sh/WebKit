//@ requireOptions("--useJSThreads=1")
// raytrace-like: numeric kernel + a steady stream of SMALL short-lived
// objects ({x,y,z} vectors), in the spirit of the V8 raytrace benchmark.
//
// Each thread renders its OWN sequence of small frames of a fixed 5-sphere
// scene with one point light and single-bounce shading. Vector math goes
// through functions returning fresh {x,y,z} objects, so the workload mixes
// double-heavy arithmetic with high-rate young-generation allocation of
// uniform small objects — sizing the eden/nursery path under N mutators
// without the pointer churn or live-set retention of splay-like. NO data is
// shared between threads; the checksum is a quantized luminance sum, so it
// is exact integer arithmetic and deterministic across runs and threads.
//
// Gate class: standard thresholds (2.8x@4 / 4.5x@8). Small-object nursery
// allocation is expected to scale; only the splay-like live-set/GC workload
// gets the relaxed STW-GC floor.
//
// Work size targets roughly 1s per thread on a release build at scale 1 (the gate's sizing; standalone corpus runs default to a fractional CORPUS_DEFAULT_SCALE - see harness.js);
// fixed frame count, no blocking ops.
load("./harness.js", "caller relative");

function raytraceWorkload() {
    // 48x48: smoke-measured 2026-06-07 — a 96x96 frame costs ~6.8s on the
    // Debug+ASAN build (per-pixel shading allocates dozens of {x,y,z}
    // temporaries, and ASAN poisoning dominates), which blew past the corpus
    // budget at ANY frame count (warmup x2 + 2 serialized threads = 4 full
    // workFn runs). Quartering the pixel count keeps the same allocation
    // profile per pixel while bounding the corpus run; the gate scales frame
    // COUNT via SCALING_WORK_SCALE, so gate work remains ample.
    const WIDTH = 48;
    const HEIGHT = 48;
    const FRAMES = Math.round(512 * scalingWorkScale());

    function vec(x, y, z) { return { x: x, y: y, z: z }; }
    function add(a, b) { return vec(a.x + b.x, a.y + b.y, a.z + b.z); }
    function sub(a, b) { return vec(a.x - b.x, a.y - b.y, a.z - b.z); }
    function scale(a, s) { return vec(a.x * s, a.y * s, a.z * s); }
    function dot(a, b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    function normalize(a) {
        const inv = 1 / Math.sqrt(dot(a, a));
        return scale(a, inv);
    }

    const spheres = [
        { center: vec(0, -100.5, -1), radius: 100, albedo: 0.5 },
        { center: vec(0, 0, -1), radius: 0.5, albedo: 0.9 },
        { center: vec(-1.1, 0, -1.2), radius: 0.5, albedo: 0.7 },
        { center: vec(1.1, 0, -1.2), radius: 0.5, albedo: 0.6 },
        { center: vec(0, 1.0, -1.6), radius: 0.4, albedo: 0.8 },
    ];
    const light = normalize(vec(0.6, 1.0, 0.4));

    // hit() returns a fresh record (small-object pressure) or null.
    function hit(origin, dir) {
        let nearestT = Infinity;
        let nearest = null;
        for (let i = 0; i < spheres.length; ++i) {
            const s = spheres[i];
            const oc = sub(origin, s.center);
            const b = dot(oc, dir);
            const c = dot(oc, oc) - s.radius * s.radius;
            const disc = b * b - c;
            if (disc <= 0)
                continue;
            const t = -b - Math.sqrt(disc);
            if (t > 0.001 && t < nearestT) {
                nearestT = t;
                nearest = s;
            }
        }
        if (nearest === null)
            return null;
        const point = add(origin, scale(dir, nearestT));
        const normal = normalize(sub(point, nearest.center));
        return { t: nearestT, point: point, normal: normal, sphere: nearest };
    }

    function shade(origin, dir) {
        const h = hit(origin, dir);
        if (h === null)
            return 0.15 + 0.1 * (dir.y + 1); // sky gradient
        let lum = 0.08; // ambient
        const lambert = dot(h.normal, light);
        if (lambert > 0) {
            // Shadow probe toward the light (second intersection pass).
            const shadow = hit(add(h.point, scale(h.normal, 0.002)), light);
            if (shadow === null)
                lum += h.sphere.albedo * lambert;
        }
        // One mirror bounce, attenuated, no recursion beyond depth 1.
        const reflected = normalize(sub(dir, scale(h.normal, 2 * dot(dir, h.normal))));
        const h2 = hit(add(h.point, scale(h.normal, 0.002)), reflected);
        if (h2 !== null) {
            const lambert2 = dot(h2.normal, light);
            if (lambert2 > 0)
                lum += 0.25 * h2.sphere.albedo * lambert2;
        } else
            lum += 0.05;
        return lum;
    }

    let checksum = 0;
    for (let frame = 0; frame < FRAMES; ++frame) {
        // Deterministic per-frame camera dolly; every thread renders the
        // exact same frame sequence.
        const camX = 0.02 * (frame % 16);
        const origin = vec(camX, 0.15, 1.2);
        for (let py = 0; py < HEIGHT; ++py) {
            for (let px = 0; px < WIDTH; ++px) {
                const u = (px + 0.5) / WIDTH * 2 - 1;
                const v = 1 - (py + 0.5) / HEIGHT * 2;
                const dir = normalize(vec(u, v, -1.4));
                const lum = shade(origin, dir);
                // Quantize before accumulating: integer math from here on,
                // so the checksum is exact and order-independent issues
                // cannot arise (single accumulation order anyway).
                let q = (lum * 255) | 0;
                if (q > 255)
                    q = 255;
                if (q < 0)
                    q = 0;
                checksum = (checksum + q + ((px ^ py) & 7)) % 0x7fffffff;
            }
        }
    }

    return checksum + ":" + FRAMES + "x" + WIDTH + "x" + HEIGHT;
}

runScalingWorkload("raytrace-like", raytraceWorkload);

// WOULD-FAIL-IF: the small-object fast allocation path stops scaling — e.g.
// per-thread allocators (TLABs / local allocator caches) regress to a shared
// locked free-list, BlockDirectory refill serializes under N mutators, or
// double-heavy code deopts to a serialized slow path when run on spawned
// threads — collapsing speedup below 2.8@4 / 4.5@8 in scaling-gate.sh --gate
// while richards-like (no allocation) still scales: the pair isolates the
// regression to the allocation path. NOTE the speedup half of this claim is
// live ONLY when the pinned --gate rung runs (see
// Tools/threads/INTEGRATE-scaling.md; default corpus runs check only the
// checksum half, plus the opt-in SCALING_SELF_TRIPWIRE in harness.js for
// gross re-serialization). Standalone, a nursery/scavenge race
// that hands two threads the same memory or tears a freshly-allocated
// {x,y,z} shows up as a luminance-checksum mismatch against the
// single-thread reference.
