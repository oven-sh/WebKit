var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { return 1; }
function bar() { return 2; }
function baz() { return 3; }

function testOr(x) {
    let r = x || foo();
    return r;
}
testOr(true);
checkBasicBlock(testOr, "x ||", ShouldHaveExecuted);
checkBasicBlock(testOr, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testOr, "return r", ShouldHaveExecuted);
testOr(false);
checkBasicBlock(testOr, "foo()", ShouldHaveExecuted);

function testAnd(x) {
    let r = x && foo();
    return r;
}
testAnd(false);
checkBasicBlock(testAnd, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testAnd, "return r", ShouldHaveExecuted);
testAnd(true);
checkBasicBlock(testAnd, "foo()", ShouldHaveExecuted);

function testCoalesce(x) {
    let r = x ?? foo();
    return r;
}
testCoalesce(0);
checkBasicBlock(testCoalesce, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testCoalesce, "return r", ShouldHaveExecuted);
testCoalesce(null);
checkBasicBlock(testCoalesce, "foo()", ShouldHaveExecuted);

function testCoalesceAfterOptionalChain(x) {
    return x?.y ?? foo();
}
testCoalesceAfterOptionalChain({ y: 1 });
checkBasicBlock(testCoalesceAfterOptionalChain, "foo()", ShouldNotHaveExecuted);
testCoalesceAfterOptionalChain(null);
checkBasicBlock(testCoalesceAfterOptionalChain, "foo()", ShouldHaveExecuted);

function testIgnoredResult(x) {
    x && foo();
    bar();
}
testIgnoredResult(false);
checkBasicBlock(testIgnoredResult, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testIgnoredResult, "bar()", ShouldHaveExecuted);

// The right operand is a binary expression, whose own location is that of `x`.
function testBinaryRightOperand(x) {
    return x || foo() + bar();
}
testBinaryRightOperand(true);
checkBasicBlock(testBinaryRightOperand, "x ||", ShouldHaveExecuted);
checkBasicBlock(testBinaryRightOperand, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testBinaryRightOperand, "bar()", ShouldNotHaveExecuted);

function testChain(x, y) {
    return x || y || foo();
}
testChain(false, true);
checkBasicBlock(testChain, "y ||", ShouldHaveExecuted);
checkBasicBlock(testChain, "foo()", ShouldNotHaveExecuted);

function testArgument(x) {
    return baz(x || foo(), bar());
}
testArgument(true);
checkBasicBlock(testArgument, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testArgument, "bar()", ShouldHaveExecuted);

function testCondition(x) {
    if (x && foo())
        bar();
    baz();
}
testCondition(false);
checkBasicBlock(testCondition, "x &&", ShouldHaveExecuted);
checkBasicBlock(testCondition, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testCondition, "bar()", ShouldNotHaveExecuted);
checkBasicBlock(testCondition, "baz()", ShouldHaveExecuted);
testCondition(true);
checkBasicBlock(testCondition, "foo()", ShouldHaveExecuted);
checkBasicBlock(testCondition, "bar()", ShouldHaveExecuted);

function testLoopCondition(n) {
    let i = 0;
    while (i < n && foo())
        i++;
    return i;
}
testLoopCondition(4);
assert(basicBlockExecutionCount(testLoopCondition, "foo()") === 4, "the right operand runs while the left one holds");
assert(basicBlockExecutionCount(testLoopCondition, "i++") === 4, "the body runs as often");
assert(basicBlockExecutionCount(testLoopCondition, "return i") === 1, "what follows the loop runs once");

function testConditionalInCondition(x, y) {
    if (x ? foo() : y)
        bar();
    baz();
}
testConditionalInCondition(false, false);
checkBasicBlock(testConditionalInCondition, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testConditionalInCondition, "y)", ShouldHaveExecuted);
checkBasicBlock(testConditionalInCondition, "bar()", ShouldNotHaveExecuted);
checkBasicBlock(testConditionalInCondition, "baz()", ShouldHaveExecuted);

function testElseInCondition(x) {
    if (x ? bar() : foo())
        baz();
}
testElseInCondition(true);
checkBasicBlock(testElseInCondition, "bar()", ShouldHaveExecuted);
checkBasicBlock(testElseInCondition, "foo()", ShouldNotHaveExecuted);

