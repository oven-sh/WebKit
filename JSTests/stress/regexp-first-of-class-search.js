//@ runDefault
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")

load("./resources/regexp-reference-matcher.js", "caller relative");

const cases = [
    ["[&<>\"']","","a<b",1,["<"]],
    ["[&<>\"']","","abc",null,null],
    ["[&<>\"']","","",null,null],
    ["[&<>\"']","","\u0126&",1,["&"]],
    ["[&<>\"']","","\u263c\u0126",null,null],
    ["([&<>\"'])","","a<b",1,["<","<"]],
    ["[&<>\"']+","","a<>&b",1,["<>&"]],
    ["[&<>\\u00e9]","","\u00e8\u00ea\u00e9",2,["\u00e9"]],
    ["[&<>\\u00e9]","","\u01e9&",1,["&"]],
    ["[^a-z]","","abc\u0416",3,["\u0416"]],
    ["[?@]","",">A?",2,["?"]],
    ["[?@]","","\u013f\u0140@",2,["@"]],
    ["[&<>\"']*","","a<",0,[""]],
    ["[&<>\"']?x","","ax",1,["x"]],
    ["[&<>\"']|$","","ab",2,[""]],
    ["[&<>a-f]","i","xzF",2,["F"]],
    ["[&<>]","u","ab<",2,["<"]],
    ["^a|b","m","ccb",2,["b"]],
    ["^a|b","m","c\na",2,["a"]],
    ["^a|b","m","cab",2,["b"]],
    ["^a|b","m","cc",null,null],
    ["\\1[&<>]|([&<>])x","","ab<",2,["<",null]],
    ["\\1[&<>]|([&<>])x","","a<x",1,["<",null]],
    ["([&<>])x|\\1[&<>]","","ab<",2,["<",null]],
    ["([&<>])x|\\1[&<>]","","a<x",1,["<x","<"]],
];
checkCases(cases);

const classes = [
    ["[&<>\"']", "&<>\"'", "x\u00e8\u0126\u263c"],
    ["[`{|}~]", "`{~", "x\u00e8\u0160\u017e"],
    ["[&<>a-f]", "&af", "z\u00e8\u0126\u0161"],
    ["[?@]", "?@", ">A\u013f\u0140"],
    ["[\\0\\x7f]", "\0\x7f", "\x01\x7e\x80\u0100\u017f"],
    ["[^a-z]", "&A\u00e8\u0416", "amz"],
    ["[^\\x00-\\x7f]", "\x80\u00ff\u0100\uffff", "x\x7f\0"],
    ["[&<>\\u00e9]", "&\u00e9", "x\u00e8\u00ea\u01e9"],
    ["[&<>\\u0416]", "&\u0416", "x\u00e8\u0415\u0116"],
    ["[\\x80-\\xff]", "\x80\u00ff", "x\x7f\u0100\u0180"],
    ["[^&<>\\u00e9]", "x\u00e8\u0416", "&<>\u00e9"],
    ["\\s", " \n\u00a0\u2028", "x\u00a1\u2027\u0120"],
    ["\\d", "09", "/:\u0130\u0660"],
    ["\\W", "-\u00e8\u0416", "aZ_9"],
    ["[0-9A-Za-z_-]", "a9-", " .\u00e8\u0141"],
    ["[0-9A-Za-z_.-]", "a9.", " !\u00e8\u0141"],
];
const caseSensitiveClasses = ["[&<>a-f]", "[^a-z]"];

const formsForAllLengths = ["C", "(C)", "C+", "Cx"];
const searchableForms = ["(C+)", "C{2}", "(?:C)x", "C$", "C\\b", "(?=C).", "C|C", "(?:C|Cx)", "C(?!x)", "C+?x", "C|Cx|x", "C?x", "(?:C|)x", "x*C", "(?:C)*x", "\\1C|(C)x"];
const unsearchableForms = ["C*", "C?", "C|$", "(?!C).", "^C", "(C)x|\\1C"];
const formsForFewLengths = searchableForms.concat(unsearchableForms);
const allLengths = [1, 2, 15, 16, 17, 18, 31, 32, 33, 34, 48, 49, 65];
const fewLengths = [1, 2, 17, 33];
const sampleLength = 64;

