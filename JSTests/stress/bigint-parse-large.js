// Exercises the linear-time BigInt(string) paths: bit packing for power-of-two radixes, and the
// balanced combination of digit-sized parts for the others, around the 4-part threshold at which
// it takes over from the multiply-add loop. The reference is a Horner loop in 9-character chunks,
// which only ever multiplies by a single digit.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected.toString(16).slice(0, 40)}... but got ${actual.toString(16).slice(0, 40)}...`);
}

function shouldThrowSyntaxError(string, message) {
    let threw = false;
    try {
        BigInt(string);
    } catch (error) {
        threw = error instanceof SyntaxError;
    }
    if (!threw)
        throw new Error(`${message}: expected a SyntaxError`);
}

function refParse(string, radix) {
    let value = 0n;
    for (let i = 0; i < string.length; i += 9) {
        const piece = string.slice(i, i + 9);
        value = value * (BigInt(radix) ** BigInt(piece.length)) + BigInt(parseInt(piece, radix));
    }
    return value;
}

const digitChars = "0123456789abcdefghijklmnopqrstuvwxyz";

function makeString(length, radix, seed, shape) {
    let mix = 0x9e3779b9 ^ seed;
    const characters = new Array(length);
    for (let i = 0; i < length; i++) {
        mix = Math.imul(mix ^ (mix >>> 15), 0x2c1b3c6d) >>> 0;
        let value;
        switch (shape) {
        case "random":
            value = mix % radix;
            break;
        case "max":
            value = radix - 1;
            break;
        case "sparse":
            value = (i * 7 + seed) % 11 === 0 ? mix % radix : 0;
            break;
        }
        characters[i] = digitChars[value];
    }
    if (characters[0] === "0")
        characters[0] = "1";
    return characters.join("");
}

const prefixes = { 2: "0b", 8: "0o", 16: "0x" };

// Power-of-two radixes: character counts around every digit boundary, which is where the bits
// of one character straddle two digits for radix 8 and 32.
for (const radix of [2, 8, 16]) {
    const bitsPerChar = Math.log2(radix);
    for (const length of [8, 9, 10, 11, 12, 16, 17, 21, 22, 23, 31, 32, 33, 42, 43, 44, 63, 64, 65, 85, 86, 87, 127, 128, 129, 300, 1000, 1001]) {
        for (const shape of ["random", "max", "sparse"]) {
            const string = makeString(length, radix, length, shape);
            const value = refParse(string, radix);
            const prefix = prefixes[radix];
            shouldBe(BigInt(prefix + string), value, `${length} chars radix ${radix} ${shape}`);
            shouldBe(BigInt(prefix.toUpperCase() + string.toUpperCase()), value, `${length} chars radix ${radix} ${shape} upper case`);
            shouldBe(BigInt(` ${prefix}000${string} `), value, `${length} chars radix ${radix} ${shape} with zeros and spaces`);
            shouldBe(value.toString(radix), string.replace(/^0+(?=.)/, ""), `${length} chars radix ${radix} ${shape} round trip`);
            if (bitsPerChar * length > 32)
                shouldBe(typeof BigInt(prefix + string), "bigint", `${length} chars radix ${radix} ${shape} type`);
        }
    }
}

// Decimal: character counts around the part boundaries (19 characters fit one digit).
for (const length of [10, 11, 18, 19, 20, 37, 38, 39, 56, 57, 58, 75, 76, 77, 95, 96, 100, 190, 191, 192, 400, 1000, 10000]) {
    for (const shape of ["random", "max", "sparse"]) {
        const string = makeString(length, 10, length, shape);
        const value = refParse(string, 10);
        shouldBe(BigInt(string), value, `${length} decimal chars ${shape}`);
        shouldBe(BigInt("-" + string), -value, `-${length} decimal chars ${shape}`);
        shouldBe(BigInt("  +" + string + "\n"), value, `${length} decimal chars ${shape} with sign and spaces`);
        shouldBe(BigInt("0000000" + string), value, `${length} decimal chars ${shape} with leading zeros`);
        shouldBe(value.toString(), string, `${length} decimal chars ${shape} round trip`);
    }
}

// Odd-sized parts: the trailing part is shorter than the others and carries its own multiplier.
for (const length of [77, 78, 79, 80, 153, 154, 155, 156]) {
    const string = makeString(length, 10, length * 3, "random");
    shouldBe(BigInt(string), refParse(string, 10), `${length} decimal chars trailing part`);
}

// An invalid character anywhere in a long string is a SyntaxError, not a partial value.
for (const [string, message] of [
    ["0x" + "f".repeat(100) + "g", "hex with a trailing invalid character"],
    ["0x" + "f".repeat(50) + "g" + "f".repeat(50), "hex with an invalid character in the middle"],
    ["0b" + "1".repeat(200) + "2", "binary with a trailing invalid character"],
    ["0o" + "7".repeat(300) + "8", "octal with a trailing invalid character"],
    ["1".repeat(600) + "a", "decimal with a trailing invalid character"],
    ["1".repeat(600) + " 1", "decimal with a space in the middle"],
    ["1".repeat(600) + "٠", "decimal with a non-ASCII digit"],
    ["0x" + "f".repeat(100) + "\u{1F600}", "hex with a surrogate pair"],
]) {
    shouldThrowSyntaxError(string, message);
}

// Values at and just above the BigInt32 range keep producing the right value.
for (const [string, expected] of [["2147483647", 2147483647n], ["2147483648", 2147483648n], ["-2147483648", -2147483648n], ["-2147483649", -2147483649n], ["0x7fffffff", 2147483647n], ["0x80000000", 2147483648n], ["0b" + "1".repeat(31), 2147483647n], ["0b1" + "0".repeat(31), 2147483648n], ["0o17777777777", 2147483647n], ["0o20000000000", 2147483648n]])
    shouldBe(BigInt(string), expected, `${string} value`);
