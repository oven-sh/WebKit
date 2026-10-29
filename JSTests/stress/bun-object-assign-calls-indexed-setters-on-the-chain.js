// Object.assign() stores with [[Set]]: an indexed setter on the target's prototype chain is called, and a read-only element there refuses the store.

function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function shouldThrow(run, errorType)
{
    let thrown = null;
    try {
        run();
    } catch (error) {
        thrown = error;
    }
    if (!(thrown instanceof errorType))
        throw new Error(`expected a ${errorType.name} but got ${thrown}`);
}

for (let i = 0; i < 100; ++i) {
    const stored = [];
    const prototype = {};
    Object.defineProperty(prototype, "0", { set(value) { stored.push(value); }, get() { return "inherited"; }, configurable: true });
    const target = Object.assign(Object.create(prototype), { 0: 1, a: 2 });
    shouldBe(stored.join(), "1");
    shouldBe(Object.keys(target).join(), "a");
    shouldBe(target[0], "inherited");
}

{
    const stored = [];
    Object.defineProperty(Object.prototype, "3", { set(value) { stored.push(value); }, configurable: true });
    try {
        const target = Object.assign({}, { 3: 1, a: 2 }, [9, 8, 7, 6]);
        shouldBe(stored.join(), "1,6");
        shouldBe(Object.keys(target).join(), "0,1,2,a");
    } finally {
        delete Object.prototype[3];
    }
}

{
    const prototype = {};
    Object.defineProperty(prototype, "1", { value: 5, writable: false });
    const target = Object.create(prototype);
    shouldThrow(() => Object.assign(target, [1, 2, 3]), TypeError);
    shouldBe(Object.keys(target).join(), "0");
}

{
    const target = Object.assign({}, { 0: 1, 1: 2, a: 3 }, ["x"]);
    shouldBe(JSON.stringify(target), '{"0":"x","1":2,"a":3}');
}
