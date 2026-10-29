var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { }

function count(func, text, expected) {
    let actual = basicBlockExecutionCount(func, text);
    assert(actual === expected, `${func.name}: "${text}" ran ${actual} times, not ${expected}`);
}

// The parts of a loop header run a different number of times than the statement.
function whileHeader(limit, more) {
    let i = 0;
    while (i < limit && more)
        i++;
}
whileHeader(3, true);
count(whileHeader, "let i", 1);
count(whileHeader, "while", 1);
count(whileHeader, "i < limit", 4);
count(whileHeader, "&& more", 3);
count(whileHeader, "i++", 3);

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

function forHeader(limit, step) {
    for (let i = 0; i < limit || step; i += step ?? 1)
        limit;
}
forHeader(3, null);
count(forHeader, "for", 1);
count(forHeader, "let i", 1);
count(forHeader, "i < limit", 4);
count(forHeader, "step;", 1);
count(forHeader, "i +=", 3);
count(forHeader, "1)", 3);
count(forHeader, "limit;", 3);

function forWithoutACondition() {
    for (let i = 0; ; i++) {
        if (i === 2)
            break;
    }
}
forWithoutACondition();
count(forWithoutACondition, "i++", 2);

function forWithContinue(limit) {
    for (let i = 0; i < limit; i++) {
        if (i)
            continue;
    }
}
forWithContinue(3);
count(forWithContinue, "i < limit", 4);
count(forWithContinue, "i++", 3);

function forNeverReached() {
    return;
    for (let i = 0; i < 1; i++)
        ;
}
forNeverReached();
count(forNeverReached, "i < 1", 0);
count(forNeverReached, "i++", 0);

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

function* conditionWithAYield(limit) {
    let i = 0;
    while (yield i, i++ < limit)
        ;
}
for (let _ of conditionWithAYield(2))
    ;
count(conditionWithAYield, "yield i", 3);
count(conditionWithAYield, "i++ <", 3);

// Annex B: the initializer runs once, before the loop.
function forInInitializer(object) {
    for (var key = foo() in object)
        ;
}
forInInitializer({ });
count(forInInitializer, "foo()", 1);
forInInitializer({ a: 1, b: 2 });
count(forInInitializer, "foo()", 2);
