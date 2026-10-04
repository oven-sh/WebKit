//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--useAOTInlining=0")
function readsProperty(o) { return o.property; }
readsProperty({ property: 1 });
const usesDataStubs = (aotRemarks("readsProperty") || []).includes("calls:GetById");
(function () {
    function check(actual, expected, what) {
        if (!Object.is(actual, expected))
            throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
    }
    function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
    function applies(name, ...patterns) {
        let remarks = aotRemarks(name);
        for (let pattern of remarks ? patterns : []) {
            if (!remarks.some(remark => matches(remark, pattern)))
                throw new Error(pattern + " does not apply to " + name + ": " + remarks.join(" "));
        }
    }
    const table = { };
    function makeWideAtom(textAndSnowman) {
        const wide = textAndSnowman.slice(0, textAndSnowman.length - 1);
        Symbol(wide);
        check(isRope(wide), false, "the slice is resolved");
        check(is8BitString(wide), false, "the slice shares the 16-bit characters");
        table[wide] = 1;
        check($vm.isAtomString(wide), true, "the key is an atom");
        check(is8BitString(wide), false, "the atom has 16-bit characters");
        return wide;
    }
    function becomesWide(narrow) {
        check(isRope(narrow), false, "the base is resolved");
        check(is8BitString(narrow), true, "the base has 8-bit characters");
        check($vm.isAtomString(narrow), false, "the base is no atom");
        check(table[narrow], 1, "the base is a key of the table");
        check($vm.isAtomString(narrow), true, "the base is an atom now");
        check(is8BitString(narrow), false, "the base has 16-bit characters now");
    }
    function nothing() { try { } catch { } }

    function equalsShort(s) { return s === "ertyui"; }
    function equalsThree(s) { return s === "ert"; }
    function equalsEight(s) { return s === "ertyuiop"; }
    function equalsLong(s) { return s === "ertyuiopasdfg"; }
    function differsFromShort(s) { return s !== "ertyui"; }
    function startsWith(s) { return s.startsWith("ert"); }
    function endsWith(s) { return s.endsWith("yui"); }
    function pick(s) { switch (s) { case "ertyui": return 1; case "x": return 2; case "yy": return 3; default: return 0; } }
    function chain(s) { return s === "qqqqqq" ? 1 : s === "ertyui" ? 2 : s === "zz" ? 3 : s === "yyy" ? 4 : 0; }
    function countInHotLoop(list) {
        let n = 0;
        for (let i = 0; i < list.length; i++) {
            if (list[i] === "ertyui")
                n++;
        }
        return n;
    }
    function countInLoopWithCall(list) {
        let n = 0;
        for (let i = 0; i < list.length; i++) {
            nothing();
            if (list[i] === "ertyui")
                n++;
        }
        return n;
    }

    const keptAlive = [makeWideAtom("qwertyuiopasdfghjkl\u2603")];
    const narrow = "QWERTYUIOPASDFGHJKL".toLowerCase();
    const slice = (start, end) => {
        const result = narrow.slice(start, end);
        check(isRope(result), true, "a slice of a resolved string is a rope");
        return result;
    };
    const six = [slice(2, 8), slice(2, 8), slice(2, 8), slice(2, 8), slice(2, 8), slice(2, 8)];
    const otherSix = [slice(3, 9), slice(3, 9), slice(3, 9), slice(3, 9)];
    const three = slice(2, 5), eight = slice(2, 10), thirteen = slice(2, 15), startsRight = slice(2, 9), startsWrong = slice(3, 9), endsRight = slice(1, 8), endsWrong = slice(1, 9);
    becomesWide(narrow);
    for (let string of [...six, ...otherSix, three, eight, thirteen, startsRight, startsWrong, endsRight, endsWrong])
        check(isRope(string), true, "the slices are still ropes");

    check(equalsShort(six[0]), true, "a slice and a literal of 6 characters");
    check(differsFromShort(six[1]), false, "a slice and a literal of 6 characters, negated");
    check(equalsShort(otherSix[0]), false, "another slice and a literal of 6 characters");
    check(equalsThree(three), true, "a slice and a literal of 3 characters");
    check(equalsEight(eight), true, "a slice and a literal of 8 characters");
    check(equalsLong(thirteen), true, "a slice and a literal of 13 characters");
    check(startsWith(startsRight), true, "startsWith on a slice");
    check(endsWith(endsRight), true, "endsWith on a slice");
    check(startsWith(startsWrong), false, "startsWith on another slice");
    check(endsWith(endsWrong), false, "endsWith on another slice");
    check(pick(six[2]), 1, "switch on a slice");
    check(pick(otherSix[1]), 0, "switch on another slice");
    check(chain(six[3]), 2, "a chain of comparisons with a slice");
    check(countInHotLoop([six[4], "x", otherSix[2]]), 1, "slices in a loop");
    check(countInLoopWithCall([six[5], "x", otherSix[3]]), 1, "slices in a loop with a call");
    check(equalsShort(narrow.slice(2, 8)), true, "a slice made afterwards");
    check(equalsShort(narrow), false, "the base itself");
    check(startsWith(narrow), false, "startsWith on the base");

    function literalInLoopWithCall(list) {
        let n = 0;
        for (let i = 0; i < list.length; i++) {
            nothing();
            if (list[i] === "asdfghjklzxcvbnm")
                n++;
        }
        return n;
    }
    function literalOutsideLoop(s) { return s === "zxcvbnmasdfghjkl"; }
    function literalInHotLoop(list) {
        let n = 0;
        for (let i = 0; i < list.length; i++) {
            if (list[i] === "mnbvcxzlkjhgfdsa")
                n++;
        }
        return n;
    }
    function returnsLiteral() { return "asdfghjklzxcvbnm"; }
    keptAlive.push(makeWideAtom("asdfghjklzxcvbnm\u2603"), makeWideAtom("zxcvbnmasdfghjkl\u2603"), makeWideAtom("mnbvcxzlkjhgfdsa\u2603"));
    const flat = "ASDFGHJKLZXCVBNM".toLowerCase();
    const sliced = "-ASDFGHJKLZXCVBNM-".toLowerCase().slice(1, 17);
    check(is8BitString(flat) && !$vm.isAtomString(flat), true, "an 8-bit string that is no atom");
    check(isRope(sliced), true, "a slice of an 8-bit string");
    check(literalInLoopWithCall([flat, "x", sliced, "ASDFGHJKLZXCVBNN".toLowerCase(), keptAlive[1]]), 3, "a literal whose atom has 16-bit characters, in a loop with a call");
    check(literalOutsideLoop("ZXCVBNMASDFGHJKL".toLowerCase()), true, "a literal whose atom has 16-bit characters");
    check(literalInHotLoop(["MNBVCXZLKJHGFDSA".toLowerCase(), "y"]), 1, "a literal whose atom has 16-bit characters, in a loop");
    if (isAOTCompiled(returnsLiteral))
        check(is8BitString(returnsLiteral()), false, "the literal is the atom that was there first");
    if (usesDataStubs) {
        applies("literalInLoopWithCall", "calls:IsStringEqualTo");
        applies("countInLoopWithCall", "calls:IsStringEqualTo");
        applies("equalsShort", "short-literal-comparison");
        applies("equalsLong", "calls:IsStringEqualToConstant");
        applies("startsWith", "calls:Latin1Characters");
    }
})();
