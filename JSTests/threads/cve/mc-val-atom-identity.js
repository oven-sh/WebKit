//@ requireOptions("--useJSThreads=1")
// MC-VAL susceptibility test (docs/threads/cve/map-MC-VAL.md, surface V6):
// validator/consumer disagreement in the sharded atom-string table.
//
// Validator: atomization (SharedAtomStringTable shardForHash + per-shard
// lock, vmstate SPEC §4.2-4.4) promises every character sequence has at
// most ONE live atom, so consumers (PropertyTable lookups, transition-table
// keys, IC identity compares) may use POINTER equality for name identity.
// Consumer assumption broken if ANY entry path bypasses A1 routing
// (vmstate §4.3: all 17 legacy locker sites + explicit-table overloads must
// reroute) or if no-resurrection tryRefAtom (StringImpl.h:1308) admits a
// revived duplicate: two distinct atoms for the same chars => a property
// written under one thread's atomization is silently invisible to another
// thread's lookup. CVE-2024-2887 analogue: same index/name, two namespaces.
//
// Deterministic: no data race needed — join() is the happens-before edge.
// Each thread constructs the SAME logical names through DIFFERENT string
// paths (fromCharCode, rope concat resolved by use, slice of a larger
// backing string) so atomization runs independently per thread per name.
// Executed post-ungil; under the phase-1 GIL it must also pass.
load("../harness.js", "caller relative");

const N = 64;
const o = {};

function nameVariant(i, variant) {
    const base = "mcValProp_" + i;
    switch (variant) {
    case 0:
        return base; // plain literal-derived rope
    case 1: {
        // Built char-by-char: distinct StringImpl, same chars.
        let s = "";
        for (let j = 0; j < base.length; ++j)
            s += String.fromCharCode(base.charCodeAt(j));
        return s;
    }
    case 2:
        // Slice out of a padded backing store.
        return ("##" + base + "##").slice(2, 2 + base.length);
    }
}

// Spawned thread atomizes variant-1 names and writes through them.
const writer = new Thread((obj, n) => {
    for (let i = 0; i < n; ++i) {
        let s = "";
        const base = "mcValProp_" + i;
        for (let j = 0; j < base.length; ++j)
            s += String.fromCharCode(base.charCodeAt(j));
        obj[s] = base + "!";
    }
    return true;
}, o, N);
shouldBe(writer.join(), true);

// Main thread looks the properties up through independently constructed
// equal strings. A duplicate atom => undefined (lost property) here.
for (let i = 0; i < N; ++i) {
    const expected = "mcValProp_" + i + "!";
    shouldBe(o[nameVariant(i, 0)], expected);
    shouldBe(o[nameVariant(i, 2)], expected);
    // Atomics property path resolves names through the same uid identity.
    shouldBe(Atomics.load(o, nameVariant(i, 1)), expected);
}

// Symbol registry leg (SPEC-ungil §H: SymbolRegistry m_lock): Symbol.for on
// two threads with equal descriptions must observe ONE registered symbol.
const symProbe = { hit: 0, sym: null };
const symThread = new Thread((probe) => {
    probe.sym = Symbol.for("mcVal-registered-symbol");
    probe.hit = 1;
    return true;
}, symProbe);
shouldBe(symThread.join(), true);
shouldBe(symProbe.hit, 1);
shouldBe(Symbol.for("mcVal-registered-symbol"), symProbe.sym);
shouldBe(Symbol.keyFor(symProbe.sym), "mcVal-registered-symbol");
