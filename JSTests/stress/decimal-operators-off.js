//@ runDefault("--useDecimal=1")
// Operators as in the proposal: they throw for Decimals, but for == and !=, which compare identity.

function shouldBe(actual, expected, what = "") {
    if (!Object.is(actual, expected))
        throw new Error(`${what} bad value: ${String(actual)}, expected ${String(expected)}`);
}

function errorOf(func) {
    try {
        func();
    } catch (error) {
        return error.name;
    }
    return "none";
}

for (const source of ["a + b", "a - b", "a * b", "a / b", "a % b", "a < b", "a <= b", "a > b", "a >= b", "-a", "a++", "--a", "a += b", "let c = a++"])
    shouldBe(errorOf(() => Function("a", "b", source)(new Decimal(1), new Decimal(1))), "TypeError", source);
const a = new Decimal(1), nan = new Decimal(NaN);
shouldBe(a == new Decimal(1), false);
shouldBe(a != new Decimal(1), true);
shouldBe(a == a, true);
shouldBe(nan == nan, true);
