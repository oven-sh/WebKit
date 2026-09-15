//@ memoryHog!
//@ slow!
//@ skip if $addressBits <= 32
//@ runDefault

// Full case mapping can make a string longer: "ß" uppercases to "SS", "ﬃ" to "FFI", and "İ"
// lowercases to "i̇" (two code units). When the converted string would be longer than the
// maximum string length, the conversion has to throw. It used to hand back the input unchanged.
// Results between 2^30 and the maximum length are fine and must neither throw nor crash.
// Every block below works on strings of a gigabyte or more, so each takes seconds.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

function shouldThrowOutOfMemory(func) {
    let error = null;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (String(error) !== "RangeError: Out of memory")
        throw new Error(`bad error: ${String(error)}`);
}

const maxLength = 2 ** 31 - 1;

function toUpperCase(string) { return string.toUpperCase(); }
noInline(toUpperCase);
function toLowerCase(string) { return string.toLowerCase(); }
noInline(toLowerCase);

// Warm these up so that the conversions below also run through the DFG's ToUpperCase and
// ToLowerCase nodes, whose slow path is a different function from the baseline one.
for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(toUpperCase("Stra\u00DFe" + i), "STRASSE" + i);
    shouldBe(toLowerCase("\u0130I" + i), "i\u0307i" + i);
}

{
    // 8-bit input. Each "ß" adds one character, so the result would be 2^31 long.
    const input = "\u00DF".repeat(2 ** 30);
    shouldThrowOutOfMemory(() => input.toUpperCase());
    shouldThrowOutOfMemory(() => input.toLocaleUpperCase("en"));
    shouldThrowOutOfMemory(() => toUpperCase(input));
    // The language-sensitive locales convert through ICU into a separate buffer, with the same limit.
    shouldThrowOutOfMemory(() => input.toLocaleUpperCase("lt"));

    // One character less and the result is exactly the maximum length.
    const fits = "\u00DF".repeat(2 ** 30 - 1) + "a";
    const upper = fits.toUpperCase();
    shouldBe(upper.length, maxLength);
    shouldBe(upper[0], "S");
    shouldBe(upper[maxLength - 1], "A");
}
gc();

{
    // 16-bit input through ICU. Each "ﬃ" becomes three characters, 3 * 715827883 = 2^31 + 1, which
    // does not even fit in ICU's int32_t length.
    const input = "\uFB03".repeat(715827883);
    shouldThrowOutOfMemory(() => input.toUpperCase());
}
gc();

{
    // 16-bit input through ICU where the result, 3 * 715827878 + 2 * 4 = 2^31 - 6 characters, fits
    // in ICU's int32_t length but is too long for a 16-bit String.
    const input = "\uFB03".repeat(715827878) + "\u00DF".repeat(4);
    shouldThrowOutOfMemory(() => input.toUpperCase());
}
gc();

{
    // Lowercasing grows too: "İ" (U+0130) becomes "i" followed by U+0307 COMBINING DOT ABOVE.
    const input = "\u0130".repeat(2 ** 30);
    shouldThrowOutOfMemory(() => input.toLowerCase());
    shouldThrowOutOfMemory(() => toLowerCase(input));
}
gc();

{
    // A result of 2^30 characters from a language-sensitive locale, which is fine. "ß" is "SS" under
    // Azerbaijani rules too, and the "i", which becomes "İ", is what sends the string to them.
    const upper = ("\u00DF".repeat(2 ** 29) + "i").toLocaleUpperCase("az");
    shouldBe(upper.length, 2 ** 30 + 1);
    shouldBe(upper[0], "S");
    shouldBe(upper[2 ** 30 - 1], "S");
    shouldBe(upper[2 ** 30], "\u0130");
}
gc();

{
    // 8-bit strings of 2^30 or more characters that have to be widened for ICU: "i" is "İ" in
    // Turkish, and "ÿ" uppercases to "Ÿ" (U+0178) everywhere.
    const dotted = "i".repeat(2 ** 30).toLocaleUpperCase("tr");
    shouldBe(dotted.length, 2 ** 30);
    shouldBe(dotted[2 ** 30 - 1], "\u0130");

    const diaeresis = toUpperCase("a".repeat(2 ** 30) + "\u00FF");
    shouldBe(diaeresis.length, 2 ** 30 + 1);
    shouldBe(diaeresis[0], "A");
    shouldBe(diaeresis[2 ** 30], "\u0178");
}
