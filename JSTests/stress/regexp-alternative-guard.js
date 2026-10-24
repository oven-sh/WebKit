// The RegExp JIT rejects a run of nested alternatives with one test of the next character (a guard), and
// dispatches the alternatives of a once-group on it. Both rely on the alternatives' first-character sets.

function test(regExp, subject, expected)
{
    // The same characters in an 8-bit (where possible) and in a 16-bit string.
    for (let string of [subject, (subject + "\u3042").slice(0, -1)]) {
        regExp.lastIndex = 0;
        let match = regExp.exec(string);
        let actual = match ? [match.index, ...match] : null;
        if (JSON.stringify(actual) !== JSON.stringify(expected))
            throw new Error(`${regExp}.exec(${JSON.stringify(string)}): expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
        regExp.lastIndex = 0;
        if (regExp.test(string) !== !!expected)
            throw new Error(`${regExp}.test(${JSON.stringify(string)}): expected ${!!expected}`);
    }
}

// A negative lookahead: the guard rejects, passes and no alternative matches, passes and one matches.
test(/^(?:(?!ab|cd|ef|gh)..)*$/, "xaxcxexg", [0, "xaxcxexg"]);
test(/^(?:(?!ab|cd|ef|gh)..)*$/, "axcxexgx", [0, "axcxexgx"]);
test(/^(?:(?!ab|cd|ef|gh)..)*$/, "axcdexgx", null);
test(/^(?:(?!ab|cd|ef|gh)..)*$/, "ghxx", null);
test(/^(?:(?!ab|cd|ef|gh)..)*$/, "xxgh", null);
test(/^(?:(?!ab|cd|ef|gh)..)*$/, "", [0, ""]);
test(/^(?:(?!ab|cd|ef|gh)..)*$/, "a", null);
test(/(?!ab|cd|ef|gh)[a-h][a-h]/g, "abcdefgh", [1, "bc"]);
test(/(?!ab|cd|ef|gh)[a-h][a-h]/g, "abcdefgha", [1, "bc"]);
test(/(?!ab|cd|ef|gh)[a-h][a-h]/g, "hgfedcba", [0, "hg"]);
test(/^(?:(?![ab]1|[cd]2|[ef]3|[gh]4)\w\d)+/, "a2b3c1", [0, "a2b3c1"]);
test(/^(?:(?![ab]1|[cd]2|[ef]3|[gh]4)\w\d)+/, "a2b1", [0, "a2"]);
test(/^(?:(?![ab]1|[cd]2|[ef]3|[gh]4)\w\d)+/, "z9a1", [0, "z9"]);
test(/^(?:(?![ab]1|[cd]2|[ef]3|[gh]4)\w\d)+/, "h4", null);

// Alternatives of different lengths near the end of the subject.
test(/x(?!abc|bc|c|dddd)/, "xab", [0, "x"]);
test(/x(?!abc|bc|c|dddd)/, "xabc", null);
test(/x(?!abc|bc|c|dddd)/, "xb", [0, "x"]);
test(/x(?!abc|bc|c|dddd)/, "xbc", null);
test(/x(?!abc|bc|c|dddd)/, "xc", null);
test(/x(?!abc|bc|c|dddd)/, "x", [0, "x"]);
test(/x(?!abc|bc|c|dddd)/, "xddd", [0, "x"]);
test(/x(?!abc|bc|c|dddd)/, "xdddd", null);
test(/(?:(?!abc|bcd|cde|d)[a-d])+$/, "ab", [0, "ab"]);
test(/(?:(?!abc|bcd|cde|d)[a-d])+$/, "abc", [1, "bc"]);
test(/(?:(?!abc|bcd|cde|d)[a-d])+$/, "aab", [0, "aab"]);
test(/(?:(?!abc|bcd|cde|d)[a-d])+$/, "bc", [0, "bc"]);
test(/(?:(?!abc|bcd|cde|d)[a-d])+$/, "cd", null);
test(/(?:(?!abc|bcd|cde|d)[a-d])+$/, "ccc", [0, "ccc"]);

// Only some of the alternatives form a run.
test(/^(?!.z|ab|cd|ef|gh)../, "az", null);
test(/^(?!.z|ab|cd|ef|gh)../, "ab", null);
test(/^(?!.z|ab|cd|ef|gh)../, "gh", null);
test(/^(?!.z|ab|cd|ef|gh)../, "ax", [0, "ax"]);
test(/^(?!.z|ab|cd|ef|gh)../, "xx", [0, "xx"]);
test(/^(?!ab|cd|ef|gh|.z)../, "az", null);
test(/^(?!ab|cd|ef|gh|.z)../, "ab", null);
test(/^(?!ab|cd|ef|gh|.z)../, "gh", null);
test(/^(?!ab|cd|ef|gh|.z)../, "ax", [0, "ax"]);
test(/^(?!ab|cd|ef|gh|.z)../, "xz", null);
test(/^(?!ab|cd|ef|gh|.z)../, "xx", [0, "xx"]);
test(/^(?!ab|cd|ef|gh|.z|ij|kl|mn|op)../, "op", null);
test(/^(?!ab|cd|ef|gh|.z|ij|kl|mn|op)../, "oz", null);
test(/^(?!ab|cd|ef|gh|.z|ij|kl|mn|op)../, "ox", [0, "ox"]);
test(/^(?!ab|cd|ef|gh|.z|ij|kl|mn|op)../, "ab", null);
test(/^(?!ab|cd|ef|gh|.z|ij|kl|mn|op)../, "xz", null);
test(/^(?!ab|cd|ef|gh|.z|ij|kl|mn|op)../, "xx", [0, "xx"]);
test(/^(?!ab|cd|ef|gh|.z|ij|kl|mn|op)../, "ij", null);
test(/^(?!ab|cd|ef|gh||ij)../, "ab", null);
test(/^(?!ab|cd|ef|gh||ij)../, "xx", null);
test(/^(?!\Bq|ab|cd|ef|gh|\b.y)../, "ab", null);
test(/^(?!\Bq|ab|cd|ef|gh|\b.y)../, "xy", null);
test(/^(?!\Bq|ab|cd|ef|gh|\b.y)../, "xx", [0, "xx"]);
test(/^(?!\Bq|ab|cd|ef|gh|\b.y)../, "gh", null);

// What a search tries first at every position.
test(/(?:&amp;|&lt;|&gt;|&quot;|&#39;)+/g, "a &amp;&lt; b", [2, "&amp;&lt;"]);
test(/(?:&amp;|&lt;|&gt;|&quot;|&#39;)+/g, "no entities here", null);
test(/(?:&amp;|&lt;|&gt;|&quot;|&#39;)+/g, "&am &gt", null);
test(/(?:&amp;|&lt;|&gt;|&quot;|&#39;)+/g, "&&&&#39;", [3, "&#39;"]);
test(/(?:a1|b2|c3|d4)+z/, "d4d4d4z", [0, "d4d4d4z"]);
test(/(?:a1|b2|c3|d4)+z/, "a1b2c3d4", null);
test(/(?:a1|b2|c3|d4)+z/, "xxa1zxx", [2, "a1z"]);
test(/(?:a1|b2|c3|d4)+z/, "a1b2z", [0, "a1b2z"]);
test(/(?:a1|b2|c3|d4)?z/, "d4z", [0, "d4z"]);
test(/(?:a1|b2|c3|d4)?z/, "z", [0, "z"]);
test(/(?:a1|b2|c3|d4)?z/, "d4", null);
test(/(?:a1|b2|c3|d4)?z/, "a1a1z", [2, "a1z"]);
test(/(?:a1|b2|c3|d4)*?z/, "d4a1z", [0, "d4a1z"]);
test(/(?:a1|b2|c3|d4)*?z/, "z", [0, "z"]);
test(/(?:a1|b2|c3|d4)*?z/, "xa1xz", [4, "z"]);
test(/(?:a1|b2|c3|d4){2}/, "a1", null);
test(/(?:a1|b2|c3|d4){2}/, "a1d4", [0, "a1d4"]);
test(/(?:a1|b2|c3|d4){2}/, "xa1b2c3", [1, "a1b2"]);
test(/(?:a1|b2|c3|d4){1,3}$/, "a1b2c3d4", [2, "b2c3d4"]);
test(/(?:a1|b2|c3|d4){1,3}$/, "a1x", null);
test(/(?:a1|b2|c3|d4){1,3}$/, "d4", [0, "d4"]);
test(/\b(?:function\b|return\b|typeof\b|delete\b)/, "a function", [2, "function"]);
test(/\b(?:function\b|return\b|typeof\b|delete\b)/, "afunction", null);
test(/\b(?:function\b|return\b|typeof\b|delete\b)/, "returns typeof", [8, "typeof"]);
test(/\b(?:function\b|return\b|typeof\b|delete\b)/, "nothing", null);
test(/(?=a1|b2|c3|d4)\w\d/, "x9d4", [2, "d4"]);
test(/(?=a1|b2|c3|d4)\w\d/, "a2b3", null);
test(/(?=a1|b2|c3|d4)\w\d/, "c3", [0, "c3"]);
test(/(?:(?:a1|b2|c3|d4)x|y)+/, "a1xyd4x", [0, "a1xyd4x"]);
test(/(?:(?:a1|b2|c3|d4)x|y)+/, "a1y", [2, "y"]);
test(/(?:(?:a1|b2|c3|d4)x|y)+/, "yy", [0, "yy"]);
test(/(?:(?:a1|b2|c3|d4)x|y)+/, "zz", null);

// Captures in guarded alternatives.
test(/(?:(a)1|(b)2|(c)3|(d)4)+/, "a1b2", [0, "a1b2", undefined, "b", undefined, undefined]);
test(/(?:(a)1|(b)2|(c)3|(d)4)+/, "d4a1", [0, "d4a1", "a", undefined, undefined, undefined]);
test(/(?:(a)1|(b)2|(c)3|(d)4)+/, "xxc3", [2, "c3", undefined, undefined, "c", undefined]);
test(/(?:(a)1|(b)2|(c)3|(d)4)+/, "a2", null);
test(/^(?:(?!(a)1|(b)2|(c)3|(d)4)..)+/, "a2b3", [0, "a2b3", undefined, undefined, undefined, undefined]);
test(/^(?:(?!(a)1|(b)2|(c)3|(d)4)..)+/, "a2b2", [0, "a2", undefined, undefined, undefined, undefined]);
test(/^(?:(?!(a)1|(b)2|(c)3|(d)4)..)+/, "xxd4", [0, "xx", undefined, undefined, undefined, undefined]);
test(/(?=(a)1|(b)2|(c)3|(d)4)../, "xxc3", [2, "c3", undefined, undefined, "c", undefined]);
test(/(?=(a)1|(b)2|(c)3|(d)4)../, "a2", null);
test(/((a)1|(b)2|(c)3|(d)4)*\1/, "a1b2b2", [0, "a1b2b2", "b2", undefined, "b", undefined, undefined]);
test(/((a)1|(b)2|(c)3|(d)4)*\1/, "d4", [0, "", undefined, undefined, undefined, undefined, undefined]);
test(/((a)1|(b)2|(c)3|(d)4)*\1/, "a1a1a1", [0, "a1a1a1", "a1", "a", undefined, undefined, undefined]);

// The first term of an alternative is optional, a group or an assertion.
test(/^(?!a?b|c*d|(?:e|f)g|\bh|(?=i)\w)./, "b", null);
test(/^(?!a?b|c*d|(?:e|f)g|\bh|(?=i)\w)./, "ab", null);
test(/^(?!a?b|c*d|(?:e|f)g|\bh|(?=i)\w)./, "ax", [0, "a"]);
test(/^(?!a?b|c*d|(?:e|f)g|\bh|(?=i)\w)./, "d", null);
test(/^(?!a?b|c*d|(?:e|f)g|\bh|(?=i)\w)./, "ccd", null);
test(/^(?!a?b|c*d|(?:e|f)g|\bh|(?=i)\w)./, "cx", [0, "c"]);
test(/^(?!a?b|c*d|(?:e|f)g|\bh|(?=i)\w)./, "eg", null);
test(/^(?!a?b|c*d|(?:e|f)g|\bh|(?=i)\w)./, "fg", null);
test(/^(?!a?b|c*d|(?:e|f)g|\bh|(?=i)\w)./, "ex", [0, "e"]);
test(/^(?!a?b|c*d|(?:e|f)g|\bh|(?=i)\w)./, "h", null);
test(/^(?!a?b|c*d|(?:e|f)g|\bh|(?=i)\w)./, "i", null);
test(/^(?!a?b|c*d|(?:e|f)g|\bh|(?=i)\w)./, "j", [0, "j"]);
test(/(?:a?b|c*d|(?:e|f)+g|[h-j]{2})+!/, "abccdefgij!", [0, "abccdefgij!"]);
test(/(?:a?b|c*d|(?:e|f)+g|[h-j]{2})+!/, "b!", [0, "b!"]);
test(/(?:a?b|c*d|(?:e|f)+g|[h-j]{2})+!/, "a!", null);
test(/(?:a?b|c*d|(?:e|f)+g|[h-j]{2})+!/, "hi", null);
test(/(?:a?b|c*d|(?:e|f)+g|[h-j]{2})+!/, "efx!d!", [4, "d!"]);

// Case-insensitive patterns.
test(/^(?:(?!ab|cd|ef|gh)..)*$/i, "XAXC", [0, "XAXC"]);
test(/^(?:(?!ab|cd|ef|gh)..)*$/i, "ABxx", null);
test(/^(?:(?!ab|cd|ef|gh)..)*$/i, "xxGh", null);
test(/^(?:(?!ab|cd|ef|gh)..)*$/i, "aXcX", [0, "aXcX"]);
test(/(?:\u00e9a|\u00e0b|\u00fcc|\u00f1d)+/i, "\u00c9A\u00f1D", [0, "\u00c9A\u00f1D"]);
test(/(?:\u00e9a|\u00e0b|\u00fcc|\u00f1d)+/i, "\u00e9b", null);
test(/(?:\u00e9a|\u00e0b|\u00fcc|\u00f1d)+/i, "xx\u00dcCxx", [2, "\u00dcC"]);
test(/(?:\u00e9a|\u00e0b|\u00fcc|\u00f1d)+/i, "ea", null);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../i, "K1", null);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../i, "\u212a1", [0, "\u212a1"]);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../i, "S2", null);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../i, "\u017f2", [0, "\u017f2"]);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../i, "\u017f4", null);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../i, "s4", [0, "s4"]);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../i, "\u00c53", null);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../i, "\u212b3", [0, "\u212b3"]);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../i, "x1", [0, "x1"]);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../iu, "K1", null);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../iu, "\u212a1", null);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../iu, "S2", null);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../iu, "\u017f2", null);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../iu, "\u017f4", null);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../iu, "s4", null);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../iu, "\u00c53", null);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../iu, "\u212b3", null);
test(/^(?![k-m]1|[s-u]2|\u00e53|\u017f4)../iu, "x1", [0, "x1"]);
test(/x(?:[a-c]lpha|[d-f]eta|\u03b3amma|\u0434elta)y/i, "xALPHAy", [0, "xALPHAy"]);
test(/x(?:[a-c]lpha|[d-f]eta|\u03b3amma|\u0434elta)y/i, "xEetay", [0, "xEetay"]);
test(/x(?:[a-c]lpha|[d-f]eta|\u03b3amma|\u0434elta)y/i, "x\u0393AMMAy", [0, "x\u0393AMMAy"]);
test(/x(?:[a-c]lpha|[d-f]eta|\u03b3amma|\u0434elta)y/i, "x\u0414eltaY", [0, "x\u0414eltaY"]);
test(/x(?:[a-c]lpha|[d-f]eta|\u03b3amma|\u0434elta)y/i, "xgammay", null);
test(/x(?:[a-c]lpha|[d-f]eta|\u03b3amma|\u0434elta)y/i, "Xclphay", [0, "Xclphay"]);
test(/(^|(?!_)(?:[><\uff5c]|[ \xa0\u2000-\u200a]|[!-#%-*,-\/:;?@\[-\]_{}\u2010-\u2027]|\ud800[\udd00-\udd02]|\ud801\udd6f|\ud802[\udc57\udd1f]|[\0-\x1f\x7f-\x9f]))(http:|https:|ftp:|\/\/|mailto:)/gi, "see HTTP://x", [3, " HTTP:", " ", "HTTP:"]);
test(/(^|(?!_)(?:[><\uff5c]|[ \xa0\u2000-\u200a]|[!-#%-*,-\/:;?@\[-\]_{}\u2010-\u2027]|\ud800[\udd00-\udd02]|\ud801\udd6f|\ud802[\udc57\udd1f]|[\0-\x1f\x7f-\x9f]))(http:|https:|ftp:|\/\/|mailto:)/gi, "seehttp://x", [7, "://", ":", "//"]);
test(/(^|(?!_)(?:[><\uff5c]|[ \xa0\u2000-\u200a]|[!-#%-*,-\/:;?@\[-\]_{}\u2010-\u2027]|\ud800[\udd00-\udd02]|\ud801\udd6f|\ud802[\udc57\udd1f]|[\0-\x1f\x7f-\x9f]))(http:|https:|ftp:|\/\/|mailto:)/gi, "a_http:", null);
test(/(^|(?!_)(?:[><\uff5c]|[ \xa0\u2000-\u200a]|[!-#%-*,-\/:;?@\[-\]_{}\u2010-\u2027]|\ud800[\udd00-\udd02]|\ud801\udd6f|\ud802[\udc57\udd1f]|[\0-\x1f\x7f-\x9f]))(http:|https:|ftp:|\/\/|mailto:)/gi, "\uff5cftp:", [0, "\uff5cftp:", "\uff5c", "ftp:"]);
test(/(^|(?!_)(?:[><\uff5c]|[ \xa0\u2000-\u200a]|[!-#%-*,-\/:;?@\[-\]_{}\u2010-\u2027]|\ud800[\udd00-\udd02]|\ud801\udd6f|\ud802[\udc57\udd1f]|[\0-\x1f\x7f-\x9f]))(http:|https:|ftp:|\/\/|mailto:)/gi, "\ud801\udd6fmailto:", [0, "\ud801\udd6fmailto:", "\ud801\udd6f", "mailto:"]);
test(/(^|(?!_)(?:[><\uff5c]|[ \xa0\u2000-\u200a]|[!-#%-*,-\/:;?@\[-\]_{}\u2010-\u2027]|\ud800[\udd00-\udd02]|\ud801\udd6f|\ud802[\udc57\udd1f]|[\0-\x1f\x7f-\x9f]))(http:|https:|ftp:|\/\/|mailto:)/gi, "\ud801mailto:", null);
test(/(^|(?!_)(?:[><\uff5c]|[ \xa0\u2000-\u200a]|[!-#%-*,-\/:;?@\[-\]_{}\u2010-\u2027]|\ud800[\udd00-\udd02]|\ud801\udd6f|\ud802[\udc57\udd1f]|[\0-\x1f\x7f-\x9f]))(http:|https:|ftp:|\/\/|mailto:)/gi, "\u2003//", [0, "\u2003//", "\u2003", "//"]);
test(/(^|(?!_)(?:[><\uff5c]|[ \xa0\u2000-\u200a]|[!-#%-*,-\/:;?@\[-\]_{}\u2010-\u2027]|\ud800[\udd00-\udd02]|\ud801\udd6f|\ud802[\udc57\udd1f]|[\0-\x1f\x7f-\x9f]))(http:|https:|ftp:|\/\/|mailto:)/gi, "http:", [0, "http:", "", "http:"]);
test(/(^|(?!_)(?:[><\uff5c]|[ \xa0\u2000-\u200a]|[!-#%-*,-\/:;?@\[-\]_{}\u2010-\u2027]|\ud800[\udd00-\udd02]|\ud801\udd6f|\ud802[\udc57\udd1f]|[\0-\x1f\x7f-\x9f]))(http:|https:|ftp:|\/\/|mailto:)/gi, "x\u3042https:", null);

// Characters above Latin-1.
test(/^(?:(?!\u3042a|\u3044b|\u3046c|\u3048d)..)+$/, "\u3042b\u3044a", [0, "\u3042b\u3044a"]);
test(/^(?:(?!\u3042a|\u3044b|\u3046c|\u3048d)..)+$/, "\u3042a", null);
test(/^(?:(?!\u3042a|\u3044b|\u3046c|\u3048d)..)+$/, "ab\u3048d", null);
test(/^(?:(?!\u3042a|\u3044b|\u3046c|\u3048d)..)+$/, "abcd", [0, "abcd"]);
test(/^(?:(?!\ud800[\udc00-\udc0f]|\ud801\udc37|\ud802[\udc10-\udc1f]|\ud803\udc00|[\0-\x1f])[^])+$/, "ab\ud800\udc10", [0, "ab\ud800\udc10"]);
test(/^(?:(?!\ud800[\udc00-\udc0f]|\ud801\udc37|\ud802[\udc10-\udc1f]|\ud803\udc00|[\0-\x1f])[^])+$/, "ab\ud800\udc05", null);
test(/^(?:(?!\ud800[\udc00-\udc0f]|\ud801\udc37|\ud802[\udc10-\udc1f]|\ud803\udc00|[\0-\x1f])[^])+$/, "\ud803\udc00", null);
test(/^(?:(?!\ud800[\udc00-\udc0f]|\ud801\udc37|\ud802[\udc10-\udc1f]|\ud803\udc00|[\0-\x1f])[^])+$/, "\ud803", [0, "\ud803"]);
test(/^(?:(?!\ud800[\udc00-\udc0f]|\ud801\udc37|\ud802[\udc10-\udc1f]|\ud803\udc00|[\0-\x1f])[^])+$/, "a\nb", null);
test(/^(?:(?!\ud800[\udc00-\udc0f]|\ud801\udc37|\ud802[\udc10-\udc1f]|\ud803\udc00|[\0-\x1f])[^])+$/, "\u3042\u3044", [0, "\u3042\u3044"]);
test(/^(?![\u0100-\u0fff]a|[\u2000-\u2fff]b|xc|yd)../, "\u0123a", null);
test(/^(?![\u0100-\u0fff]a|[\u2000-\u2fff]b|xc|yd)../, "\u0123b", [0, "\u0123b"]);
test(/^(?![\u0100-\u0fff]a|[\u2000-\u2fff]b|xc|yd)../, "\u2123b", null);
test(/^(?![\u0100-\u0fff]a|[\u2000-\u2fff]b|xc|yd)../, "xc", null);
test(/^(?![\u0100-\u0fff]a|[\u2000-\u2fff]b|xc|yd)../, "xd", [0, "xd"]);
test(/^(?![\u0100-\u0fff]a|[\u2000-\u2fff]b|xc|yd)../, "\u3042a", [0, "\u3042a"]);

// Sticky and anchored patterns only guard negative lookaheads.
test(/(?:a1|b2|c3|d4)+/y, "a1d4x", [0, "a1d4"]);
test(/(?:a1|b2|c3|d4)+/y, "xa1", null);
test(/^(?:a1|b2|c3|d4)+/, "a1d4x", [0, "a1d4"]);
test(/^(?:a1|b2|c3|d4)+/, "xa1", null);
test(/(?:(?!a1|b2|c3|d4)..)+/y, "a2b3c3", [0, "a2b3"]);
test(/(?:(?!a1|b2|c3|d4)..)+/y, "a1", null);
test(/^(?:a1|b2|c3|d4)+/m, "x\nd4", [2, "d4"]);
test(/^(?:a1|b2|c3|d4)+/m, "xd4", null);

// Lookbehinds next to guarded groups.
test(/(?<=x)(?:a1|b2|c3|d4)+/, "xa1b2", [1, "a1b2"]);
test(/(?<=x)(?:a1|b2|c3|d4)+/, "ya1xd4", [4, "d4"]);
test(/(?<=x)(?:a1|b2|c3|d4)+/, "a1", null);
test(/(?<!a1|b2|c3|d4)z/, "a1z", null);
test(/(?<!a1|b2|c3|d4)z/, "a2z", [2, "z"]);
test(/(?<!a1|b2|c3|d4)z/, "z", [0, "z"]);
test(/(?<!a1|b2|c3|d4)z/, "d4zz", [3, "z"]);
test(/(?<=(?!a1|b2|c3|d4)..)z/, "a1z", null);
test(/(?<=(?!a1|b2|c3|d4)..)z/, "a2z", [2, "z"]);
test(/(?<=(?!a1|b2|c3|d4)..)z/, "xxd4z", null);

// Groups nested many levels deep: each level needs the first characters of every level below it.
{
    let letters = "bcdefghijklmnopqrstuvwxy";
    let source = "a0|a1|a2";
    for (let depth = 0; depth < 500; ++depth)
        source = `(?:${source})|${letters[depth % letters.length]}1|${letters[depth % letters.length]}2`;
    test(new RegExp(`(?:${source})+`), "zzzza2y1b0", [4, "a2y1"]);
    test(new RegExp(`(?:${source})+`), "a3b3y3z1", null);
    test(new RegExp(`(?:${source})z`), "a2b1y2z", [4, "y2z"]);
    test(new RegExp(`^(?:(?!${source})..)*$`), "a3b3y3z1", [0, "a3b3y3z1"]);
    test(new RegExp(`^(?:(?!${source})..)*$`), "a3b3y3a0", null);
    test(new RegExp(`^(?:(?!${source})..)*$`, "i"), "a3b3Y2", null);
}
