var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { }

function testBreakNotTaken(n) {
    for (let i = 0; i < n; i++) {
        if (i > n)
            break;
        foo();
    }
}
testBreakNotTaken(3);
checkBasicBlock(testBreakNotTaken, "break", ShouldNotHaveExecuted);
assert(basicBlockExecutionCount(testBreakNotTaken, "foo()") === 3, "what follows the if runs each time");

function testBreakTaken(n) {
    for (let i = 0; i < n; i++) {
        if (i === 1)
            break;
        foo();
    }
}
testBreakTaken(3);
assert(basicBlockExecutionCount(testBreakTaken, "break") === 1, "break runs once");
assert(basicBlockExecutionCount(testBreakTaken, "foo()") === 1, "what follows the if runs until then");

function testContinue(n) {
    for (let i = 0; i < n; i++) {
        if (i % 2)
            continue;
        foo();
    }
}
testContinue(5);
assert(basicBlockExecutionCount(testContinue, "continue") === 2, "continue runs for the odd ones");
assert(basicBlockExecutionCount(testContinue, "foo()") === 3, "what follows the if runs for the even ones");

function testContinueNotTaken(n) {
    for (let i = 0; i < n; i++) {
        if (i > n) {
            continue;
        }
        foo();
    }
}
testContinueNotTaken(2);
checkBasicBlock(testContinueNotTaken, "continue", ShouldNotHaveExecuted);
checkBasicBlock(testContinueNotTaken, "foo()", ShouldHaveExecuted);
