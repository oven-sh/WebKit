// Function.prototype.bind() takes a `length` of its target that is no number for 0, in every tier: it is not converted.
function shouldBe(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(`${what}: ${String(actual)}, expected ${String(expected)}`);
}

function withLength(value) {
    function target(a, b, c, d, e) { }
    Object.defineProperty(target, "length", { value });
    return target;
}

function bind(target) {
    return target.bind(null, 1);
}
noInline(bind);

let conversions = 0;
const cases = [
    ["5", 0], [10n, 0], [true, 0], [null, 0], [undefined, 0], [Symbol.iterator, 0], [[7], 0],
    [{ valueOf() { conversions++; return 7; } }, 0],
    [5, 4], [5.7, 4], [1, 0], [0, 0], [-3, 0], [NaN, 0], [Infinity, Infinity], [-Infinity, 0],
].map(([value, expected]) => [withLength(value), expected, typeof value]);

for (let i = 0; i < testLoopCount; i++) {
    for (const [target, expected, what] of cases)
        shouldBe(bind(target).length, expected, what);
}
shouldBe(conversions, 0, "conversions");
