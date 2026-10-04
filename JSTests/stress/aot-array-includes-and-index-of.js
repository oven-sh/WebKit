//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(f.name);
    if (!remarks && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + f.name + ": " + remarks.join(" "));
    }
}
function readsProperty(o) { return o.property; }
readsProperty({ property: 1 });
const usesDataStubs = (remarksOf(readsProperty) || []).some(remark => /^calls:(GetById|ReadNameInSlot\d+)$/.test(remark));
const loweredIncludes = "lowered-builtin:Array.prototype.includes", loweredIndexOf = "lowered-builtin:Array.prototype.indexOf";
const includesStub = "calls:ArrayIncludes", indexOfStub = "calls:ArrayIndexOf";
const includesOperation = "calls:operationArrayIncludesValueInt32OrContiguous", indexOfOperation = "calls:operationArrayIndexOfValueInt32OrContiguous";

(function () {
    const other = createGlobalObject();
    function expected(array, needle) {
        let index = -1, isIncluded = false;
        for (let i = 0; i < array.length; ++i) {
            const element = array[i];
            if (index < 0 && i in array && element === needle)
                index = i;
            isIncluded ||= element === needle || (element !== element && needle !== needle);
        }
        return isIncluded + "," + index;
    }
    noInline(expected);

    function ofAnything(needle, which) {
        const object = needles[0], symbol = needles[3];
        const array = which === 0 ? [1, 2, 3] : which === 1 ? [object, 1, "text", symbol, null, true, undefined, false] : which === 2 ? [] : which === 3 ? [1, , 3] : which === 4 ? [object, , null] : which === 5 ? [0, -1, 2147483647, -2147483648]
            : which === 6 ? [1.5, 2.5, NaN] : which === 7 ? ["a", "b", "ab"] : which === 8 ? [10n, object] : [undefined];
        return array.includes(needle) + "," + array.indexOf(needle) + "/" + expected(array, needle);
    }
    const needles = [{}, {}, function () { }, Symbol("a"), Symbol("a"), Symbol.iterator, [], null, undefined, true, false, 0, -0, 1, 2, 3, 4, -1, 2147483647, -2147483648, 2147483648, 1.5, NaN, Infinity, "", "a", "text", "a" + String("b"), 10n, 11n, new Proxy({}, {}), /x/];
    for (let round = 0; round < 3; ++round) {
        for (const needle of needles) {
            for (let which = 0; which < 10; ++which) {
                const [actual, wanted] = ofAnything(needle, which).split("/");
                check(actual, wanted, "searching array " + which + " for " + (typeof needle) + " " + String(needle));
            }
        }
    }
    applies(ofAnything, loweredIncludes, loweredIndexOf);
    if (usesDataStubs)
        applies(ofAnything, includesStub, indexOfStub);
    else {
        applies(ofAnything, includesOperation, indexOfOperation);
        doesNotApply(ofAnything, includesStub, indexOfStub);
    }

    function forInt32(i) { const array = [1, 2, 3]; return array.includes(i & 3) + "," + array.indexOf(i & 3); }
    function forObject(flag) { const o = {}, p = {}; const array = [{}, o, {}]; const needle = flag ? o : p; return array.includes(needle) + "," + array.indexOf(needle); }
    function forBoolean(x) { const array = [0, false, "true"]; return array.includes(x > 1) + "," + array.indexOf(x > 1); }
    function forNull() { const array = [undefined, 0, null]; return array.includes(null) + "," + array.indexOf(null); }
    function forSymbol() { const array = [Symbol.iterator, Symbol.hasInstance]; return array.includes(Symbol.hasInstance) + "," + array.indexOf(Symbol.hasInstance) + "," + array.includes(Symbol.match) + "," + array.indexOf(Symbol.match); }
    function forString(i) { const array = ["a", "b", "c"]; const needle = String("abcd"[i & 3]); return array.includes(needle) + "," + array.indexOf(needle); }
    function forRope(a, b) { const array = ["ab", "cd"]; const needle = String(a) + String(b); return array.includes(needle) + "," + array.indexOf(needle); }
    function forBigInt() { const array = [1n, 2n ** 70n]; return array.includes(1180591620717411303424n) + "," + array.indexOf(1180591620717411303424n); }
    function forUndefined() { const array = [1, , undefined]; return array.includes(undefined) + "," + array.indexOf(undefined); }
    function forUndefinedWithoutAny() { const array = [1, null]; return array.includes(undefined) + "," + array.indexOf(undefined); }
    function forHalf(x) { const array = [1, 2, 3]; return array.includes(x / 2) + "," + array.indexOf(x / 2); }
    for (let i = 0; i < 100; ++i) {
        check(forInt32(i), i & 3 ? "true," + ((i & 3) - 1) : "false,-1", "an integer");
        check(forObject(i & 1), i & 1 ? "true,1" : "false,-1", "an object");
        check(forBoolean(i & 3), (i & 3) > 1 ? "false,-1" : "true,1", "a boolean");
        check(forNull(), "true,2", "null");
        check(forSymbol(), "true,1,false,-1", "a symbol");
        check(forString(i), (i & 3) < 3 ? "true," + (i & 3) : "false,-1", "a string");
        check(forRope("a", "b"), "true,0", "a rope");
        check(forRope("c", "e"), "false,-1", "a rope that is not there");
        check(forBigInt(), "true,1", "a BigInt");
        check(forUndefined(), "true,2", "undefined, with a hole before it");
        check(forUndefinedWithoutAny(), "false,-1", "undefined that is not there");
        check(forHalf(i & 7), (i & 7) === 2 ? "true,0" : (i & 7) === 4 ? "true,1" : (i & 7) === 6 ? "true,2" : "false,-1", "a quotient");
    }
    for (const f of [forInt32, forObject, forBoolean, forNull, forSymbol, forString, forRope, forBigInt, forUndefined, forHalf])
        applies(f, loweredIncludes, loweredIndexOf);
    if (usesDataStubs) {
        for (const f of [forInt32, forObject, forBoolean, forNull, forSymbol, forHalf, forString, forRope]) {
            applies(f, includesStub, indexOfStub);
            doesNotApply(f, includesOperation, indexOfOperation);
        }
        applies(forBigInt, includesOperation, indexOfOperation);
        doesNotApply(forBigInt, includesStub, indexOfStub);
        for (const f of [forUndefined, forUndefinedWithoutAny]) {
            applies(f, includesOperation, indexOfStub);
            doesNotApply(f, includesStub, indexOfOperation);
        }
    }

    function made(text) { return text.split("").join(""); }
    function ropeOf(text) { return text.substring(0, 1) + text.substring(1); }
    noInline(made);
    noInline(ropeOf);
    function amongLiterals(needle) { const array = ["get", "post", "put", "delete", "", "caf\xe9", "\u03b1\u03b2"]; return array.includes(needle) + "," + array.indexOf(needle); }
    function amongMadeStrings(needle) { const array = [made("get"), made("post"), made("put"), made("delete"), made("caf\xe9"), made("\u03b1\u03b2")]; return array.includes(needle) + "," + array.indexOf(needle); }
    function amongRopes(needle) { const array = ["x", ropeOf("get"), ropeOf("post-and-more")]; return array.includes(needle) + "," + array.indexOf(needle); }
    function amongOtherThings(needle) { const array = [1, , null, {}, undefined, true, Symbol.iterator, 1.5, 10n, "last"]; return array.includes(needle) + "," + array.indexOf(needle); }
    function amongIntegers(needle) { const array = [1, 2, 3]; return array.includes(needle) + "," + array.indexOf(needle); }
    for (let i = 0; i < 100; ++i) {
        const literals = ["get", "post", "put", "delete", "", "caf\xe9", "\u03b1\u03b2"];
        for (let position = 0; position < literals.length; ++position) {
            const text = literals[position];
            for (const needle of [text, made(text), text.length > 1 ? ropeOf(text) : text, String(text), (text + "!").slice(0, -1)]) {
                check(amongLiterals(needle), "true," + position, "a string among literals");
                if (text.length)
                    check(amongMadeStrings(needle), "true," + (position > 4 ? position - 1 : position), "a string among strings that were made");
            }
        }
        for (const needle of ["ge", "gets", "got", made("got"), ropeOf("got"), "GET", "pot", made("pos"), "caf\xe8", "cafe", "\u03b1\u03b3", made("\u03b1\u03b3"), " ", "undefined", "null", "1"]) {
            check(amongLiterals(needle), "false,-1", "a string that is not among literals");
            check(amongMadeStrings(needle), "false,-1", "a string that is not among strings that were made");
            check(amongOtherThings(needle), "false,-1", "a string that is not among other things");
            check(amongIntegers(needle), "false,-1", "a string among integers");
        }
        check(amongRopes("get"), "true,1", "a literal among ropes");
        check(amongRopes(made("get")), "true,1", "a string that was made among ropes");
        check(amongRopes(ropeOf("post-and-more")), "true,2", "a rope among ropes");
        check(amongRopes("x"), "true,0", "a literal before ropes");
        check(amongRopes("got"), "false,-1", "a literal that is not among ropes");
        check(amongOtherThings("last"), "true,9", "a string after other things");
        check(amongOtherThings(made("last")), "true,9", "a string that was made after other things");
    }
    if (usesDataStubs) {
        for (const f of [amongLiterals, amongMadeStrings, amongRopes, amongOtherThings, amongIntegers])
            applies(f, includesStub, indexOfStub);
    }

    function doublesAmongOthers(x, needle) {
        const array = ["text", x * 0.5, x * -0, x / 0.5, {}];
        return array.includes(needle) + "," + array.indexOf(needle);
    }
    for (let i = 0; i < 100; ++i) {
        check(doublesAmongOthers(2, 1), "true,1", "an integer among doubles that are integers");
        check(doublesAmongOthers(2, 0), "true,2", "zero when the array holds negative zero");
        check(doublesAmongOthers(2, 4), "true,3", "an integer after doubles");
        check(doublesAmongOthers(2, 5), "false,-1", "an integer that is not among the doubles");
        check(doublesAmongOthers(3, 1), "false,-1", "an integer when the double is not one");
        check(doublesAmongOthers(3, 1.5), "true,1", "a double");
        check(doublesAmongOthers(2, "text"), "true,0", "a string before doubles");
    }

    function ofLength(length, position, kind) {
        const array = [];
        const needle = kind === 0 ? -7 : kind === 1 ? {} : kind === 2 ? Symbol() : true;
        for (let i = 0; i < length; ++i)
            array.push(kind === 0 ? i : kind === 1 ? {} : kind === 2 ? Symbol() : false);
        if (position >= 0)
            array[position] = needle;
        return array.includes(needle) + "," + array.indexOf(needle);
    }
    for (const length of [1, 2, 3, 63, 64, 65, 66, 127, 128, 1000]) {
        for (let kind = 0; kind < 4; ++kind) {
            check(ofLength(length, -1, kind), "false,-1", "a needle that is not among " + length);
            for (const position of [0, length >> 1, length - 1])
                check(ofLength(length, position, kind), "true," + position, "a needle at " + position + " of " + length);
        }
    }

    function firstOfSeveral(o) { const array = [1, o, 2, o, 1, 2]; return array.indexOf(o) + "," + array.indexOf(1) + "," + array.indexOf(2); }
    function afterShrinking(o) { const array = [1, 2, o, 3]; array.length = 2; return array.includes(o) + "," + array.indexOf(o) + "," + array.includes(3) + "," + array.indexOf(3); }
    function afterPopping(o) { const array = [1, o]; array.pop(); return array.includes(o) + "," + array.indexOf(o); }
    function withStart(o) { const array = [o, 1, o]; return array.includes(o, 1) + "," + array.indexOf(o, 1) + "," + array.includes(1, 2) + "," + array.indexOf(1, 2) + "," + array.indexOf(o, -1); }
    function withoutNeedle() { const array = [1, undefined], holey = [1, , 2]; return array.includes() + "," + array.indexOf() + "," + holey.includes() + "," + holey.indexOf(); }
    function ofFrozen(o) { const array = Object.freeze([1, o]); return array.includes(o) + "," + array.indexOf(o) + "," + array.includes(1) + "," + array.indexOf(2); }
    function ofSparse(o) { const array = [o]; array[100000] = 1; return array.includes(o) + "," + array.indexOf(o) + "," + array.includes(1) + "," + array.indexOf(1) + "," + array.includes(undefined) + "," + array.indexOf(undefined); }
    function ofDoubles(x) { const array = [0.5, 1.5, 2]; return array.includes(x) + "," + array.indexOf(x); }
    function ignoresResults(o) { const array = [1, o]; array.includes(o); array.indexOf(o); array.includes(1); array.indexOf(1); return "done"; }
    const object = {};
    for (let i = 0; i < 100; ++i) {
        check(firstOfSeveral(object), "1,0,2", "the first of several");
        check(afterShrinking(object), "false,-1,false,-1", "elements beyond the length");
        check(afterPopping(object), "false,-1", "an element that was popped");
        check(withStart(object), "true,2,false,-1,2", "a start index");
        check(withoutNeedle(), "true,1,true,-1", "no needle");
        check(ofFrozen(object), "true,1,true,-1", "a frozen array");
        check(ofSparse(object), "true,0,true,100000,true,-1", "a sparse array");
        check(ofDoubles(2), "true,2", "an integer in an array of doubles");
        check(ofDoubles(object), "false,-1", "an object in an array of doubles");
        check(ignoresResults(object), "done", "results that are not used");
    }
    doesNotApply(withStart, includesStub, indexOfStub, includesOperation, indexOfOperation);

    class Subclass extends Array { }
    class Overriding extends Array { includes(needle) { return "overridden"; } indexOf(needle) { return "overridden"; } }
    function ofUnknownReceiver(array, needle) { return array.includes(needle) + "," + array.indexOf(needle); }
    noInline(ofUnknownReceiver);
    function withOwnMethods(o, replace) {
        const array = [1, o];
        if (replace) {
            array.includes = function () { return "own includes"; };
            array.indexOf = function () { return "own indexOf"; };
        }
        return array.includes(o) + "," + array.indexOf(o);
    }
    for (let i = 0; i < 100; ++i) {
        check(ofUnknownReceiver([1, object], object), "true,1", "an array as a parameter");
        check(ofUnknownReceiver(Subclass.of(1, object), object), "true,1", "an instance of a subclass");
        check(ofUnknownReceiver(Overriding.of(1, object), object), "overridden,overridden", "an instance of a subclass that overrides the methods");
        check(ofUnknownReceiver(other.Array.of(1, object), object), "true,1", "an array of another realm");
        check(ofUnknownReceiver("text", "x"), "true,2", "a string");
        check(ofUnknownReceiver(new Uint8Array([1, 2]), 2), "true,1", "a typed array");
        check(ofUnknownReceiver({ includes() { return "mine"; }, indexOf() { return "mine"; } }, 1), "mine,mine", "an object");
        check(withOwnMethods(object, false), "true,1", "no own methods");
        check(withOwnMethods(object, true), "own includes,own indexOf", "own methods");
    }

    function ofHoley(needle) { const array = [0, , 2]; return array.includes(needle) + "," + array.indexOf(needle); }
    check(ofHoley("inherited"), "false,-1", "a hole");
    check(ofHoley(7), "false,-1", "a hole");
    Array.prototype[1] = "inherited";
    for (let i = 0; i < 3; ++i) {
        check(ofHoley("inherited"), "true,1", "a hole when Array.prototype has an element");
        check(ofHoley(undefined), "false,-1", "undefined when Array.prototype has an element");
        check(ofHoley(2), "true,2", "an element after a hole when Array.prototype has an element");
    }
    Array.prototype[1] = 7;
    check(ofHoley(7), "true,1", "a hole when Array.prototype has an integer");
    Array.prototype[1] = object;
    check(ofHoley(object), "true,1", "a hole when Array.prototype has an object");
    delete Array.prototype[1];
    check(ofHoley(object), "false,-1", "a hole after the inherited element is gone");
    check(ofHoley(undefined), "true,-1", "undefined after the inherited element is gone");
})();
