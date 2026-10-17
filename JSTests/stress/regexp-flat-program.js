//@ runDefault("--useRegExpJIT=0", "--validateRegExpFastMatcher=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault
//@ runDefault("--useExecutableAllocationFuzz=true", "--fireExecutableAllocationFuzzAtOrAfter=1")
//@ runDefault("--useExecutableAllocationFuzz=true", "--fireExecutableAllocationFuzzRandomly=true")

function shouldBe(actual, expected, label) {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
        throw new Error(label + ": " + JSON.stringify(actual) + " instead of " + JSON.stringify(expected));
}

const cases = [
    ["(){2}a","","a",0,["a",""]],
    ["()*a","","a",0,["a",null]],
    ["()+a","","a",0,["a",""]],
    ["()??a","","a",0,["a",null]],
    ["(?:()|(b)){2}a","","a",0,["a","",null]],
    ["(?:$){3}","","ab",2,[""]],
    ["(?:(?=(a))){2}a","","a",0,["a","a"]],
    ["(?:(?=(a)))*a","","a",0,["a",null]],
    ["(?:(?!a)){2}.","","ab",1,["b"]],
    ["(?:\\b){2,}x","","a x",2,["x"]],
    ["(?:(x)|(){2})+y","","xy",0,["xy","x",null]],
    ["(a)\\1*b","","aaab",0,["aaab","a"]],
    ["(a)\\1{2}","","aaaa",0,["aaa","a"]],
    ["(a|b)\\1+?c","","abbbc",1,["bbbc","b"]],
    ["(a?)\\1*b","","b",0,["b",""]],
    ["(ab)\\1{1,2}?$","","ababab",0,["ababab","ab"]],
    ["x.*\\/y","","x/a/b/y/c",0,["x/a/b/y"]],
    ["x.*?\\/y","","x/a/b/y/c/y",0,["x/a/b/y"]],
    ["[a-z]{2,4}?[0-9]","","abcd7",0,["abcd7"]],
    ["[!-~]+ [@-Z]+","","ab~! AZ@[",0,["ab~! AZ@"]],
    ["A\\s\\sM","","x A  M",2,["A  M"]],
    ["a\\s\\s\\sm","i","A M-A   M",4,["A   M"]],
    ["\\d{31}x","","0123456789012345678901234567890x",0,["0123456789012345678901234567890x"]],
    ["\\d{3}-\\d{2}-\\d{4}","","id 123-45-6789.",3,["123-45-6789"]],
    ["([a-c])x[0-9]y","","bx7y",0,["bx7y","b"]],
    ["(x?)(?:\\1a)b","","ab",0,["ab",""]],
    ["(x?)(?:\\1a|c)b","","zab",1,["ab",""]],
    ["()(?:\\1a)b","","ab",0,["ab",""]],
    ["((?!$((()))))((\\1[^])){2}.","","\n b",0,["\n b","",null,null,null," "," "]],
    ["(a?)(?:(?:\\1b)+|c)d","","bbd",0,["bbd",""]],
    ["(a?)x*(?:\\1x)y","","xxy",0,["xxy",""]],
    ["^\\/(?:async\\/([^/]+)$()|users\\/([^/]+)\\/posts\\/([^/]+)$())","","/users/42/posts/7",0,["/users/42/posts/7",null,null,"42","7",""]],
    ["^(?:\\/api(?:\\/projects\\/([^/]+)\\/tasks(?:$()|\\/([^/]+)$())|(?:|\\/.*)$())|.*$())","","/api/projects/3/tasks/317",0,["/api/projects/3/tasks/317","3",null,"317","",null,null]],
    ["^(?:\\/api(?:\\/projects\\/([^/]+)\\/tasks(?:$()|\\/([^/]+)$())|(?:|\\/.*)$())|.*$())","","/api/projects/3/taskz",0,["/api/projects/3/taskz",null,null,null,null,"",null]],
    ["^(?:\\/api(?:\\/projects\\/([^/]+)\\/tasks(?:$()|\\/([^/]+)$())|(?:|\\/.*)$())|.*$())","","/other",0,["/other",null,null,null,null,null,""]],
    ["a*(?:x|(b))c|ab","","abd",0,["ab",null]],
    ["a*(?:(b)c|b)d","","abd",0,["abd",null]],
    ["(z)?a*(?:x|(b))c|(ab)","","abd",0,["ab",null,null,"ab"]],
    ["(a*)?b","","b",0,["b",null]],
    ["(a*)??b","","aab",0,["aab","aa"]],
    ["(?:(a)|b)?c","","bc",0,["bc",null]],
    ["x*(a)?b|x","","xab",0,["xab","a"]],
    ["[^/]+$","","a/bc",2,["bc"]],
    ["\\bfoo\\B","","a foob",2,["foo"]],
    ["^b$","m","a\nb\nc",2,["b"]],
    ["a{2,3}?b","","aaab",0,["aaab"]],
    ["\\u0100+","i","xāĀ",1,["āĀ"]],
    ["[\\u0100-\\u0200]{2}","","Őő",0,["Őő"]],
    ["[^a]","","aሴ",1,["ሴ"]],
    ["(z)((a+)?(b+)?(c))*","","zaacbbbcac",0,["zaacbbbcac","z","ac","a",null,"c"]],
    ["(a*)*","","b",0,["",null]],
    ["(a*)+","","b",0,["",""]],
    ["(a*)+","","aab",0,["aa","aa"]],
    ["(?:a*?)*?b","","aab",0,["aab"]],
    ["(?=(a+))a*b\\1","","baaabac",3,["aba","a"]],
    ["(?=(a+))","","baaabac",1,["","aaa"]],
    ["(.*?)a(?!(a+)b\\2c)\\2(.*)","","baaabaac",0,["baaabaac","ba",null,"abaac"]],
    ["(?:(a)|b)*","","ab",0,["ab",null]],
    ["(?:(a)|(b))+","","ab",0,["ab",null,"b"]],
    ["(a\\1)","","aa",0,["a","a"]],
    ["\\1(a)","","aa",0,["a","a"]],
    ["(?:(a)\\1|b)+","","aabaa",0,["aabaa","a"]],
    ["(?:(?=(a))a|b)*c","","abc",0,["abc",null]],
    ["(?:(?=(a))a|b)*c","","abd abc",4,["abc",null]],
    ["(?:(?!(a))b|a)+","","aba",0,["aba",null]],
    ["(a|ab)(c|bcd)(d*)","","abcd",0,["abcd","a","bcd",""]],
    ["(?:ab|a){2}b","","abab",0,["abab"]],
    ["(?:xy|x){2,3}?yxw","","xyxw",null,null],
    ["((a+){2,3}){2,3}$","","aaaaaa",0,["aaaaaa","aa","a"]],
    ["(?:a|b){0,2}?c","","abc",0,["abc"]],
    ["(?:(a)|(b)|(c)){3}","","abc",0,["abc",null,null,"c"]],
    ["(?:a?){3}b","","ab",0,["ab"]],
    ["(?:(a?)){2,}b","","ab",0,["ab",""]],
    ["(?<year>\\d{4})-(?<month>\\d\\d)","d","on 2026-10",3,["2026-10","2026","10"]],
    ["(?<q>['\"]).*?\\k<q>","","say 'hi' \"x\"",4,["'hi'","'"]],
    ["(a)\\1","i","aA",0,["aA","a"]],
    ["(\\u00e9)\\1","i","éÉ",0,["éÉ","é"]],
    ["^\\/api\\/?(?=\\/|$)","i","/API/users",0,["/API"]],
    ["^\\/?(?=\\/|$)","i","/users",0,[""]],
    ["^\\/users\\/(?:([^\\/]+?))\\/?$","i","/users/42/",0,["/users/42/","42"]],
    ["^(?:\\/users\\/([^\\/]+?))(?:\\/$)?$","i","/users/42",0,["/users/42","42"]],
    ["^ {0,3}(`{3,}(?=[^`\\n]*(?:\\n|$))|~{3,})([^\\n]*)(?:\\n|$)(?:|([\\s\\S]*?)(?:\\n|$))(?: {0,3}\\1[~`]* *(?=\\n|$)|$)","","```js\nlet a\n```\nrest",0,["```js\nlet a\n```","```","js","let a"]],
    ["^(?:[^\\n]+(?:\\n|$))+","","one\ntwo\n\nthree",0,["one\ntwo\n"]],
    ["^(#{1,6})(?: +|$)(.*?)(?: +#+)? *(?:\\n+|$)","","## title ##\nbody",0,["## title ##\n","##","title"]],
    ["(?:\\d{1,3}\\.){3}\\d{1,3}","","at 10.0.12.255 ok",3,["10.0.12.255"]],
    ["^[+-]?\\d*\\.?\\d+(?:[Ee][+-]?\\d+)?$","","-4.5e10",0,["-4.5e10"]],
    ["^[^@\\s]+@[^@\\s]+$","","zoe@example.com",0,["zoe@example.com"]],
    ["(?:(?:a|b)+c)*d","","abcbcd",0,["abcbcd"]],
    ["(?:a(?=b)|b(?!c))+","","ababc",0,["aba"]],
];
for (const [source, flags, subject, index, expected] of cases) {
    for (const text of [subject, subject + "Ж"]) {
        if (text !== subject && /\$|\(\?!|\.\*\)$/.test(source))
            continue;
        const result = new RegExp(source, flags).exec(text);
        shouldBe(result && result.index, index, "index of /" + source + "/" + flags);
        shouldBe(result && Array.from(result, value => value === undefined ? null : value), expected, "/" + source + "/" + flags);
        shouldBe(new RegExp(source, flags).test(text), expected !== null, "test of /" + source + "/" + flags);
    }
}

