//@ requireOptions("--useJSThreads=1")
// splay-like: allocation + pointer-churn + GC pressure (Octane splay style).
//
// Each thread builds and churns its OWN top-down splay tree: every operation
// allocates a fresh node plus a short-lived payload (small array + string),
// keys arrive from a thread-local deterministic PRNG, and a FIFO of live keys
// evicts the oldest entry once the tree reaches steady-state size — exactly
// the old-young pointer churn that stresses the write barrier and keeps a
// large live set across collections. NO data is shared between threads.
//
// Scaling expectation (scaling-gate.sh --gate): this is the GC-bound
// workload; stop-the-world collections are a known SERIAL component until
// SPEC-congc lands, so its thresholds are the relaxed 2.0x@4 / 3.0x@8 rather
// than the 2.8/4.5 demanded of the non-allocating workloads.
//
// Work size targets roughly 1s per thread on a release build at scale 1 (the gate's sizing; standalone corpus runs default to a fractional CORPUS_DEFAULT_SCALE - see harness.js);
// bounded by construction (fixed op count, no waits) so a corpus run stays
// far below the 120s timeout even on debug builds.
load("./harness.js", "caller relative");

function splayWorkload() {
    const TREE_SIZE = 2000;
    const OPS = Math.round(1200000 * scalingWorkScale());

    // Thread-local xorshift32; identical seed in every thread => identical
    // independent work and identical checksums.
    let seed = 0x2545f491 | 0;
    function nextKey() {
        seed ^= seed << 13;
        seed ^= seed >>> 17;
        seed ^= seed << 5;
        seed |= 0;
        return seed >>> 0;
    }

    function Node(key, value) {
        this.key = key;
        this.value = value;
        this.left = null;
        this.right = null;
    }

    let root = null;
    let size = 0;

    // Top-down splay (Sleator/Tarjan, as in Octane splay).
    function splay(key) {
        if (root === null)
            return;
        const dummy = new Node(0, null);
        let left = dummy;
        let right = dummy;
        let current = root;
        for (;;) {
            if (key < current.key) {
                if (current.left === null)
                    break;
                if (key < current.left.key) {
                    const tmp = current.left;
                    current.left = tmp.right;
                    tmp.right = current;
                    current = tmp;
                    if (current.left === null)
                        break;
                }
                right.left = current;
                right = current;
                current = current.left;
            } else if (key > current.key) {
                if (current.right === null)
                    break;
                if (key > current.right.key) {
                    const tmp = current.right;
                    current.right = tmp.left;
                    tmp.left = current;
                    current = tmp;
                    if (current.right === null)
                        break;
                }
                left.right = current;
                left = current;
                current = current.right;
            } else
                break;
        }
        left.right = current.left;
        right.left = current.right;
        current.left = dummy.right;
        current.right = dummy.left;
        root = current;
    }

    function insert(key, value) {
        if (root === null) {
            root = new Node(key, value);
            size++;
            return true;
        }
        splay(key);
        if (root.key === key)
            return false; // duplicate; keep existing
        const node = new Node(key, value);
        if (key > root.key) {
            node.left = root;
            node.right = root.right;
            root.right = null;
        } else {
            node.right = root;
            node.left = root.left;
            root.left = null;
        }
        root = node;
        size++;
        return true;
    }

    function remove(key) {
        if (root === null)
            return false;
        splay(key);
        if (root.key !== key)
            return false;
        if (root.left === null)
            root = root.right;
        else {
            const right = root.right;
            root = root.left;
            splay(key); // largest key < removed key surfaces
            root.right = right;
        }
        size--;
        return true;
    }

    function makePayload(key) {
        // Short-lived garbage + a retained leaf: array of 8 doubles plus a
        // rope-ish tag string. Mirrors Octane splay's payload shape.
        const array = [key, key + 1, key + 2, key + 3, key * 0.5, key * 0.25, key * 0.125, key * 0.0625];
        return { array: array, tag: "node-" + (key & 0xffff) + "-payload" };
    }

    // FIFO of live keys: evict oldest once the tree is at steady state.
    const liveKeys = new Array(TREE_SIZE);
    let head = 0;
    let count = 0;
    let checksum = 0;
    let removed = 0;

    for (let op = 0; op < OPS; ++op) {
        const key = nextKey();
        if (insert(key, makePayload(key))) {
            if (count === TREE_SIZE) {
                const victim = liveKeys[head];
                if (remove(victim))
                    removed++;
                liveKeys[head] = key;
                head = (head + 1) % TREE_SIZE;
            } else {
                liveKeys[(head + count) % TREE_SIZE] = key;
                count++;
            }
        }
        if ((op & 1023) === 0) {
            // Periodic probe: splay a pseudo-random key and fold the root
            // into the checksum (also exercises pure-lookup splays).
            splay(key ^ 0x55555555);
            checksum = (checksum + (root.key % 65521) + root.value.array.length) % 0x7fffffff;
        }
    }

    return checksum + ":" + size + ":" + removed;
}

runScalingWorkload("splay-like", splayWorkload);

// WOULD-FAIL-IF: (a) per-thread allocation or GC paths regress to a serial
// bottleneck — a reintroduced global allocator/heap lock, directory/TLAB
// contention, or GC pauses growing with mutator count — which collapses
// speedup(4)/speedup(8) below the documented 2.0/3.0 floor and trips
// scaling-gate.sh --gate on exactly this workload — NOTE this half of the
// claim is live ONLY when the pinned --gate rung runs (see
// Tools/threads/INTEGRATE-scaling.md; default corpus runs check only the
// checksum half, plus the opt-in SCALING_SELF_TRIPWIRE in harness.js for
// gross re-serialization); (b) parallel mutators
// corrupt supposedly thread-local pointer graphs (write-barrier or
// sweep/scavenge races against a churning old-young tree), which makes a
// thread's splay tree diverge from the single-thread reference and fails the
// harness checksum comparison deterministically, even in report-only runs.
