//@ requireOptions("--useJSThreads=1", "--useJIT=0", "--useThreadedLLIntICs=0")
// With useThreadedLLIntICs=0 the flag-on LLInt one-word property caches are
// never published, so every get_by_id / try_get_by_id / get_by_id_direct /
// put_by_id stays on the slow path; the results must not change. The same
// run covers the flag-on freeze of op_resolve_scope metadata: a global
// defined by a later script, after the reading function's CodeBlock linked,
// resolves through the slow path forever and must still observe later
// redefinitions.

function check(cond, msg) { if (!cond) throw new Error(msg); }

function getA(o) { return o.a; }
function getDirect(o) { return o.b; }
function putA(o, v) { o.a = v; }
function getLength(o) { return o.length; }
function readLateVar() { return lateVar; }
function readLateLexical() { return lateLexical; }

function Point(a, b) { this.a = a; this.b = b; }
var points = [];
for (var i = 0; i < 64; ++i)
    points.push(new Point(i, i * 2));

for (var iteration = 0; iteration < 200; ++iteration) {
    for (var j = 0; j < points.length; ++j) {
        var p = points[j];
        check(getA(p) === j + iteration, "get_by_id iteration " + iteration + " index " + j);
        check(getDirect(p) === j * 2, "get_by_id_direct index " + j);
        putA(p, j + iteration + 1);
    }
    check(getLength(points) === 64, "array length");
    check(getLength("abc") === 3, "string length");
}

function expectReferenceError(f, what) {
    var threw = false;
    try {
        f();
    } catch (e) {
        threw = e instanceof ReferenceError;
    }
    check(threw, what + " must throw ReferenceError before definition");
}

expectReferenceError(readLateVar, "late global var");
expectReferenceError(readLateLexical, "late lexical binding");

loadString("var lateVar = 1;");
for (var k = 0; k < 100; ++k)
    check(readLateVar() === 1, "late global var, iteration " + k);
loadString("lateVar = 2;");
check(readLateVar() === 2, "late global var must observe redefinition");

loadString("let lateLexical = 3;");
for (var k = 0; k < 100; ++k)
    check(readLateLexical() === 3, "late lexical binding, iteration " + k);
loadString("lateLexical = 4;");
check(readLateLexical() === 4, "late lexical binding must observe reassignment");
