var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { return "key"; }
var object = { nothing: null, key: 1, method() { return object; } };
object.self = object;

function testMember(x) {
    let r = x?.self.method(foo());
    return r;
}
testMember(null);
checkBasicBlock(testMember, "x?.", ShouldHaveExecuted);
checkBasicBlock(testMember, "self", ShouldNotHaveExecuted);
checkBasicBlock(testMember, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testMember, "return r", ShouldHaveExecuted);
testMember(object);
checkBasicBlock(testMember, "self", ShouldHaveExecuted);
checkBasicBlock(testMember, "foo()", ShouldHaveExecuted);

function testSubscript(x) {
    let r = x?.[foo()];
    return r;
}
testSubscript(undefined);
checkBasicBlock(testSubscript, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testSubscript, "return r", ShouldHaveExecuted);
testSubscript(object);
checkBasicBlock(testSubscript, "foo()", ShouldHaveExecuted);

function testCall(f) {
    let r = f?.(foo());
    return r;
}
testCall(null);
checkBasicBlock(testCall, "?.(", ShouldNotHaveExecuted);
checkBasicBlock(testCall, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testCall, "return r", ShouldHaveExecuted);
testCall(foo);
checkBasicBlock(testCall, "foo()", ShouldHaveExecuted);

function testMethodCall(x) {
    return x.nothing?.(foo());
}
testMethodCall(object);
checkBasicBlock(testMethodCall, "x.nothing", ShouldHaveExecuted);
checkBasicBlock(testMethodCall, "foo()", ShouldNotHaveExecuted);

function testSubscriptCall(x) {
    return x["nothing"]?.(foo());
}
testSubscriptCall(object);
checkBasicBlock(testSubscriptCall, "foo()", ShouldNotHaveExecuted);

// Each `?.` skips what is left of the chain.
function testSecondLink(x) {
    let r = x?.method(1).nothing?.method(foo()).self;
    return r;
}
testSecondLink(object);
checkBasicBlock(testSecondLink, "method(1)", ShouldHaveExecuted);
checkBasicBlock(testSecondLink, "nothing", ShouldHaveExecuted);
checkBasicBlock(testSecondLink, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testSecondLink, "self", ShouldNotHaveExecuted);
checkBasicBlock(testSecondLink, "return r", ShouldHaveExecuted);

function testChainInChain(x, y) {
    return x?.method(y?.method(foo()), 2);
}
testChainInChain(object, null);
checkBasicBlock(testChainInChain, "y?.", ShouldHaveExecuted);
checkBasicBlock(testChainInChain, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testChainInChain, ", 2", ShouldHaveExecuted);

function testCallAndApply(x) {
    x?.method.call(x, foo(1));
    x?.method.apply(x, [foo(2)]);
    object.nothing?.call(x, foo(3));
    object.nothing?.apply(x, [foo(4)]);
    return x;
}
testCallAndApply(null);
checkBasicBlock(testCallAndApply, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testCallAndApply, "foo(2)", ShouldNotHaveExecuted);
checkBasicBlock(testCallAndApply, "foo(3)", ShouldNotHaveExecuted);
checkBasicBlock(testCallAndApply, "foo(4)", ShouldNotHaveExecuted);
checkBasicBlock(testCallAndApply, "return x", ShouldHaveExecuted);
testCallAndApply(object);
checkBasicBlock(testCallAndApply, "foo(1)", ShouldHaveExecuted);
checkBasicBlock(testCallAndApply, "foo(2)", ShouldHaveExecuted);
checkBasicBlock(testCallAndApply, "foo(3)", ShouldNotHaveExecuted);

// Calls with their own node types.
function testKindsOfCall(x, o, Reflect, skipped1, skipped2) {
    x?.hasOwnProperty(skipped1);
    o.hasOwnProperty?.(skipped2);
    o.call?.(x, foo(1));
    o.apply?.(x, [foo(2)]);
    (0, x)?.(foo(3));
    Reflect?.construct(Object, [foo(4)]);
    Reflect = { };
    Reflect.construct?.(Object, [foo(5)]);
    x?.["method"](foo(6));
    return x;
}
testKindsOfCall(null, { hasOwnProperty: null }, null);
checkBasicBlock(testKindsOfCall, "(skipped1", ShouldNotHaveExecuted);
checkBasicBlock(testKindsOfCall, "(skipped2", ShouldNotHaveExecuted);
for (let i = 1; i <= 6; ++i)
    checkBasicBlock(testKindsOfCall, "foo(" + i + ")", ShouldNotHaveExecuted);
