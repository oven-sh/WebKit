var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { return 1; }
function bar() { return 2; }
function key() { return "k"; }

function testNeverConstructed() {
    class A {
        a = foo();
        static b = bar();
    }
    return A;
}
testNeverConstructed();
checkBasicBlock(testNeverConstructed, "class A", ShouldHaveExecuted);
checkBasicBlock(testNeverConstructed, "a = foo", ShouldNotHaveExecuted);
checkBasicBlock(testNeverConstructed, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testNeverConstructed, "static b", ShouldHaveExecuted);
checkBasicBlock(testNeverConstructed, "bar()", ShouldHaveExecuted);
checkBasicBlock(testNeverConstructed, "return A", ShouldHaveExecuted);

function testCounts(times) {
    class A {
        a = foo();
        [key()] = bar();
        static c = foo();
    }
    for (let i = 0; i < times; i++)
        new A;
}
testCounts(3);
assert(basicBlockExecutionCount(testCounts, "class A") === 1, "the class is evaluated once");
assert(basicBlockExecutionCount(testCounts, "a = foo") === 3, "a field is initialized once per instance");
assert(basicBlockExecutionCount(testCounts, "key()") === 1, "a computed key is evaluated once");
assert(basicBlockExecutionCount(testCounts, "bar()") === 3, "the value of a computed field is evaluated once per instance");
assert(basicBlockExecutionCount(testCounts, "static c") === 1, "a static field is initialized once");
assert(basicBlockExecutionCount(testCounts, "for (") === 1, "what follows the class runs once");

function testBranchInField(x) {
    class A {
        a = x ? foo() : bar();
    }
    return new A;
}
testBranchInField(true);
checkBasicBlock(testBranchInField, "foo()", ShouldHaveExecuted);
checkBasicBlock(testBranchInField, "bar()", ShouldNotHaveExecuted);
checkBasicBlock(testBranchInField, "return new A", ShouldHaveExecuted);

function testStaticBlock(x) {
    class A {
        static {
            if (x)
                foo();
            else
                bar();
        }
    }
    return A;
}
testStaticBlock(true);
checkBasicBlock(testStaticBlock, "if (x)", ShouldHaveExecuted);
checkBasicBlock(testStaticBlock, "foo()", ShouldHaveExecuted);
checkBasicBlock(testStaticBlock, "bar()", ShouldNotHaveExecuted);
checkBasicBlock(testStaticBlock, "return A", ShouldHaveExecuted);
