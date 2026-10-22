var hasBasicBlockExecuted = $vm.hasBasicBlockExecuted;

load("./driver/driver.js");

function forRegular(limit) {
    var sum = 0;
    for (var i = 0; i < limit; i++) {
        sum += i;
    }

    return sum;
}

function forIn(o) {
    var s = "";
    var p;
    for (p in o) {
        s += p;
    }
}

function forOf(a) {
    var s = "";
    var p;
    for (p of a) {
        s += p;
    }
}

function whileLoop(limit) {
    var i = 0;
    var sum = 0;
    while (i < limit) {
        sum += i;
        i++;
    }

    return sum;
}

assert(!hasBasicBlockExecuted(forRegular, "var sum"), "should not have executed yet.");

forRegular(0);
assert(hasBasicBlockExecuted(forRegular, "var sum"), "should have executed.");
assert(!hasBasicBlockExecuted(forRegular, "sum += i"), "should not have executed yet.");

forRegular(1);
assert(hasBasicBlockExecuted(forRegular, "sum += i"), "should have executed.");


assert(!hasBasicBlockExecuted(forIn, "var s"), "should not have executed yet.");

forIn({});
assert(hasBasicBlockExecuted(forIn, "var s"), "should have executed.");
assert(!hasBasicBlockExecuted(forIn, "s += p"), "should not have executed yet.");

forIn({foo: "bar"});
assert(hasBasicBlockExecuted(forIn, "s += p"), "should have executed.");


assert(!hasBasicBlockExecuted(forOf, "var s"), "should not have executed yet.");

forOf([]);
assert(hasBasicBlockExecuted(forOf, "var s"), "should have executed.");
assert(!hasBasicBlockExecuted(forOf, "s += p"), "should not have executed yet.");

forOf(["a"]);
assert(hasBasicBlockExecuted(forOf, "s += p"), "should have executed.");


assert(!hasBasicBlockExecuted(whileLoop, "var sum"), "should not have executed yet.");

whileLoop(0);
assert(hasBasicBlockExecuted(whileLoop, "var sum"), "should have executed.");
assert(!hasBasicBlockExecuted(whileLoop, "sum += i"), "should not have executed yet.");

whileLoop(1);
assert(hasBasicBlockExecuted(whileLoop, "sum += i"), "should have executed.");


// The parts of the head of a loop run a different number of times than the statement.
var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

function count(func, expr, expected) {
    let actual = basicBlockExecutionCount(func, expr);
    assert(actual === expected, `"${expr}" in ${func.name} ran ${expected} times, not ${actual}`);
}

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
