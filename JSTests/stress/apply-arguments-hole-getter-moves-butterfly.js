// Function.prototype.apply reads the argument list one index at a time. A hole sends that read
// to the prototype chain, where a getter can run. The getter can grow the arguments object,
// which moves its butterfly, and can store new values into it. The copy must read what the
// getter left behind, not the butterfly that the object had before the getter ran.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: expected ${expected} but got ${actual}`);
}

function makeArguments() {
    "use strict";
    return arguments;
}

// A JSGlobalProxy hides the indexed accessor of its target from the prototype chain walk that
// gives an object SlowPutArrayStorage. So `args` keeps its contiguous butterfly and the copy
// takes the path that reads that butterfly directly.
const target = $vm.createGlobalObject();
const proxy = $vm.createGlobalProxy(target);

let getterCalls = 0;
Object.defineProperty(target, "0", {
    configurable: true,
    get() {
        getterCalls++;
        const args = argumentsUnderTest;
        for (let i = 4; i < 2000; i++)
            args[i] = i; // grows the butterfly of args, which moves it
        args[1] = "one";
        args[2] = "two";
        args[3] = "three";
        return "zero";
    },
});

let argumentsUnderTest = null;

function check(call) {
    const beforeTheGetter = { tag: "before the getter" };
    argumentsUnderTest = makeArguments(0, beforeTheGetter, 2, 3);
    delete argumentsUnderTest[0];
    Object.setPrototypeOf(argumentsUnderTest, proxy);

    const calls = getterCalls;
    const seen = call(function (a, b, c, d) { return [a, b, c, d]; }, argumentsUnderTest);
    shouldBe(getterCalls, calls + 1);
    shouldBe(seen[0], "zero");
    shouldBe(seen[1], "one");
    shouldBe(seen[2], "two");
    shouldBe(seen[3], "three");
}

check((fn, args) => fn.apply(null, args));
check((fn, args) => Reflect.apply(fn, null, args));
