//@ runDefault("--useDecimal=1")
// The algorithms of https://tc39.es/proposal-decimal/ written out step by step, but for their checks of the arguments, against all pairs of a set of values. A Decimal value is
// one of the strings below, or { n, e } for the mathematical value n * 10^e.

const NaND = "NaN", PINF = "Infinity", NINF = "-Infinity", P0 = "0", N0 = "-0";

function shouldBe(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(`${what()}: bad value: ${actual}, expected ${expected}`);
}

const powers = new Map;
function tenTo(n) {
    let power = powers.get(n);
    if (power === undefined)
        powers.set(n, power = 10n ** BigInt(n));
    return power;
}
const abs = n => n < 0n ? -n : n;
const isMV = d => typeof d === "object";
const zero = d => d === P0 || d === N0;
const infinite = d => d === PINF || d === NINF;
const finite = d => isMV(d) || zero(d);
const sign = d => isMV(d) ? (d.n < 0n ? -1 : 1) : d === NINF || d === N0 ? -1 : 1;
const MV = d => isMV(d) ? d : { n: 0n, e: 0 };
function aligned(x, y) {
    const e = Math.min(x.e, y.e);
    return [x.n * tenTo(x.e - e), y.n * tenTo(y.e - e), e];
}

// The number of digits. String() of a long BigInt is slow.
function length(n) {
    if (n < tenTo(300))
        return String(n).length;
    let digits = Math.floor((n.toString(16).length - 1) * Math.log10(16)) + 1;
    while (n >= tenTo(digits))
        ++digits;
    return digits;
}

// ApplyRoundingModeToPositive ( m = n / d, roundingMode )
function applyRoundingModeToPositive(n, d, mode) {
    const low = n / d, fraction = n % d;
    if (fraction === 0n)
        return low;
    const high = low + 1n;
    if (mode === "floor" || mode === "trunc")
        return low;
    if (mode === "ceil")
        return high;
    if (fraction * 2n < d)
        return low;
    if (fraction * 2n > d)
        return high;
    if (mode === "halfExpand")
        return high;
    return low % 2n === 0n ? low : high;
}

// RoundToDecimalDomain ( v = n / d * 10^x, roundingMode )
function roundToDecimalDomain(n, d, x, mode = "halfEven") {
    if (n === 0n)
        return P0;
    if (n < 0n) {
        const result = roundToDecimalDomain(-n, d, x, mode === "floor" ? "ceil" : mode === "ceil" ? "floor" : mode);
        return result === PINF ? NINF : result === P0 ? N0 : { n: -result.n, e: result.e };
    }
    if (x > 0) {
        n *= tenTo(x);
        x = 0;
    }
    // 10^e <= v < 10^(e + 1)
    let e = length(n) - length(d) + x;
    const scaled = k => [k > 0 ? n * tenTo(k) : n, k < 0 ? d * tenTo(-k) : d];
    const [a, b] = scaled(x - e);
    if (a < b)
        --e;
    let te = e - 33;
    if (te < -6176)
        te = -6176;
    let rounded = applyRoundingModeToPositive(...scaled(x - te), mode);
    if (rounded === tenTo(34)) {
        te += 1;
        rounded = tenTo(33);
    }
    if (te > 6111)
        return PINF;
    if (rounded === 0n)
        return P0;
    return { n: rounded, e: te };
}

