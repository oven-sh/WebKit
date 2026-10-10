//@ $skipModes << :aot << :aot_validate # TODO(bun): f.arguments and f.caller are null while f runs ahead-of-time compiled code.
function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

function foo(arr) {
    return [...arr];
}

let obj = {};
obj[Symbol.iterator] = function bar() {
    shouldBe(arguments.callee.caller, foo);
    return {
        next() { return { done: true }; }
    };
};
foo(obj);
