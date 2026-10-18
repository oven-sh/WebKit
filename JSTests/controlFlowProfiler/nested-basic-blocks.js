var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { }

function count(func, text, expected) {
    let actual = basicBlockExecutionCount(func, text);
    assert(actual === expected, `${func.name}: "${text}" ran ${actual} times, not ${expected}`);
}

// Nothing is after the last statement: what is left has the count of the basic block the statements are in.

function testReturnLast() {
    return 1;
    /*end*/
}
testReturnLast();
testReturnLast();
count(testReturnLast, "/*end*/", 2);

function testThrowLast() {
    throw 1;
    /*end*/
}
try {
    testThrowLast();
} catch { }
count(testThrowLast, "/*end*/", 1);

function testBreakLast() {
    for (;;) {
        break;
        /*body*/
    }
    /*end*/
}
testBreakLast();
count(testBreakLast, "/*body*/", 1);
count(testBreakLast, "/*end*/", 1);

function testContinueLast() {
    for (let i = 0; i < 3; i++) {
        continue;
        /*body*/
    }
    /*end*/
}
testContinueLast();
count(testContinueLast, "/*body*/", 3);
count(testContinueLast, "/*end*/", 1);

function testFunctionDeclarationAfterReturn() {
    return bar();
    /*end*/
    function bar() { }
}
testFunctionDeclarationAfterReturn();
count(testFunctionDeclarationAfterReturn, "/*end*/", 1);

function testDeadCode() {
    return 1;
    foo();
}
testDeadCode();
count(testDeadCode, "foo()", 0);

// A statement that is a part of another has its own text, and no more.

function testIfLast(x) {
    foo();
    if (x) {
        foo(1);
        /*then*/
    }
    /*end*/
}
testIfLast(false);
testIfLast(false);
count(testIfLast, "foo(1)", 0);
count(testIfLast, "/*then*/", 0);
count(testIfLast, "/*end*/", 2);

function testNestedIfLast(x, y) {
    if (x) {
        foo();
        if (y) {
            foo(1);
        }
        /*outer*/
    }
    /*end*/
}
testNestedIfLast(true, false);
count(testNestedIfLast, "foo(1)", 0);
count(testNestedIfLast, "/*outer*/", 1);
count(testNestedIfLast, "/*end*/", 1);

function testElseIf(x) {
    if (x === 1) {
        foo(1);
    } else if (x === 2) {
        foo(2);
    } else {
        foo(3);
        /*else*/
    }
    /*end*/
}
testElseIf(2);
testElseIf(3);
testElseIf(3);
count(testElseIf, "foo(1)", 0);
count(testElseIf, "x === 2", 3);
count(testElseIf, "foo(2)", 1);
count(testElseIf, "foo(3)", 2);
count(testElseIf, "/*else*/", 2);
count(testElseIf, "/*end*/", 3);

function testBraceOfElseIf(x) {
    if (x === 1) {
        foo(1);
    } else if (x === 2) {
        foo(2);
        foo(2);
    }
    foo(3);
}
testBraceOfElseIf(1);
count(testBraceOfElseIf, "}\n    foo(3)", 0);

function testBraceOfElseAfterElseIf(x) {
    if (x === 1) {
        foo(1);
    } else if (x === 2) {
        foo(2);
    } else {
        foo(3);
        foo(3);
    }
    foo(4);
}
testBraceOfElseAfterElseIf(1);
count(testBraceOfElseAfterElseIf, "}\n    foo(4)", 0);

function testFunctionDeclarationInIf(x) {
    if (x) function bar() { }/*after*/
    foo();
}
testFunctionDeclarationInIf(false);
count(testFunctionDeclarationInIf, "/*after*/", 1);

function testFunctionAtTheStartOfABasicBlock(x) {
    for (;;) {
        if (x) { break; }function inner() { }
        foo();
    }
}
testFunctionAtTheStartOfABasicBlock(true);
count(testFunctionAtTheStartOfABasicBlock, "inner", 0);
count(testFunctionAtTheStartOfABasicBlock, "foo()", 0);

function testFunctionThatIsABasicBlock(x) {
    if (x) y => y
    foo();
}
testFunctionThatIsABasicBlock(true);
count(testFunctionThatIsABasicBlock, "y => y", 0);
count(testFunctionThatIsABasicBlock, "foo()", 1);

