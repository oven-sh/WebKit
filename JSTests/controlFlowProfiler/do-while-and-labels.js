var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { }
function bar() { }

function testDoWhile(n) {
    let i = 0;
    do {
        foo();
        i++;
    } while (i < n);
    bar();
}
testDoWhile(3);
assert(basicBlockExecutionCount(testDoWhile, "let i") === 1, "what precedes the loop runs once");
assert(basicBlockExecutionCount(testDoWhile, "foo()") === 3, "the body runs each time");
assert(basicBlockExecutionCount(testDoWhile, "i < n") === 3, "the condition runs each time");
assert(basicBlockExecutionCount(testDoWhile, "bar()") === 1, "what follows the loop runs once");

function testDoWhileBreak() {
    do {
        foo();
        if (true)
            break;
        bar();
    } while (true);
    foo();
}
testDoWhileBreak();
checkBasicBlock(testDoWhileBreak, "bar()", ShouldNotHaveExecuted);
checkBasicBlock(testDoWhileBreak, "while (true)", ShouldNotHaveExecuted);
checkBasicBlock(testDoWhileBreak, "foo();\n}", ShouldHaveExecuted);

function testDoWhileContinue(x) {
    do {
        if (x)
            continue;
        foo();
    } while (bar());
}
testDoWhileContinue(true);
checkBasicBlock(testDoWhileContinue, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testDoWhileContinue, "bar()", ShouldHaveExecuted);

function testLabeledBlock(x) {
    label: {
        if (x)
            break label;
        foo();
    }
    bar();
}
testLabeledBlock(true);
checkBasicBlock(testLabeledBlock, "break label", ShouldHaveExecuted);
checkBasicBlock(testLabeledBlock, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testLabeledBlock, "bar()", ShouldHaveExecuted);

function testNestedLabels(x) {
    outer: inner: for (;;) {
        if (x)
            break outer;
        foo();
        break inner;
    }
    bar();
}
testNestedLabels(true);
checkBasicBlock(testNestedLabels, "break outer", ShouldHaveExecuted);
checkBasicBlock(testNestedLabels, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testNestedLabels, "bar()", ShouldHaveExecuted);

function testLabeledFunction() {
    label: function labeled() { }
    bar();
}
testLabeledFunction();
checkBasicBlock(testLabeledFunction, "bar()", ShouldHaveExecuted);
