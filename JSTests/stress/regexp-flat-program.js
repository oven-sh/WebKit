//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault
//@ runDefault("--useExecutableAllocationFuzz=true", "--fireExecutableAllocationFuzzAtOrAfter=1")
//@ runDefault("--useExecutableAllocationFuzz=true", "--fireExecutableAllocationFuzzRandomly=true")

function shouldBe(actual, expected, label) {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
        throw new Error(label + ": " + JSON.stringify(actual) + " instead of " + JSON.stringify(expected));
}

const cases = [
    ["(?:x*)*y|","","xx",0,[""]],
    ["(?:|x+)+y","","xxy",0,["xxy"]],
    ["(?:(x*)|z)*y","","xxzy",0,["xxzy",null]],
    ["(?:x*|z)*?y","","xxzy",0,["xxzy"]],
    ["(?:(x?)(z?))*y","","xzxy",0,["xzxy","x",""]],
    ["(?:x*){2,}y","","xxy",0,["xxy"]],
    ["(?=(?:x*)*y)x","","xxy",0,["x"]],
    ["(?!(?:x*)*z)x","","xxy",0,["x"]],
    ["\\0*","","\0\0a",0,["\0\0"]],
    ["\\0*","","a\0",0,[""]],
    ["a*","","aa\0",0,["aa"]],
    ["a*","","\0a",0,[""]],
    ["\\0*a*","","\0\0aab",0,["\0\0aa"]],
    ["\\0*a*","","aa\0",0,["aa"]],
    ["a*\\0*b","","xaa\0\0b",1,["aa\0\0b"]],
    ["a*\\0*b","","\0ab",1,["ab"]],
    ["\\0*?a","","\0\0a",0,["\0\0a"]],
    ["\\0+a","","a\0\0a",1,["\0\0a"]],
    ["\\0{2}","","\0\0\0",0,["\0\0"]],
    ["a*ab","","aaab",0,["aaab"]],
    ["a*ab","i","aaab",0,["aaab"]],
    ["a*ab","","ab",0,["ab"]],
    ["a*ab","i","ab",0,["ab"]],
    ["a*ab","","aab",0,["aab"]],
    ["a*ab","i","aab",0,["aab"]],
    ["a*aab","","aab",0,["aab"]],
    ["a*aab","i","aab",0,["aab"]],
    ["a*aab","","ab",null,null],
    ["a*aab","i","ab",null,null],
    ["a+ab","","aab",0,["aab"]],
    ["a+ab","i","aab",0,["aab"]],
    ["a+ab","","ab",null,null],
    ["a+ab","i","ab",null,null],
    ["a*?ab","","aaab",0,["aaab"]],
    ["a*?ab","i","aaab",0,["aaab"]],
    ["a+?ab","","aab",0,["aab"]],
    ["a+?ab","i","aab",0,["aab"]],
    ["a+?ab","","ab",null,null],
    ["a+?ab","i","ab",null,null],
    ["a{0,2}aa","","aaa",0,["aaa"]],
    ["a{0,2}aa","i","aaa",0,["aaa"]],
    ["a{0,2}aa","","aa",0,["aa"]],
    ["a{0,2}aa","i","aa",0,["aa"]],
    ["a{0,2}aa","","a",null,null],
    ["a{0,2}aa","i","a",null,null],
    ["a{1,3}aa","","aaa",0,["aaa"]],
    ["a{1,3}aa","i","aaa",0,["aaa"]],
    ["a{1,3}aa","","aa",null,null],
    ["a{1,3}aa","i","aa",null,null],
    ["a{1,3}?aa","","aaaa",0,["aaa"]],
    ["a{1,3}?aa","i","aaaa",0,["aaa"]],
    ["a{2,}a","","aaa",0,["aaa"]],
    ["a{2,}a","i","aaa",0,["aaa"]],
    ["a{2,}a","","aa",null,null],
    ["a{2,}a","i","aa",null,null],
    ["a{2,}?a","","aaaa",0,["aaa"]],
    ["a{2,}?a","i","aaaa",0,["aaa"]],
    ["(a*)(a{2})","","aaaa",0,["aaaa","aa","aa"]],
    ["(a*)(a{2})","i","aaaa",0,["aaaa","aa","aa"]],
    ["(a*)(a{2})","","aa",0,["aa","","aa"]],
    ["(a*)(a{2})","i","aa",0,["aa","","aa"]],
    ["(a*?)(a{2})","","aaaa",0,["aa","","aa"]],
    ["(a*?)(a{2})","i","aaaa",0,["aa","","aa"]],
    ["(a*)(a*)(a)","","aaa",0,["aaa","aa","","a"]],
    ["(a*)(a*)(a)","i","aaa",0,["aaa","aa","","a"]],
    ["(a*?)(a*?)(a)","","aaa",0,["a","","","a"]],
    ["(a*?)(a*?)(a)","i","aaa",0,["a","","","a"]],
    ["(a?)(a?)(a?)aaa","","aaa",0,["aaa","","",""]],
    ["(a?)(a?)(a?)aaa","i","aaa",0,["aaa","","",""]],
    ["(a?)(a?)(a?)aaa","","aaaa",0,["aaaa","a","",""]],
    ["(a?)(a?)(a?)aaa","i","aaaa",0,["aaaa","a","",""]],
    ["(a?)(a?)(a?)aaa","","aaaaaaa",0,["aaaaaa","a","a","a"]],
    ["(a?)(a?)(a?)aaa","i","aaaaaaa",0,["aaaaaa","a","a","a"]],
    ["(a??)(a??)aa","","aaa",0,["aa","",""]],
    ["(a??)(a??)aa","i","aaa",0,["aa","",""]],
    ["[ab]*abc","","xxabc",2,["abc"]],
    ["[ab]*abc","","abc",0,["abc"]],
    ["[ab]*abc","","ababc",0,["ababc"]],
    ["[ab]*?abc","","ababc",0,["ababc"]],
    ["[a-c]*c[a-c]","","abcab",0,["abca"]],
    ["[a-c]*?c[a-c]","","abcab",0,["abca"]],
    ["[a-c]+bc","","abc",0,["abc"]],
    ["[a-c]+bc","","bc",null,null],
    ["x[^y]*yz","","x12yz",0,["x12yz"]],
    ["x[^z]*yz","","x12yz",0,["x12yz"]],
    ["x[^z]*yz","","xyz",0,["xyz"]],
    ["x[^z]*?yz","","xyyz",0,["xyyz"]],
    ["x\\w*\\d\\d","","xab12",0,["xab12"]],
    ["x\\w*\\d\\d","","x12",0,["x12"]],
    ["x\\w*\\d\\d","","x1",null,null],
    ["x\\w*?\\d\\d","","xa123",0,["xa12"]],
    ["(?:ab)?abc","","abc",0,["abc"]],
    ["(?:ab)?abc","i","abc",0,["abc"]],
    ["(?:ab)?abc","","ababc",0,["ababc"]],
    ["(?:ab)?abc","i","ababc",0,["ababc"]],
    ["(ab)?abc","","abc",0,["abc",null]],
    ["(ab)?abc","i","abc",0,["abc",null]],
    ["(ab)?abc","","ababc",0,["ababc","ab"]],
    ["(ab)?abc","i","ababc",0,["ababc","ab"]],
    ["(ab)??abc","","ababc",0,["ababc","ab"]],
    ["(ab)??abc","i","ababc",0,["ababc","ab"]],
    ["(ab)??abc","","abc",0,["abc",null]],
    ["(ab)??abc","i","abc",0,["abc",null]],
    ["(?:ab)?(?:ab)?ab","","ab",0,["ab"]],
    ["(?:ab)?(?:ab)?ab","i","ab",0,["ab"]],
    ["(?:ab)?(?:ab)?ab","","abab",0,["abab"]],
    ["(?:ab)?(?:ab)?ab","i","abab",0,["abab"]],
    ["(ab)?(ab)?ab","","ababab",0,["ababab","ab","ab"]],
    ["(ab)?(ab)?ab","i","ababab",0,["ababab","ab","ab"]],
    ["(?:ab)*abc","","ababc",0,["ababc"]],
    ["(?:ab)*abc","i","ababc",0,["ababc"]],
    ["(?:ab)*abc","","abc",0,["abc"]],
    ["(?:ab)*abc","i","abc",0,["abc"]],
    ["(?:ab)*abc","","ab",null,null],
    ["(?:ab)*abc","i","ab",null,null],
    ["(?:ab)*?abc","","ababc",0,["ababc"]],
    ["(?:ab)*?abc","i","ababc",0,["ababc"]],
    ["(?:ab){1,3}abc","","ababc",0,["ababc"]],
    ["(?:ab){1,3}abc","i","ababc",0,["ababc"]],
    ["(?:ab){1,3}abc","","abc",null,null],
    ["(?:ab){1,3}abc","i","abc",null,null],
    ["(?:ab){2,}ab","","ababab",0,["ababab"]],
    ["(?:ab){2,}ab","i","ababab",0,["ababab"]],
    ["(?:ab){2,}ab","","abab",null,null],
    ["(?:ab){2,}ab","i","abab",null,null],
    ["(ab)*ab","","ababab",0,["ababab","ab"]],
    ["(ab)*ab","i","ababab",0,["ababab","ab"]],
    ["(ab)*?ab","","ababab",0,["ab",null]],
    ["(ab)*?ab","i","ababab",0,["ab",null]],
    ["(ab)+ab","","abab",0,["abab","ab"]],
    ["(ab)+ab","i","abab",0,["abab","ab"]],
    ["(ab)+ab","","ab",null,null],
    ["(ab)+ab","i","ab",null,null],
    ["(ab|c)*abc","","abcabc",0,["abcabc","c"]],
    ["(ab|c)*abc","i","abcabc",0,["abcabc","c"]],
    ["(?:a|b)*ab","","abab",0,["abab"]],
    ["(?:a|b)*ab","i","abab",0,["abab"]],
    ["(?:a|bc)*abc","","abcabc",0,["abcabc"]],
    ["(?:a|bc)*abc","i","abcabc",0,["abcabc"]],
    ["(?:a|abc)bc","","abc",0,["abc"]],
    ["(?:a|abc)bc","i","abc",0,["abc"]],
    ["(?:abc|a)bc","","abc",0,["abc"]],
    ["(?:abc|a)bc","i","abc",0,["abc"]],
    ["(?:abc|a)bc","","abcbc",0,["abcbc"]],
    ["(?:abc|a)bc","i","abcbc",0,["abcbc"]],
    ["(?:(abc)|(a))bc","","abc",0,["abc",null,"a"]],
    ["(?:(abc)|(a))bc","i","abc",0,["abc",null,"a"]],
    ["(?:(a)|(abc))bc","","abcbc",0,["abc","a",null]],
    ["(?:(a)|(abc))bc","i","abcbc",0,["abc","a",null]],
    ["(?:a|ab|abc)c","","abc",0,["abc"]],
    ["(?:a|ab|abc)c","i","abc",0,["abc"]],
    ["(?:abc|ab|a)c","","ac",0,["ac"]],
    ["(?:abc|ab|a)c","i","ac",0,["ac"]],
    ["(?:abc|ab|a)c","","abcc",0,["abcc"]],
    ["(?:abc|ab|a)c","i","abcc",0,["abcc"]],
    ["(?:|a|aa)aa","","aa",0,["aa"]],
    ["(?:|a|aa)aa","i","aa",0,["aa"]],
    ["(?:aa|a|)aa","","aaa",0,["aaa"]],
    ["(?:aa|a|)aa","i","aaa",0,["aaa"]],
    ["(?:(?:aa|a|)b)*aab","","abaab",0,["abaab"]],
    ["(?:(?:aa|a|)b)*aab","i","abaab",0,["abaab"]],
    ["(?=a*b)a*","","aab",0,["aa"]],
    ["(?=a*b)a*","i","aab",0,["aa"]],
    ["a*(?=ab)ab","","aaab",0,["aaab"]],
    ["a*(?=ab)ab","i","aaab",0,["aaab"]],
    ["(?=(a+))a*b\\1","","baaabac",3,["aba","a"]],
    ["(?=(a+))a*b\\1","i","baaabac",3,["aba","a"]],
    ["(?!a*c)a*b","","aab",0,["aab"]],
    ["(?!a*c)a*b","i","aab",0,["aab"]],
    ["(?=a*ab)a","","aab",0,["a"]],
    ["(?=a*ab)a","i","aab",0,["a"]],
    ["(?=(?:ab)*c)ab","","ababc",0,["ab"]],
    ["(?=(?:ab)*c)ab","i","ababc",0,["ab"]],
    ["a*(?!b)a","","aab",0,["aa"]],
    ["a*(?!b)a","i","aab",0,["aa"]],
    ["(?=(a*))\\1b","","aab",0,["aab","aa"]],
    ["(?=(a*))\\1b","i","aab",0,["aab","aa"]],
    ["(?:a(?=a*b))*ab","","aaab",0,["aaab"]],
    ["(?:a(?=a*b))*ab","i","aaab",0,["aaab"]],
    ["(a*)\\1b","","aaaab",0,["aaaab","aa"]],
    ["(a*)\\1b","i","aaaab",0,["aaaab","aa"]],
    ["(a*)b\\1","","aabaa",0,["aabaa","aa"]],
    ["(a*)b\\1","i","aabaa",0,["aabaa","aa"]],
    ["(a*)\\1\\1","","aaaaaaa",0,["aaaaaa","aa"]],
    ["(a*)\\1\\1","i","aaaaaaa",0,["aaaaaa","aa"]],
    ["(a+)\\1*b","","aaab",0,["aaab","aaa"]],
    ["(a+)\\1*b","i","aaab",0,["aaab","aaa"]],
    ["(a?)\\1a","","aaa",0,["aaa","a"]],
    ["(a?)\\1a","i","aaa",0,["aaa","a"]],
    ["(a*)(?:\\1b)?ab","","aab",0,["aab","a"]],
    ["(a*)(?:\\1b)?ab","i","aab",0,["aab","a"]],
    ["(?:(?:a*)b)*ab","","abaab",0,["ab"]],
    ["(?:(?:a*)b)*ab","i","abaab",0,["ab"]],
    ["(?:a*b)*ab","","ab",0,["ab"]],
    ["(?:a*b)*ab","i","ab",0,["ab"]],
    ["(?:(a*)b)*ab","","aabab",0,["aabab","aa"]],
    ["(?:(a*)b)*ab","i","aabab",0,["aabab","aa"]],
    ["(?:a*?b)*?ab","","aabab",0,["aabab"]],
    ["(?:a*?b)*?ab","i","aabab",0,["aabab"]],
    ["((a*)*)ab","","aaab",0,["aaab","aa","aa"]],
    ["((a*)*)ab","i","aaab",0,["aaab","aa","aa"]],
    ["(?:(?:ab)?c)*abc","","abcabc",0,["abcabc"]],
    ["(?:(?:ab)?c)*abc","i","abcabc",0,["abcabc"]],
    ["(?:x(?:ab)*)*xab","","xabxab",0,["xabxab"]],
    ["(?:x(?:ab)*)*xab","i","xabxab",0,["xabxab"]],
    ["a*b*c*abc","","aabbccabc",0,["aabbccabc"]],
    ["a*b*c*abc","i","aabbccabc",0,["aabbccabc"]],
    ["a*b*c*abc","","abc",0,["abc"]],
    ["a*b*c*abc","i","abc",0,["abc"]],
    ["a*a*a*aaa","","aaa",0,["aaa"]],
    ["a*a*a*aaa","i","aaa",0,["aaa"]],
    ["a*a*a*aaa","","aaaa",0,["aaaa"]],
    ["a*a*a*aaa","i","aaaa",0,["aaaa"]],
    ["a*?a*?a*?aaa","","aaaaa",0,["aaa"]],
    ["a*?a*?a*?aaa","i","aaaaa",0,["aaa"]],
    ["\\d*\\d{3}","","12345",0,["12345"]],
    ["\\d*?\\d{3}","","12345",0,["123"]],
    ["\\d{1,2}\\d{3}","","1234",0,["1234"]],
    ["\\d{1,2}\\d{3}","","123",null,null],
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
    // Shapes with many ways to fail: the flat program gives up early and the bytecode interpreter answers. The answers are the RegExp JIT's.
    const a = count => "a".repeat(count);
    const optional = count => "^" + "a?".repeat(count) + a(count) + "$";
    const manyWays = [
        ["^(?:a?){26}a{26}$", "", a(26), true], ["^(?:a?){26}a{26}$", "", a(25), false], ["^(?:a?){26}a{26}$", "", a(27), true],
        ["^(?:a?){40}a{40}$", "", a(40), true], ["^(?:a?){300}a{300}$", "", a(300), true], ["^(?:a??){26}a{26}$", "", a(26), true],
        [optional(26), "", a(26), true], [optional(30), "", a(30), true], [optional(30), "", a(29), false], [optional(40), "", a(41), true],
        ["^(?:(?:a|)){26}a{26}$", "", a(26), true], ["^(?:(a)?){26}a{26}$", "", a(26), true], ["^(?:a?){26}a{26}$", "i", "A".repeat(26), true],
        ["^(?:Ж?){26}Ж{26}$", "", "Ж".repeat(26), true],
        ["(a*)*b", "", a(18), false], ["(a*)*b", "", a(18) + "b", true], ["(a+)+b", "", a(18), false], ["(a+)+b", "", a(18) + "b", true],
        ["(?:a|a)*b", "", a(18), false], ["(?:a|a)*b", "", a(18) + "b", true], ["(a|aa)+b", "", a(26), false], ["(a|aa)+b", "", a(26) + "b", true],
        ["(?:(?:[^]+)+)*/", "", a(12), false], ["(?:(?:[^]+)+)*/", "", a(12) + "/", true], ["(?:(?:|a+)+)*/", "", a(10), false],
        ["a*a*a*b", "", a(60), false], ["a*a*a*b", "", a(60) + "b", true], ["^(\\w+\\s?)*$", "", "aaa aaa aaa aaa aaa aaa!", false],
        ["((?:((?!0)(?:(){2}|$|[^]+)+)*)*/)*", "m", a(7), true],
    ];
    for (const [source, flags, subject, expected] of manyWays) {
        shouldBe(new RegExp(source, flags).test(subject), expected, "many ways: test of /" + source.slice(0, 40) + "/" + flags);
        const result = new RegExp(source, flags).exec(subject);
        shouldBe(result !== null, expected, "many ways: exec of /" + source.slice(0, 40) + "/" + flags);
    }
    shouldBe(Array.from(/^(?:(a)?){26}a{26}$/.exec(a(26))), [a(26), null], "many ways: captures");

    const kept = new RegExp("^(?:a?){26}a{26}$");
    for (const [subject, expected] of [["b", false], [a(26), true], ["b", false], [a(30), true], [a(26), true], [a(25), false]])
        shouldBe(kept.test(subject), expected, "many ways: one object, " + subject.length + " characters");
    const global = new RegExp("(?:a?){26}a{26}", "g");
    shouldBe(global.test(a(26)), true, "many ways: global");
    shouldBe(global.lastIndex, 26, "many ways: lastIndex");
    shouldBe(global.test(a(26)), false, "many ways: global, from the end");
    shouldBe(global.lastIndex, 0, "many ways: lastIndex is reset");
    shouldBe(a(26).replace(new RegExp("^(?:a?){26}a{26}$"), "[$&]"), "[" + a(26) + "]", "many ways: replace");
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

{
    const maxIndexCount = 0xffff;
    const checkDistinctClasses = classCount => {
        let source = "";
        let subject = "";
        for (let i = 0; i < classCount; ++i) {
            const low = String.fromCharCode(0x100 + (i & 0xff));
            const high = String.fromCharCode(0x1000 + (i >> 8));
            source += "[" + low + high + "]";
            subject += high;
        }
        const regexp = new RegExp(source);
        const label = classCount + " distinct classes";
        const result = regexp.exec("x" + subject);
        shouldBe(result && [result.index, result[0].length], [1, classCount], label);
        shouldBe(regexp.test(subject.slice(1)), false, label + ", a subject one character short");
        shouldBe(regexp.test(subject.slice(0, -1) + "\u1000"), false, label + ", a member of the first class in the last position");
    };
    checkDistinctClasses(maxIndexCount);
    checkDistinctClasses(maxIndexCount + 17);
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