function testCoalesceInCondition(x) {
    if (x ?? foo())
        bar();
}
testCoalesceInCondition(true);
checkBasicBlock(testCoalesceInCondition, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testCoalesceInCondition, "bar()", ShouldHaveExecuted);

// The function has a basic block of its own, from where it starts to where it ends.
function testFunctionInConditional(x) {
    let f = x ? () => foo() : () => bar();
    f();
    f();
}
testFunctionInConditional(true);
assert(basicBlockExecutionCount(testFunctionInConditional, "foo()") === 2, "the function runs as often as it is called");
checkBasicBlock(testFunctionInConditional, ": ()", ShouldNotHaveExecuted);
checkBasicBlock(testFunctionInConditional, "bar()", ShouldNotHaveExecuted);

function testFunctionInPartOfConditional(x) {
    return x ? [() => foo(), bar()] : [() => foo(), baz()];
}
testFunctionInPartOfConditional(false);
checkBasicBlock(testFunctionInPartOfConditional, "bar()", ShouldNotHaveExecuted);
checkBasicBlock(testFunctionInPartOfConditional, "baz()", ShouldHaveExecuted);
checkBasicBlock(testFunctionInPartOfConditional, "return", ShouldHaveExecuted);

function testConditionalEnd(x) {
    return [x ? foo() : 2, foo()];
}
testConditionalEnd(true);
checkBasicBlock(testConditionalEnd, "2", ShouldNotHaveExecuted);
checkBasicBlock(testConditionalEnd, ", foo()]", ShouldHaveExecuted);

// The condition and the update of a loop are before its body in the text and after it in the bytecode.
function thrower() { throw new Error; }

function testBodyThatThrows(x) {
    while (x && foo()) {
        thrower();
    }
}
try { testBodyThatThrows(true); } catch { }
checkBasicBlock(testBodyThatThrows, "thrower()", ShouldHaveExecuted);

function testEndOfBodyThatThrows(x) {
    while (x ? foo() : 0) {
        if (x)
            bar();
        thrower();
    }
}
try { testEndOfBodyThatThrows(true); } catch { }
checkBasicBlock(testEndOfBodyThatThrows, "thrower()", ShouldHaveExecuted);

function testWhileContinue(x) {
    let i = 0;
    while (x && i < 3) {
        i++;
        if (i == 2)
            continue;
        baz();
    }
}
testWhileContinue(true);
assert(basicBlockExecutionCount(testWhileContinue, "baz()") === 2, "continue skips the end of the body of a while loop");

function testForContinue(x) {
    for (let i = 0; x && i < 3; i = x ? i + 1 : 0) {
        if (i == 1)
            continue;
        baz();
    }
}
testForContinue(true);
assert(basicBlockExecutionCount(testForContinue, "baz()") === 2, "continue skips the end of the body of a for loop");

// A loop has the bytecode of its condition twice.
function testConditionOfLaterIteration() {
    let i = 0;
    while (i++ < 1 || (i < 4 && bar())) { }
}
testConditionOfLaterIteration();
assert(basicBlockExecutionCount(testConditionOfLaterIteration, "bar()") === 2, "the operand runs in later iterations only");

// What skips an operand that has basic blocks does not skip what follows it.
let truthy = 1, falsy = 0, nullish = null;

function testInLogicalAssignment() {
    truthy ||= falsy || foo();
    truthy ||= falsy ? foo() : bar();
    falsy &&= falsy ?? foo();
    baz();
}
testInLogicalAssignment();
checkBasicBlock(testInLogicalAssignment, "baz()", ShouldHaveExecuted);

function testInOptionalChain() {
    nullish?.method(falsy || foo());
    nullish?.[falsy ?? foo()];
    baz();
}
testInOptionalChain();
checkBasicBlock(testInOptionalChain, "baz()", ShouldHaveExecuted);

function testInDefaultValue() {
    let [x = falsy && foo()] = [1];
    baz();
}
testInDefaultValue();
checkBasicBlock(testInDefaultValue, "baz()", ShouldHaveExecuted);

// The tests of a switch statement are all before its clauses in the bytecode.
function testInCaseTests(x) {
    switch (x) {
    case falsy || falsy || 1:
        foo();
        break;
    case 5 || 6:
        return bar();
    }
}
testInCaseTests(5);
checkBasicBlock(testInCaseTests, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testInCaseTests, "break", ShouldNotHaveExecuted);
checkBasicBlock(testInCaseTests, "return bar()", ShouldHaveExecuted);
