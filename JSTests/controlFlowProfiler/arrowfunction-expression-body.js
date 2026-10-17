var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { return 1; }

// Nothing of an arrow function follows the expression that is its body.
function testLastCharacter() {
    let f = () => foo();
    f();
    f();
}
testLastCharacter();
assert(basicBlockExecutionCount(testLastCharacter, "foo()") === 2, "the body runs per call");
assert(basicBlockExecutionCount(testLastCharacter, ");\n    f();") === 2, "and so does its last character");

function testConditionalEnd(x) {
    return [x ? foo() : 2, foo()];
}
testConditionalEnd(true);
checkBasicBlock(testConditionalEnd, "2", ShouldNotHaveExecuted);
checkBasicBlock(testConditionalEnd, ", foo()]", ShouldHaveExecuted);
