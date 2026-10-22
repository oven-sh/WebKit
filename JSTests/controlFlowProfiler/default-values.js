var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { return 1; }

function testParameter(a = foo(1), b = foo(2)) {
    return a + b;
}
testParameter(1);
checkBasicBlock(testParameter, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testParameter, "foo(2)", ShouldHaveExecuted);
checkBasicBlock(testParameter, "return a", ShouldHaveExecuted);
testParameter();
checkBasicBlock(testParameter, "foo(1)", ShouldHaveExecuted);

var testArrowFunctionParameter = (a = foo()) => a;
testArrowFunctionParameter(1);
checkBasicBlock(testArrowFunctionParameter, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testArrowFunctionParameter, "=> a", ShouldHaveExecuted);

function testPatternParameter({ a = foo(1) }, [b = foo(2)] = [foo(3)]) {
    return a + b;
}
testPatternParameter({ a: 1 }, [1]);
checkBasicBlock(testPatternParameter, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testPatternParameter, "foo(2)", ShouldNotHaveExecuted);
checkBasicBlock(testPatternParameter, "foo(3)", ShouldNotHaveExecuted);
checkBasicBlock(testPatternParameter, "return a", ShouldHaveExecuted);
testPatternParameter({ });
checkBasicBlock(testPatternParameter, "foo(1)", ShouldHaveExecuted);
checkBasicBlock(testPatternParameter, "foo(2)", ShouldNotHaveExecuted);
checkBasicBlock(testPatternParameter, "foo(3)", ShouldHaveExecuted);

function testDeclaration(o, l) {
    const { a = foo(1), b: { c = foo(2) } = { c: foo(3) } } = o;
    let [d = foo(4), e = foo(5)] = l;
    return a + c + d + e;
}
testDeclaration({ a: 1 }, [1]);
checkBasicBlock(testDeclaration, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testDeclaration, "foo(2)", ShouldNotHaveExecuted);
checkBasicBlock(testDeclaration, "foo(3)", ShouldHaveExecuted);
checkBasicBlock(testDeclaration, "foo(4)", ShouldNotHaveExecuted);
checkBasicBlock(testDeclaration, "foo(5)", ShouldHaveExecuted);
checkBasicBlock(testDeclaration, "return a", ShouldHaveExecuted);

function testAssignment(o, l) {
    let a, b;
    ({ a = foo(1) } = o);
    [b = foo(2)] = l;
    return a + b;
}
testAssignment({ a: 1 }, [1]);
checkBasicBlock(testAssignment, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testAssignment, "foo(2)", ShouldNotHaveExecuted);
checkBasicBlock(testAssignment, "return a", ShouldHaveExecuted);

function testForOfAndCatch(l) {
    for (const { a = foo(1) } of l)
        foo(2);
    try {
        throw { b: 1 };
    } catch ({ b = foo(3) }) {
        foo(4);
    }
}
testForOfAndCatch([{ a: 1 }]);
checkBasicBlock(testForOfAndCatch, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testForOfAndCatch, "foo(2)", ShouldHaveExecuted);
checkBasicBlock(testForOfAndCatch, "foo(3)", ShouldNotHaveExecuted);
checkBasicBlock(testForOfAndCatch, "foo(4)", ShouldHaveExecuted);

// An arrow function as the default value does not share a basic block with its own body.
function testArrowFunction(a = () => foo()) {
    return a;
}
var arrowFunction = testArrowFunction();
checkBasicBlock(testArrowFunction, "foo()", ShouldNotHaveExecuted);
arrowFunction();
arrowFunction();
assert(basicBlockExecutionCount(testArrowFunction, "foo()") === 2, "the arrow function ran twice");

function testArrowFunctionInAPattern({ a = () => foo() }) {
    return a;
}
arrowFunction = testArrowFunctionInAPattern({ });
checkBasicBlock(testArrowFunctionInAPattern, "foo()", ShouldNotHaveExecuted);
arrowFunction();
arrowFunction();
assert(basicBlockExecutionCount(testArrowFunctionInAPattern, "foo()") === 2, "the arrow function ran twice");

// The test of a `case` runs when it is compared, and its clause when it matches.
function testCaseTest(o) {
    let a;
    switch (2) {
    case { a = foo(1) } = o: foo(2);
    }
}
testCaseTest({ });
checkBasicBlock(testCaseTest, "foo(1)", ShouldHaveExecuted);
checkBasicBlock(testCaseTest, "foo(2)", ShouldNotHaveExecuted);

function testCount(a = foo()) {
    return a;
}
testCount();
testCount(1);
testCount(undefined);
assert(basicBlockExecutionCount(testCount, "foo()") === 2, "the default value runs when there is no argument");
assert(basicBlockExecutionCount(testCount, "return a") === 3, "the body runs each time");
