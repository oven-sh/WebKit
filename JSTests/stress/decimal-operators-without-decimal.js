//@ runDefault("--useDecimalOperators=1")
// The option does nothing without useDecimal.

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

shouldBe("Decimal" in globalThis, false);
shouldBe(errorOf(() => Function("1m")), "SyntaxError");
const log = [];
const object = (name, value) => ({ valueOf() { log.push(name); return value; } });
shouldBe(object("a", 1) + object("b", 2), 3);
shouldBe(object("c", 1) < object("d", 2), true);
shouldBe(object("e", 1) == object("f", 1), false);
shouldBe(log.join(), "a,b,c,d");