function positionsIn(length) {
    if (length <= 18)
        return Array.from({ length }, (_, position) => position);
    return [...new Set([0, 1, 15, 16, 17, length - 18, length - 17, length - 16, length - 2, length - 1])];
}

function subjectsFor(members, fillers, lengths) {
    const subjects = [fillers[0].repeat(sampleLength), ""];
    for (const filler of fillers) {
        for (const length of lengths) {
            subjects.push(filler.repeat(length));
            for (const position of positionsIn(length))
                subjects.push(filler.repeat(position) + members[position % members.length] + filler.repeat(length - position - 1));
        }
    }
    return subjects;
}

function checkForms(forms, characterClass, flags, subjects, options = { }) {
    for (const form of forms)
        checkRegExp(form.replaceAll("C", () => characterClass), flags, subjects, options);
}

for (const [characterClass, members, fillers] of classes) {
    const fewLengthSubjects = subjectsFor(members, fillers, fewLengths);
    checkForms(formsForAllLengths, characterClass, "", subjectsFor(members, fillers, allLengths));
    checkForms(formsForFewLengths, characterClass, "", fewLengthSubjects);
    checkForms(formsForAllLengths, characterClass, "u", fewLengthSubjects);
    if (caseSensitiveClasses.includes(characterClass))
        checkForms(formsForAllLengths, characterClass, "i", fewLengthSubjects);

    const filler = fillers[0];
    const member = members[0];
    checkForms(["^C|x"], characterClass, "m", subjectsFor(members, "\n" + filler, fewLengths));

    const multipleMemberSubject = filler.repeat(3) + member + filler.repeat(16) + member + filler.repeat(16) + member + filler.repeat(2);
    checkForms(["C", "C+"], characterClass, "", [filler.repeat(sampleLength), multipleMemberSubject], { allStartIndexes: true });
    checkForms(["C", "C+"], characterClass, "", [filler.repeat(1000) + member, filler.repeat(1000)]);
}

const literals = [
    ["abcdefghijkl", "abcdefghijkl"],
    ["abcdefghijklmnopqrst", "abcdefghijklmnopqrst"],
    ["abcdefgh\\u00ffjklmnopqrst", "abcdefgh\u00ffjklmnopqrst"],
    ["abcdefgh\\0jklmnopqrst", "abcdefgh\0jklmnopqrst"],
];
for (const [source, text] of literals) {
    const subjects = [text, "xy" + text + "z", text.slice(1), text.slice(0, -1)];
    for (let position = 0; position < text.length; ++position) {
        const codeUnit = text.charCodeAt(position);
        for (const replacement of [String.fromCharCode(codeUnit + 0x100), String.fromCharCode(codeUnit | 0x8000), "\u01ff", "\uffff", "\0"]) {
            const damaged = text.slice(0, position) + replacement + text.slice(position + 1);
            subjects.push(damaged, damaged + text);
        }
    }
    // Without flags a literal is an atom, found by StringView::find. "m" keeps it in generated code, compared case-sensitively.
    for (const flags of ["", "i", "m"])
        checkRegExp(source, flags, subjects);
}

