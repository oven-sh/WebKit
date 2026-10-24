//@ requireOptions("--useConcurrentJIT=false")

// The legacy static properties of the RegExp constructor are custom accessors. Each one throws a TypeError unless
// |this| is the RegExp constructor of the realm it is called with, so a call with the wrong realm shows without $vm.
// custom-accessor-realm-of-inlined-code-origin.js says why the DFG and the FTL made that call: they pass the realm of
// the code origin, and the guard that allows it looked at the realm of the function that inlined the code origin.
//
// The concurrent JIT is off so that every tier-up happens inside testLoopCount iterations.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${actual}, expected ${expected}`);
}

const guest = runString("");

/(host)/.test("host");

// One level of inlining: a function of this realm inlines a guest function that reads RegExp.$1 of this realm.
(function () {
    const guestRead = guest.eval("(function (constructor) { return constructor.$1; })");
    const host = constructor => guestRead(constructor);
    noInline(host);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(host(RegExp), "host");
})();

// Two levels: the middle function is in this realm and inlines the guest reader. It is compiled on its own, so its
// optimized code block holds the inline cache that the outer function reads the status from. This needs no FTL.
(function () {
    const guestRead = guest.eval("(function (constructor) { return constructor.$1; })");
    const middle = constructor => guestRead(constructor);
    const warm = () => {
        for (let i = 0; i < testLoopCount; ++i)
            shouldBe(middle(RegExp), "host");
    };
    const outer = () => {
        for (let i = 0; i < testLoopCount; ++i)
            shouldBe(middle(RegExp), "host");
    };
    noInline(warm);
    noInline(outer);
    warm();
    outer();
})();

// The setter twin: a guest function that writes RegExp.input of this realm.
(function () {
    const guestWrite = guest.eval("(function (constructor, value) { constructor.input = value; })");
    const host = (constructor, value) => guestWrite(constructor, value);
    noInline(host);
    for (let i = 0; i < testLoopCount; ++i) {
        const value = "input" + (i & 3);
        host(RegExp, value);
        shouldBe(RegExp.input, value);
    }
})();

// The direction reversed: the RegExp constructor of the guest realm, read by a function of this realm that a guest
// function inlines.
(function () {
    const hostRead = function (constructor) { return constructor.$1; };
    const firstWrong = guest.eval(`
        (hostRead => {
            /(guest)/.test("guest");
            const caller = constructor => hostRead(constructor);
            noInline(caller);
            for (let i = 0; i < testLoopCount; ++i) {
                if (caller(RegExp) !== "guest")
                    return i;
            }
            return -1;
        });
    `)(hostRead);
    shouldBe(firstWrong, -1);
})();
