//@ requireOptions("--useJSThreads=1")
// for (k in o) o[k] = v over an object with more properties than its inline
// capacity: every out-of-line property must be written under its own name.
// The baseline enumerator_put_by_val fast path used to reroute the flag-on
// out-of-line store into the structure-mismatch tail with the enumeration
// index still in the register the IndexedMode test reads, so odd indices were
// stored as o[index] instead of o[name].

function fill(o, n) {
    for (var i = 0; i < n; i++)
        o["p" + i] = i;
    return o;
}
noInline(fill);

function store(o, v) {
    for (var k in o)
        o[k] = v;
}
noInline(store);

const count = 20;
for (var iter = 0; iter < 2000; iter++) {
    var o = fill({}, count);
    store(o, -1);
    var keys = Object.keys(o);
    if (keys.length !== count)
        throw new Error("iteration " + iter + ": unexpected keys " + keys.join(","));
    for (var i = 0; i < count; i++) {
        if (o["p" + i] !== -1)
            throw new Error("iteration " + iter + ": p" + i + " = " + o["p" + i]);
    }
}
