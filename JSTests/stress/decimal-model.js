//@ requireOptions("--useDecimal=1")
// Compares Decimal with a model of it in BigInt that follows the steps of https://tc39.es/proposal-decimal/ on exact fractions.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: bad value: ${actual}, expected ${expected}`);
}

let seed = 12345;
function random(count) {
    seed = Math.imul(seed, 1103515245) + 12345 >>> 0;
    return Math.floor((seed >>> 4) / 2 ** 28 * count);
}
const pick = values => values[random(values.length)];

const powers = [];
function tenTo(n) {
    return powers[n] ??= 10n ** BigInt(n);
}

// ApplyRoundingModeToPositive of numerator / denominator.
function applyRoundingMode(numerator, denominator, mode) {
    const low = numerator / denominator;
    const fraction = numerator % denominator;
    if (!fraction || mode === "floor" || mode === "trunc")
        return low;
    if (mode === "ceil" || fraction * 2n > denominator)
        return low + 1n;
    if (fraction * 2n < denominator)
        return low;
    return mode === "halfExpand" || low & 1n ? low + 1n : low;
}

const reverse = { floor: "ceil", ceil: "floor", trunc: "trunc", halfExpand: "halfExpand", halfEven: "halfEven" };

// RoundToDecimalDomain of numerator / denominator * 10^exponent, as the text of an exponential string.
function roundToDomain(numerator, denominator, exponent, mode = "halfEven") {
    if (!numerator)
        return "0e+0";
    if (numerator < 0n)
        return "-" + roundToDomain(-numerator, denominator, exponent, reverse[mode]);
    let e = exponent + String(numerator).length - String(denominator).length;
    const [n, d] = [exponent >= e ? numerator * tenTo(exponent - e) : numerator, exponent >= e ? denominator : denominator * tenTo(e - exponent)];
    if (n < d)
        --e;
    let te = Math.max(e - 33, -6176);
    const shift = exponent - te;
    let rounded = applyRoundingMode(shift > 0 ? numerator * tenTo(shift) : numerator, shift < 0 ? denominator * tenTo(-shift) : denominator, mode);
    if (rounded === tenTo(34)) {
        ++te;
        rounded = tenTo(33);
    }
    if (te > 6111)
        return "Infinity";
    if (!rounded)
        return "0e+0";
    const digits = String(rounded);
    const significant = digits.replace(/0+$/, "");
    const scientific = te + digits.length - 1;
    return significant[0] + (significant.length > 1 ? "." + significant.slice(1) : "") + "e" + scientific;
}

function coefficient() {
    const length = pick([1, 2, 5, 9, 10, 17, 18, 19, 33, 34, 34, 34, 1 + random(34)]);
    const kind = random(4);
    let digits = "";
    for (let i = 0; i < length; ++i)
        digits += kind === 0 ? pick("09") : kind === 1 ? (i + 1 === length ? "5" : "0") : kind === 2 ? "9" : String(random(10));
    return BigInt("1" + digits.slice(1));
}

