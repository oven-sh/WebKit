//@ requireOptions("--useDecimal=1")
// https://tc39.es/proposal-decimal/

function shouldBe(actual, expected, what = "") {
    if (!Object.is(actual, expected))
        throw new Error(`${what} bad value: ${String(actual)}, expected ${String(expected)}`);
}

const d = value => new Decimal(value);

// Number(decimal) in every tier, among other arguments.
{
    function toNumber(value) { return Number(value); }
    noInline(toNumber);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(toNumber(i), i);
    const decimal = d("1.5");
    const object = { valueOf() { return 4; } };
    for (let i = 0; i < testLoopCount; ++i) {
        // One of these each time, so that the call stays polymorphic and is not the length of the test.
        switch (i & 3) {
        case 0:
            shouldBe(toNumber(decimal), 1.5);
            break;
        case 1:
            shouldBe(toNumber("2"), 2);
            break;
        case 2:
            shouldBe(toNumber(3n), 3);
            break;
        case 3:
            shouldBe(toNumber(object), 4);
            break;
        }
    }
    function arithmetic(x, y) { return x.add(y).multiply(y).lessThan(x); }
    noInline(arithmetic);
    for (let i = 0; i < testLoopCount / 10; ++i)
        shouldBe(arithmetic(decimal, decimal), false);
}
