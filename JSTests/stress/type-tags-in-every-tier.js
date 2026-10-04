//@ requireOptions("--useTypeTags=1")
//@ runDefault("--useLOLJIT=1")
//@ runDefault("--useLOLJIT=1", "--useDFGJIT=0")
//@ runDefault
//@ runDefault("--useJIT=0")
//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function makes(a) { return 000001{ x: a, y: a + 1 }; }
function reads(o) { return o.000001x + o.000001y; }
function writes(o, v) { o.000001x = v; return o; }
function destructures(o) { let { 000001x, 000001y } = o; return x * y; }
function updates(o) { o.000001x++; o.000001y += 2; return o; }
function Point(x) { this.000001x = x; }
for (let i = 0; i < 20000; ++i) {
    let o = makes(i);
    check(reads(o), 2 * i + 1, "reads");
    check(writes(o, 5).x, 5, "writes");
    check(destructures(o), 5 * (i + 1), "destructures");
    check(reads(updates(o)), 6 + i + 3, "updates");
    check(new Point(i).x, i, "a constructor");
}
