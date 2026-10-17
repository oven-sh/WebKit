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

// All of it is one basic block, whose range is the one the function that initializes the fields has.
function testScopeOfOneBlock() {
    class A {
        a = foo();
    }
    new A;
    new A;
    new A;
}
testScopeOfOneBlock();
testScopeOfOneBlock();
assert(basicBlockExecutionCount(testScopeOfOneBlock, "class A") === 2, "the class is evaluated once per call");
assert(basicBlockExecutionCount(testScopeOfOneBlock, "a = foo") === 6, "a field is initialized once per instance");
assert(basicBlockExecutionCount(testScopeOfOneBlock, "new A") === 2, "what follows the class runs once per call");

function testParenthesizedField() {
    class A {
        a = (foo());
    }
    new A;
}
testParenthesizedField();
checkBasicBlock(testParenthesizedField, "foo()", ShouldHaveExecuted);
checkBasicBlock(testParenthesizedField, ");", ShouldHaveExecuted);

function testFunctionInComputedField() {
    class A {
        [key()] = () => foo();
    }
    new A;
    let a = new A;
    a.k();
    a.k();
    a.k();
}
testFunctionInComputedField();
assert(basicBlockExecutionCount(testFunctionInComputedField, "foo()") === 3, "the function runs as often as it is called");
assert(basicBlockExecutionCount(testFunctionInComputedField, " () =>") === 2, "the field is initialized once per instance");
assert(basicBlockExecutionCount(testFunctionInComputedField, "key()") === 1, "the key is evaluated once");

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
checkBasicBlock(testStaticBlock, "static {", ShouldHaveExecuted);
checkBasicBlock(testStaticBlock, "if (x)", ShouldHaveExecuted);
checkBasicBlock(testStaticBlock, "foo()", ShouldHaveExecuted);
checkBasicBlock(testStaticBlock, "bar()", ShouldNotHaveExecuted);
checkBasicBlock(testStaticBlock, "return A", ShouldHaveExecuted);

// What follows a field in the file is not the field, however far it is.
function testStaticElementsAfterThrow() {
    try {
        class A {
            static a = (() => { throw new Error; })();
            static b = bar();
            static {
                foo();
            }
        }
    } catch { }
}
testStaticElementsAfterThrow();
checkBasicBlock(testStaticElementsAfterThrow, "throw new", ShouldHaveExecuted);
checkBasicBlock(testStaticElementsAfterThrow, "bar()", ShouldNotHaveExecuted);
checkBasicBlock(testStaticElementsAfterThrow, "foo()", ShouldNotHaveExecuted);

function testFarFromField() {
    class A {
        a = foo();
    }
    new A;
    new A;
    function never() {
        // ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        bar();
    }
    return A;
}
testFarFromField();
checkBasicBlock(testFarFromField, "bar()", ShouldNotHaveExecuted);
assert(basicBlockExecutionCount(testFarFromField, "return A") === 1, "what follows the class runs once");

function testFieldAfterAccessor() {
    class A {
        get b() { return 1; }
        a = foo();
    }
    return A;
}
testFieldAfterAccessor();
checkBasicBlock(testFieldAfterAccessor, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testFieldAfterAccessor, "return A", ShouldHaveExecuted);

function testSkippedClass(x) {
    x ??= class { a = foo(); };
    bar();
}
testSkippedClass(1);
checkBasicBlock(testSkippedClass, "bar()", ShouldHaveExecuted);

function testStaticFieldOfSkippedClass(x) {
    x ??= class { static a = foo(); };
    bar();
}
testStaticFieldOfSkippedClass(1);
checkBasicBlock(testStaticFieldOfSkippedClass, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testStaticFieldOfSkippedClass, "bar()", ShouldHaveExecuted);

function testStaticFieldAfterAccessorOfSkippedClass(x) {
    x ??= class { get a() { return 1; } static b = foo(); };
    bar();
}
testStaticFieldAfterAccessorOfSkippedClass(1);
checkBasicBlock(testStaticFieldAfterAccessorOfSkippedClass, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testStaticFieldAfterAccessorOfSkippedClass, "bar()", ShouldHaveExecuted);
