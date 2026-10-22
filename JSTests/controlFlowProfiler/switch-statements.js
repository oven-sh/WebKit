var hasBasicBlockExecuted = $vm.hasBasicBlockExecuted;

load("./driver/driver.js");

var a, b, c;
function testSwitch(s) {
    switch (s) {
    case "foo":
        return a;
    case "bar":
        return b;
    default:
        return c;
    }
}

assert(!hasBasicBlockExecuted(testSwitch, "switch"), "should not have executed yet.");

testSwitch("foo");
assert(hasBasicBlockExecuted(testSwitch, "switch"), "should have executed.");
assert(hasBasicBlockExecuted(testSwitch, "return a"), "should have executed.");
assert(!hasBasicBlockExecuted(testSwitch, "return b"), "should not have executed yet.");
assert(!hasBasicBlockExecuted(testSwitch, "return c"), "should not have executed yet.");

testSwitch("bar");
assert(hasBasicBlockExecuted(testSwitch, "return b"), "should have executed.");
assert(!hasBasicBlockExecuted(testSwitch, "return c"), "should not have executed yet.");

testSwitch("");
assert(hasBasicBlockExecuted(testSwitch, "return c"), "should have executed.");


// A test that is a constant or a name is a label: it has the count of its clause. Any other test runs when it is compared.
var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

function count(func, expr, expected) {
    let actual = basicBlockExecutionCount(func, expr);
    assert(actual === expected, `"${expr}" in ${func.name} ran ${expected} times, not ${actual}`);
}

var Kind = { A: 100, inner: { B: 101 } };
var name = 102;
function call() { return -1; }

function labels(s) {
    switch (s) {
    case Kind.A: return a;
    case Kind.inner.B: return b;
    case name: return c;
    case 103: return 1;
    case "s": return 2;
    case -1: return 3;
    case null: return 4;
    }
}
labels(0);
for (let label of ["Kind.A", "Kind.inner.B", "name:", "103", "\"s\"", "-1", "null"])
    count(labels, label, 0);
labels(101);
count(labels, "Kind.A", 0);
count(labels, "Kind.inner.B", 1);
count(labels, "return b", 1);
count(labels, "name:", 0);

function tests(s, object) {
    switch (s) {
    case object && object.a: return 1;
    case object?.b: return 2;
    case object ? 3 : 4: return 3;
    case call(): return 5;
    case s > 6: return 6;
    case Kind["A"]: return 7;
    default: return 8;
    case (object ??= { }): return 9;
    }
}
tests(0, { });
tests(0, null);
count(tests, "object &&", 2);
count(tests, "object.a", 1);
count(tests, "return 1", 0);
count(tests, "object?.b", 2);
count(tests, "?.b", 1);
count(tests, "return 2", 0);
count(tests, "object ? 3", 2);
count(tests, "3 :", 1);
count(tests, "4:", 1);
count(tests, "call()", 2);
count(tests, "return 5", 0);
count(tests, "s > 6", 2);
count(tests, "Kind[", 2);
count(tests, "object ??=", 2);
count(tests, "{ })", 1);
count(tests, "return 9", 0);
count(tests, "return 8", 2);

function notCompared(s) {
    switch (s) {
    case call() + 2: return 1;
    case call(): return 2;
    }
}
notCompared(1);
count(notCompared, "call() + 2", 1);
count(notCompared, "return 1", 1);
count(notCompared, "call():", 0);
