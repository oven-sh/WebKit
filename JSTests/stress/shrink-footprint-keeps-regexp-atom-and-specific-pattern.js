//@ runDefault("--releaseIdleRegExpCodeWhenShrinkingFootprint=1", "--useConcurrentGC=0")
//@ runDefault("--releaseIdleRegExpCodeWhenShrinkingFootprint=1", "--useConcurrentGC=0", "--useJIT=0")
//@ runDefault("--useConcurrentGC=0")

// Releasing a RegExp's code leaves what describes its pattern alone: the single string it matches (its "atom") and the recognized
// shapes (newlines, leading / trailing spaces). A global match or removal of an atom, a split at newlines and a trim use those
// without going through RegExp::match, so they neither run the code nor count as a use of it. The legacy static properties
// reify a global one-character match from the atom.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

const oneCharacter = /a/g;
const word = /needle/g;
const newlines = /\r\n?|\n/;
const leadingSpaces = /^\s+/;
const trailingSpaces = /\s+$/;
const all = [oneCharacter, word, newlines, leadingSpaces, trailingSpaces];

function throughTheFastPaths() {
    shouldBe("needle in a haystack, needle".match(word).length, 2);
    shouldBe("a needle in a needle case".replace(word, ""), "a  in a  case");
    shouldBe("one\r\ntwo\nthree\rfour".split(newlines).join("|"), "one|two|three|four");
    shouldBe("  \t padded \n ".replace(leadingSpaces, "").replace(trailingSpaces, ""), "padded");
    // Last, so that it is the match on record.
    shouldBe("xaya".match(oneCharacter).length, 2);
}
noInline(throughTheFastPaths);

// Compile all of them.
for (let regExp of all)
    regExp.exec("a needle\n ");
const withCodeBefore = $vm.codeBlockCensus().regExpsWithCode;
shouldBe(withCodeBefore >= all.length, true, "the RegExps have code: " + withCodeBefore);

function shrink(keepCodeInUse, then) {
    // The code was last used before this collection; the fast paths are used after it.
    fullGC();
    throughTheFastPaths();
    const keepCodeThatNeedsParsing = true;
    $vm.shrinkFootprintWhenIdle(keepCodeThatNeedsParsing, keepCodeInUse);
    setTimeout(() => {
        if (!keepCodeInUse || jscOptions().releaseIdleRegExpCodeWhenShrinkingFootprint)
            shouldBe($vm.codeBlockCensus().regExpsWithCode <= withCodeBefore - all.length, true, "the code is gone");
        // "xaya".match(/a/g) recorded that one character matched and left the position for later: the last "a", after "xay".
        shouldBe(RegExp.leftContext, "xay");
        shouldBe(RegExp.lastMatch, "a");
        shouldBe(RegExp["$&"], "a");
        shouldBe(RegExp.rightContext, "");
        shouldBe(RegExp.input, "xaya");
        // Reifying that ran the RegExp. The fast paths do not: nothing is compiled for them.
        const withCodeBeforeTheFastPaths = $vm.codeBlockCensus().regExpsWithCode;
        throughTheFastPaths();
        shouldBe($vm.codeBlockCensus().regExpsWithCode, withCodeBeforeTheFastPaths, "the fast paths need no code");
        shouldBe(RegExp["$`"], "xay");
        for (let regExp of all)
            regExp.exec("a needle\n ");
        shouldBe($vm.codeBlockCensus().regExpsWithCode >= withCodeBefore, true, "and compiles again when it is run");
        then();
    }, 0);
}

// KeepCodeInUse (releases RegExp code only with the option), then the shrink that drops all RegExp code.
shrink(true, () => shrink(false, () => { }));
