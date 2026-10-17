var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { }
function bar() { }

function* neverResumed() {
    foo();
}
neverResumed();
checkBasicBlock(neverResumed, "foo()", ShouldNotHaveExecuted);

function* resumedOnce() {
    foo();
    yield 1;
    bar();
}
resumedOnce().next();
checkBasicBlock(resumedOnce, "foo()", ShouldHaveExecuted);
checkBasicBlock(resumedOnce, "yield 1", ShouldHaveExecuted);
checkBasicBlock(resumedOnce, "bar()", ShouldNotHaveExecuted);

function* resumedTwice() {
    foo();
    let x = yield 1;
    bar(x);
}
{
    let generator = resumedTwice();
    generator.next();
    generator.next();
}
checkBasicBlock(resumedTwice, "bar(x)", ShouldHaveExecuted);

function* delegates() {
    yield* [1, 2];
    bar();
}
delegates().next();
checkBasicBlock(delegates, "yield*", ShouldHaveExecuted);
checkBasicBlock(delegates, "bar()", ShouldNotHaveExecuted);

async function neverSettles() {
    foo();
    await new Promise(function() { });
    bar();
}
neverSettles();
drainMicrotasks();
checkBasicBlock(neverSettles, "foo()", ShouldHaveExecuted);
checkBasicBlock(neverSettles, "await new", ShouldHaveExecuted);
checkBasicBlock(neverSettles, "bar()", ShouldNotHaveExecuted);

async function settles() {
    await 1;
    bar();
}
settles();
drainMicrotasks();
checkBasicBlock(settles, "bar()", ShouldHaveExecuted);