checkBasicBlock(testKindsOfCall, "return x", ShouldHaveExecuted);

function testDelete(x) {
    let r = delete x?.[foo()];
    delete x?.skipped;
    return r;
}
testDelete(null);
checkBasicBlock(testDelete, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testDelete, "skipped", ShouldNotHaveExecuted);
checkBasicBlock(testDelete, "return r", ShouldHaveExecuted);

function testParenthesizedCallee(x) {
    try {
        (x?.[foo()])();
    } catch { }
    try {
        (x?.skipped)();
    } catch { }
    return x;
}
testParenthesizedCallee(null);
checkBasicBlock(testParenthesizedCallee, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testParenthesizedCallee, "skipped", ShouldNotHaveExecuted);
checkBasicBlock(testParenthesizedCallee, "return x", ShouldHaveExecuted);

function testCoalesce(x) {
    return x?.[foo(1)] ?? foo(2);
}
testCoalesce(null);
checkBasicBlock(testCoalesce, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testCoalesce, "foo(2)", ShouldHaveExecuted);

function testCondition(x) {
    if (x?.[foo(1)])
        foo(2);
    if (x?.[foo(3)] ?? true)
        foo(4);
    return x;
}
testCondition(null);
checkBasicBlock(testCondition, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testCondition, "foo(2)", ShouldNotHaveExecuted);
checkBasicBlock(testCondition, "foo(3)", ShouldNotHaveExecuted);
checkBasicBlock(testCondition, "foo(4)", ShouldHaveExecuted);
checkBasicBlock(testCondition, "return x", ShouldHaveExecuted);

// The basic block ends with the chain.
function testPropertyValue(x) {
    return [{ a: x?.[foo(1)] , b: 1 }, { a: x?.[foo(2)] /* last */ }];
}
testPropertyValue(null);
checkBasicBlock(testPropertyValue, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testPropertyValue, ", b", ShouldHaveExecuted);
checkBasicBlock(testPropertyValue, "foo(2)", ShouldNotHaveExecuted);
checkBasicBlock(testPropertyValue, "/* last */", ShouldHaveExecuted);

function testParentheses(x) {
    return (x?.[foo()] /* last */);
}
testParentheses(null);
checkBasicBlock(testParentheses, "foo()", ShouldNotHaveExecuted);
checkBasicBlock(testParentheses, "/* last */", ShouldHaveExecuted);

function testParenthesesAndCoalesce(x) {
    if ((x?.[foo(1)] /* first */) ?? true)
        foo(2);
    return (x?.[foo(3)] /* second */) ?? foo(4);
}
testParenthesesAndCoalesce(null);
checkBasicBlock(testParenthesesAndCoalesce, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testParenthesesAndCoalesce, "/* first */", ShouldHaveExecuted);
checkBasicBlock(testParenthesesAndCoalesce, "foo(3)", ShouldNotHaveExecuted);
checkBasicBlock(testParenthesesAndCoalesce, "/* second */", ShouldHaveExecuted);
checkBasicBlock(testParenthesesAndCoalesce, "foo(4)", ShouldHaveExecuted);

function testCount(x) {
    return x?.[foo()];
}
testCount(null);
testCount(object);
testCount(null);
testCount(object);
testCount(object);
assert(basicBlockExecutionCount(testCount, "x?.") === 5, "the base runs each time");
assert(basicBlockExecutionCount(testCount, "foo()") === 3, "the rest runs when the base is an object");

// No basic block in the test of a `case`: its text belongs to the clause.
function testCaseTest(x) {
    switch (2) {
    case x?.[foo(1)]: foo(2);
    }
}
testCaseTest(object);
checkBasicBlock(testCaseTest, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testCaseTest, "foo(2)", ShouldNotHaveExecuted);