{
    const sticky = /b/y;
    sticky.lastIndex = 1;
    shouldBe(sticky.exec("abb")[0], "b", "sticky");
    shouldBe(sticky.lastIndex, 2, "sticky lastIndex");
    sticky.lastIndex = 0;
    shouldBe(sticky.exec("abb"), null, "sticky miss");
    shouldBe(sticky.lastIndex, 0, "sticky lastIndex after a miss");
    shouldBe("a-b-c".replace(new RegExp("-", "g"), "+"), "a+b+c", "global");
    shouldBe("a1b22c".split(new RegExp("(\\d)+")), ["a", "1", "b", "2", "c"], "split");
    const dated = new RegExp("(?<year>\\d{4})-(?<month>\\d\\d)", "d").exec("on 2026-10");
    shouldBe(dated.indices.groups.month, [8, 10], "indices");
    shouldBe(dated.groups.year, "2026", "groups");
    shouldBe(new RegExp("(?:a|b)*c").exec("ab".repeat(2000) + "c").index, 0, "a long run of iterations");
    shouldBe(new RegExp("(?:a|b)*c").exec("ab".repeat(2000)), null, "a long run of iterations that fails");
    shouldBe(new RegExp("a?".repeat(1 << 18)).exec("x")[0], "", "a long row of optional characters");
    shouldBe(new RegExp("(?:" + "a?|".repeat(1 << 12) + "b)" + "c?".repeat(1 << 12) + "d").exec("bd")[0], "bd", "a wide alternation ahead of a long row");
}