function check(x, ex, y, ey, mode) {
    const [a, b] = [new Decimal(x + "e" + ex), new Decimal(y + "e" + ey)];
    const what = `${x}e${ex} ${y}e${ey}`;
    const e = Math.min(ex, ey);
    const [u, v] = [x * tenTo(ex - e), y * tenTo(ey - e)];
    shouldBe(a.add(b).toExponential(), roundToDomain(u + v, 1n, e), what + " add");
    shouldBe(a.subtract(b).toExponential(), roundToDomain(u - v, 1n, e), what + " subtract");
    shouldBe(a.multiply(b).toExponential(), roundToDomain(x * y, 1n, ex + ey), what + " multiply");
    shouldBe(a.divide(b).toExponential(), y < 0n ? roundToDomain(-x, -y, ex - ey) : roundToDomain(x, y, ex - ey), what + " divide");
    const remainder = u % v;
    shouldBe(a.remainder(b).toExponential(), remainder ? roundToDomain(remainder, 1n, e) : x < 0n ? "-0e+0" : "0e+0", what + " remainder");
    shouldBe(a.compare(b), u < v ? -1 : u > v ? 1 : 0, what + " compare");

    // Some digits are rounded away, or none, or all.
    const digits = -ex - random(38) + 2;
    const shift = ex + digits;
    const [magnitude, sign] = x < 0n ? [-x, -1n] : [x, 1n];
    const rounded = shift >= 0 ? magnitude * tenTo(shift) : applyRoundingMode(magnitude, tenTo(-shift), x < 0n ? reverse[mode] : mode);
    shouldBe(a.round(digits, mode).toExponential(), roundToDomain(sign * rounded, 1n, -digits, mode), `${what} round ${digits} ${mode}`);
    if (digits >= 0 && digits < 100) {
        const text = String(rounded).padStart(digits + 1, "0");
        const fixed = digits ? text.slice(0, -digits) + "." + text.slice(-digits) : text;
        shouldBe(a.toFixed({ digits, roundingMode: mode }), (x < 0n ? "-" : "") + fixed, `${what} toFixed ${digits} ${mode}`);
    }
    const power = pick([random(100) - 50, random(12000) - 6000]);
    shouldBe(a.scale10(power).toExponential(), roundToDomain(x, 1n, ex + power), `${what} scale10 ${power}`);
}

for (let i = 0; i < 120; ++i) {
    const [x, y] = [coefficient() * pick([1n, -1n]), coefficient() * pick([1n, -1n])];
    // Alignments from none to more than the working precision. The ends of the range come with the power of scale10 and below.
    const ex = random(60) - 30;
    check(x, ex, y, ex + pick([0, random(10) - 5, random(90) - 45, random(200) - 100]), pick(Object.keys(reverse)));
}

// The ends of the range.
for (const [x, ex, y, ey] of [
    [tenTo(34) - 1n, 6111, 1n, 6111], [tenTo(34) - 1n, 6111, 5n, 6110], [tenTo(34) - 1n, 6111, 49n, 6109], [-1n, -6176, 2n, 0], [3n, -6176, 2n, 0], [1n, -6176, -1n, -6176],
    [tenTo(33), -6176, 1n, -6176], [tenTo(34) - 1n, -6176, 1n, -6176], [1n, -6143, 3n, 0], [tenTo(34) - 1n, -3088, tenTo(34) - 1n, -3122], [tenTo(34) - 1n, 3055, tenTo(34) - 1n, 3056],
])
    check(x, ex, y, ey, "halfEven");

// The smaller operand decides a tie, or the side of a power of ten, from any distance.
for (const distance of [1, 33, 34, 35, 41, 42, 43, 44, 75, 76, 77, 100, 1000]) {
    for (const y of [1n, -1n, 9n, -9n, 5n, -5n]) {
        check(tenTo(33) + 1n, 0, y, -distance, "ceil");
        check(1n, 0, y, -distance, "floor");
        check(tenTo(34) - 1n, 0, y, -distance, "halfExpand");
    }
}

// The remainder where the quantum of the divisor is as far above that of the dividend as it can be without the dividend being the result.
for (const distance of [31, 32, 33, 34, 35]) {
    for (const y of [1n, 3n, 9n, 11n, 99n])
        check(tenTo(34) - 7n, 0, y, distance, "trunc");
}

// Operands for which the trial digit of the long division is too large by one after its first correction, with limbs of 32 bits.
for (const [x, y] of [
    [2280482960145792789786198761897396n, 2596148429267431299294969492583481n],
    [2517348038092827295883013111314871n, 2596148429267430595343300186145911n],
    [1342806977855025174556287140677842n, 2596148429267415399711924102451417n],
    [1592775240641056411991170804269948n, 2596148429267418253714024531364753n],
    [1254122151376933547327856039253164n, 2596148429267414904661608542063143n],
    [2470678788795843958341559363963898n, 2596148429267431291627494232390693n],
])
    check(x, 0, y, 0, "trunc");
for (const [x, y] of [
    [2336533586340675516401633991692347n, 2596148429267417240497447364953067n],
    [1298074214633708940449933280366374n, 2596148429267417880941756450828671n],
    [2596148429267424552902535317017389n, 2596148429267424552980633381228639n],
    [1817303900487200414996766905401618n, 2596148429267429164449375174662679n],
    [1557689057560455515937356157329873n, 2596148429267425859908105242246499n],
])
    check(x, 1, y, 0, "trunc");

