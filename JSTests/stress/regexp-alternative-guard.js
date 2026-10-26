//@ runDefault
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpAlternativeGuard=0")
//@ runDefault("--useRegExpAlternationDispatch=0")
//@ runDefault("--verifyRegExpJITReads=1")

load("./resources/regexp-reference-matcher.js", "caller relative");

// The RegExp JIT rejects a run of nested alternatives with one test of the next character (a guard), and
// dispatches the alternatives of a once-group on it. Both rely on the alternatives' first-character sets.

// A case is [subject, index, match, ...captures], or [subject, null] where there is no match.
function check(regExp, cases, options)
{
    checkCases(cases.map(([subject, index, ...captures]) => [regExp.source, regExp.flags, subject, index, captures]), options);
}

// A negative lookahead: the guard rejects, passes and no alternative matches, passes and one matches.
check(/^(?:(?!ab|cd|ef|gh)..)*$/, [
    ["xaxcxexg", 0, "xaxcxexg"],
    ["axcxexgx", 0, "axcxexgx"],
    ["axcdexgx", null],
    ["ghxx", null],
    ["xxgh", null],
    ["", 0, ""],
    ["a", null],
]);
check(/(?!ab|cd|ef|gh)[a-h][a-h]/, [
    ["abcdefgh", 1, "bc"],
    ["abcdefgha", 1, "bc"],
    ["hgfedcba", 0, "hg"],
]);
check(/^(?:(?![ab]1|[cd]2|[ef]3|[gh]4)\w\d)+/, [
    ["a2b3c1", 0, "a2b3c1"],
    ["a2b1", 0, "a2"],
    ["z9a1", 0, "z9"],
    ["h4", null],
]);

// Alternatives of different lengths near the end of the subject.
check(/x(?!abc|bc|c|dddd)/, [
    ["xab", 0, "x"],
    ["xabc", null],
    ["xb", 0, "x"],
    ["xbc", null],
    ["xc", null],
    ["x", 0, "x"],
    ["xddd", 0, "x"],
    ["xdddd", null],
]);
check(/(?:(?!abc|bcd|cde|d)[a-d])+$/, [
    ["ab", 0, "ab"],
    ["abc", 1, "bc"],
    ["aab", 0, "aab"],
    ["bc", 0, "bc"],
    ["cd", null],
    ["ccc", 0, "ccc"],
]);

// Only some of the alternatives form a run.
check(/^(?!.z|ab|cd|ef|gh)../, [
    ["az", null],
    ["ab", null],
    ["gh", null],
    ["ax", 0, "ax"],
    ["xx", 0, "xx"],
]);
check(/^(?!ab|cd|ef|gh|.z)../, [
    ["az", null],
    ["ab", null],
    ["gh", null],
    ["ax", 0, "ax"],
    ["xz", null],
    ["xx", 0, "xx"],
]);
check(/^(?!ab|cd|ef|gh|.z|ij|kl|mn|op)../, [
    ["op", null],
    ["oz", null],
    ["ox", 0, "ox"],
    ["ab", null],
    ["xz", null],
    ["xx", 0, "xx"],
    ["ij", null],
]);
check(/^(?!ab|cd|ef|gh||ij)../, [
    ["ab", null],
    ["xx", null],
]);
check(/^(?!\Bq|ab|cd|ef|gh|\b.y)../, [
    ["ab", null],
    ["xy", null],
    ["xx", 0, "xx"],
    ["gh", null],
]);