let seed = typeof fuzzSeed === "number" ? 12345 + fuzzSeed * 7919 : 12345;
function random(n) {
    seed = (Math.imul(seed, 1103515245) + 12345) & 0x7fffffff;
    return (seed >>> 8) % n;
}
const pick = list => list[random(list.length)];
let groups = 0;
function atom(depth) {
    switch (random(depth > 3 ? 7 : 13)) {
    case 0: case 1: return pick(["a", "b", "c", "/", "\\u0100"]);
    case 2: return pick(["[ab]", "[^/]", "[^a]", "\\w", "\\d", "\\s", ".", "[\\u0100-\\u0101a]", "\\W"]);
    case 3: return pick(["^", "$", "\\b", "\\B"]);
    case 4: return pick(["ab", "abc", "/a"]);
    case 5: ++groups; return "()";
    case 6: return groups ? "\\" + (1 + random(groups + 1)) : "a";
    case 7: case 8: ++groups; return "(" + disjunction(depth + 1) + ")";
    case 9: case 10: return "(?:" + disjunction(depth + 1) + ")";
    case 11: return "(?=" + disjunction(depth + 1) + ")";
    default: return "(?!" + disjunction(depth + 1) + ")";
    }
}
function term(depth) {
    const text = atom(depth);
    if (/^(\^|\$|\\b|\\B)$/.test(text) || /^\(\?[=!]/.test(text))
        return text;
    if (/^\\\d+$/.test(text) && random(5))
        return text;
    return text + pick(["", "", "", "", "*", "+", "?", "*?", "+?", "??", "{2}", "{1,2}", "{0,2}?", "{2,}", "{2,3}?"]);
}
function alternative(depth) {
    let text = "";
    for (let n = random(4); n >= 0; --n)
        text += term(depth);
    return text;
}
function disjunction(depth) {
    let text = alternative(depth);
    for (let n = random(3); n > 0 && random(2); --n)
        text += "|" + (random(4) ? alternative(depth) : "");
    return text;
}
function subject() {
    let text = "";
    for (let n = random(12); n > 0; --n)
        text += pick(["a", "a", "b", "c", "/", "\n", " ", "1", "A", "Ā", "ā"]);
    return text;
}

let checksum = 0;
const rounds = typeof fuzzRounds === "number" ? fuzzRounds : 1500;
for (let i = 0; i < rounds; ++i) {
    groups = 0;
    const source = disjunction(0);
    const regexp = new RegExp(source, pick(["", "", "g", "y", "i", "m", "s", "gi", "d"]));
    if (typeof showPatterns !== "undefined")
        print(regexp);
    for (let j = 0; j < 6; ++j) {
        const text = subject();
        regexp.lastIndex = random(3);
        const result = regexp.exec(text);
        if (result) {
            const line = JSON.stringify([result.index, Array.from(result, value => value === undefined ? null : value), regexp.lastIndex]);
            for (let k = 0; k < line.length; ++k)
                checksum = (Math.imul(checksum, 31) + line.charCodeAt(k)) | 0;
        }
        checksum = (Math.imul(checksum, 31) + regexp.lastIndex) | 0;
    }
}
if (typeof showChecksum !== "undefined")
    print(checksum);
