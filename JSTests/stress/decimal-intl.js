//@ requireOptions("--useDecimal=1")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}

function shouldThrow(func, errorType) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error(`expected ${errorType.name}, got ${String(error)}`);
}

const d = value => new Decimal(value);

shouldBe(d("1234567.891").toLocaleString("en-US"), "1,234,567.891");
shouldBe(d("1234567.891").toLocaleString("de-DE"), "1.234.567,891");
shouldBe(d("-0.5").toLocaleString("en-US", { style: "percent" }), "-50%");
shouldBe(d("413.09").toLocaleString("de-DE", { style: "currency", currency: "EUR" }), "413,09 €");
for (const [text, expected] of [["NaN", "NaN"], ["Infinity", "∞"], ["-Infinity", "-∞"], ["0", "0"], ["-0", "-0"], ["1.20", "1.2"]])
    shouldBe(d(text).toLocaleString("en-US"), expected);

const digits = { maximumFractionDigits: 40 };
// More digits than a Number has.
shouldBe(d("1234567890123456789012345678901.234").toLocaleString("en-US", digits), "1,234,567,890,123,456,789,012,345,678,901.234");
shouldBe(d("-0.1234567890123456789012345678901234").toLocaleString("en-US", digits), "-0.1234567890123456789012345678901234");
// Magnitudes outside of those of a Number.
shouldBe(d("1e400").toLocaleString("en-US", { useGrouping: false }), "1" + "0".repeat(400));
shouldBe(d("1e-400").toLocaleString("en-US"), "0");
shouldBe(d("1e6144").toLocaleString("en-US", { notation: "scientific" }), "1E6144");

const format = new Intl.NumberFormat("en-US", digits);
shouldBe(format.format(d("0.1").add(d("0.2"))), "0.3");
shouldBe(format.format(d("9007199254740993.5")), "9,007,199,254,740,993.5");
shouldBe(format.formatToParts(d("-1234.5")).map(part => part.type).join(), "minusSign,integer,group,integer,decimal,fraction");
shouldBe(format.formatRange(d("1.5"), d("2.5")), "1.5–2.5");
shouldThrow(() => format.formatRange(d("NaN"), d("1")), RangeError);

const plural = new Intl.PluralRules("en-US");
shouldBe(plural.select(d("1")), "one");
shouldBe(plural.select(d("1.5")), "other");
shouldBe(plural.select(d("NaN")), "other");
shouldBe(plural.selectRange(d("1"), d("2")), "other");
shouldBe(new Intl.PluralRules("en-US", { type: "ordinal" }).select(d("22")), "two");
// By the Number value.
shouldBe(new Intl.PluralRules("en-US", { type: "ordinal" }).select(d("9007199254740993")), "two");
shouldBe(plural.select(d("1.00000000000000000001")), "one");
shouldBe(plural.select(d("1e400")), "other");
shouldBe(plural.selectRange(d("1e400"), d("1e401")), plural.selectRange(Infinity, Infinity));
shouldThrow(() => plural.selectRange(d("NaN"), d("1")), RangeError);

for (const receiver of [undefined, 1, "1", {}])
    shouldThrow(() => Decimal.prototype.toLocaleString.call(receiver), TypeError);
shouldThrow(() => d("1").toLocaleString("x"), RangeError);

// The category is that of the Number value, not that of the exact one.
shouldBe(new Intl.PluralRules("en", { maximumFractionDigits: 20 }).select(d("1.00000000000000000001")), "one");
shouldBe(new Intl.PluralRules("en", { maximumFractionDigits: 20 }).select("1.00000000000000000001"), "other");

// The ends of a range are written the same: the category of the ends.
for (const [locale, options, start, end, expected] of [
    ["en", {}, "1", "1", "one"], ["en", {}, "1.0001", "1", "one"], ["en", {}, "1", "2", "other"], ["ar", {}, "0", "0", "zero"], ["ar", {}, "2", "2", "two"],
    ["en", { type: "ordinal" }, "2", "2", "two"], ["en", {}, "Infinity", "Infinity", "other"], ["en", {}, "-0", "0", "other"], ["sl", {}, "1", "2", "two"], ["sl", {}, "101", "3", "few"], ["en", {}, "1e400", "1e401", "other"],
]) {
    const rules = new Intl.PluralRules(locale, options);
    shouldBe(rules.selectRange(d(start), d(end)), expected, `${locale} ${start} ${end}`);
}
shouldBe(new Intl.PluralRules("en").selectRange(d("1"), 1), "one");
shouldBe(new Intl.PluralRules("en").selectRange(1n, d("1")), "one");
for (const [start, end] of [[d("NaN"), d("1")], [d("1"), NaN], [d("1"), d("NaN")]]) {
    let error;
    try {
        new Intl.PluralRules("en").selectRange(start, end);
    } catch (e) {
        error = e;
    }
    shouldBe(error instanceof RangeError, true);
}
