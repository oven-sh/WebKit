var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { }
function bar() { }

class Base { }

function testReturnInTryOfDerivedConstructor() {
    class Derived extends Base {
        constructor(x) {
            super();
            try {
                return x;
            } catch {
                foo();
            }
            bar();
        }
    }
    new Derived({ });
}
testReturnInTryOfDerivedConstructor();
checkBasicBlock(testReturnInTryOfDerivedConstructor, "return x", ShouldHaveExecuted);
checkBasicBlock(testReturnInTryOfDerivedConstructor, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testReturnInTryOfDerivedConstructor, "bar()", ShouldNotHaveExecuted);

function testSometimesReturnInTryOfDerivedConstructor() {
    class Derived extends Base {
        constructor(x) {
            super();
            try {
                if (x)
                    return x;
            } catch {
                foo();
            }
            bar();
        }
    }
    new Derived({ });
    new Derived({ });
    new Derived(null);
}
testSometimesReturnInTryOfDerivedConstructor();
assert(basicBlockExecutionCount(testSometimesReturnInTryOfDerivedConstructor, "return x") === 2, "two of three return");
assert(basicBlockExecutionCount(testSometimesReturnInTryOfDerivedConstructor, "bar()") === 1, "what follows the statement runs when nothing returns");
checkBasicBlock(testSometimesReturnInTryOfDerivedConstructor, "foo()", ShouldNotHaveExecuted);
