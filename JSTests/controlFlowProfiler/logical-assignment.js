var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { return 1; }
var global = 1;

function testOr(x) {
    x ||= foo();
    return x;
}
testOr(1);
checkBasicBlock(testOr, "x ||=", ShouldHaveExecuted);
checkBasicBlock(testOr, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testOr, "return x", ShouldHaveExecuted);
testOr(0);
checkBasicBlock(testOr, "foo()", ShouldHaveExecuted);

function testAnd(x) {
    x &&= foo();
    return x;
}
testAnd(0);
checkBasicBlock(testAnd, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testAnd, "return x", ShouldHaveExecuted);
testAnd(1);
checkBasicBlock(testAnd, "foo()", ShouldHaveExecuted);

function testCoalesce(x) {
    x ??= foo();
    return x;
}
testCoalesce(0);
checkBasicBlock(testCoalesce, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testCoalesce, "return x", ShouldHaveExecuted);
testCoalesce(null);
checkBasicBlock(testCoalesce, "foo()", ShouldHaveExecuted);

function testMember(o) {
    o.a ||= foo(1);
    o["a"] &&= foo(2);
    o.b ??= foo(3);
    return o;
}
testMember({ a: 0, b: 0 });
checkBasicBlock(testMember, "foo(1)", ShouldHaveExecuted);
checkBasicBlock(testMember, "foo(2)", ShouldHaveExecuted);
checkBasicBlock(testMember, "foo(3)", ShouldNotHaveExecuted);
checkBasicBlock(testMember, "return o", ShouldHaveExecuted);

function testSubscript(o) {
    o[foo(1)] ||= foo(2);
    return o;
}
testSubscript([0, 1]);
checkBasicBlock(testSubscript, "foo(1)", ShouldHaveExecuted);
checkBasicBlock(testSubscript, "foo(2)", ShouldNotHaveExecuted);

function testKindsOfVariable() {
    let captured = 1;
    const constant = 1;
    let assignedOnTheRight = 1;
    (() => captured);
    captured ||= foo(1);
    constant ||= foo(2);
    global ||= foo(3);
    assignedOnTheRight ||= (assignedOnTheRight = foo(4));
    return captured;
}
testKindsOfVariable();
checkBasicBlock(testKindsOfVariable, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testKindsOfVariable, "foo(2)", ShouldNotHaveExecuted);
checkBasicBlock(testKindsOfVariable, "foo(3)", ShouldNotHaveExecuted);
checkBasicBlock(testKindsOfVariable, "foo(4)", ShouldNotHaveExecuted);
checkBasicBlock(testKindsOfVariable, "return captured", ShouldHaveExecuted);

function testChain(x, y) {
    x ||= y &&= foo(1) + foo(2);
    return x;
}
testChain(0, 0);
checkBasicBlock(testChain, "y &&=", ShouldHaveExecuted);
checkBasicBlock(testChain, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testChain, "foo(2)", ShouldNotHaveExecuted);
checkBasicBlock(testChain, "return x", ShouldHaveExecuted);

// The basic block of the arrow function is not the one of the right side.
function testArrowFunction(x) {
    x ||= () => foo();
    return x;
}
var arrowFunction = testArrowFunction(0);
checkBasicBlock(testArrowFunction, "foo()", ShouldNotHaveExecuted);
arrowFunction();
arrowFunction();
assert(basicBlockExecutionCount(testArrowFunction, "foo()") === 2, "the arrow function ran twice");

// Not in the test of a `case`.
function testCaseTest(x) {
    switch (2) {
    case x ||= foo(1): foo(2);
    }
}
testCaseTest(0);
checkBasicBlock(testCaseTest, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testCaseTest, "foo(2)", ShouldNotHaveExecuted);

function testCount(x) {
    x ||= foo();
    return x;
}
testCount(0);
testCount(1);
testCount(0);
assert(basicBlockExecutionCount(testCount, "x ||=") === 3, "the left side runs each time");
assert(basicBlockExecutionCount(testCount, "foo()") === 2, "the right side runs when the left side is falsy");
assert(basicBlockExecutionCount(testCount, "return x") === 3, "what is after it runs each time");
