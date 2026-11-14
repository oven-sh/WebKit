//@ requireOptions("--useDecimal=1")
// https://tc39.es/proposal-decimal/

function shouldBe(actual, expected, what = "") {
    if (!Object.is(actual, expected))
        throw new Error(`${what} bad value: ${String(actual)}, expected ${String(expected)}`);
}

function shouldThrow(func, errorType) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error(`expected ${errorType.name} from ${func}, got ${String(error)}`);
}

const d = value => new Decimal(value);
const max = "9.999999999999999999999999999999999e6144";

// Construction.
{
    shouldThrow(() => Decimal("1"), TypeError);
    for (const value of [undefined, null, true, {}, [], Symbol(), d("1"), new Number(1), new String("1"), { valueOf() { throw new Error("read"); } }])
        shouldThrow(() => new Decimal(value), TypeError);
    shouldThrow(() => new Decimal(), TypeError);

    for (const [text, expected] of [
        ["0", "0"], ["-0", "-0"], ["+0", "0"], ["1", "1"], ["+1", "1"], ["-1", "-1"], ["1.", "1"], [".5", "0.5"], ["-.5", "-0.5"], ["007", "7"],
        ["1.20", "1.2"], ["100", "100"], ["1e3", "1000"], ["1E3", "1000"], ["1e+3", "1000"], ["1e-3", "0.001"], ["1.e1", "10"], ["-0e5", "-0"], ["0.000", "0"],
        ["NaN", "NaN"], ["Infinity", "Infinity"], ["+Infinity", "Infinity"], ["-Infinity", "-Infinity"],
        ["123.456", "123.456"], ["0.1", "0.1"],
        ["1e999999999999999999999", "Infinity"], ["-1e999999999999999999999", "-Infinity"], ["1e-999999999999999999999", "0"], ["-1e-999999999999999999999", "-0"],
        ["0e999999999999999999999", "0"],
        [max, max], ["1e6144", "1e6144"], ["1e6145", "Infinity"], ["1e-6176", "1e-6176"], ["1e-6177", "0"], ["5e-6177", "0"], ["5.1e-6177", "1e-6176"],
        ["15e-6177", "2e-6176"], ["25e-6177", "2e-6176"],
        ["9999999999999999999999999999999999", "9999999999999999999999999999999999"],
        ["99999999999999999999999999999999994", "9.999999999999999999999999999999999e34"],
        ["99999999999999999999999999999999995", "1e35"],
        ["9.9999999999999999999999999999999995e6144", "Infinity"],
        ["9.9999999999999999999999999999999994e6144", max],
        ["1" + "0".repeat(10000) + "e-10000", "1"],
        ["0." + "0".repeat(10000) + "1e10000", "0.1"],
        // A tie that a digit far behind decides.
        ["10000000000000000000000000000000005" + "0".repeat(200), "1e234"],
        ["10000000000000000000000000000000005" + "0".repeat(200) + "1", "1.000000000000000000000000000000001e235"],
    ])
        shouldBe(d(text).toString(), expected, text.slice(0, 50));

    for (const text of ["", " ", " 1", "1 ", "1\n", ".", "+", "-", "e1", ".e1", "1e", "1e+", "1e1.0", "1_000", "0x10", "0b1", "0o7", "1n", "1.2.3", "--1", "+-1",
        "infinity", "INFINITY", "Inf", "nan", "-NaN", "+NaN", "NaN ", "Infinityx", "1e1e1", "١", "１", "1\0", "١", "−1"])
        shouldThrow(() => d(text), SyntaxError);

    // Ropes and 16 bit strings.
    const rope = "1".repeat(10) + "." + "5".repeat(10);
    shouldBe(d(rope).toString(), "1111111111.5555555555");
    shouldBe(d("€12.5".slice(1)).toString(), "12.5");

    for (const [number, expected] of [
        [0, "0"], [1, "1"], [-1, "-1"], [0.1, "0.1"], [0.1 + 0.2, "0.30000000000000004"], [1e21, "1000000000000000000000"], [5e-324, "5e-324"],
        [Number.MAX_VALUE, "1.7976931348623157e308"], [NaN, "NaN"], [Infinity, "Infinity"], [-Infinity, "-Infinity"], [2 ** 31, "2147483648"], [-(2 ** 31), "-2147483648"],
        // Number.prototype.toExponential, by which the spec converts, has no -0.
        [-0, "0"],
    ])
        shouldBe(d(number).toString(), expected);

    for (const [bigint, expected] of [
        [0n, "0"], [-1n, "-1"], [2n ** 64n, "18446744073709551616"], [10n ** 34n - 1n, "9999999999999999999999999999999999"], [10n ** 34n + 5n, "1e34"],
        [10n ** 34n + 15n, "1.000000000000000000000000000000002e34"], [10n ** 6144n, "1e6144"], [10n ** 6145n, "Infinity"], [-(10n ** 6145n), "-Infinity"],
    ])
        shouldBe(d(bigint).toString(), expected);

    // The prototype of newTarget is read before the text is parsed, and after the type is checked.
    let reads = 0;
    const newTarget = function () { }.bind();
    Object.defineProperty(newTarget, "prototype", { get() { ++reads; return null; } });
    shouldThrow(() => Reflect.construct(Decimal, [{}], newTarget), TypeError);
    shouldBe(reads, 0);
    shouldThrow(() => Reflect.construct(Decimal, ["x"], newTarget), SyntaxError);
    shouldBe(reads, 1);
    shouldBe(Object.getPrototypeOf(Reflect.construct(Decimal, ["1"], newTarget)), Decimal.prototype);

    class Derived extends Decimal { }
    const derived = new Derived("1.5");
    shouldBe(derived instanceof Derived && derived instanceof Decimal, true);
    shouldBe(derived.toString(), "1.5");
    // Results are of the intrinsic class.
    shouldBe(Object.getPrototypeOf(derived.add(derived)), Decimal.prototype);

    const other = createGlobalObject();
    shouldBe(other.Decimal !== Decimal, true);
    shouldBe(d("1").add(new other.Decimal("2")).toString(), "3");
    shouldBe(Object.getPrototypeOf(Reflect.construct(Decimal, ["1"], Object.defineProperty(other.Function().bind(), "prototype", { value: null }))), other.Decimal.prototype);
}

