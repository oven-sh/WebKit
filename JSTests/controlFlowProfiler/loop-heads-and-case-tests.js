var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { }

function count(func, text, expected) {
    let actual = basicBlockExecutionCount(func, text);
    assert(actual === expected, `${func.name}: "${text}" ran ${actual} times, not ${expected}`);
}

// The parts of the head of a loop run a different number of times than the statement.
function whileHead(limit, more) {
    let i = 0;
    while (i < limit && more)
        i++;
}
whileHead(3, true);
count(whileHead, "let i", 1);
count(whileHead, "while", 1);
count(whileHead, "i < limit", 4);
count(whileHead, "&& more", 3);
count(whileHead, "i++", 3);

function whileLeftEarly(limit) {
    let i = 0;
    while (i < limit) {
        if (i++ === 1)
            break;
    }
}
whileLeftEarly(9);
count(whileLeftEarly, "i < limit", 2);

function whileTrue() {
    while (true)
        break;
}
whileTrue();
count(whileTrue, "true", 1);

function whileArrowFunction() {
    let calls = 0;
    while (() => noCall())
        if (calls++ === 1)
            break;
}
whileArrowFunction();
count(whileArrowFunction, "(() =>", 2);
count(whileArrowFunction, "noCall()", 0);

function forHead(limit, step) {
    for (let i = 0; i < limit || step; i += step ?? 1)
        limit;
}
forHead(3, null);
count(forHead, "for", 1);
count(forHead, "let i", 1);
count(forHead, "i < limit", 4);
count(forHead, "step;", 1);
count(forHead, "i +=", 3);
count(forHead, "1)", 3);
count(forHead, "limit;", 3);

function forWithoutATest() {
    for (let i = 0; ; i++) {
        if (i === 2)
            break;
    }
}
forWithoutATest();
count(forWithoutATest, "i++", 2);

function forWithContinue(limit) {
    for (let i = 0; i < limit; i++) {
        if (i)
            continue;
    }
}
forWithContinue(3);
count(forWithContinue, "i < limit", 4);
count(forWithContinue, "i++", 3);

function forNeverEntered() {
    return;
    for (let i = 0; i < 1; i++)
        ;
}
forNeverEntered();
count(forNeverEntered, "i < 1", 0);
count(forNeverEntered, "i++", 0);

function forOfName(list) {
    for (const item of list)
        item;
}
forOfName([]);
count(forOfName, "for", 1);
count(forOfName, "item of", 0);
count(forOfName, "of list", 1);
forOfName([1, 2]);
count(forOfName, "for", 2);
count(forOfName, "item of", 2);
count(forOfName, "of list", 2);

function forOfPattern(list) {
    for (const [item = noCall()] of list)
        item;
}
forOfPattern([[1], [2], [3]]);
count(forOfPattern, "item =", 3);
count(forOfPattern, "noCall()", 0);
count(forOfPattern, "of list", 1);

function forOfAssignment(list, object) {
    for (object.item of list)
        ;
    for ([object.first = 1] of list)
        ;
}
forOfAssignment([[], [2]], { });
count(forOfAssignment, "object.item", 2);
count(forOfAssignment, "object.first", 2);
count(forOfAssignment, "1]", 1);

function forInName(object) {
    for (var key in object)
        key;
}
forInName({ });
count(forInName, "key in", 0);
count(forInName, "in object", 1);
forInName({ a: 1, b: 2 });
count(forInName, "key in", 2);
count(forInName, "in object", 2);

function forInAssignment(object, target) {
    for (target.key in object)
        ;
}
forInAssignment({ a: 1, b: 2, c: 3 }, { });
count(forInAssignment, "target.key", 3);

async function forAwait(list) {
    for await (const item of list)
        item;
}
forAwait([1, 2]);
drainMicrotasks();
count(forAwait, "item of", 2);
count(forAwait, "of list", 1);

function* testWithAYield(limit) {
    let i = 0;
    while (yield i, i++ < limit)
        ;
}
for (let _ of testWithAYield(2))
    ;
count(testWithAYield, "yield i", 3);
count(testWithAYield, "i++ <", 3);

// Annex B: the initializer runs once, before the loop.
function forInInitializer(object) {
    for (var key = foo() in object)
        ;
}
forInInitializer({ });
count(forInInitializer, "foo()", 1);
forInInitializer({ a: 1, b: 2 });
count(forInInitializer, "foo()", 2);

// A test that is a constant or a name is a label: it has the count of its clause. Any other test runs when it is compared.
var Kind = { A: 100, inner: { B: 101 } };
var name = 102;
function call() { return -1; }

function labels(s) {
    switch (s) {
    case Kind.A: return 10;
    case Kind.inner.B: return 11;
    case name: return 12;
    case 103: return 1;
    case "s": return 2;
    case -1: return 3;
    case null: return 4;
    case this: return 5;
    case this.kind: return 6;
    }
}
labels(0);
for (let label of ["Kind.A", "Kind.inner.B", "name:", "103", "\"s\"", "-1", "null", "this:", "this.kind"])
    count(labels, label, 0);
labels(101);
count(labels, "Kind.A", 0);
count(labels, "Kind.inner.B", 1);
count(labels, "return 11", 1);
count(labels, "name:", 0);

let object = {
    labels(s) {
        switch (s) {
        case super.kind: return 7;
        }
    }
};
object.labels(0);
count(object.labels, "super.kind", 0);

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