const steps = {
    isNaN: d => d === NaND,
    isFinite: d => !(d === NaND || infinite(d)),
    exponent(d) {
        if (d === NaND)
            return NaN;
        if (infinite(d))
            return Infinity;
        if (zero(d))
            return -Infinity;
        return String(abs(d.n)).length - 1 + d.e;
    },
    mantissa(d) {
        if (d === NaND)
            return NaND;
        if (infinite(d))
            return PINF;
        if (zero(d))
            return d;
        return { n: d.n, e: 1 - String(abs(d.n)).length };
    },
    abs(d) {
        if (d === NaND)
            return NaND;
        if (infinite(d))
            return PINF;
        if (zero(d))
            return P0;
        return { n: abs(d.n), e: d.e };
    },
    negate(d) {
        if (isMV(d))
            return { n: -d.n, e: d.e };
        return { [NaND]: NaND, [PINF]: NINF, [NINF]: PINF, [P0]: N0, [N0]: P0 }[d];
    },
    add(d1, d2) {
        if (d1 === NaND || d2 === NaND)
            return NaND;
        if (d1 === PINF || d2 === PINF)
            return d1 === NINF || d2 === NINF ? NaND : PINF;
        if (d1 === NINF || d2 === NINF)
            return NINF;
        if (d1 === N0)
            return d2;
        if (d2 === N0)
            return d1;
        const [x, y, e] = aligned(MV(d1), MV(d2));
        return roundToDecimalDomain(x + y, 1n, e);
    },
    subtract(d1, d2) {
        if (d1 === NaND || d2 === NaND)
            return NaND;
        if (infinite(d1))
            return d2 === d1 ? NaND : d1;
        if (d2 === PINF)
            return NINF;
        if (d2 === NINF)
            return PINF;
        if (d1 === N0 && d2 === P0)
            return N0;
        const [x, y, e] = aligned(MV(d1), MV(d2));
        return roundToDecimalDomain(x - y, 1n, e);
    },
    multiply(d1, d2) {
        if (d1 === NaND || d2 === NaND)
            return NaND;
        if (d1 === PINF || d2 === PINF) {
            if (zero(d1) || zero(d2))
                return NaND;
            return sign(d1) === -1 || sign(d2) === -1 ? NINF : PINF;
        }
        if (d1 === NINF || d2 === NINF) {
            if (zero(d1) || zero(d2))
                return NaND;
            return sign(d1) === -1 && sign(d2) === -1 ? PINF : NINF;
        }
        if (d1 === P0 || d2 === P0)
            return sign(d1) === -1 || sign(d2) === -1 ? N0 : P0;
        if (d1 === N0 || d2 === N0)
            return sign(d1) === -1 && sign(d2) === -1 ? P0 : N0;
        return roundToDecimalDomain(d1.n * d2.n, 1n, d1.e + d2.e);
    },
    divide(d1, d2) {
        if (d1 === NaND || d2 === NaND)
            return NaND;
        if (d1 === PINF)
            return infinite(d2) ? NaND : sign(d2) === -1 ? NINF : PINF;
        if (d1 === NINF)
            return infinite(d2) ? NaND : sign(d2) === -1 ? PINF : NINF;
        if (d2 === PINF)
            return sign(d1) === -1 ? N0 : P0;
        if (d2 === NINF)
            return sign(d1) === -1 ? P0 : N0;
        if (d2 === P0)
            return zero(d1) ? NaND : d1.n < 0n ? NINF : PINF;
        if (d2 === N0)
            return zero(d1) ? NaND : d1.n < 0n ? PINF : NINF;
        if (d1 === P0)
            return d2.n < 0n ? N0 : P0;
        if (d1 === N0)
            return d2.n < 0n ? P0 : N0;
        return d2.n < 0n ? roundToDecimalDomain(-d1.n, -d2.n, d1.e - d2.e) : roundToDecimalDomain(d1.n, d2.n, d1.e - d2.e);
    },
    remainder(d1, d2) {
        if (d1 === NaND || d2 === NaND)
            return NaND;
        if (infinite(d1))
            return NaND;
        if (infinite(d2))
            return d1;
        if (zero(d2))
            return NaND;
        if (zero(d1))
            return d1;
        const [x, y, e] = aligned(d1, d2);
        const q = x / y, r = x - y * q;
        if (r === 0n)
            return d1.n < 0n ? N0 : P0;
        return { n: r, e };
    },
    compare(d1, d2) {
        if (d1 === NaND || d2 === NaND)
            return NaN;
        if (d1 === PINF)
            return d2 === PINF ? 0 : 1;
        if (d1 === NINF)
            return d2 === NINF ? 0 : -1;
        if (d2 === PINF)
            return -1;
        if (d2 === NINF)
            return 1;
        const [x, y] = aligned(MV(d1), MV(d2));
        return x < y ? -1 : y < x ? 1 : 0;
    },
    equals(d1, d2) {
        if (d1 === NaND || d2 === NaND)
            return false;
        if (infinite(d1))
            return d2 === d1;
        if (infinite(d2))
            return false;
        const [x, y] = aligned(MV(d1), MV(d2));
        return x === y;
    },
    notEquals(d1, d2) {
        if (d1 === NaND || d2 === NaND)
            return false;
        if (infinite(d1))
            return d2 !== d1;
        if (infinite(d2))
            return true;
        const [x, y] = aligned(MV(d1), MV(d2));
        return x !== y;
    },
    lessThan(d1, d2) {
        if (d1 === NaND || d2 === NaND)
            return false;
        if (d1 === PINF)
            return false;
        if (d1 === NINF)
            return d2 !== NINF;
        if (d2 === PINF)
            return true;
        if (d2 === NINF)
            return false;
        const [x, y] = aligned(MV(d1), MV(d2));
        return x < y;
    },
    lessThanOrEqual(d1, d2) {
        if (d1 === NaND || d2 === NaND)
            return false;
        if (d1 === PINF)
            return d2 === PINF;
        if (d1 === NINF)
            return true;
        if (d2 === PINF)
            return true;
        if (d2 === NINF)
            return false;
        const [x, y] = aligned(MV(d1), MV(d2));
        return x <= y;
    },
    greaterThan(d1, d2) {
        if (d1 === NaND || d2 === NaND)
            return false;
        if (d1 === PINF)
            return d2 !== PINF;
        if (d1 === NINF)
            return false;
        if (d2 === PINF)
            return false;
        if (d2 === NINF)
            return true;
        const [x, y] = aligned(MV(d1), MV(d2));
        return x > y;
    },
    greaterThanOrEqual(d1, d2) {
        if (d1 === NaND || d2 === NaND)
            return false;
        if (d1 === PINF)
            return true;
        if (d1 === NINF)
            return d2 === NINF;
        if (d2 === PINF)
            return false;
        if (d2 === NINF)
            return true;
        const [x, y] = aligned(MV(d1), MV(d2));
        return x >= y;
    },
};

