//@ runDefault

load("./resources/regexp-reference-matcher.js", "caller relative");

function shouldBe(actual, expected) {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
        throw new Error(`${JSON.stringify(actual)} instead of ${JSON.stringify(expected)}`);
}

function shouldThrow(run, errorClass, messagePart) {
    let error = null;
    try {
        run();
    } catch (thrown) {
        error = thrown;
    }
    if (error === null)
        throw new Error(`${run} did not throw`);
    if (error.constructor !== errorClass || !error.message.includes(messagePart))
        throw error;
}

function withReplacedFunction(object, name, createReplacement, run) {
    const original = object[name];
    object[name] = createReplacement(original);
    try {
        run();
    } finally {
        object[name] = original;
    }
}

const specificationCases = [
    ["((a)|(ab))((c)|(bc))","","abc",0,["abc","a","a",null,"bc",null,"bc"]], ["a[a-z]{2,4}","","abcdefghi",0,["abcde"]], ["a[a-z]{2,4}?","","abcdefghi",0,["abc"]], ["(aa|aabaac|ba|b|c)*","","aabaac",0,["aaba","ba"]],
    ["(z)((a+)?(b+)?(c))*","","zaacbbbcac",0,["zaacbbbcac","z","ac","a",null,"c"]], ["(a*)*","","b",0,["",null]], ["(a*)b\\1+","","baaaac",0,["b",""]], ["(?=(a+))","","baaabac",1,["","aaa"]],
    ["(?=(a+))a*b\\1","","baaabac",3,["aba","a"]], ["(.*?)a(?!(a+)b\\2c)\\2(.*)","","baaabaac",0,["baaabaac","ba",null,"abaac"]], ["a|ab","","abc",0,["a"]], ["^b$","m","a\nb\nc",2,["b"]], [".","","\na",1,["a"]],
    [".","s","\na",0,["\n"]], ["[^a-c]","i","aBd",2,["d"]], ["\\u017f","iu","s",0,["s"]], ["\\u017f","i","s",null,null], ["(?<x>a)\\k<x>","","aa",0,["aa","a"]], ["\\k<x>(?<x>a)","","a",0,["a","a"]],
];
const syntaxCases = [
    ["[a-c-]+","","x-b-d",1,["-b-"]], ["[\\d-x]+","","a1-x2b",1,["1-x2"]], ["[\\b]","","a\bc",1,["\b"]], ["[^]","","\n",0,["\n"]], ["[]","","a",null,null], ["\\x41\\u0042\\0","","AB\0",0,["AB\0"]],
    ["\\t\\n\\v\\f\\r\\/\\.","","\t\n\v\f\r/.",0,["\t\n\v\f\r/."]], ["\\D\\d\\W\\w\\S\\s","","a1-b. ",0,["a1-b. "]], ["a{2}","","aaa",0,["aa"]], ["a{2,}","","aaa",0,["aaa"]], ["a{1,2}?","","aaa",0,["a"]],
    ["\\Bb","","b ab",3,["b"]], ["\\bb","","ab b",3,["b"]], ["(?=(a))?b","","ab",1,["b",null]], ["(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\\10","","abcdefghijj",0,["abcdefghijj","a","b","c","d","e","f","g","h","i","j"]],
    ["a","d","ba",1,["a"]], ["a","u","ba",1,["a"]], ["a","v","ba",1,["a"]],
];
const caseFoldingCases = [
    ["\\u212a","iu","k",0,["k"]], ["\\u212a","i","k",null,null], ["\\w+","iu","\u017f\u212a-",0,["\u017f\u212a"]], ["\\w","i","\u017f\u212a",null,null], ["\\u00b5","i","\u03bc",0,["\u03bc"]], ["\\u00df","i","SS",null,null],
    ["(\\u00e9)\\1","i","\u00e9\u00c9",0,["\u00e9\u00c9","\u00e9"]], ["\\u2d00","i","\u2d00",0,["\u2d00"]], ["\\u2d00","i","\u10a0",0,["\u10a0"]], ["\\u10a0","iu","\u2d00",0,["\u2d00"]], ["\\uff21","i","\uff41",0,["\uff41"]],
    ["[\\uff21-\\uff3a]+","i","\uff41\uff3a",0,["\uff41\uff3a"]], ["[^\\uff21]","i","\uff41\uff42",1,["\uff42"]], ["\\W","iu","iI",null,null], ["\\u0131","iu","i",null,null], ["\\u0131","iu","\u0131",0,["\u0131"]],
];
checkCases(specificationCases);
checkCases(syntaxCases);
checkCases(caseFoldingCases);
checkCases([["(?<=a)b","","ab",1,["b"]]], { skipReference: true });

