var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { return 1; }

// An expression that is the body of an arrow function is the end of it.
function testLastCharacter() {
    let f = () => foo();
    f();
    f();
}
testLastCharacter();
assert(basicBlockExecutionCount(testLastCharacter, "foo()") === 2, "the body runs per call");
assert(basicBlockExecutionCount(testLastCharacter, ");\n    f();") === 2, "and so does its last character");