function round(d, digits, mode) {
    if (d === NaND || infinite(d) || zero(d))
        return d;
    let scaled = d.n;
    const s = sign(d);
    let used = mode;
    if (s === -1) {
        scaled = -scaled;
        used = mode === "floor" ? "ceil" : mode === "ceil" ? "floor" : mode;
    }
    const k = d.e + digits;
    let rounded = applyRoundingModeToPositive(k > 0 ? scaled * tenTo(k) : scaled, k < 0 ? tenTo(-k) : 1n, used);
    if (s === -1)
        rounded = -rounded;
    return roundToDecimalDomain(rounded, 1n, -digits, used);
}

function scale10(d, n) {
    if (d === NaND || infinite(d) || zero(d))
        return d;
    if (Number.isNaN(n))
        return NaND;
    if (n === Infinity)
        return d.n > 0n ? PINF : NINF;
    if (n === -Infinity)
        return d.n > 0n ? P0 : N0;
    return roundToDecimalDomain(d.n, 1n, d.e + n);
}

function canonicalizeDecimalString(digits) {
    if (!digits.includes("."))
        return digits;
    if (!digits.endsWith("0"))
        return digits;
    const lhs = digits.slice(0, digits.indexOf("."));
    const rhs = digits.slice(digits.indexOf(".") + 1);
    const noTrailingZeroes = rhs.replace(/0+$/, "");
    return noTrailingZeroes === "" ? lhs : lhs + "." + noTrailingZeroes;
}

// DecimalToDecimalString. Not as in the steps: an integer has the prefix too, and nonIntegerPart is not cut to numDigits digits.
function toDecimal(d) {
    if (!isMV(d) && !zero(d))
        return d;
    const prefix = sign(d) === -1 ? "-" : "";
    if (zero(d))
        return prefix + "0";
    const argument = steps.abs(d);
    const x = argument.e >= 0 ? argument.n * tenTo(argument.e) : argument.n / tenTo(-argument.e);
    const y = argument.e >= 0 ? 0n : argument.n % tenTo(-argument.e);
    const fractionalPart = y === 0n ? "" : String(y).padStart(-argument.e, "0");
    let integerPart = x === 0n ? "" : String(x);
    if (y === 0n)
        return prefix + integerPart;
    if (integerPart === "")
        integerPart = "0";
    return canonicalizeDecimalString(prefix + integerPart + "." + fractionalPart);
}

