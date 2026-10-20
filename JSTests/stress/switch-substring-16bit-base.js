// The DFG and the FTL read a substring rope that says it is 8-bit in place, from its base. The base can turn 16-bit later:
// used as a property key, it takes the characters of the equal atom, and that atom may be 16-bit.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + String(expected) + " but got " + String(actual));
}

function classify(string) {
    switch (string) {
    case "for":
        return 1;
    case "fort":
        return 2;
    case "forte":
        return 3;
    case "with":
        return 4;
    case "abcdefghij":
        return 5;
    default:
        return 0;
    }
}
noInline(classify);

const wideParts = ["xano", "forte", "for", "with", "abcde", "fghij", "nofortex"];
const narrowParts = ["yano", "forte", "for", "with", "abcde", "fghij", "nofortey"];
const table = {};

const wide = $vm.make16BitStringIfPossible(wideParts.join(""));
table[wide] = true;
shouldBe(is8BitString(wide), false);
table[narrowParts.join("")] = true;

const slices = [[4, 3, 1], [4, 4, 2], [4, 5, 3], [9, 3, 1], [12, 4, 4], [16, 10, 5], [28, 4, 2], [28, 5, 3], [0, 4, 0], [5, 3, 0]];

function check(parts, usesAsKey, baseIs8BitAfterwards) {
    for (const [offset, length, answer] of slices) {
        const base = createNonRopeNonAtomString(parts.join(""));
        shouldBe(is8BitString(base), true);
        const slice = base.substring(offset, offset + length);
        shouldBe(isRope(slice), true);
        shouldBe(is8BitString(slice), true);
        if (usesAsKey) {
            shouldBe(table[base], true);
            shouldBe(is8BitString(base), baseIs8BitAfterwards);
            shouldBe(isRope(slice), true);
            shouldBe(is8BitString(slice), true);
        }
        shouldBe(classify(slice), answer);
    }
}

for (let i = 0; i < testLoopCount; ++i) {
    check(wideParts, false, true);
    check(narrowParts, false, true);
}

for (let i = 0; i < testLoopCount; ++i) {
    check(wideParts, true, false);
    check(narrowParts, true, true);
    check(wideParts, false, true);
}
