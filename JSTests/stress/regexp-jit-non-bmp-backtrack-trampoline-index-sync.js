/*
 * Regression test for non-BMP first-character optimization match underflow.
 *
 * The BodyAlternativeEnd backtrack trampoline, when looping from a last
 * alternative whose minimumSize is greater than the first alternative's,
 * advanced matchStart by firstCharacterAdditionalReadSize but left
 * m_regs.index unchanged before jumping to beginOp->m_reentry. If the first
 * alternative is a zero-width assertion that succeeds at that position
 * (e.g. \B between the two halves of a surrogate pair), the generated code
 * returned output[0] = matchStart = index + 1 and output[1] = index, so the
 * computed match length was (unsigned)-1 = 4294967295.
 *
 * Requires m_useFirstNonBMPCharacterOptimization (decodeSurrogatePairs &&
 * !inlineTest && !multiline && !containsBOL && !containsLookbehinds &&
 * !containsModifiers), which today is ARM64-only.
 */

function shouldBe(actual, expected, ctx) {
    if (actual !== expected)
        throw new Error("bad value" + (ctx ? " (" + ctx + ")" : "") + ": actual=" + actual + " expected=" + expected);
}

function shouldBeOneOf(actual, expectedList, ctx) {
    for (var i = 0; i < expectedList.length; ++i)
        if (actual === expectedList[i])
            return;
    throw new Error("bad value" + (ctx ? " (" + ctx + ")" : "") + ": actual=" + actual + " expected one of " + expectedList.join(","));
}

for (var i = 0; i < 1e4; ++i) {
    // Zero-width first alternative, variable-width second alternative whose
    // first fixed term reads a surrogate pair right before the trampoline.
    var m = /\B|x{1,2}?/u.exec("a\u{10ffff}b");
    // The interpreter (and engines without the optimization) return an empty
    // match at code-unit index 2; with the optimization, that mid-surrogate
    // position is skipped and no later position satisfies \B. Either outcome
    // is acceptable; what must not happen is start > end.
    if (m !== null) {
        shouldBe(m[0].length, 0, "/\\B|x{1,2}?/u len");
        shouldBe(m.index, 2, "/\\B|x{1,2}?/u index");
    }

    // delta > 1 variant (last alternative minimumSize 2, first 0).
    var m2 = /\B|xy{1,2}?/u.exec("a\u{10ffff}b");
    if (m2 !== null) {
        shouldBe(m2[0].length, 0, "/\\B|xy{1,2}?/u len");
        shouldBe(m2.index, 2, "/\\B|xy{1,2}?/u index");
    }

    // Ensure a plain second alternative that does match still wins at the
    // earliest valid position past the surrogate pair.
    var m3 = /\B|b{1,2}?/u.exec("a\u{10ffff}b");
    if (m3 !== null) {
        shouldBe(typeof m3[0], "string", "/\\B|b{1,2}?/u type");
        // Either the empty \B at 2 (no optimization) or "b" at 3 (optimization).
        if (m3[0] === "")
            shouldBe(m3.index, 2, "/\\B|b{1,2}?/u empty index");
        else {
            shouldBe(m3[0], "b", "/\\B|b{1,2}?/u value");
            shouldBe(m3.index, 3, "/\\B|b{1,2}?/u index");
        }
    } else
        throw new Error("/\\B|b{1,2}?/u should match");

    // Surrogate pair at end of input (exercises the input-check fallthrough).
    var m4 = /\B|x{1,2}?/u.exec("a\u{10ffff}");
    if (m4 !== null) {
        shouldBe(m4[0].length, 0, "/\\B|x{1,2}?/u EOS len");
        shouldBeOneOf(m4.index, [2, 3], "/\\B|x{1,2}?/u EOS index");
    }

    // Sanity: /m disables the optimization, behaviour must still be sound.
    var m5 = /\B|x{1,2}?/mu.exec("a\u{10ffff}b");
    shouldBe(m5[0].length, 0, "mu len");
    shouldBe(m5.index, 2, "mu index");
}
