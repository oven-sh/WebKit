//@ skip if not $jitTests
//@ runDefault
// Only with the JIT: the interpreter rejects some of these patterns.

// Each alternative claims at most INT_MAX characters, but a nested one claims on top of the ones around it.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

const max = 2147483647;

// The nested claims add up to more than INT_MAX, and something under them adds to the checked offset.
let neverMatches = [
    [`(?:(?:^.)?a{${max}})?b{${max}}`, "m", "ab"],
    [`(?:(?:\\b.)?a{${max}})?b{${max}}`, "", "ab"],
    [`(?:[^x]*a{${max}})?b{${max}}`, "u", "ab\u{1F600}"],
    [`(?<=b{${max}}(?:a{${max}}(?:.$)?)?)`, "m", "ab"],
    [`(?<=b{${max}}(?:a{${max}}(?:.\\b)?)?)`, "", "ab"],
    [`(?:(?:foo|barbaz|quxxxx|zipzap)a{${max - 3}})?b{${max}}`, "", "ab"],
    [`a{${max}}(?=(?:foo|barbaz|quxxxx|zipzap)b{${max - 3}})`, "", "ab"],
    [`(?<=(?=(?:foo|barbaz|quxxxx|zipzap)a{${max - 3}})b{${max}})`, "", "ab"],
];

for (let [source, flags, input] of neverMatches) {
    shouldBe(new RegExp(source, flags).exec(input), null);
    // A different source: the minimum size recorded when exec() compiled would reject the input before test() compiles.
    shouldBe(new RegExp(`(?:${source})`, flags).test(input), false);
}

// The rest of the pattern still matches.
let matches = [
    [`(?:(?:^.)?a{${max}})?b`, "m", "ab", "b"],
    [`(?:(?<=b{${max}})a{${max}})?cc`, "", "cc", "cc"],
    [`(?<!(?:a{${max}})+)bb`, "", "bb", "bb"],
    [`(?:(?:b{${max}})*c{${max}})*aa`, "", "aa", "aa"],
    [`(?:(?<=b{${max}})a{${max}})?cc|(?<=(?=a{${max}}(?:b{${max}})?)c{2})`, "", "cc", "cc"],
];

for (let [source, flags, input, expected] of matches) {
    shouldBe(new RegExp(source, flags).exec(input)[0], expected);
    shouldBe(new RegExp(`(?:${source})`, flags).test(input), true);
}