// DecimalToExponentialString. Not as in the steps: e is the one for which 1 <= argument * 10^-e < 10.
function toExponential(d) {
    if (d === NaND || infinite(d))
        return d;
    const prefix = sign(d) === -1 ? "-" : "";
    if (zero(d))
        return prefix + "0e+0";
    const argument = steps.abs(d);
    const e = length(argument.n) - 1 + argument.e;
    const mantissa = { n: argument.n, e: argument.e - e };
    return prefix + toDecimal(mantissa) + "e" + e;
}

function toStringOf(d) {
    if (!isMV(d))
        return d;
    const e = steps.exponent(d);
    return e < -6 || e >= 34 ? toExponential(d) : toDecimal(d);
}

const object = d => new Decimal(isMV(d) ? d.n + "e" + d.e : d);
const answer = value => value instanceof Decimal ? value.toExponential() : value;
const expected = value => typeof value === "object" || [NaND, PINF, NINF, P0, N0].includes(value) ? toExponential(value) : value;

const max = tenTo(34) - 1n;
const values = [NaND, PINF, NINF, P0, N0];
for (const [n, e] of [[1n, 0], [5n, -1], [15n, -1], [7n, 3], [12345n, -2], [1n, 34], [1n, -7], [max, 0], [max, 6111], [1n, 6144], [1n, -6176], [max, -6176], [tenTo(33) + 1n, -33], [1n, 33], [49n, -50]])
    values.push({ n, e }, { n: -n, e });

for (const d1 of values) {
    const x = object(d1);
    const what1 = name => () => `${toExponential(d1)} ${name}`;
    for (const name of ["isNaN", "isFinite", "exponent", "mantissa", "abs", "negate"])
        shouldBe(answer(x[name]()), expected(steps[name](d1)), what1(name));
    shouldBe(x.toExponential(), toExponential(d1), what1("toExponential"));
    shouldBe(x.toString(), toStringOf(d1), what1("toString"));
    shouldBe(x.toPrecision({}), toDecimal(d1), what1("toPrecision({})"));
    shouldBe(Number(x), Number(toExponential(d1)), what1("Number"));
    for (const n of [0, 1, -1, 34, -34, NaN, Infinity, -Infinity])
        shouldBe(x.scale10(n).toExponential(), toExponential(scale10(d1, n)), what1("scale10 " + n));
    const isLong = isMV(d1) && Math.abs(d1.e) > 1000;
    for (const digits of isLong ? [0, -d1.e - 1] : [0, 1, 2, -1, 33, -33, 50]) {
        for (const mode of isLong ? ["ceil", "halfEven"] : ["ceil", "floor", "trunc", "halfExpand", "halfEven"])
            shouldBe(x.round(digits, mode).toExponential(), toExponential(round(d1, digits, mode)), what1(`round ${digits} ${mode}`));
    }
    // DecimalToBigInt
    let bigint;
    try {
        bigint = BigInt(x);
    } catch (error) {
        bigint = error.name;
    }
    const integer = isMV(d1) && (d1.e >= 0 || d1.e > -40 && d1.n % tenTo(-d1.e) === 0n);
    shouldBe(String(bigint), !finite(d1) ? "RangeError" : zero(d1) ? "0" : integer ? toDecimal(d1) : "RangeError", what1("BigInt"));
}

const binary = ["add", "subtract", "multiply", "divide", "remainder", "compare", "equals", "notEquals", "lessThan", "lessThanOrEqual", "greaterThan", "greaterThanOrEqual"];
// The numbers of the model are long where the exponents are far apart, so there it is one pair in 70.
const isFar = (x, y) => isMV(x) && isMV(y) && Math.abs(x.e - y.e) > 1000;
let count = 0;
for (const d1 of values) {
    for (const d2 of values) {
        if (isFar(d1, d2) && ++count % 70)
            continue;
        const x = object(d1), y = object(d2);
        for (const name of binary)
            shouldBe(answer(x[name](y)), expected(steps[name](d1, d2)), () => `${toExponential(d1)} ${name} ${toExponential(d2)}`);
    }
}