function testEmptyBlock(x) {
    if (x) { }
    foo();
}
testEmptyBlock(false);
count(testEmptyBlock, "}", 0);
count(testEmptyBlock, "foo()", 1);

function testLoopsLast() {
    for (let i = 0; i < 0; i++) {
        foo(1);
    }
    /*end*/
}
testLoopsLast();
count(testLoopsLast, "foo(1)", 0);
count(testLoopsLast, "/*end*/", 1);

function testWhileLast() {
    let i = 0;
    while (i < 3) {
        i++;
    }
    /*end*/
}
testWhileLast();
count(testWhileLast, "i++", 3);
count(testWhileLast, "/*end*/", 1);

function testDoWhileLast() {
    let i = 0;
    do {
        i++;
    } while (i < 3);
    /*end*/
}
testDoWhileLast();
count(testDoWhileLast, "i++", 3);
count(testDoWhileLast, "/*end*/", 1);

function testForInLast() {
    for (let key in { a: 1, b: 2, c: 3 }) {
        foo(1);
    }
    /*end*/
}
testForInLast();
count(testForInLast, "foo(1)", 3);
count(testForInLast, "/*end*/", 1);

function testForOfLast() {
    for (let value of [1, 2, 3]) {
        foo(1);
    }
    /*end*/
}
testForOfLast();
count(testForOfLast, "foo(1)", 3);
count(testForOfLast, "/*end*/", 1);

function testSwitch(x) {
    switch (x) {
    case 1:
        return 1;
    case 2:
        foo(2);
        break;
    default:
        return 3;
        /*switch*/
    }
    /*end*/
}
testSwitch(1);
testSwitch(1);
testSwitch(3);
count(testSwitch, "return 1", 2);
count(testSwitch, "foo(2)", 0);
count(testSwitch, "return 3", 1);
count(testSwitch, "/*switch*/", 3);
count(testSwitch, "/*end*/", 3);

function testTry(x) {
    try {
        if (x)
            throw 1;
        foo(1);
    } catch {
        foo(2);
    } finally {
        foo(3);
        /*finally*/
    }
    /*end*/
}
testTry(false);
testTry(false);
testTry(true);
count(testTry, "foo(1)", 2);
count(testTry, "foo(2)", 1);
count(testTry, "foo(3)", 3);
count(testTry, "/*finally*/", 3);
count(testTry, "/*end*/", 3);

function* testFinallyOfAGenerator() {
    try {
        yield 1;
    } finally {
        foo();
    }
}
testFinallyOfAGenerator().next();
count(testFinallyOfAGenerator, "yield 1", 1);
count(testFinallyOfAGenerator, "foo()", 0);

// What is after a statement goes up to the end of the block, and up to the brace that closes a function.

function testAfterIf(x) {
    if (x) {
        return 1;
        /*then*/
    }
    foo();
    /*end*/
}
testAfterIf(true);
count(testAfterIf, "/*then*/", 1);
count(testAfterIf, "foo()", 0);
count(testAfterIf, "/*end*/", 0);

function testBraceOfAFunction(x) {
    if (x)
        return 1;
    foo();
}
testBraceOfAFunction(true);
count(testBraceOfAFunction, "foo()", 0);
count(testBraceOfAFunction, "}", 1);

function testAfterIfInABlock(x, y) {
    if (x) {
        if (y) {
            return 1;
        }
        foo(1);
        /*outer*/
    }
    foo(2);
}
testAfterIfInABlock(true, true);
testAfterIfInABlock(false, false);
count(testAfterIfInABlock, "foo(1)", 0);
count(testAfterIfInABlock, "/*outer*/", 0);
count(testAfterIfInABlock, "foo(2)", 1);

function testTwoIfs(x, y) {
    if (x) {
        return 1;
    }
    foo(1);
    if (y) {
        return 2;
    }
    foo(2);
}
testTwoIfs(false, true);
testTwoIfs(false, false);
testTwoIfs(true, false);
count(testTwoIfs, "return 1", 1);
count(testTwoIfs, "foo(1)", 2);
count(testTwoIfs, "if (y)", 2);
count(testTwoIfs, "return 2", 1);
count(testTwoIfs, "foo(2)", 1);

