//@ requireOptions("--useDecimal=1")
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

const one = new Decimal(1), two = new Decimal(2), nan = new Decimal(NaN);

// The operands are computed in sight, so optimized code has no object for them until the operator asks.
function check(source, expected, x = two) {
    const f = Function("x", "y", `let a = x.add(y), b = x.subtract(y); try { return ${source}; } catch (error) { return error.name; }`);
    noInline(f);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(f(x, one), expected, source);
}

const withNumbers = {
    "a + b": 4, "a - b": 2, "a * b": 3, "a / b": 3, "a % b": 0, "a ** b": 3, "a & b": 1, "a | b": 3, "a ^ b": 2, "a << b": 6, "a >> b": 1, "a >>> b": 1,
    "a < b": false, "a <= b": false, "a > b": true, "a >= b": true, "-a": -3, "+a": 3, "~a": -4, "a++": 3, "--a": 2, "a += b": 4,
    "a + 1": 4, "1 - a": -2, "a + ''": "3", "a < 4": true, "a == 3": true, "'3' != a": false, "a == 3n": true, "a == true": false,
};
for (const source in withNumbers)
    check(source, "TypeError");
shouldBe(errorOf(() => { let a = one; a++; }), "TypeError");

const identities = {
    "a == x.add(y)": false, "a != x.add(y)": true, "a === x.add(y)": false, "a == a": true, "a != a": false, "a === a": true,
    "a == null": false, "a != undefined": true, "a == { }": false, "!a": false, "typeof a": "object", "`${a}`": "3",
};
for (const source in identities)
    check(source, identities[source]);
check("a == a", true, nan);

// It is valueOf() that throws.
Decimal.prototype.valueOf = function () { return Number(this); };
for (const source in withNumbers)
    check(source, withNumbers[source]);

// Nor is there a literal.
for (const source of ["1m", "1.5m", ".5m", "1e3m", "-1m"])
    shouldBe(errorOf(() => Function(source)), "SyntaxError", source);
