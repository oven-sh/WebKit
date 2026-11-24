// RegExp.prototype[Symbol.replace] called directly takes the fast path of String.prototype.replace
// when the RegExp is primordial. The fast path must not be observable, and every step that runs user
// code before the RegExp is used (ToString(string), ToString(replaceValue)) must still be able to
// turn it off.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message ? message + ": " : ""}expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

function shouldThrow(func, errorType) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error(`expected ${errorType.name} but got ${error}`);
}

const symbolReplace = RegExp.prototype[Symbol.replace];

function describeOutcome(func) {
    try {
        return "value:" + func();
    } catch (e) {
        return "throw:" + e.constructor.name + ":" + e.message;
    }
}

// An own property sends the RegExp to the generic path and changes nothing else.
function generic(source, flags) {
    const regexp = new RegExp(source, flags);
    regexp.custom = true;
    return regexp;
}

// 1. The fast path and the generic path agree on the result and on lastIndex.
{
    const patterns = [
        ["\\d+", "g"], ["\\d+", ""], ["(\\d)(\\w)?", "g"], ["a|(b)", "g"], ["(a)|(b)|(c)", ""],
        ["(?<year>\\d{4})-(?<month>\\d{2})", "g"], ["(?<year>\\d{4})-(?<month>\\d{2})", ""],
        ["(?<a>x)|(?<a>y)", "g"], ["(?<first>x)|(?<second>y)", "g"], ["(?<__proto__>x)(?<constructor>y)?", "g"],
        ["(?:)", "g"], ["(?:)", "gu"], ["(?:)", ""], ["x*", "g"], ["^", "gm"], ["$", "gm"],
        ["\\u{1F600}", "gu"], [".", "gsu"], [".", "g"], ["\\p{L}+", "gv"],
        ["b", "y"], ["b", "gy"], ["(?:)", "y"], ["B", "gi"], ["(?<=a)b", "g"], ["nomatch", "g"], ["nomatch", ""],
    ];
    const inputs = [
        "", "abc 123 def 456 ghi", "2024-01-15 and 2025-12-31", "a\u{1F600}b\u{1F600}", "\uD83D", "xxyyxx",
        "line1\nline2\n", "abab", "bbab", "ABab", "a1b2c3".repeat(20),
    ];
    // What a replacer function is called with: the number of arguments, undefined captures as such, and the
    // groups object with its prototype, extensibility, key order and property attributes.
    function describeArgument(value) {
        if (value === undefined)
            return "<undefined>";
        if (typeof value !== "object" || value === null)
            return value;
        return {
            prototype: Object.getPrototypeOf(value) === null ? "null" : "an object",
            extensible: Object.isExtensible(value),
            properties: Reflect.ownKeys(value).map(key => {
                const descriptor = Object.getOwnPropertyDescriptor(value, key);
                const attributes = (descriptor.writable ? "w" : "-") + (descriptor.enumerable ? "e" : "-") + (descriptor.configurable ? "c" : "-");
                return [String(key), "value" in descriptor ? describeArgument(descriptor.value) : "<accessor>", attributes];
            }),
        };
    }
    const collect = function() {
        return "<" + JSON.stringify([arguments.length, Array.prototype.map.call(arguments, describeArgument)]) + ">";
    };
    const replacements = [
        "-", "", "$&$&", "[$1|$2|$3]", "$<year>/$<month>", "$<nope>", "$<a>", "$`|$'", "$$", "$0", "$10", "$",
        undefined, null, 42, 10n, {}, { toString() { return "[$&]"; } },
        collect, m => m.toUpperCase(), () => undefined, () => 7, () => "$&", () => ({ toString() { return "obj"; } }),
        Symbol("replacement"), () => { throw new RangeError("from the replacer"); }, { toString() { throw new SyntaxError("from toString"); } },
        new Proxy(function() { return "proxied"; }, {}),
    ];

    for (const [source, flags] of patterns) {
        // Only a sticky RegExp starts at lastIndex. The others reset it (global) or leave it alone.
        const lastIndices = flags.includes("y") ? [0, 1, 3, 1000] : [0, 3];
        for (const input of inputs) {
            for (let i = 0; i < replacements.length; ++i) {
                const replacement = replacements[i];
                for (const lastIndex of lastIndices) {
                    const fast = new RegExp(source, flags);
                    const slow = generic(source, flags);
                    const viaString = new RegExp(source, flags);
                    fast.lastIndex = slow.lastIndex = viaString.lastIndex = lastIndex;

                    const expected = describeOutcome(() => symbolReplace.call(slow, input, replacement));
                    const label = `/${source}/${flags} lastIndex=${lastIndex} on ${JSON.stringify(input)} with replacements[${i}]`;
                    shouldBe(describeOutcome(() => fast[Symbol.replace](input, replacement)), expected, label);
                    shouldBe(fast.lastIndex, slow.lastIndex, label + " (lastIndex)");
                    shouldBe(describeOutcome(() => input.replace(viaString, replacement)), expected, label + " (String.prototype.replace)");
                    shouldBe(viaString.lastIndex, slow.lastIndex, label + " (String.prototype.replace lastIndex)");
                }
            }
        }
    }
}