// With limbs of 64 bits a divisor of two needs no second correction. Operands for which the top limbs of the rest and of the divisor are equal.
for (const [x, distance, y] of [
    [7771135073656204685254358248312996n, 30, 8170053252940960280918763671765145n],
    [6498112176903250593509936280224884n, 20, 5871056116044179001590093089040525n],
    [1585126956570014810528308034154647n, 20, 8592990449892729467632206478167877n],
    [5308610160308995980708463107855932n, 25, 4979622184439983842522458431210022n],
    [7263050777677427810511247264569349n, 25, 1676391724719927726321416329501587n],
    [4684387845518446571492769431114347n, 0, 8847955067630455685166766360834638n],
    [5369824267026835368153603001195683n, 0, 7031872643907555917944739682669277n],
    [1533177050685936082193256863779259n, 0, 5231483734497832193119552612286109n],
    [5633749724281762448944550492536505n, 0, 2777769516730073128123212505686166n],
    [2068341488308710321082018099269212n, 0, 8405233167251912253802471198276967n],
])
    check(x, distance, y, 0, "trunc");
// The second correction is needed for three or four, that is where the product is divided by at least 10^56.
for (const [x, y, power] of [
    [4029681481067123834115960162893836n, 9435768355053925179000700782622422n, 60],
    [4668837074636497405066305284838042n, 9552835224491632189387699376092956n, 57],
    [1171834883166880303940945153700584n, 9384446698054064228356938756955329n, 60],
    [3324506420961999609449937994647213n, 9859974609558644881912509213302425n, 59],
    [4951466313107695666409945788875077n, 5184875646238009932720650863239513n, 59],
    [8349309687282298815781189600268002n, 7762170218541176090335839079621564n, 60],
])
    check(x, -3100, y, -6176 - power + 3100, "trunc");

// Where sums and products leave 64 bits, and short coefficients at the greatest exponents, whose sum is infinite.
for (const x of [1n, 2n, 9n, 2n ** 32n - 1n, 2n ** 32n, 2n ** 63n - 1n, 2n ** 63n, 2n ** 64n - 1n, 2n ** 64n, tenTo(19) - 1n, tenTo(19), 18446744073709551n]) {
    for (const y of [1n, 3n, 9n, 10n, 1001n, 2n ** 32n + 1n, 2n ** 63n, 2n ** 64n - 1n, tenTo(19)]) {
        for (const distance of [0, 1, 2, 9, 18, 19, 20]) {
            check(x, distance, y, 0, "halfEven");
            check(x, 0, -y, distance, "halfEven");
            check(-x, 0, y, -distance, "halfEven");
        }
        check(x, -6176, y, -6176, "halfEven");
        check(x, -6176, -y, -6170, "halfEven");
    }
}
for (const [x, y, exponent] of [[9n, 9n, 6144], [9n, 1n, 6144], [-9n, -9n, 6144], [4n, 5n, 6144], [9n, -9n, 6144], [9n, 9n, 6112], [9n, 9n, 6111], [99n, 1n, 6143]]) {
    for (const distance of [0, 1, 5])
        check(x, exponent - distance, y, exponent, "halfEven");
}

// One path each: 18 to 20 and 34 to 40 digits, quotients that end early, late and never, and units around 10^19 and 10^38.
for (const digits of [17, 18, 19, 20, 33, 34]) {
    for (const x of [tenTo(digits) - 1n, tenTo(digits - 1) + 1n, tenTo(digits - 1)]) {
        for (const y of [1n, 2n, 3n, 7n, 8n, 1024n, tenTo(18) + 1n, 2n ** 63n, 2n ** 64n - 1n, 2n ** 64n, 2n ** 64n + 1n]) {
            for (const distance of [0, 1, 18, 19, 20, 37, 38, 39, 42, 43, 44]) {
                check(x, distance, y, 0, "halfEven");
                check(x, 0, y, distance, "halfExpand");
            }
        }
    }
}