const usesLeadSurrogatePrefilter = {
    optionalFirstTerm: [
        ["\\u{10000}?\\u{10001}", "u", "\u{10001}", 0, ["\u{10001}"]],
        ["\\u{10000}?\\u{10001}", "u", "\u{10000}\u{10001}", 0, ["\u{10000}\u{10001}"]],
        ["\\u{10000}?\\u{10001}", "u", "x\u{10000}\u{10001}", 1, ["\u{10000}\u{10001}"]],
        ["\\u{10000}?\\u{10001}", "u", "\u{10000}x\u{10001}", 3, ["\u{10001}"]],
        ["\\u{10000}?\\u{10001}", "u", "\u{10000}\u{10000}\u{10001}", 2, ["\u{10000}\u{10001}"]],
        ["\\u{10000}?\\u{10001}", "u", "\u{10400}\u{10001}", 2, ["\u{10001}"]],
        ["\\u{10000}?\\u{10001}", "u", "\ud800\u{10001}", 1, ["\u{10001}"]],
        ["\\u{10000}?\\u{10001}", "u", "\udc01\u{10001}", 1, ["\u{10001}"]],
        ["\\u{10000}?\\u{10001}", "u", "x".repeat(40) + "\u{10001}", 40, ["\u{10001}"]],
        ["\\u{10000}?\\u{10001}", "u", "\u{10000}", null, null],
        ["\\u{10000}?\\u{10001}", "u", "\u{10000}\udc01", null, null],
        ["\\u{10000}?\\u{10001}", "u", "\ud800", null, null],
        ["\\u{10000}?\\u{10001}", "u", "xy\ud800", null, null],
        ["\\u{10000}?\\u{10001}", "u", "xyz", null, null],
        ["\\u{10000}?\\u{10001}", "u", "", null, null],
    ],
    lineStartBeforeFirstTerm: [
        ["^\\u{10000}", "mu", "\u{10000}", 0, ["\u{10000}"]],
        ["^\\u{10000}", "mu", "\u{10000}\u{10000}", 0, ["\u{10000}"]],
        ["^\\u{10000}", "mu", "x\n\u{10000}", 2, ["\u{10000}"]],
        ["^\\u{10000}", "mu", "x\r\u{10000}", 2, ["\u{10000}"]],
        ["^\\u{10000}", "mu", "x\u2028\u{10000}", 2, ["\u{10000}"]],
        ["^\\u{10000}", "mu", "x\u2029\u{10000}", 2, ["\u{10000}"]],
        ["^\\u{10000}", "mu", "\u{10001}\n\u{10000}", 3, ["\u{10000}"]],
        ["^\\u{10000}", "mu", "x\u{10000}\n\u{10000}y", 4, ["\u{10000}"]],
        ["^\\u{10000}", "mu", "x\u{10000}", null, null],
        ["^\\u{10000}", "mu", "\n", null, null],
        ["^\\u{10000}", "mu", "\n\ud800", null, null],
        ["^\\u{10000}", "mu", "\n\ud800x", null, null],
    ],
    wordBoundaryBeforeFirstTerm: [
        ["\\b\\u{10000}", "u", "x\u{10000}", 1, ["\u{10000}"]],
        ["\\b\\u{10000}", "u", "\u{10000}x\u{10000}", 3, ["\u{10000}"]],
        ["\\b\\u{10000}", "u", "\u{10000}", null, null],
        ["\\b\\u{10000}", "u", " \u{10000}", null, null],
        ["\\b\\u{10000}", "u", "x\u{10001}", null, null],
    ],
    lookbehindBeforeFirstTerm: [
        ["(?<=a)\\u{10000}", "u", "a\u{10000}", 1, ["\u{10000}"]],
        ["(?<=a)\\u{10000}", "u", "b\u{10000}a\u{10000}", 4, ["\u{10000}"]],
        ["(?<=a)\\u{10000}", "u", "aa\u{10001}a\u{10000}", 5, ["\u{10000}"]],
        ["(?<=a)\\u{10000}", "u", "\u{10000}", null, null],
        ["(?<=a)\\u{10000}", "u", "\u{10000}\u{10000}", null, null],
        ["(?<=a)\\u{10000}", "u", "a\u{10001}", null, null],
        ["(?<=a)\\u{10000}", "u", "a\ud800", null, null],
    ],
    groupOfAlternatives: [
        ["(?:\\u{10000}|\\u{10001})x", "u", "\u{10000}x", 0, ["\u{10000}x"]],
        ["(?:\\u{10000}|\\u{10001})x", "u", "\u{10001}x", 0, ["\u{10001}x"]],
        ["(?:\\u{10000}|\\u{10001})x", "u", "\u{10000}\u{10001}x", 2, ["\u{10001}x"]],
        ["(?:\\u{10000}|\\u{10001})x", "u", "\u{10000}y\u{10001}x", 3, ["\u{10001}x"]],
        ["(?:\\u{10000}|\\u{10001})x", "u", "\u{10400}x\u{10000}x", 3, ["\u{10000}x"]],
        ["(?:\\u{10000}|\\u{10001})x", "u", "\u{10000}".repeat(20) + "\u{10001}x", 40, ["\u{10001}x"]],
        ["(?:\\u{10000}|\\u{10001})x", "u", "\u{10002}x", null, null],
        ["(?:\\u{10000}|\\u{10001})x", "u", "\u{10000}", null, null],
        ["(?:\\u{10000}|\\u{10001})x", "u", "x", null, null],
        ["(\\u{10000}|\\u{10001})x", "u", "\u{10000}\u{10001}x", 2, ["\u{10001}x", "\u{10001}"]],
    ],
    caseVariants: [
        ["\\u{10400}x", "iu", "\u{10400}x", 0, ["\u{10400}x"]],
        ["\\u{10400}x", "iu", "\u{10428}x", 0, ["\u{10428}x"]],
        ["\\u{10400}x", "iu", "y\u{10428}X", 1, ["\u{10428}X"]],
        ["\\u{10400}x", "iu", "\u{10000}x\u{10428}x", 3, ["\u{10428}x"]],
        ["\\u{10400}x", "iu", "\u{10429}x", null, null],
    ],
};