// 2. Hot loop, so that every tier calls it.
{
    function replaceDigits(regexp, string) {
        return regexp[Symbol.replace](string, "#");
    }
    noInline(replaceDigits);

    function replaceWithFunction(regexp, string) {
        return regexp[Symbol.replace](string, (match, p1, offset) => `${p1}@${offset}`);
    }
    noInline(replaceWithFunction);

    const global = /(\d)\d*/g;
    const single = /(\d)\d*/;
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(replaceDigits(global, "abc 123 def 456 ghi"), "abc # def # ghi");
        shouldBe(replaceDigits(single, "abc 123 def 456 ghi"), "abc # def 456 ghi");
        shouldBe(replaceWithFunction(global, "abc 123 def 456 ghi"), "abc 1@4 def 4@12 ghi");
        shouldBe(replaceWithFunction(single, "abc 123 def 456 ghi"), "abc 1@4 def 456 ghi");
        shouldBe(global.lastIndex, 0);
        shouldBe(single.lastIndex, 0);
    }
}

// 3. The arguments are converted once, in order, and a non-RegExp |this| still works.
{
    const log = [];
    const string = { toString() { log.push("string"); return "aXbX"; } };
    const replacement = { toString() { log.push("replaceValue"); return "-"; } };
    shouldBe(/X/g[Symbol.replace](string, replacement), "a-b-");
    shouldBe(generic("X", "g")[Symbol.replace](string, replacement), "a-b-");
    shouldBe(log.join(), "string,replaceValue,string,replaceValue");

    const fake = {
        flags: "",
        called: 0,
        exec(s) {
            if (this.called++)
                return null;
            const result = ["b"];
            result.index = 1;
            return result;
        },
    };
    shouldBe(symbolReplace.call(fake, "abc", "[$&]"), "a[b]c");
    shouldThrow(() => symbolReplace.call("not an object", "abc", "x"), TypeError);
    // The flags getter rejects a Proxy, after ToString(replaceValue).
    let conversions = 0;
    shouldThrow(() => symbolReplace.call(new Proxy(/b/, {}), "abc", { toString() { ++conversions; return "B"; } }), TypeError);
    shouldBe(conversions, 1);
}

// 4. The receiver stops being primordial while an argument is converted.
{
    // ToString(string) adds an own exec.
    let regexp = /a/g;
    let calls = 0;
    let result = regexp[Symbol.replace]({
        toString() {
            regexp.exec = function() { ++calls; return null; };
            return "aaa";
        }
    }, "b");
    shouldBe(result, "aaa");
    shouldBe(calls, 1);

    // ToString(replaceValue) adds an own exec.
    regexp = /a/g;
    calls = 0;
    let conversions = 0;
    result = regexp[Symbol.replace]("aaa", {
        toString() {
            ++conversions;
            regexp.exec = function() { ++calls; return null; };
            return "b";
        }
    });
    shouldBe(result, "aaa");
    shouldBe(calls, 1);
    shouldBe(conversions, 1);

    // ToString(replaceValue) makes the read of lastIndex observable.
    regexp = /a/y;
    const log = [];
    result = regexp[Symbol.replace]("aaa", {
        toString() {
            log.push("replaceValue");
            regexp.lastIndex = { valueOf() { log.push("lastIndex"); return 1; } };
            return "b";
        }
    });
    shouldBe(result, "aba");
    shouldBe(log.join(), "replaceValue,lastIndex");
    shouldBe(regexp.lastIndex, 2);

    // ToString(replaceValue) changes the flags that the generic path reads.
    regexp = /a/g;
    result = regexp[Symbol.replace]("aaa", {
        toString() {
            Object.defineProperty(regexp, "flags", { value: "" });
            return "b";
        }
    });
    shouldBe(result, "baa");

    // ToString(replaceValue) recompiles the RegExp. Both paths use the new pattern.
    regexp = /a/g;
    result = regexp[Symbol.replace]("aabb", {
        toString() {
            regexp.compile("b", "g");
            return "-";
        }
    });
    shouldBe(result, "aa--");
}