// What a search tries first at every position.
check(/(?:&amp;|&lt;|&gt;|&quot;|&#39;)+/, [
    ["a &amp;&lt; b", 2, "&amp;&lt;"],
    ["no entities here", null],
    ["&am &gt", null],
    ["&&&&#39;", 3, "&#39;"],
]);
check(/(?:a1|b2|c3|d4)+z/, [
    ["d4d4d4z", 0, "d4d4d4z"],
    ["a1b2c3d4", null],
    ["xxa1zxx", 2, "a1z"],
    ["a1b2z", 0, "a1b2z"],
]);
check(/(?:a1|b2|c3|d4)?z/, [
    ["d4z", 0, "d4z"],
    ["z", 0, "z"],
    ["d4", null],
    ["a1a1z", 2, "a1z"],
]);
check(/(?:a1|b2|c3|d4)*?z/, [
    ["d4a1z", 0, "d4a1z"],
    ["z", 0, "z"],
    ["xa1xz", 4, "z"],
]);
check(/(?:a1|b2|c3|d4){2}/, [
    ["a1", null],
    ["a1d4", 0, "a1d4"],
    ["xa1b2c3", 1, "a1b2"],
]);
check(/(?:a1|b2|c3|d4){1,3}$/, [
    ["a1b2c3d4", 2, "b2c3d4"],
    ["a1x", null],
    ["d4", 0, "d4"],
]);
check(/\b(?:function\b|return\b|typeof\b|delete\b)/, [
    ["a function", 2, "function"],
    ["afunction", null],
    ["returns typeof", 8, "typeof"],
    ["nothing", null],
]);
check(/(?=a1|b2|c3|d4)\w\d/, [
    ["x9d4", 2, "d4"],
    ["a2b3", null],
    ["c3", 0, "c3"],
]);
check(/(?:(?:a1|b2|c3|d4)x|y)+/, [
    ["a1xyd4x", 0, "a1xyd4x"],
    ["a1y", 2, "y"],
    ["yy", 0, "yy"],
    ["zz", null],
]);

// A group matched exactly once, whose minimum size the enclosing alternative claims: the guarded
// alternative claims the rest or nothing, and the run ends the group or does not.
check(/(?:fo+|ba+|qu+)x/, [
    ["foox", 0, "foox"],
    ["zzbax", 2, "bax"],
    ["qux", 0, "qux"],
    ["fox fo", 0, "fox"],
    ["fo ba qu", null],
    ["x", null],
    ["", null],
]);
check(/(?:foo+|ba+|qu+)x/, [
    ["fox foox", 4, "foox"],
    ["zbax", 1, "bax"],
    ["fo", null],
    ["zzz", null],
]);
check(/(?:fo+|ba+|qu+|.z)x!/, [
    ["azx!", 0, "azx!"],
    ["fzx!", 0, "fzx!"],
    ["fox!", 0, "fox!"],
    ["aax!quux!", 4, "quux!"],
    ["fox", null],
]);
check(/(?:.z|foo+|ba+|quu+)x/, [
    ["azx", 0, "azx"],
    ["foox", 0, "foox"],
    ["fox bax", 4, "bax"],
    ["qux", null],
    ["quux", 0, "quux"],
]);
check(/(?:.z|fo+|ba+|qu+)x/, [
    ["azx", 0, "azx"],
    ["fzx", 0, "fzx"],
    ["zzbaax", 2, "baax"],
    ["aax", null],
    ["fo ba qu", null],
]);
check(/(?:.z|fo+|ba+|qu+|.a)x/, [
    ["azx", 0, "azx"],
    ["qux", 0, "qux"],
    ["aax", 0, "aax"],
    ["quz", null],
]);
check(/(?:(fo+)|(ba+)|(qu+))\1x/, [
    ["fofox", 0, "fofox", "fo", undefined, undefined],
    ["bax", 0, "bax", undefined, "ba", undefined],
    ["foofox", null],
]);

// Groups whose alternatives are dispatched on, or follow a dispatch on, the same character.
check(/(?:(?:a1+|b2+|c3+)x|(?:d1+|e2+|f3+)y|(?:g1+|h2+|i3+)z|(?:j1+|k2+|l3+)w)!/, [
    ["b22x!", 0, "b22x!"],
    ["a1y! f3y!", 5, "f3y!"],
    ["l3w!", 0, "l3w!"],
    ["a2x!", null],
    ["m1x!", null],
]);
check(/(?:a1+|b2+|c3+)x|(?:d1+|e2+|f3+)y|(?:g1+|h2+|i3+)z|(?:j1+|k2+|l3+)w/, [
    ["b22x", 0, "b22x"],
    ["a1y f3y", 4, "f3y"],
    ["l3w", 0, "l3w"],
    ["a2x", null],
]);
check(/(?:(?:a1+|b2+|c3+)?x|dd|ee|ff)!/, [
    ["x!", 0, "x!"],
    ["c33x!", 0, "c33x!"],
    ["c3!ee!", 3, "ee!"],
    ["a1!", null],
]);