const skipsLeadSurrogatePrefilter = {
    twoLeadSurrogates: [
        ["\\u{10000}|\\u{10400}", "u", "\u{10000}", 0, ["\u{10000}"]],
        ["\\u{10000}|\\u{10400}", "u", "\u{10400}", 0, ["\u{10400}"]],
        ["\\u{10000}|\\u{10400}", "u", "x\u{10400}", 1, ["\u{10400}"]],
        ["\\u{10000}|\\u{10400}", "u", "\u{10401}\u{10400}", 2, ["\u{10400}"]],
        ["\\u{10000}|\\u{10400}", "u", "\u{10001}\u{10400}\u{10000}", 2, ["\u{10400}"]],
        ["\\u{10000}|\\u{10400}", "u", "\u{10001}\u{10401}", null, null],
        ["\\u{10000}|\\u{10400}", "u", "xy", null, null],
    ],
    bmpMember: [
        ["\\u{10000}|a", "u", "a", 0, ["a"]],
        ["\\u{10000}|a", "u", "ba", 1, ["a"]],
        ["\\u{10000}|a", "u", "b\u{10000}", 1, ["\u{10000}"]],
        ["\\u{10000}|a", "u", "\u{10001}a", 2, ["a"]],
        ["\\u{10000}|a", "u", "a\u{10000}", 0, ["a"]],
        ["\\u{10000}|a", "u", "\u{10000}a", 0, ["\u{10000}"]],
        ["\\u{10000}|a", "u", "\u{10001}", null, null],
        ["\\u{10000}|a", "u", "b", null, null],
    ],
    canMatchEmpty: [
        ["\\u{10000}*", "u", "", 0, [""]],
        ["\\u{10000}*", "u", "x", 0, [""]],
        ["\\u{10000}*", "u", "x\u{10000}", 0, [""]],
        ["\\u{10000}*", "u", "\u{10001}", 0, [""]],
        ["\\u{10000}*", "u", "\u{10000}\u{10000}x", 0, ["\u{10000}\u{10000}"]],
    ],
};

for (const group of [...Object.values(usesLeadSurrogatePrefilter), ...Object.values(skipsLeadSurrogatePrefilter)]) {
    checkCases(group, { skipReference: true });
    checkCases(group.map(([source, flags, ...rest]) => [source, flags.replace("u", "v"), ...rest]), { skipReference: true });
}
