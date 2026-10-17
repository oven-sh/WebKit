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