// Captures in guarded alternatives.
check(/(?:(a)1|(b)2|(c)3|(d)4)+/, [
    ["a1b2", 0, "a1b2", undefined, "b", undefined, undefined],
    ["d4a1", 0, "d4a1", "a", undefined, undefined, undefined],
    ["xxc3", 2, "c3", undefined, undefined, "c", undefined],
    ["a2", null],
]);
check(/(?:(a)1|(b)2|(c)3|.z)+/, [
    ["a1xz", 0, "a1xz", undefined, undefined, undefined],
    ["xzc3", 0, "xzc3", undefined, undefined, "c"],
    ["b2c3xz", 0, "b2c3xz", undefined, undefined, undefined],
]);
check(/^(?:(?!(a)1|(b)2|(c)3|(d)4)..)+/, [
    ["a2b3", 0, "a2b3", undefined, undefined, undefined, undefined],
    ["a2b2", 0, "a2", undefined, undefined, undefined, undefined],
    ["xxd4", 0, "xx", undefined, undefined, undefined, undefined],
]);
check(/(?=(a)1|(b)2|(c)3|(d)4)../, [
    ["xxc3", 2, "c3", undefined, undefined, "c", undefined],
    ["a2", null],
]);
check(/((a)1|(b)2|(c)3|(d)4)*\1/, [
    ["a1b2b2", 0, "a1b2b2", "b2", undefined, "b", undefined, undefined],
    ["d4", 0, "", undefined, undefined, undefined, undefined, undefined],
    ["a1a1a1", 0, "a1a1a1", "a1", "a", undefined, undefined, undefined],
]);

// The first term of an alternative is optional, a group or an assertion.
check(/^(?!a?b|c*d|(?:e|f)g|\bh|(?=i)\w)./, [
    ["b", null],
    ["ab", null],
    ["ax", 0, "a"],
    ["d", null],
    ["ccd", null],
    ["cx", 0, "c"],
    ["eg", null],
    ["fg", null],
    ["ex", 0, "e"],
    ["h", null],
    ["i", null],
    ["j", 0, "j"],
]);
check(/(?:a?b|c*d|(?:e|f)+g|[h-j]{2})+!/, [
    ["abccdefgij!", 0, "abccdefgij!"],
    ["b!", 0, "b!"],
    ["a!", null],
    ["hi", null],
    ["efx!d!", 4, "d!"],
]);