shouldBe(stringsOver("ab", 2), ["", "a", "b", "aa", "ab", "ba", "bb"]);
shouldBe(stringsOver("ab", 0), [""]);
shouldBe(stringsOver("abc", 3).length, 40);
shouldBe(to16Bit("abc"), "abc");

shouldBe(new ReferenceRegExp("b", "y").exec("ab", 0), null);
shouldBe(new ReferenceRegExp("b", "y").exec("ab", 1), { index: 1, captures: ["b"] });
shouldBe(new ReferenceRegExp("b", "g").exec("abb", 2), { index: 2, captures: ["b"] });
shouldBe(new ReferenceRegExp("", "").exec("ab", 2), { index: 2, captures: [""] });
shouldBe(new ReferenceRegExp("", "").exec("ab", 3), null);
shouldBe(new ReferenceRegExp("(a)|b", "").exec("b").captures[1], undefined);
checkRegExp("a|b+", "", ["xabb"], { allStartIndexes: true });

for (const wrongCase of [["a","","a",0,["b"]], ["a","","b",0,["a"]], ["a","","a",null,null], ["a","","ba",0,["a"]], ["(a)?","","b",0,["",""]], ["(a)?","","b",0,[""]]]) {
    shouldThrow(() => checkCases([wrongCase]), Error, "the reference matcher disagrees with the expected result");
    shouldThrow(() => checkCases([wrongCase], { skipReference: true }), Error, ".exec on");
}
shouldThrow(() => checkCases([["(?<=a)b","","ab",1,["b"]]]), ReferenceRegExp.Unsupported, "lookbehind");
shouldThrow(() => checkCases([["a","g","a",0,["a"]]]), Error, "does not accept");
shouldThrow(() => checkCases([["a","y","a",0,["a"]]]), Error, "does not accept");

const unsupportedSources = [
    "(?<=a)b", "(?<!a)b", "\\p{L}", "\\P{L}", "\\cA", "\\u{61}", "\\u61", "\\x4", "\\01", "[\\1]", "a)", "(a", "[a", "a\\", "\\2(a)", "\\10(a)", "\\k<y>(?<x>a)", "\\ka", "(?<x>a)(?<x>b)", "(?<x",
    "(?i:a)", "a**", "^*", "\\b+", "*", "a{", "a{,2}", "a{2,1}", "{", "[b-a]",
];
for (const source of unsupportedSources)
    shouldThrow(() => new ReferenceRegExp(source, ""), ReferenceRegExp.Unsupported, "does not support");
shouldThrow(() => new ReferenceRegExp("a", "x"), ReferenceRegExp.Unsupported, "flag x");
shouldThrow(() => new ReferenceRegExp("[a]", "v"), ReferenceRegExp.Unsupported, "class");
shouldThrow(() => new ReferenceRegExp(".", "u").exec("\ud83d\ude00"), ReferenceRegExp.Unsupported, "surrogate");
shouldThrow(() => new ReferenceRegExp(".", "v").exec("a\ud83d"), ReferenceRegExp.Unsupported, "surrogate");
shouldBe(new ReferenceRegExp(".", "").exec("\ud83d\ude00"), { index: 0, captures: ["\ud83d"] });
shouldBe(new ReferenceRegExp.Unsupported("") instanceof Error, true);

const addOneToIndex = original => function (text) {
    const match = original.call(this, text);
    if (match !== null)
        ++match.index;
    return match;
};
const negate = original => function (text) {
    return !original.call(this, text);
};
const ignoreLastIndex = original => function (text) {
    this.lastIndex = 0;
    return original.call(this, text);
};
const appendCharacter = original => function (subject) {
    return original(subject + "b");
};
withReplacedFunction(RegExp.prototype, "exec", addOneToIndex, () => shouldThrow(() => checkRegExp("a", "", ["a"]), Error, "/a/.exec on \"a\" from index 0"));
withReplacedFunction(RegExp.prototype, "test", negate, () => shouldThrow(() => checkRegExp("a", "", ["a"]), Error, "/a/.test on \"a\" from index 0"));
withReplacedFunction(RegExp.prototype, "test", negate, () => shouldThrow(() => checkRegExp("a", "", ["b"]), Error, "/a/.test on \"b\" from index 0"));
withReplacedFunction(RegExp.prototype, "exec", ignoreLastIndex, () => {
    checkRegExp("a", "", ["aa"]);
    shouldThrow(() => checkRegExp("a", "", ["aa"], { allStartIndexes: true }), Error, "/a/g.exec on \"aa\" from index 1");
});
withReplacedFunction(globalThis, "to16Bit", appendCharacter, () => shouldThrow(() => checkRegExp("a$", "", ["a"]), Error, "/a$/.exec (16-bit subject) on \"ab\" from index 0"));
checkRegExp("a$", "", ["a", "b", "aa"], { allStartIndexes: true });