function testSwitchAfterIf(x, y) {
    if (x)
        return 1;
    switch (y) {
    case 1:
        if (y)
            foo(1);
        foo(2);
        /*switch*/
    }
    foo(3);
}
testSwitchAfterIf(true, 1);
testSwitchAfterIf(false, 1);
count(testSwitchAfterIf, "foo(2)", 1);
count(testSwitchAfterIf, "/*switch*/", 1);
count(testSwitchAfterIf, "foo(3)", 1);

function testPartOfAnExpressionAfterIf(x) {
    if (x) {
        return 1;
    }
    let a = x || foo(1);
    foo(2);
}
testPartOfAnExpressionAfterIf(true);
count(testPartOfAnExpressionAfterIf, "let a", 0);
count(testPartOfAnExpressionAfterIf, "foo(1)", 0);
count(testPartOfAnExpressionAfterIf, "foo(2)", 0);

// Control flow can leave a block, a with statement and a labelled statement.

function testBlock(x) {
    {
        if (x) {
            return 1;
        }
        foo(1);
    }
    foo(2);
}
testBlock(true);
testBlock(false);
count(testBlock, "foo(1)", 1);
count(testBlock, "foo(2)", 1);

function testBlockThatReturns() {
    {
        return 1;
    }
    foo();
}
testBlockThatReturns();
count(testBlockThatReturns, "foo()", 0);

function testWith(x) {
    with ({ }) {
        if (x)
            return 1;
    }
    foo();
}
testWith(true);
testWith(false);
count(testWith, "foo()", 1);

function testLabel() {
    outer: for (;;) {
        for (;;) {
            break outer;
        }
        foo(1);
    }
    foo(2);
}
testLabel();
count(testLabel, "foo(1)", 0);
count(testLabel, "foo(2)", 1);

function testReturnInLabel(x) {
    label: {
        if (x)
            return 1;
    }
    foo();
}
testReturnInLabel(true);
count(testReturnInLabel, "foo()", 0);

function testReturnInTryFinally(x) {
    try {
        if (x)
            return 1;
    } finally {
        foo(1);
    }
    foo(2);
}
testReturnInTryFinally(true);
testReturnInTryFinally(false);
count(testReturnInTryFinally, "foo(1)", 2);
count(testReturnInTryFinally, "foo(2)", 1);

function testReturnInTryFinallyLast(x) {
    try {
        if (x)
            return 1;
    } finally {
        foo();
    }
    /*end*/
}
testReturnInTryFinallyLast(true);
testReturnInTryFinallyLast(false);
count(testReturnInTryFinallyLast, "/*end*/", 2);

function testReturnInWhile() {
    while (true)
        return 1;
    foo();
}
testReturnInWhile();
count(testReturnInWhile, "foo()", 0);
count(testReturnInWhile, "}", 1);

function testReturnInDoWhile() {
    do
        return 1;
    while (true);
    foo();
}
testReturnInDoWhile();
count(testReturnInDoWhile, "foo()", 0);
count(testReturnInDoWhile, "}", 1);

function testReturnInFor() {
    for (;;)
        return 1;
    foo();
}
testReturnInFor();
count(testReturnInFor, "foo()", 0);
count(testReturnInFor, "}", 1);

function testReturnInForIn(object) {
    for (let key in object)
        return 1;
    foo();
}
testReturnInForIn({ a: 1 });
count(testReturnInForIn, "foo()", 0);
count(testReturnInForIn, "}", 1);

function testReturnInForOf() {
    for (let value of [1])
        return 1;
    foo();
}
testReturnInForOf();
count(testReturnInForOf, "foo()", 0);
count(testReturnInForOf, "}", 1);

// What is after a loop runs once.

function testAfterLoops() {
    for (let key in { a: 1, b: 2, c: 3 }) {
        foo(1);
    }
    foo(2);
    for (let value of [1, 2, 3]) {
        foo(3);
    }
    foo(4);
    let i = 0;
    while (i < 3) {
        i++;
    }
    foo(5);
}
testAfterLoops();
count(testAfterLoops, "foo(1)", 3);
count(testAfterLoops, "foo(2)", 1);
count(testAfterLoops, "foo(3)", 3);
count(testAfterLoops, "foo(4)", 1);
count(testAfterLoops, "i++", 3);
count(testAfterLoops, "foo(5)", 1);
