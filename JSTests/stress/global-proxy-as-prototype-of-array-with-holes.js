function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

globalThis[1] = "inherited";
const holey = Object.setPrototypeOf([0, , 2], globalThis);

function test() {
    shouldBe(holey[1], "inherited");
    shouldBe(Array.prototype.join.call(holey), "0,inherited,2");
    shouldBe(Array.prototype.slice.call(holey)[1], "inherited");
    shouldBe(Array.prototype.concat.call([], holey)[1], "inherited");
    shouldBe(Array.prototype.indexOf.call(holey, "inherited"), 1);
    shouldBe(Array.prototype.includes.call(holey, "inherited"), true);
    shouldBe(((a, b) => b).apply(null, holey), "inherited");
    shouldBe([...Array.prototype.values.call(holey)][1], "inherited");
}
noInline(test);

for (let i = 0; i < testLoopCount; ++i)
    test();
