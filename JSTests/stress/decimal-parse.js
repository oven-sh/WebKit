//@ requireOptions("--useDecimal=1")
// Texts are read eight characters at a time. Those of 16-bit characters are read one by one.

const grammar = /^([+-]?)(?:(\d+)\.?(\d*)|\.(\d+))(?:[eE]([+-]?\d+))?$/;
function parse(text) {
    try {
        return new Decimal(text).toExponential();
    } catch (error) {
        return error.name;
    }
}
function check(rope) {
    const text = JSON.parse(JSON.stringify(rope)), actual = parse(text), match = grammar.exec(text);
    if (actual !== parse(("ሴ" + text).slice(1)))
        throw new Error(`${JSON.stringify(text)}: ${actual} of 8-bit characters`);
    if (!match) {
        if (actual !== "SyntaxError")
            throw new Error(`${JSON.stringify(text)}: ${actual}`);
        return;
    }
    const fraction = match[3] ?? match[4], digits = ((match[2] ?? "") + fraction).replace(/^0+/, ""), significant = digits.replace(/0+$/, "");
    if (significant.length > 34)
        return;
    const exponent = Number(match[5] ?? 0) - fraction.length + digits.length - 1;
    const expected = (match[1] === "-" ? "-" : "") + (significant ? significant[0] + (significant.length > 1 ? "." + significant.slice(1) : "") + "e" + exponent : "0e+0");
    if (actual !== expected)
        throw new Error(`${JSON.stringify(text)}: expected ${expected}, got ${actual}`);
}

function checkAll(alphabet, length, prefix = "") {
    check(prefix);
    if (prefix.length < length) {
        for (const character of alphabet)
            checkAll(alphabet, length, prefix + character);
    }
}
checkAll("07.e-", 5);
checkAll("09.E+/:", 4);
checkAll("05.", 7);

// Of every length, with something else at every place, and with zeros from every place on or up to it.
for (let length = 1; length <= 50; ++length) {
    const digits = "1234567891234567891234567891234567891234567891234567".slice(0, length);
    for (const sign of ["", "-", "+"]) {
        check(sign + digits);
        check(sign + "9".repeat(length));
        check(sign + "0".repeat(length));
        for (let place = 0; place <= length; ++place) {
            for (const other of [".", "e", "/", ":", "\0", "\xae", "\xb0", "\xff", " ", ".."]) {
                // A long exponent is out of the domain.
                if (other !== "e" || length - place < 4)
                    check(sign + digits.slice(0, place) + other + digits.slice(place));
            }
            check(sign + digits.slice(0, place) + "0".repeat(length - place));
            check(sign + digits.slice(0, place) + "0".repeat(length - place) + ".");
            check(sign + digits.slice(0, 1) + "." + digits.slice(0, place) + "0".repeat(length - place));
            check(sign + "0".repeat(place) + "." + digits.slice(place) + "e-12");
            check(sign + "." + "0".repeat(place) + digits.slice(place));
        }
    }
}