// 5. Subclasses, a lastIndex that cannot be written, and a RegExp of another realm.
{
    class Counting extends RegExp {
        exec(string) {
            ++Counting.calls;
            return super.exec(string);
        }
    }
    Counting.calls = 0;
    shouldBe(new Counting("a", "g")[Symbol.replace]("aaa", "b"), "bbb");
    shouldBe(Counting.calls, 4);

    const frozen = Object.freeze(/a/g);
    shouldThrow(() => frozen[Symbol.replace]("aaa", "b"), TypeError);
    const frozenSingle = Object.freeze(/a/);
    shouldBe(frozenSingle[Symbol.replace]("aaa", "b"), "baa");

    const other = createGlobalObject();
    shouldBe(other.RegExp.prototype[Symbol.replace].call(/(l+)/g, "hello world", "[$1]"), "he[ll]o wor[l]d");
    shouldBe(symbolReplace.call(new other.RegExp("(l+)", "g"), "hello world", "[$1]"), "he[ll]o wor[l]d");
    shouldBe(symbolReplace.call(new other.RegExp("(l+)", "g"), "hello world", (match, p1) => p1.length), "he2o wor1d");

    // A RegExp of another realm takes the generic path. Its "exec" updates the legacy static properties of the
    // realm of the RegExp, not those of the realm of the function that was called.
    function lastMatches(call) {
        /here/.exec("here");
        other.eval("/there/.exec('there')");
        call();
        return RegExp.lastMatch + "," + other.RegExp.lastMatch;
    }
    shouldBe(lastMatches(() => symbolReplace.call(/a1/, "a1", "-")), "a1,there");
    shouldBe(lastMatches(() => other.RegExp.prototype[Symbol.replace].call(/a1/, "a1", "-")), "a1,there");
    shouldBe(lastMatches(() => other.RegExp.prototype[Symbol.replace].call(/a1/g, "a1", () => "-")), "a1,there");
    shouldBe(lastMatches(() => symbolReplace.call(new other.RegExp("a1"), "a1", "-")), "here,a1");
    shouldBe(lastMatches(() => symbolReplace.call(new other.RegExp("a1", "g"), "a1", () => "-")), "here,a1");
}

// 6. The legacy static properties. After the call they are those of the last match on both paths.
{
    /(\d)(\d)/g[Symbol.replace]("a12b34c", "-");
    shouldBe(RegExp.lastMatch, "34");
    shouldBe(RegExp.$1, "3");
    shouldBe(RegExp.$2, "4");
    shouldBe(RegExp.leftContext, "a12b");
    shouldBe(RegExp.rightContext, "c");

    /(b)/[Symbol.replace]("abc", () => "");
    shouldBe(RegExp.lastMatch, "b");
    shouldBe(RegExp.$1, "b");
}

// Inside a replacer function of a global RegExp the two paths differ, and this is the one thing a program can
// see of the fast path. The generic path finds every match before the first call, as the specification says, so
// every call sees the last match. The fast path calls the function after each match, unless the function is a
// JS function, the RegExp has no named groups and the string has thresholdForStringReplaceCache (4096)
// characters or more: then it finds every match first too. The direct call sees what String.prototype.replace sees.
{
    function seenInside(call, regexp, string, wrap) {
        const seen = [];
        call(regexp, string, wrap(match => {
            seen.push(RegExp.lastMatch + RegExp.$1);
            return match;
        }));
        return seen.join();
    }
    const direct = (regexp, string, replacer) => regexp[Symbol.replace](string, replacer);
    const viaString = (regexp, string, replacer) => string.replace(regexp, replacer);
    const viaGeneric = (regexp, string, replacer) => generic(regexp.source, regexp.flags)[Symbol.replace](string, replacer);

    const short = "a1b2c3";
    const long = short + "x".repeat(5000);
    const plain = f => f;
    const bound = f => f.bind(null);
    const afterEachMatch = "11,22,33";
    const afterAllMatches = "33,33,33";

    const cases = [
        [() => /(\d)/g, short, plain, afterEachMatch],
        [() => /(\d)/g, short, bound, afterEachMatch],
        [() => /(\d)/g, long, plain, afterAllMatches],
        [() => /(\d)/g, long, bound, afterEachMatch],
        [() => /(?<digit>\d)/g, short, plain, afterEachMatch],
        [() => /(?<digit>\d)/g, long, plain, afterEachMatch],
        [() => /(\d)/, short, plain, "11"],
        [() => /(\d)/, long, plain, "11"],
    ];
    for (const [makeRegExp, string, wrap, expected] of cases) {
        const label = `${makeRegExp()} on ${string.length} characters`;
        shouldBe(seenInside(direct, makeRegExp(), string, wrap), expected, label);
        shouldBe(seenInside(viaString, makeRegExp(), string, wrap), expected, label + " (String.prototype.replace)");
        shouldBe(seenInside(viaGeneric, makeRegExp(), string, wrap), makeRegExp().global ? afterAllMatches : expected, label + " (generic path)");
    }
}

// 7. A replaced RegExp.prototype.exec is observed: while an argument is converted, and from then on.
//    This fires the primordial watchpoint for good, so it comes last.
{
    const originalExec = RegExp.prototype.exec;
    let calls = 0;
    const regexp = /a/g;
    let result = regexp[Symbol.replace]("aaa", {
        toString() {
            RegExp.prototype.exec = function(string) {
                ++calls;
                return originalExec.call(this, string);
            };
            return "b";
        }
    });
    shouldBe(result, "bbb");
    shouldBe(calls, 4);

    calls = 0;
    shouldBe(/a/g[Symbol.replace]("aaa", "c"), "ccc");
    shouldBe(calls, 4);

    RegExp.prototype.exec = function() { return null; };
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(/a/g[Symbol.replace]("aaa", "c"), "aaa");
}
