load("./driver/driver.js");

function foo() { }

// Calling a generator does not run its body.
function* neverResumed() {
    foo();
}
neverResumed();
checkBasicBlock(neverResumed, "foo()", ShouldNotHaveExecuted);

function* resumed() {
    foo();
}
resumed().next();
checkBasicBlock(resumed, "foo()", ShouldHaveExecuted);
