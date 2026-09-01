//@ requireOptions("--useJSThreads=1")
// for (k in o) o[k] = v over an object whose butterfly a spawned thread
// converted to a segmented spine by adding an out-of-line property. The
// object stays on the ordinary post-add Structure the enumerator caches, so
// the compiled own-structure fast path passes its structure check; its
// out-of-line store must run the butterfly write predicate instead of
// storing through the masked spine word (which used to silently miss the
// property and write before the spine allocation).

function make(n) {
    var o = {};
    for (var i = 0; i < n; i++)
        o["p" + i] = i;
    return o;
}
noInline(make);

function store(o, v) {
    for (var k in o)
        o[k] = v;
}
noInline(store);

const count = 12; // more properties than the literal's inline capacity

function check(label) {
    var o = make(count);
    var t = new Thread(() => { o.foreign = "foreign"; return 1; });
    if (t.join() !== 1)
        throw new Error(label + ": thread did not run");

    store(o, -7);

    var keys = Object.keys(o);
    if (keys.length !== count + 1)
        throw new Error(label + ": unexpected keys " + keys.join(","));
    for (var i = 0; i < count; i++) {
        if (o["p" + i] !== -7)
            throw new Error(label + ": p" + i + " = " + o["p" + i]);
    }
    if (o.foreign !== -7)
        throw new Error(label + ": foreign = " + o.foreign);
}

var iter = 0;
for (const [target, label] of [[500, "warm"], [2000, "dfg"], [20000, "ftl"]]) {
    for (; iter < target; iter++)
        store(make(count), iter);
    check(label);
}