// Case-insensitive patterns.
check(/^(?:(?!ab|cd|ef|gh)..)*$/i, [
    ["XAXC", 0, "XAXC"],
    ["ABxx", null],
    ["xxGh", null],
    ["aXcX", 0, "aXcX"],
]);
check(/(?:\u00e9a|\u00e0b|\u00fcc|\u00f1d)+/i, [
    ["\u00c9A\u00f1D", 0, "\u00c9A\u00f1D"],
    ["\u00e9b", null],
    ["xx\u00dcCxx", 2, "\u00dcC"],
    ["ea", null],
]);
check(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../i, [
    ["K1", null],
    ["\u212a1", 0, "\u212a1"],
    ["S2", null],
    ["\u017f2", 0, "\u017f2"],
    ["\u017f4", null],
    ["s4", 0, "s4"],
    ["\u00c53", null],
    ["\u212b3", 0, "\u212b3"],
    ["x1", 0, "x1"],
]);
check(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../iu, [
    ["K1", null],
    ["\u212a1", null],
    ["S2", null],
    ["\u017f2", null],
    ["\u017f4", null],
    ["s4", null],
    ["\u00c53", null],
    ["\u212b3", null],
    ["x1", 0, "x1"],
]);
check(/x(?:[a-c]lpha|[d-f]eta|\u03b3amma|\u0434elta)y/i, [
    ["xALPHAy", 0, "xALPHAy"],
    ["xEetay", 0, "xEetay"],
    ["x\u0393AMMAy", 0, "x\u0393AMMAy"],
    ["x\u0414eltaY", 0, "x\u0414eltaY"],
    ["xgammay", null],
    ["Xclphay", 0, "Xclphay"],
]);
check(/(^|(?!_)(?:[><\uff5c]|[ \xa0\u2000-\u200a]|[!-#%-*,-\/:;?@\[-\]_{}\u2010-\u2027]|\ud800[\udd00-\udd02]|\ud801\udd6f|\ud802[\udc57\udd1f]|[\0-\x1f\x7f-\x9f]))(http:|https:|ftp:|\/\/|mailto:)/i, [
    ["see HTTP://x", 3, " HTTP:", " ", "HTTP:"],
    ["seehttp://x", 7, "://", ":", "//"],
    ["a_http:", null],
    ["\uff5cftp:", 0, "\uff5cftp:", "\uff5c", "ftp:"],
    ["\ud801\udd6fmailto:", 0, "\ud801\udd6fmailto:", "\ud801\udd6f", "mailto:"],
    ["\ud801mailto:", null],
    ["\u2003//", 0, "\u2003//", "\u2003", "//"],
    ["http:", 0, "http:", "", "http:"],
    ["x\u3042https:", null],
]);

// Characters above Latin-1.
check(/^(?:(?!\u3042a|\u3044b|\u3046c|\u3048d)..)+$/, [
    ["\u3042b\u3044a", 0, "\u3042b\u3044a"],
    ["\u3042a", null],
    ["ab\u3048d", null],
    ["abcd", 0, "abcd"],
]);
check(/^(?:(?!\ud800[\udc00-\udc0f]|\ud801\udc37|\ud802[\udc10-\udc1f]|\ud803\udc00|[\0-\x1f])[^])+$/, [
    ["ab\ud800\udc10", 0, "ab\ud800\udc10"],
    ["ab\ud800\udc05", null],
    ["\ud803\udc00", null],
    ["\ud803", 0, "\ud803"],
    ["a\nb", null],
    ["\u3042\u3044", 0, "\u3042\u3044"],
]);
check(/^(?![\u0100-\u0fff]a|[\u2000-\u2fff]b|xc|yd)../, [
    ["\u0123a", null],
    ["\u0123b", 0, "\u0123b"],
    ["\u2123b", null],
    ["xc", null],
    ["xd", 0, "xd"],
    ["\u3042a", 0, "\u3042a"],
]);

// Sticky and anchored patterns only guard negative lookaheads.
checkRegExp("(?:a1|b2|c3|d4)+", "", ["a1d4x", "xa1"], { allStartIndexes: true });
checkRegExp("(?:(?!a1|b2|c3|d4)..)+", "", ["a2b3c3", "a1"], { allStartIndexes: true });
check(/^(?:a1|b2|c3|d4)+/, [
    ["a1d4x", 0, "a1d4"],
    ["xa1", null],
]);
check(/^(?:a1|b2|c3|d4)+/m, [
    ["x\nd4", 2, "d4"],
    ["xd4", null],
]);

// Lookbehinds next to guarded groups.
check(/(?<=x)(?:a1|b2|c3|d4)+/, [
    ["xa1b2", 1, "a1b2"],
    ["ya1xd4", 4, "d4"],
    ["a1", null],
], { skipReference: true });
check(/(?<!a1|b2|c3|d4)z/, [
    ["a1z", null],
    ["a2z", 2, "z"],
    ["z", 0, "z"],
    ["d4zz", 3, "z"],
], { skipReference: true });
check(/(?<=(?!a1|b2|c3|d4)..)z/, [
    ["a1z", null],
    ["a2z", 2, "z"],
    ["xxd4z", null],
], { skipReference: true });

// Groups nested many levels deep: each level needs the first characters of every level below it.
{
    let letters = "bcdefghijklmnopqrstuvwxy";
    let source = "a0|a1|a2";
    for (let depth = 0; depth < 500; ++depth)
        source = `(?:${source})|${letters[depth % letters.length]}1|${letters[depth % letters.length]}2`;
    check(new RegExp(`(?:${source})+`), [
        ["zzzza2y1b0", 4, "a2y1"],
        ["a3b3y3z1", null],
    ], { skipReference: true });
    check(new RegExp(`(?:${source})z`), [
        ["a2b1y2z", 4, "y2z"],
    ], { skipReference: true });
    check(new RegExp(`^(?:(?!${source})..)*$`), [
        ["a3b3y3z1", 0, "a3b3y3z1"],
        ["a3b3y3a0", null],
    ], { skipReference: true });
    check(new RegExp(`^(?:(?!${source})..)*$`, "i"), [
        ["a3b3Y2", null],
    ], { skipReference: true });
}