// No white space or line terminator is allowed anywhere.
for (const c of ["\t", "\v", "\f", "\n", "\r", " ", "\u00A0", "\u1680", "\u2000", "\u200B", "\u2028", "\u2029", "\u3000", "\uFEFF", "\0"]) {
    for (const text of [c, c + "1", "1" + c, "1" + c + "1", "-" + c + "1", "1e" + c + "1", c + "NaN", "NaN" + c, c + "Infinity", "Infinity" + c])
        shouldThrow(() => d(text), SyntaxError);
}

// An exception of the Get of the prototype goes before that of the text, and errors are of the realm of the constructor.
{
    const throwing = Object.defineProperty(function () { }.bind(), "prototype", { get() { throw new EvalError; } });
    for (const text of ["1", "zz"])
        shouldThrow(() => Reflect.construct(Decimal, [text], throwing), EvalError);
    shouldThrow(() => Reflect.construct(Decimal, [null], throwing), TypeError);

    const { proxy, revoke } = Proxy.revocable(function () { }, { get() { revoke(); return undefined; } });
    shouldThrow(() => Reflect.construct(Decimal, ["1"], proxy), TypeError);
    shouldThrow(() => Reflect.construct(Decimal, ["1"], proxy), TypeError);

    const other = createGlobalObject();
    shouldThrow(() => new other.Decimal("zz"), other.SyntaxError);
    shouldThrow(() => new other.Decimal({}), other.TypeError);
    shouldThrow(() => other.Decimal("1"), other.TypeError);
    shouldThrow(() => Reflect.construct(Decimal, ["zz"], other.Object), SyntaxError);
}
